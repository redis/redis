#include "server.h"
#include "bitmap_roaring.h"
#include "endianconv.h"
#include "rdb.h"

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/run.h>
#include <roaring/memory.h>
#include <roaring/roaring.h>
#include <roaring/roaring64.h>
#include <roaring/roaring_array.h>
/* Local Redis patch to CRoaring: exposes struct roaring64_bitmap_s and the
 * leaf encoding so memory accounting, fork-child dismissal and active defrag
 * can walk every allocation, like they do for the other core types. Must come
 * after containers.h (it needs container_t). */
#include <roaring/roaring64_internal.h>

/* Native bitmaps always support 64-bit indexing: the maximum addressable bit
 * offset is what the protocol can express as a non-negative signed 64-bit
 * integer, independent of proto-max-bulk-len. Legacy string bitmaps remain
 * bounded by proto-max-bulk-len; the per-command offset checks in bitops.c
 * enforce that split (reads accept 64-bit offsets on either representation,
 * writes only on native bitmaps). */
typedef struct bitmapObject {
    uint64_t byte_len;
    roaring64_bitmap_t *roaring;
} bitmapObject;

static bitmapObject *getBitmapObject(const robj *o) {
    serverAssert(o->type == OBJ_BITMAP);
    serverAssert(o->encoding == OBJ_ENCODING_BITMAP_ROARING);
    return o->ptr;
}

static void *bitmapRoaringMalloc(size_t size) {
    return zmalloc(size);
}

static void *bitmapRoaringRealloc(void *ptr, size_t size) {
    return zrealloc(ptr, size);
}

static void *bitmapRoaringCalloc(size_t nmemb, size_t size) {
    return zcalloc_num(nmemb, size);
}

static void bitmapRoaringFree(void *ptr) {
    if (ptr == NULL) return;
    zfree(ptr);
}

/* Build a roaring bitmap from raw bitmap string bytes. Batch insertions
 * through add_many: this conversion runs on every bitmap-default-roaring write
 * that converts a string and on every string BITOP source, and per-bit
 * roaring64_bitmap_add dominates the cost on dense inputs. The optimize pass
 * is only worth paying for bitmaps that are kept (run_optimize/shrink_to_fit
 * walk every container); BITOP operand temporaries are freed within the
 * command, so their callers pass optimize=0. */
static roaring64_bitmap_t *bitmapObjectRoaringFromString(const unsigned char *buf,
                                                         size_t len, int optimize)
{
    roaring64_bitmap_t *roaring = roaring64_bitmap_create();

    uint64_t pending[1024];
    size_t npending = 0;

    for (size_t byte = 0; byte < len; byte++) {
        unsigned char value = buf[byte];
        while (value) {
            int bit = __builtin_ctz(value);
            pending[npending++] = (uint64_t)byte * 8 + (7 - bit);
            value &= value - 1;
            if (npending == sizeof(pending) / sizeof(pending[0])) {
                roaring64_bitmap_add_many(roaring, npending, pending);
                npending = 0;
            }
        }
    }
    if (npending) roaring64_bitmap_add_many(roaring, npending, pending);

    if (optimize) {
        roaring64_bitmap_run_optimize(roaring);
        roaring64_bitmap_shrink_to_fit(roaring);
    }
    return roaring;
}

/* The converters below are compiled on every architecture, even though the
 * save/load call sites only need them on big-endian hosts: DEBUG
 * BITMAP-ENDIAN-CHECK round-trips payloads through them so the parser gets CI
 * coverage on little-endian builds instead of shipping untested. */
static uint16_t bitmapPortableRead16(const char *p, int from_little_endian) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return from_little_endian ? intrev16(v) : v;
}

static uint32_t bitmapPortableRead32(const char *p, int from_little_endian) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return from_little_endian ? intrev32(v) : v;
}

static uint64_t bitmapPortableRead64(const char *p, int from_little_endian) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return from_little_endian ? intrev64(v) : v;
}

/* Convert one embedded 32-bit Roaring portable bitmap between host and little
 * endianness in place. Parses at most 'len' bytes from 'buf' and stores the
 * number of bytes the bitmap occupies in '*consumed'. */
static int bitmapPortableConvertEndian32(char *buf, size_t len,
                                         int from_little_endian,
                                         size_t *consumed)
{
    size_t pos = 0;

    if (len < sizeof(uint32_t)) return C_ERR;
    uint32_t cookie = bitmapPortableRead32(buf, from_little_endian);
    memrev32(buf);
    pos += sizeof(uint32_t);

    int32_t size;
    int hasrun;
    if ((cookie & 0xFFFF) == SERIAL_COOKIE) {
        size = (int32_t)((cookie >> 16) + 1);
        hasrun = 1;
    } else if (cookie == SERIAL_COOKIE_NO_RUNCONTAINER) {
        if (len - pos < sizeof(uint32_t)) return C_ERR;
        size = (int32_t)bitmapPortableRead32(buf + pos, from_little_endian);
        memrev32(buf + pos);
        pos += sizeof(uint32_t);
        hasrun = 0;
    } else {
        return C_ERR;
    }
    if (size < 0 || size > (1 << 16)) return C_ERR;

    char *runmap = NULL;
    if (hasrun) {
        size_t runmap_len = ((size_t)size + 7) / 8;
        if (len - pos < runmap_len) return C_ERR;
        runmap = buf + pos;
        pos += runmap_len;
    }

    size_t keycard_len = (size_t)size * 2 * sizeof(uint16_t);
    if (len - pos < keycard_len) return C_ERR;
    char *keycards = buf + pos;
    pos += keycard_len;

    if ((!hasrun) || (size >= NO_OFFSET_THRESHOLD)) {
        size_t offsets_len = (size_t)size * sizeof(uint32_t);
        if (len - pos < offsets_len) return C_ERR;
        for (int32_t i = 0; i < size; i++)
            memrev32(buf + pos + (size_t)i * sizeof(uint32_t));
        pos += offsets_len;
    }

    for (int32_t i = 0; i < size; i++) {
        char *key = keycards + (size_t)i * 2 * sizeof(uint16_t);
        char *cardp = key + sizeof(uint16_t);
        uint32_t cardinality = (uint32_t)bitmapPortableRead16(cardp, from_little_endian) + 1;
        int isbitmap = cardinality > DEFAULT_MAX_SIZE;
        int isrun = 0;

        memrev16(key);
        memrev16(cardp);

        if (hasrun && (runmap[i / 8] & (1 << (i % 8)))) {
            isbitmap = 0;
            isrun = 1;
        }

        if (isbitmap) {
            size_t words_len = BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
            if (len - pos < words_len) return C_ERR;
            for (size_t j = 0; j < BITSET_CONTAINER_SIZE_IN_WORDS; j++)
                memrev64(buf + pos + j * sizeof(uint64_t));
            pos += words_len;
        } else if (isrun) {
            if (len - pos < sizeof(uint16_t)) return C_ERR;
            uint16_t runs = bitmapPortableRead16(buf + pos, from_little_endian);
            memrev16(buf + pos);
            pos += sizeof(uint16_t);
            if (runs > (len - pos) / (2 * sizeof(uint16_t))) return C_ERR;
            for (uint32_t j = 0; j < (uint32_t)runs * 2; j++)
                memrev16(buf + pos + (size_t)j * sizeof(uint16_t));
            pos += (size_t)runs * 2 * sizeof(uint16_t);
        } else {
            if (cardinality > (len - pos) / sizeof(uint16_t)) return C_ERR;
            for (uint32_t j = 0; j < cardinality; j++)
                memrev16(buf + pos + (size_t)j * sizeof(uint16_t));
            pos += (size_t)cardinality * sizeof(uint16_t);
        }
    }

    *consumed = pos;
    return C_OK;
}

/* CRoaring's portable format is host-endian despite being byte-compatible
 * with the Roaring format spec on little-endian hosts. Redis RDB payloads
 * must be architecture-portable, so big-endian builds translate the CRoaring
 * payload to little-endian before saving and back to host-endian before
 * deserializing. The 64-bit framing is a u64 bucket count followed by, per
 * bucket, a u32 high key and an embedded 32-bit portable bitmap (see
 * RoaringFormatSpec, "extension for 64-bit implementations"). */
static int bitmapPortableConvertEndian(char *buf, size_t len, int from_little_endian) {
    size_t pos = 0;

    if (len < sizeof(uint64_t)) return C_ERR;
    uint64_t buckets = bitmapPortableRead64(buf, from_little_endian);
    memrev64(buf);
    pos += sizeof(uint64_t);
    if (buckets > UINT32_MAX) return C_ERR;

    for (uint64_t i = 0; i < buckets; i++) {
        if (len - pos < sizeof(uint32_t)) return C_ERR;
        memrev32(buf + pos);
        pos += sizeof(uint32_t);

        size_t consumed;
        if (bitmapPortableConvertEndian32(buf + pos, len - pos,
                                          from_little_endian,
                                          &consumed) != C_OK)
            return C_ERR;
        pos += consumed;
    }

    return pos == len ? C_OK : C_ERR;
}

static int bitmapRoaringNormalizeAlignment(size_t *alignment) {
    size_t normalized;

    if (*alignment < sizeof(void *)) {
        *alignment = sizeof(void *);
        return C_OK;
    }
    if ((*alignment & (*alignment - 1)) == 0) return C_OK;

    normalized = sizeof(void *);
    while (normalized < *alignment) {
        if (normalized > SIZE_MAX / 2) return C_ERR;
        normalized <<= 1;
    }
    *alignment = normalized;
    return C_OK;
}

static void *bitmapRoaringAlignedMalloc(size_t alignment, size_t size) {
    void *base;
    uintptr_t raw, aligned;
    size_t extra;

    if (bitmapRoaringNormalizeAlignment(&alignment) != C_OK) return NULL;
    if (alignment - 1 > SIZE_MAX - sizeof(void *)) return NULL;
    extra = alignment - 1 + sizeof(void *);
    if (size > SIZE_MAX - extra) return NULL;

    base = zmalloc(size + extra);
    raw = (uintptr_t)base + sizeof(void *);
    aligned = (raw + alignment - 1) & ~((uintptr_t)alignment - 1);
    ((void **)aligned)[-1] = base;
    return (void *)aligned;
}

static void *bitmapRoaringAlignedAllocBase(void *ptr) {
    if (ptr == NULL) return NULL;
    return ((void **)ptr)[-1];
}

static void bitmapRoaringAlignedFree(void *ptr) {
    if (ptr == NULL) return;
    void *base = bitmapRoaringAlignedAllocBase(ptr);
    zfree(base);
}

static size_t bitmapRoaringMallocSize(const void *ptr) {
    return ptr == NULL ? 0 : zmalloc_size((void *)ptr);
}

static size_t bitmapRoaringAlignedMallocSize(void *ptr) {
    void *base = bitmapRoaringAlignedAllocBase(ptr);
    return base == NULL ? 0 : zmalloc_size(base);
}

void bitmapRoaringInit(void) {
    roaring_memory_t memory_hook = {
        .malloc = bitmapRoaringMalloc,
        .realloc = bitmapRoaringRealloc,
        .calloc = bitmapRoaringCalloc,
        .free = bitmapRoaringFree,
        .aligned_malloc = bitmapRoaringAlignedMalloc,
        .aligned_free = bitmapRoaringAlignedFree,
    };

    roaring_init_memory_hook(memory_hook);
}

robj *createBitmapObject(void) {
    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = 0;
    bitmap->roaring = roaring64_bitmap_create();

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *createBitmapObjectFromString(const unsigned char *buf, size_t len) {
#if SIZE_MAX > BITMAP_OBJECT_MAX_BYTES_RAW
    if ((uint64_t)len > BITMAP_OBJECT_MAX_BYTES) return NULL;
#endif

    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = len;
    bitmap->roaring = bitmapObjectRoaringFromString(buf, len, 1);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *createBitmapObjectFromPortable(uint64_t byte_len, const char *buf, size_t len, int deep_validate) {
    if (byte_len > BITMAP_OBJECT_MAX_BYTES) return NULL;

    const char *portable = buf;
    sds converted = NULL;
#if (BYTE_ORDER == BIG_ENDIAN)
    converted = sdsnewlen(buf, len);
    if (bitmapPortableConvertEndian(converted, len, 1) != C_OK) {
        sdsfree(converted);
        return NULL;
    }
    portable = converted;
#else
    if (roaring64_bitmap_portable_deserialize_size(buf, len) != len) return NULL;
#endif

    roaring64_bitmap_t *roaring =
        roaring64_bitmap_portable_deserialize_safe(portable, len);
    sdsfree(converted);
    if (roaring == NULL) return NULL;

    /* The safe deserializer bounds the reads but does not verify structural
     * invariants (sorted array containers, sorted non-overlapping runs);
     * CRoaring documents bitmaps from untrusted input as unsafe to use until
     * roaring64_bitmap_internal_validate passes. */
    if (deep_validate) {
        const char *reason = NULL;
        if (!roaring64_bitmap_internal_validate(roaring, &reason)) {
            roaring64_bitmap_free(roaring);
            return NULL;
        }
    }

    if (roaring64_bitmap_get_cardinality(roaring) != 0) {
        uint64_t max = roaring64_bitmap_maximum(roaring);
        if (max >= byte_len * 8) {
            roaring64_bitmap_free(roaring);
            return NULL;
        }
    }

    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = byte_len;
    bitmap->roaring = roaring;

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *bitmapTypeDup(const robj *o) {
    bitmapObject *src = getBitmapObject(o);
    bitmapObject *dst = zmalloc(sizeof(*dst));
    dst->byte_len = src->byte_len;
    dst->roaring = roaring64_bitmap_copy(src->roaring);

    robj *copy = createObject(OBJ_BITMAP, dst);
    copy->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return copy;
}

void freeBitmapObject(robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    roaring64_bitmap_free(bitmap->roaring);
    zfree(bitmap);
}

uint64_t bitmapObjectLen(const robj *o) {
    return getBitmapObject(o)->byte_len;
}

/* --- ART walk helpers ---
 *
 * CRoaring's 64-bit bitmap keeps one container per 2^16-bit chunk, indexed by
 * the chunk's high 48 bits in an ART. ART nodes are index-based (no parent or
 * child pointers), the leaf value encodes a typecode plus an index into one
 * flat container pointer array, and copy-on-write does not exist for 64-bit
 * bitmaps, so shared containers can never appear.
 *
 * art_t.nodes[] is indexed by node typecode 1..5 (leaf, node4, node16,
 * node48, node256); slot 0 belongs to the null typecode and is deliberately
 * left uninitialized by art_init_cleared(), so walks must never touch it. */
#define BITMAP_ART_MIN_NODE_TYPE 1
#define BITMAP_ART_MAX_NODE_TYPE 5

static void bitmapArtKeyFromHigh48(uint64_t high48, art_key_chunk_t key[ART_KEY_BYTES]) {
    for (int i = 0; i < ART_KEY_BYTES; i++)
        key[i] = (art_key_chunk_t)(high48 >> (8 * (ART_KEY_BYTES - 1 - i)));
}

static uint64_t bitmapArtKeyToHigh48(const art_key_chunk_t key[ART_KEY_BYTES]) {
    uint64_t v = 0;
    for (int i = 0; i < ART_KEY_BYTES; i++) v = (v << 8) | key[i];
    return v;
}

static int bitmapRoaringIsFrozen(const roaring64_bitmap_t *r) {
    return r->flags & ROARING_FLAG_FROZEN;
}

static size_t bitmapRoaringContainerAllocSize(const roaring64_bitmap_t *r,
                                              const container_t *container,
                                              uint8_t typecode)
{
    if (container == NULL) return 0;

    size_t size = bitmapRoaringMallocSize(container);
    if (bitmapRoaringIsFrozen(r)) return size;

    switch (typecode) {
    case ARRAY_CONTAINER_TYPE: {
        const array_container_t *array = const_CAST_array(container);
        return size + bitmapRoaringMallocSize(array->array);
    }
    case BITSET_CONTAINER_TYPE: {
        const bitset_container_t *bitset = const_CAST_bitset(container);
        return size + bitmapRoaringAlignedMallocSize(bitset->words);
    }
    case RUN_CONTAINER_TYPE: {
        const run_container_t *run = const_CAST_run(container);
        return size + bitmapRoaringMallocSize(run->runs);
    }
    case SHARED_CONTAINER_TYPE: {
        const shared_container_t *shared = const_CAST_shared(container);
        return size + bitmapRoaringContainerAllocSize(r, shared->container,
                                                      shared->typecode);
    }
    default:
        serverPanic("Unknown roaring bitmap container type");
    }
}

static size_t bitmapRoaringAllocSize(const roaring64_bitmap_t *r) {
    size_t size = bitmapRoaringMallocSize(r);

    if (!bitmapRoaringIsFrozen(r)) {
        for (int i = BITMAP_ART_MIN_NODE_TYPE; i <= BITMAP_ART_MAX_NODE_TYPE; i++)
            size += bitmapRoaringMallocSize(r->art.nodes[i]);
    }
    size += bitmapRoaringMallocSize(r->containers);

    art_iterator_t it = art_init_iterator((art_t *)&r->art, true);
    while (it.value != NULL) {
        roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
        size += bitmapRoaringContainerAllocSize(
            r, r->containers[roaring64_leaf_index(leaf)],
            roaring64_leaf_typecode(leaf));
        art_iterator_next(&it);
    }
    return size;
}

size_t bitmapObjectAllocSize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return bitmapRoaringMallocSize(bitmap) +
           bitmapRoaringAllocSize(bitmap->roaring);
}

size_t bitmapObjectContainerCount(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    size_t count = 0;

    art_iterator_t it = art_init_iterator((art_t *)&bitmap->roaring->art, true);
    while (it.value != NULL) {
        count++;
        art_iterator_next(&it);
    }
    return count;
}

static uint16_t bitmapRdbRead16(const char *p, int from_little_endian) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
#if (BYTE_ORDER == BIG_ENDIAN)
    return from_little_endian ? intrev16(v) : v;
#else
    UNUSED(from_little_endian);
    return v;
#endif
}

#define BITMAP_RDB_MAX_HIGH48 UINT64_C(0xffffffffffff)
#define BITMAP_RDB_MAX_CONTAINER_CARDINALITY (1 << 16)
#define BITMAP_RDB_CONTAINER_BYTES (1 << 13)

static int bitmapContainerPayloadLenValid(uint8_t typecode, uint64_t cardinality,
                                          const char *payload, size_t payload_len)
{
    if (cardinality == 0 || cardinality > BITMAP_RDB_MAX_CONTAINER_CARDINALITY)
        return C_ERR;

    switch (typecode) {
    case ARRAY_CONTAINER_TYPE:
        return payload_len == cardinality * sizeof(uint16_t) ? C_OK : C_ERR;
    case BITSET_CONTAINER_TYPE:
        return payload_len == BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t) ?
            C_OK : C_ERR;
    case RUN_CONTAINER_TYPE: {
        if (payload_len < sizeof(uint16_t)) return C_ERR;
        uint16_t runs = bitmapRdbRead16(payload, 1);
        if (runs == 0) return C_ERR;
        size_t expected = sizeof(uint16_t) + (size_t)runs * sizeof(rle16_t);
        return payload_len == expected ? C_OK : C_ERR;
    }
    default:
        return C_ERR;
    }
}

static int bitmapContainerConvertEndian(char *payload, size_t payload_len,
                                        uint8_t typecode, int from_little_endian)
{
#if (BYTE_ORDER == BIG_ENDIAN)
    switch (typecode) {
    case ARRAY_CONTAINER_TYPE:
        for (size_t i = 0; i < payload_len / sizeof(uint16_t); i++)
            memrev16(payload + i * sizeof(uint16_t));
        break;
    case BITSET_CONTAINER_TYPE:
        for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i++)
            memrev64(payload + i * sizeof(uint64_t));
        break;
    case RUN_CONTAINER_TYPE: {
        if (payload_len < sizeof(uint16_t)) return C_ERR;
        uint16_t runs = bitmapRdbRead16(payload, from_little_endian);
        if (payload_len != sizeof(uint16_t) + (size_t)runs * sizeof(rle16_t))
            return C_ERR;

        memrev16(payload);
        for (size_t i = 0; i < (size_t)runs * 2; i++)
            memrev16(payload + sizeof(uint16_t) + i * sizeof(uint16_t));
        break;
    }
    default:
        return C_ERR;
    }
#else
    UNUSED(payload);
    UNUSED(payload_len);
    UNUSED(typecode);
    UNUSED(from_little_endian);
#endif
    return C_OK;
}

static container_t *bitmapContainerFromRdbPayload(uint8_t typecode,
                                                  uint64_t cardinality,
                                                  char *payload,
                                                  size_t payload_len,
                                                  int deep_validate)
{
    if (bitmapContainerPayloadLenValid(typecode, cardinality, payload,
                                       payload_len) != C_OK)
        return NULL;

    if (bitmapContainerConvertEndian(payload, payload_len, typecode, 1) != C_OK)
        return NULL;

    container_t *container = NULL;
    switch (typecode) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array =
            array_container_create_given_capacity((int32_t)cardinality);
        if (array == NULL) return NULL;
        if ((size_t)array_container_read((int32_t)cardinality, array,
                                         payload) != payload_len) {
            array_container_free(array);
            return NULL;
        }
        container = (container_t *)array;
        break;
    }
    case BITSET_CONTAINER_TYPE: {
        bitset_container_t *bitset = bitset_container_create();
        if (bitset == NULL) return NULL;
        if ((size_t)bitset_container_read((int32_t)cardinality, bitset,
                                          payload) != payload_len) {
            bitset_container_free(bitset);
            return NULL;
        }
        container = (container_t *)bitset;
        break;
    }
    case RUN_CONTAINER_TYPE: {
        uint16_t runs = bitmapRdbRead16(payload, 0);
        run_container_t *run = run_container_create_given_capacity(runs);
        if (run == NULL) return NULL;
        if ((size_t)run_container_read((int32_t)cardinality, run,
                                       payload) != payload_len) {
            run_container_free(run);
            return NULL;
        }
        container = (container_t *)run;
        break;
    }
    default:
        return NULL;
    }

    int actual_cardinality = container_get_cardinality(container, typecode);
    if (actual_cardinality < 0 || (uint64_t)actual_cardinality != cardinality) {
        container_free(container, typecode);
        return NULL;
    }

    if (deep_validate) {
        const char *reason = NULL;
        if (!container_internal_validate(container, typecode, &reason)) {
            container_free(container, typecode);
            return NULL;
        }
    }

    return container;
}

static int bitmapRoaringResizeContainerArray(roaring64_bitmap_t *r,
                                             uint64_t capacity)
{
    if (capacity == 0) {
        bitmapRoaringFree(r->containers);
        r->containers = NULL;
        r->capacity = 0;
        return C_OK;
    }

    if (capacity > SIZE_MAX / sizeof(*r->containers)) return C_ERR;

    size_t old_capacity = (size_t)r->capacity;
    size_t bytes = (size_t)capacity * sizeof(*r->containers);
    /* ztryrealloc() frees the original pointer on this overflow guard, so
     * reject it before calling while the bitmap is still internally valid. */
    if (bytes >= SIZE_MAX / 2) return C_ERR;

    container_t **containers = ztryrealloc(r->containers, bytes);
    if (containers == NULL) return C_ERR;

    if (capacity > r->capacity) {
        memset(containers + old_capacity, 0,
               ((size_t)capacity - old_capacity) * sizeof(*containers));
    }

    r->containers = containers;
    r->capacity = capacity;
    return C_OK;
}

static int bitmapRoaringEnsureContainerCapacity(roaring64_bitmap_t *r,
                                                uint64_t min_capacity)
{
    if (min_capacity <= r->capacity) return C_OK;

    uint64_t capacity = r->capacity == 0 ? 2 : r->capacity;
    while (capacity < min_capacity) {
        if (capacity < 1024) {
            if (capacity > UINT64_MAX / 2) return C_ERR;
            capacity *= 2;
        } else {
            uint64_t increase = capacity / 4;
            if (increase == 0 || capacity > UINT64_MAX - increase)
                return C_ERR;
            capacity += increase;
        }
    }

    return bitmapRoaringResizeContainerArray(r, capacity);
}

static void bitmapRoaringTryShrinkContainerArray(roaring64_bitmap_t *r) {
    if (r->first_free < r->capacity)
        (void)bitmapRoaringResizeContainerArray(r, r->first_free);
}

static int bitmapRoaringAppendContainer(roaring64_bitmap_t *r,
                                        container_t *container,
                                        uint8_t typecode,
                                        roaring64_leaf_t *leaf)
{
    if (r->first_free == UINT64_MAX) return C_ERR;
    if (bitmapRoaringEnsureContainerCapacity(r, r->first_free + 1) != C_OK)
        return C_ERR;

    uint64_t index = r->first_free++;
    r->containers[index] = container;
    *leaf = (index << 8) | typecode;
    return C_OK;
}

ssize_t bitmapObjectSaveRdb(rio *rdb, const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    roaring64_bitmap_t *roaring = bitmap->roaring;
    ssize_t n, nwritten = 0;

    if ((n = rdbSaveLen(rdb, bitmap->byte_len)) == -1) return -1;
    nwritten += n;

    if ((n = rdbSaveLen(rdb, bitmapObjectContainerCount(o))) == -1) return -1;
    nwritten += n;

    art_iterator_t it = art_init_iterator((art_t *)&roaring->art, true);
    while (it.value != NULL) {
        roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
        uint8_t typecode = roaring64_leaf_typecode(leaf);
        container_t *container = roaring->containers[roaring64_leaf_index(leaf)];
        uint64_t high48 = bitmapArtKeyToHigh48(it.key);
        int cardinality = container_get_cardinality(container, typecode);
        int32_t payload_len = container_size_in_bytes(container, typecode);

        serverAssert(cardinality > 0);
        serverAssert(payload_len >= 0);

        if ((n = rdbSaveLen(rdb, high48)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb, typecode)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb, (uint64_t)cardinality)) == -1) return -1;
        nwritten += n;

        sds payload = sdsnewlen(SDS_NOINIT, (size_t)payload_len);
        int32_t written = container_write(container, typecode, payload);
        serverAssert(written == payload_len);
        if (bitmapContainerConvertEndian(payload, (size_t)payload_len,
                                         typecode, 0) != C_OK) {
            sdsfree(payload);
            return -1;
        }

        if ((n = rdbSaveRawString(rdb, (unsigned char *)payload,
                                  (size_t)payload_len)) == -1) {
            sdsfree(payload);
            return -1;
        }
        nwritten += n;
        sdsfree(payload);

        art_iterator_next(&it);
    }

    return nwritten;
}

robj *createBitmapObjectFromRdb(rio *rdb, int deep_validate) {
    uint64_t byte_len, container_count;
    roaring64_bitmap_t *roaring = NULL;
    sds payload = NULL;

    if ((byte_len = rdbLoadLen(rdb, NULL)) == RDB_LENERR) return NULL;
    if (byte_len > BITMAP_OBJECT_MAX_BYTES) return NULL;
    if ((container_count = rdbLoadLen(rdb, NULL)) == RDB_LENERR) return NULL;

    uint64_t max_containers = byte_len == 0 ? 0 :
        ((byte_len - 1) / BITMAP_RDB_CONTAINER_BYTES) + 1;
    if (container_count > max_containers) return NULL;

    roaring = roaring64_bitmap_create();

    uint64_t previous_high48 = 0;
    int have_previous_high48 = 0;
    for (uint64_t i = 0; i < container_count; i++) {
        uint64_t high48, raw_typecode, cardinality;
        size_t payload_len;

        if ((high48 = rdbLoadLen(rdb, NULL)) == RDB_LENERR) goto fail;
        if (high48 > BITMAP_RDB_MAX_HIGH48) goto fail;
        if (have_previous_high48 && high48 <= previous_high48) goto fail;
        previous_high48 = high48;
        have_previous_high48 = 1;

        if ((raw_typecode = rdbLoadLen(rdb, NULL)) == RDB_LENERR) goto fail;
        if (raw_typecode > UINT8_MAX) goto fail;
        uint8_t typecode = (uint8_t)raw_typecode;

        if ((cardinality = rdbLoadLen(rdb, NULL)) == RDB_LENERR) goto fail;

        payload = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, &payload_len);
        if (payload == NULL) goto fail;

        container_t *container = bitmapContainerFromRdbPayload(
            typecode, cardinality, payload, payload_len, deep_validate);
        sdsfree(payload);
        payload = NULL;
        if (container == NULL) goto fail;

        art_key_chunk_t key[ART_KEY_BYTES];
        bitmapArtKeyFromHigh48(high48, key);
        roaring64_leaf_t leaf;
        if (bitmapRoaringAppendContainer(roaring, container, typecode,
                                         &leaf) != C_OK) {
            container_free(container, typecode);
            goto fail;
        }
        art_insert(&roaring->art, key, (art_val_t)leaf);
    }

    if (roaring64_bitmap_get_cardinality(roaring) != 0) {
        uint64_t max = roaring64_bitmap_maximum(roaring);
        if (max >= byte_len * 8) goto fail;
    }
    bitmapRoaringTryShrinkContainerArray(roaring);

    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = byte_len;
    bitmap->roaring = roaring;

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;

fail:
    sdsfree(payload);
    if (roaring != NULL) roaring64_bitmap_free(roaring);
    return NULL;
}

static void bitmapObjectDismissContainer(container_t *container, uint8_t type) {
    if (container == NULL) return;

    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array = CAST_array(container);
        dismissMemory(array->array,
                      (size_t)array->capacity * sizeof(*array->array));
        dismissMemory(array, sizeof(*array));
        break;
    }
    case BITSET_CONTAINER_TYPE: {
        bitset_container_t *bitset = CAST_bitset(container);
        dismissMemory(bitmapRoaringAlignedAllocBase(bitset->words), 0);
        dismissMemory(bitset, sizeof(*bitset));
        break;
    }
    case RUN_CONTAINER_TYPE: {
        run_container_t *run = CAST_run(container);
        dismissMemory(run->runs, (size_t)run->capacity * sizeof(*run->runs));
        dismissMemory(run, sizeof(*run));
        break;
    }
    default:
        serverPanic("Unknown roaring bitmap container type");
    }
}

void dismissBitmapObject(robj *o, size_t size_hint) {
    bitmapObject *bitmap = getBitmapObject(o);
    roaring64_bitmap_t *r = bitmap->roaring;
    size_t count = bitmapObjectContainerCount(o);

    /* Iterate the containers only when the average container payload is
     * likely to span whole pages, like the other complex types (see
     * dismissObject()). */
    if (count > 0 && size_hint / count >= server.page_size) {
        art_iterator_t it = art_init_iterator((art_t *)&r->art, true);
        while (it.value != NULL) {
            roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
            bitmapObjectDismissContainer(
                r->containers[roaring64_leaf_index(leaf)],
                roaring64_leaf_typecode(leaf));
            art_iterator_next(&it);
        }
    }

    for (int i = BITMAP_ART_MIN_NODE_TYPE; i <= BITMAP_ART_MAX_NODE_TYPE; i++) {
        if (r->art.nodes[i] != NULL) dismissMemory(r->art.nodes[i], 0);
    }
    dismissMemory(r->containers, 0);
    dismissMemory(r, sizeof(*r));
    dismissMemory(bitmap, sizeof(*bitmap));
}

/* --- Active defrag support ---
 *
 * Every allocation CRoaring makes for a bitmap goes through the zmalloc-backed
 * memory hook installed in bitmapRoaringInit(), so active defrag can relocate
 * each one with activeDefragAlloc(). The walk mirrors dismissBitmapObject():
 * the wrapper struct, the roaring bitmap struct, the per-type ART node arrays
 * (which hold indices, not pointers, so relocating them needs no fixups), the
 * flat container pointer array, and every container with its payload buffer.
 * Relocating a container only requires updating its slot in the container
 * pointer array; the ART leaf stores an index that does not change. */

/* CRoaring requests this alignment for bitset container word buffers in the
 * portable build Redis uses (see align_size in CRoaring's
 * bitset_container_create(); the SIMD-gated 64-byte case is compiled out by
 * ROARING_DISABLE_X64). bitmapRoaringAlignedMalloc() over-allocates by
 * alignment - 1 + sizeof(void *), so re-deriving an aligned offset inside a
 * relocated block always fits. */
#define BITMAP_ROARING_BITSET_WORDS_ALIGNMENT 32

static void *bitmapObjectActiveDefragAlloc(void *ptr) {
    return activeDefragAlloc(ptr);
}

static bitmapObject *bitmapObjectActiveDefragSelf(bitmapObject *bitmap) {
    return activeDefragAlloc(bitmap);
}

static void bitmapObjectDefragBitsetWords(bitset_container_t *bitset) {
    void *base, *newbase;
    uintptr_t aligned;
    size_t old_off, new_off;

    if (bitset->words == NULL) return;
    base = bitmapRoaringAlignedAllocBase(bitset->words);
    newbase = bitmapObjectActiveDefragAlloc(base);
    if (newbase == NULL) return;

    /* The relocation copied the block verbatim, so the words still sit at
     * their old offset; re-derive the aligned offset for the new base address
     * and slide the words when the two differ. */
    old_off = (size_t)((char *)bitset->words - (char *)base);
    aligned = ((uintptr_t)newbase + sizeof(void *) +
               (BITMAP_ROARING_BITSET_WORDS_ALIGNMENT - 1)) &
              ~((uintptr_t)BITMAP_ROARING_BITSET_WORDS_ALIGNMENT - 1);
    new_off = (size_t)(aligned - (uintptr_t)newbase);
    if (new_off != old_off)
        memmove((char *)newbase + new_off, (char *)newbase + old_off,
                sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    ((void **)aligned)[-1] = newbase;
    bitset->words = (uint64_t *)aligned;
}

/* Defrag one container; returns the moved container pointer, or NULL if the
 * container struct itself did not move (its payload may still have moved). */
static container_t *bitmapObjectDefragContainer(container_t *container,
                                                uint8_t type) {
    void *moved;

    if (container == NULL) return NULL;

    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array = CAST_array(container);
        uint16_t *values;
        if ((moved = bitmapObjectActiveDefragAlloc(array)) != NULL)
            array = (array_container_t *)moved;
        if (array->array != NULL &&
            (values = bitmapObjectActiveDefragAlloc(array->array)) != NULL)
            array->array = values;
        return (container_t *)moved;
    }
    case BITSET_CONTAINER_TYPE: {
        bitset_container_t *bitset = CAST_bitset(container);
        if ((moved = bitmapObjectActiveDefragAlloc(bitset)) != NULL)
            bitset = (bitset_container_t *)moved;
        bitmapObjectDefragBitsetWords(bitset);
        return (container_t *)moved;
    }
    case RUN_CONTAINER_TYPE: {
        run_container_t *run = CAST_run(container);
        rle16_t *runs;
        if ((moved = bitmapObjectActiveDefragAlloc(run)) != NULL)
            run = (run_container_t *)moved;
        if (run->runs != NULL &&
            (runs = bitmapObjectActiveDefragAlloc(run->runs)) != NULL)
            run->runs = runs;
        return (container_t *)moved;
    }
    default:
        serverPanic("Unknown roaring bitmap container type");
    }
}

/* Relocate the wrapper struct, roaring struct, ART node arrays and container
 * pointer array, and refresh o->ptr. Container payloads are handled
 * separately so the walk can be split into incremental steps. */
static roaring64_bitmap_t *bitmapObjectDefragTopLevel(robj *o) {
    bitmapObject *bitmap = getBitmapObject(o), *newbitmap;
    roaring64_bitmap_t *r, *newroaring;

    if ((newbitmap = bitmapObjectActiveDefragSelf(bitmap)) != NULL)
        o->ptr = bitmap = newbitmap;
    if ((newroaring = bitmapObjectActiveDefragAlloc(bitmap->roaring)) != NULL)
        bitmap->roaring = newroaring;
    r = bitmap->roaring;

    for (int i = BITMAP_ART_MIN_NODE_TYPE; i <= BITMAP_ART_MAX_NODE_TYPE; i++) {
        void *moved;
        if (r->art.nodes[i] != NULL &&
            (moved = bitmapObjectActiveDefragAlloc(r->art.nodes[i])) != NULL)
            r->art.nodes[i] = moved;
    }
    if (r->containers != NULL) {
        container_t **moved = bitmapObjectActiveDefragAlloc(r->containers);
        if (moved != NULL) r->containers = moved;
    }
    return r;
}

static void bitmapObjectDefragLeafContainer(roaring64_bitmap_t *r,
                                            roaring64_leaf_t leaf) {
    uint64_t index = roaring64_leaf_index(leaf);
    container_t *moved = bitmapObjectDefragContainer(
        r->containers[index], roaring64_leaf_typecode(leaf));
    if (moved != NULL) r->containers[index] = moved;
}

void bitmapObjectDefrag(robj *o) {
    roaring64_bitmap_t *r = bitmapObjectDefragTopLevel(o);

    art_iterator_t it = art_init_iterator(&r->art, true);
    while (it.value != NULL) {
        bitmapObjectDefragLeafContainer(r, (roaring64_leaf_t)*it.value);
        art_iterator_next(&it);
    }
}

/* Incremental defrag step for bitmaps queued with defragLater(): 'cursor' is
 * one plus the high 48 bits of the last container processed (so it is never 0
 * mid-walk; 0 also relocates the top-level allocations). The key-space cursor
 * survives concurrent writes: containers added or removed between steps only
 * make the walk skip or revisit a few containers, which is harmless for
 * defrag. On builds where unsigned long is 32 bits the cursor truncates,
 * which can only cause the same harmless skip/revisit. Returns the new
 * cursor, 0 when done. */
unsigned long bitmapObjectDefragIncremental(robj *o, unsigned long cursor) {
    roaring64_bitmap_t *r;
    art_iterator_t it;
    long iterations = 0;

    if (cursor == 0) {
        r = bitmapObjectDefragTopLevel(o);
        it = art_init_iterator(&r->art, true);
    } else {
        art_key_chunk_t key[ART_KEY_BYTES];
        r = getBitmapObject(o)->roaring;
        bitmapArtKeyFromHigh48((uint64_t)cursor, key);
        it = art_lower_bound(&r->art, key);
    }

    while (it.value != NULL) {
        bitmapObjectDefragLeafContainer(r, (roaring64_leaf_t)*it.value);
        uint64_t high48 = bitmapArtKeyToHigh48(it.key);
        art_iterator_next(&it);
        if (++iterations >= 128 && it.value != NULL)
            return (unsigned long)(high48 + 1);
    }
    return 0;
}

uint64_t bitmapObjectCardinality(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return roaring64_bitmap_get_cardinality(bitmap->roaring);
}

uint64_t bitmapObjectRangeCardinality(const robj *o, uint64_t start,
                                      uint64_t end)
{
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t bit_len = bitmap->byte_len * 8;

    if (start >= end || start >= bit_len) return 0;
    if (end > bit_len) end = bit_len;
    return roaring64_bitmap_range_cardinality(bitmap->roaring, start, end);
}

static long long bitmapObjectFirstSetBit(bitmapObject *bitmap, uint64_t start,
                                         uint64_t end)
{
    long long result = -1;

    if (start >= end) return -1;

    roaring64_iterator_t *it = roaring64_iterator_create(bitmap->roaring);
    if (roaring64_iterator_move_equalorlarger(it, start)) {
        uint64_t value = roaring64_iterator_value(it);
        if (value < end) result = (long long)value;
    }
    roaring64_iterator_free(it);
    return result;
}

/* First clear bit in [from, 1<<16) within one container, or -1 if every bit
 * from 'from' to the end of the container is set. */
static int32_t bitmapContainerFirstClearBit(const container_t *container,
                                            uint8_t type, uint32_t from)
{
    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        const array_container_t *array = const_CAST_array(container);
        int32_t lo = 0, hi = array->cardinality;
        while (lo < hi) {
            int32_t mid = (lo + hi) / 2;
            if (array->array[mid] < from) lo = mid + 1;
            else hi = mid;
        }
        uint32_t expected = from;
        for (int32_t i = lo; i < array->cardinality; i++) {
            if (array->array[i] != expected) return (int32_t)expected;
            expected++;
        }
        return expected < (1 << 16) ? (int32_t)expected : -1;
    }
    case BITSET_CONTAINER_TYPE: {
        const bitset_container_t *bitset = const_CAST_bitset(container);
        uint32_t word_idx = from >> 6;
        uint64_t word = ~bitset->words[word_idx] &
                        (UINT64_MAX << (from & 63));
        while (1) {
            if (word) return (int32_t)(word_idx * 64 + __builtin_ctzll(word));
            if (++word_idx >= BITSET_CONTAINER_SIZE_IN_WORDS) return -1;
            word = ~bitset->words[word_idx];
        }
    }
    case RUN_CONTAINER_TYPE: {
        const run_container_t *run = const_CAST_run(container);
        uint32_t pos = from;
        for (int32_t i = 0; i < run->n_runs; i++) {
            uint32_t rstart = run->runs[i].value;
            uint32_t rend = rstart + run->runs[i].length;
            if (pos < rstart) return (int32_t)pos;
            if (pos <= rend) pos = rend + 1;
        }
        return pos < (1 << 16) ? (int32_t)pos : -1;
    }
    default:
        serverPanic("Unknown roaring bitmap container type");
    }
}

/* Walk containers in key order looking for the first clear bit: a gap before
 * the next container is all zeroes, and within a container each type can find
 * its first absent bit directly. This stays proportional to the containers
 * spanned by set bits, never to the number of set bits, so dense runs spanning
 * billions of bits do not degrade BITPOS. */
static long long bitmapObjectFirstClearBit(bitmapObject *bitmap,
                                           uint64_t start, uint64_t end,
                                           int end_given)
{
    roaring64_bitmap_t *r = bitmap->roaring;
    uint64_t pos = start;

    if (start >= end) return -1;

    art_key_chunk_t key[ART_KEY_BYTES];
    bitmapArtKeyFromHigh48(pos >> 16, key);
    art_iterator_t it = art_lower_bound(&r->art, key);

    while (pos < end) {
        if (it.value == NULL) return (long long)pos;

        uint64_t container_base = bitmapArtKeyToHigh48(it.key) << 16;
        if (container_base > pos) return (long long)pos;

        roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
        int32_t clear = bitmapContainerFirstClearBit(
            r->containers[roaring64_leaf_index(leaf)],
            roaring64_leaf_typecode(leaf),
            (uint32_t)(pos - container_base));
        if (clear >= 0) {
            uint64_t clear_pos = container_base + (uint64_t)clear;
            if (clear_pos < end) return (long long)clear_pos;
            break;
        }

        pos = container_base + (1 << 16);
        art_iterator_next(&it);
    }

    /* Every bit in [start, end) is set. When the caller gave no explicit end
     * the range is implicitly followed by an imaginary trailing zero. */
    return end_given ? -1 : (long long)end;
}

long long bitmapObjectBitpos(const robj *o, int bit, uint64_t start,
                             uint64_t end, int end_given)
{
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t bit_len = bitmap->byte_len * 8;

    if (start >= end || start >= bit_len) return -1;
    if (end > bit_len) end = bit_len;

    return bit ? bitmapObjectFirstSetBit(bitmap, start, end) :
                 bitmapObjectFirstClearBit(bitmap, start, end, end_given);
}

int bitmapObjectCanRepresentBit(uint64_t bitoffset) {
    return bitoffset <= BITMAP_OBJECT_MAX_BITOFFSET;
}

int bitmapObjectGetBit(const robj *o, uint64_t bitoffset) {
    bitmapObject *bitmap = getBitmapObject(o);

    if (!bitmapObjectCanRepresentBit(bitoffset)) return 0;
    if ((bitoffset >> 3) >= bitmap->byte_len) return 0;
    return roaring64_bitmap_contains(bitmap->roaring, bitoffset);
}

int bitmapObjectSetBit(robj *o, uint64_t bitoffset, int on) {
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t byte = bitoffset >> 3;

    if (!bitmapObjectCanRepresentBit(bitoffset))
        return C_ERR;

    if (byte + 1 > bitmap->byte_len)
        bitmap->byte_len = byte + 1;

    if (on)
        roaring64_bitmap_add(bitmap->roaring, bitoffset);
    else
        roaring64_bitmap_remove(bitmap->roaring, bitoffset);

    return C_OK;
}

static sds bitmapObjectMaterializeRoaring(const roaring64_bitmap_t *roaring,
                                          size_t byte_len)
{
    sds raw = sdsnewlen(NULL, byte_len);
    roaring64_iterator_t *it = roaring64_iterator_create(roaring);
    uint64_t values[1024];
    uint64_t count;

    do {
        count = roaring64_iterator_read(it, values,
                                        sizeof(values) / sizeof(values[0]));
        for (uint64_t i = 0; i < count; i++) {
            uint64_t offset = values[i];
            size_t byte = offset >> 3;
            int bit = 7 - (offset & 7);
            serverAssert(byte < byte_len);
            raw[byte] |= (1 << bit);
        }
    } while (count == sizeof(values) / sizeof(values[0]));

    roaring64_iterator_free(it);
    return raw;
}

/* Flatten the bitmap into its logical raw string bytes. Returns NULL when the
 * logical length exceeds proto-max-bulk-len: native bitmaps can address bit
 * offsets far beyond any representable string, so callers that need flat
 * bytes must handle oversized bitmaps explicitly. */
sds bitmapObjectMaterialize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    if (bitmap->byte_len > (uint64_t)server.proto_max_bulk_len) return NULL;
    if (bitmap->byte_len > (uint64_t)SIZE_MAX) return NULL;
    return bitmapObjectMaterializeRoaring(bitmap->roaring,
                                          (size_t)bitmap->byte_len);
}

typedef struct bitmapBitopSource {
    const roaring64_bitmap_t *roaring;
    roaring64_bitmap_t *owned;
} bitmapBitopSource;

static void bitmapObjectReleaseBitopSources(bitmapBitopSource *sources,
                                            size_t numkeys)
{
    for (size_t i = 0; i < numkeys; i++) {
        if (sources[i].owned != NULL)
            roaring64_bitmap_free(sources[i].owned);
    }
}

static void bitmapObjectPrepareBitopSources(robj **objects,
                                            bitmapBitopSource *sources,
                                            size_t numkeys)
{
    for (size_t i = 0; i < numkeys; i++) {
        robj *o = objects[i];

        sources[i].roaring = NULL;
        sources[i].owned = NULL;
        if (o == NULL) continue;

        if (o->type == OBJ_BITMAP) {
            sources[i].roaring = getBitmapObject(o)->roaring;
        } else {
            serverAssert(o->type == OBJ_STRING);
            sources[i].owned =
                bitmapObjectRoaringFromString((unsigned char *)o->ptr,
                                              sdslen(o->ptr), 0);
            sources[i].roaring = sources[i].owned;
        }
    }
}

static roaring64_bitmap_t *bitmapObjectCopyBitopSource(bitmapBitopSource *source) {
    /* Roarings built from string sources are owned temporaries that no later
     * operand reads again (every caller seeds the accumulator from this
     * source exactly once), so steal them instead of deep-copying. */
    if (source->owned != NULL) {
        roaring64_bitmap_t *stolen = source->owned;
        source->owned = NULL;
        source->roaring = NULL;
        return stolen;
    }

    roaring64_bitmap_t *copy = source->roaring != NULL ?
        roaring64_bitmap_copy(source->roaring) : roaring64_bitmap_create();
    serverAssert(copy != NULL);
    return copy;
}

static roaring64_bitmap_t *bitmapObjectUnionBitopSources(bitmapBitopSource *sources,
                                                         size_t start,
                                                         size_t numkeys)
{
    roaring64_bitmap_t *result = roaring64_bitmap_create();
    serverAssert(result != NULL);

    for (size_t i = start; i < numkeys; i++) {
        if (sources[i].roaring != NULL)
            roaring64_bitmap_or_inplace(result, sources[i].roaring);
    }
    return result;
}

static roaring64_bitmap_t *bitmapObjectExactlyOneBitopSources(bitmapBitopSource *sources,
                                                              size_t numkeys)
{
    roaring64_bitmap_t *result = bitmapObjectCopyBitopSource(&sources[0]);
    roaring64_bitmap_t *multiple = roaring64_bitmap_create();
    serverAssert(multiple != NULL);

    for (size_t i = 1; i < numkeys; i++) {
        roaring64_bitmap_t *both;

        if (sources[i].roaring == NULL) continue;

        both = roaring64_bitmap_and(result, sources[i].roaring);
        serverAssert(both != NULL);
        roaring64_bitmap_or_inplace(multiple, both);
        roaring64_bitmap_free(both);

        roaring64_bitmap_xor_inplace(result, sources[i].roaring);
        roaring64_bitmap_andnot_inplace(result, multiple);
    }

    roaring64_bitmap_free(multiple);
    return result;
}

/* Compute a BITOP over string and native bitmap sources entirely in roaring
 * space and return the result as a new native bitmap object whose logical
 * length is 'maxlen', matching the string semantics where the destination
 * length equals the longest source. The operation never materializes flat
 * bytes, so it supports 64-bit logical lengths; the BITOP NOT length guard
 * lives at the command layer because complementing is inherently dense. */
robj *bitmapObjectsBitopBitmap(bitmapBitop op, robj **objects, size_t numkeys,
                               uint64_t maxlen)
{
    bitmapBitopSource *sources;
    roaring64_bitmap_t *result = NULL;

    serverAssert(numkeys > 0);
    serverAssert(maxlen <= BITMAP_OBJECT_MAX_BYTES);

    sources = zcalloc(sizeof(*sources) * numkeys);
    bitmapObjectPrepareBitopSources(objects, sources, numkeys);

    switch (op) {
    case BITMAP_BITOP_AND:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_and_inplace(result, sources[i].roaring);
            else
                roaring64_bitmap_clear(result);
        }
        break;
    case BITMAP_BITOP_OR:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_or_inplace(result, sources[i].roaring);
        }
        break;
    case BITMAP_BITOP_XOR:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_xor_inplace(result, sources[i].roaring);
        }
        break;
    case BITMAP_BITOP_NOT:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        roaring64_bitmap_flip_inplace(result, 0, maxlen * 8);
        break;
    case BITMAP_BITOP_DIFF:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_andnot_inplace(result, sources[i].roaring);
        }
        break;
    case BITMAP_BITOP_DIFF1:
        result = bitmapObjectUnionBitopSources(sources, 1, numkeys);
        if (sources[0].roaring != NULL)
            roaring64_bitmap_andnot_inplace(result, sources[0].roaring);
        break;
    case BITMAP_BITOP_ANDOR:
        result = bitmapObjectUnionBitopSources(sources, 1, numkeys);
        if (sources[0].roaring != NULL)
            roaring64_bitmap_and_inplace(result, sources[0].roaring);
        else
            roaring64_bitmap_clear(result);
        break;
    case BITMAP_BITOP_ONE:
        result = bitmapObjectExactlyOneBitopSources(sources, numkeys);
        break;
    default:
        serverPanic("Unknown native bitmap BITOP");
    }

    bitmapObjectReleaseBitopSources(sources, numkeys);

    /* Unlike BITOP temporaries, the result is stored in the keyspace, so the
     * container-conversion work pays for itself. */
    roaring64_bitmap_run_optimize(result);
    roaring64_bitmap_shrink_to_fit(result);
    zfree(sources);

    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = maxlen;
    bitmap->roaring = result;

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

size_t bitmapObjectSerializedSize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return roaring64_bitmap_portable_size_in_bytes(bitmap->roaring);
}

sds bitmapObjectSerialize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    size_t len = roaring64_bitmap_portable_size_in_bytes(bitmap->roaring);
    sds payload = sdsnewlen(SDS_NOINIT, len);
    size_t written = roaring64_bitmap_portable_serialize(bitmap->roaring, payload);
    serverAssert(written == len);
#if (BYTE_ORDER == BIG_ENDIAN)
    serverAssert(bitmapPortableConvertEndian(payload, len, 0) == C_OK);
#endif
    return payload;
}

/* DEBUG BITMAP-ENDIAN-CHECK: the serialized payload is little-endian on every
 * host, so swapping it to the opposite endianness and back must parse cleanly
 * in both directions and reproduce the original bytes. Big-endian hosts start
 * from their already-converted payload, little-endian hosts start from the
 * native one, so the parser is exercised either way. */
int bitmapObjectEndianRoundtripCheck(const robj *o) {
    sds payload = bitmapObjectSerialize(o);
    size_t len = sdslen(payload);
    sds swapped = sdsnewlen(payload, len);
    int first_from_le = (BYTE_ORDER == BIG_ENDIAN);

    int ok = bitmapPortableConvertEndian(swapped, len, first_from_le) == C_OK &&
             bitmapPortableConvertEndian(swapped, len, !first_from_le) == C_OK &&
             memcmp(swapped, payload, len) == 0;

    sdsfree(swapped);
    sdsfree(payload);
    return ok ? C_OK : C_ERR;
}
