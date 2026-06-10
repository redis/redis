#include "server.h"
#include "bitmap_roaring.h"

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/run.h>
#include <roaring/memory.h>
#include <roaring/roaring.h>
#include <roaring/roaring_array.h>

typedef struct bitmapObject {
    size_t byte_len;
    roaring_bitmap_t *roaring;
} bitmapObject;

#define BITMAP_OBJECT_MAX_BYTES (((uint64_t)UINT32_MAX + 1) / 8)

static bitmapObject *getBitmapObject(const robj *o) {
    serverAssert(o->type == OBJ_BITMAP);
    serverAssert(o->encoding == OBJ_ENCODING_BITMAP_ROARING);
    return o->ptr;
}

#if (BYTE_ORDER == BIG_ENDIAN)
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

/* CRoaring's portable format is host-endian despite being byte-compatible with
 * the Roaring format on little-endian hosts. Redis RDB payloads must be
 * architecture-portable, so big-endian builds translate the CRoaring payload to
 * little-endian before saving and back to host-endian before deserializing. */
static int bitmapPortableConvertEndian(char *buf, size_t len, int from_little_endian) {
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

    return pos == len ? C_OK : C_ERR;
}
#endif

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
    zfree(bitmapRoaringAlignedAllocBase(ptr));
}

static size_t bitmapRoaringAlignedAllocSize(void *ptr) {
    if (ptr == NULL) return 0;
    return zmalloc_size(bitmapRoaringAlignedAllocBase(ptr));
}

void bitmapRoaringInit(void) {
    roaring_memory_t memory_hook = {
        .malloc = zmalloc,
        .realloc = zrealloc,
        .calloc = zcalloc_num,
        .free = zfree,
        .aligned_malloc = bitmapRoaringAlignedMalloc,
        .aligned_free = bitmapRoaringAlignedFree,
    };

    roaring_init_memory_hook(memory_hook);
}

robj *createBitmapObject(void) {
    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = 0;
    bitmap->roaring = roaring_bitmap_create();

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *createBitmapObjectFromString(const unsigned char *buf, size_t len) {
    if (len > BITMAP_OBJECT_MAX_BYTES) return NULL;

    robj *o = createBitmapObject();
    bitmapObject *bitmap = getBitmapObject(o);
    bitmap->byte_len = len;

    for (size_t byte = 0; byte < len; byte++) {
        unsigned char value = buf[byte];
        while (value) {
            int bit = __builtin_ctz(value);
            uint32_t offset = (uint32_t)(byte * 8 + (7 - bit));
            roaring_bitmap_add(bitmap->roaring, offset);
            value &= value - 1;
        }
    }

    roaring_bitmap_run_optimize(bitmap->roaring);
    roaring_bitmap_shrink_to_fit(bitmap->roaring);
    return o;
}

robj *createBitmapObjectFromPortable(size_t byte_len, const char *buf, size_t len, int deep_validate) {
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
    if (roaring_bitmap_portable_deserialize_size(buf, len) != len) return NULL;
#endif

    roaring_bitmap_t *roaring = roaring_bitmap_portable_deserialize_safe(portable, len);
    sdsfree(converted);
    if (roaring == NULL) return NULL;

    /* The safe deserializer bounds the reads but does not verify structural
     * invariants (sorted array containers, sorted non-overlapping runs);
     * CRoaring documents bitmaps from untrusted input as unsafe to use until
     * roaring_bitmap_internal_validate passes. */
    if (deep_validate) {
        const char *reason = NULL;
        if (!roaring_bitmap_internal_validate(roaring, &reason)) {
            roaring_bitmap_free(roaring);
            return NULL;
        }
    }

    if (roaring_bitmap_get_cardinality(roaring) != 0) {
        uint32_t max = roaring_bitmap_maximum(roaring);
        if ((uint64_t)max >= (uint64_t)byte_len * 8) {
            roaring_bitmap_free(roaring);
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
    dst->roaring = roaring_bitmap_copy(src->roaring);

    robj *copy = createObject(OBJ_BITMAP, dst);
    copy->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return copy;
}

void freeBitmapObject(robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    roaring_bitmap_free(bitmap->roaring);
    zfree(bitmap);
}

size_t bitmapObjectLen(const robj *o) {
    return getBitmapObject(o)->byte_len;
}

static size_t bitmapObjectContainerAllocSize(container_t *container, uint8_t type) {
    if (container == NULL) return 0;

    if (type == SHARED_CONTAINER_TYPE) {
        shared_container_t *shared = CAST_shared(container);
        return zmalloc_size(shared) +
               bitmapObjectContainerAllocSize(shared->container, shared->typecode);
    }

    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array = CAST_array(container);
        return zmalloc_size(array) +
               (array->array ? zmalloc_size(array->array) : 0);
    }
    case BITSET_CONTAINER_TYPE: {
        bitset_container_t *bitset = CAST_bitset(container);
        return zmalloc_size(bitset) +
               bitmapRoaringAlignedAllocSize(bitset->words);
    }
    case RUN_CONTAINER_TYPE: {
        run_container_t *run = CAST_run(container);
        return zmalloc_size(run) +
               (run->runs ? zmalloc_size(run->runs) : 0);
    }
    default:
        serverPanic("Unknown roaring bitmap container type");
    }
}

size_t bitmapObjectAllocSize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    roaring_array_t *array = &bitmap->roaring->high_low_container;
    size_t size = zmalloc_size(bitmap) + zmalloc_size(bitmap->roaring);

    if (array->containers != NULL) size += zmalloc_size(array->containers);

    for (int32_t i = 0; i < array->size; i++) {
        size += bitmapObjectContainerAllocSize(
            (container_t *)array->containers[i], array->typecodes[i]);
    }

    return size;
}

static void bitmapObjectDismissContainer(container_t *container, uint8_t type) {
    if (container == NULL) return;

    if (type == SHARED_CONTAINER_TYPE) {
        /* Shared containers exist only when CRoaring copy-on-write is in use,
         * which Redis never enables. If one ever appears it may be referenced
         * by another live bitmap, and a fork child that dismisses pages another
         * object still has to serialize would corrupt that object's output, so
         * leave shared containers alone. */
        return;
    }

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
    roaring_array_t *array = &bitmap->roaring->high_low_container;

    /* Iterate the container directory only when the average container payload
     * is likely to span whole pages, like the other complex types (see
     * dismissObject()). */
    if (array->size > 0 && size_hint / (size_t)array->size >= server.page_size) {
        for (int32_t i = 0; i < array->size; i++) {
            bitmapObjectDismissContainer(
                (container_t *)array->containers[i], array->typecodes[i]);
        }
    }

    dismissMemory(array->containers,
                  (size_t)array->allocation_size *
                      (sizeof(*array->containers) + sizeof(*array->keys) +
                       sizeof(*array->typecodes)));
    dismissMemory(bitmap->roaring, sizeof(*bitmap->roaring));
    dismissMemory(bitmap, sizeof(*bitmap));
}

/* --- Active defrag support ---
 *
 * Every allocation CRoaring makes for a bitmap goes through the zmalloc-backed
 * memory hook installed in bitmapRoaringInit(), so active defrag can relocate
 * each one with activeDefragAlloc(). The walk mirrors dismissBitmapObject():
 * the wrapper struct, the roaring bitmap struct, the container directory
 * (containers/keys/typecodes share one allocation), and every container with
 * its payload buffer. */

/* CRoaring requests this alignment for bitset container word buffers in the
 * portable build Redis uses (see align_size in CRoaring's
 * bitset_container_create(); the SIMD-gated 64-byte case is compiled out by
 * ROARING_DISABLE_X64). bitmapRoaringAlignedMalloc() over-allocates by
 * alignment - 1 + sizeof(void *), so re-deriving an aligned offset inside a
 * relocated block always fits. */
#define BITMAP_ROARING_BITSET_WORDS_ALIGNMENT 32

static void bitmapObjectDefragBitsetWords(bitset_container_t *bitset) {
    void *base, *newbase;
    uintptr_t aligned;
    size_t old_off, new_off;

    if (bitset->words == NULL) return;
    base = bitmapRoaringAlignedAllocBase(bitset->words);
    newbase = activeDefragAlloc(base);
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

    if (type == SHARED_CONTAINER_TYPE) {
        /* Shared containers exist only when CRoaring copy-on-write is in use,
         * which Redis never enables; one could be referenced by another live
         * bitmap, so relocating it from here would corrupt the other owner.
         * Leave them alone. */
        return NULL;
    }

    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array = CAST_array(container);
        uint16_t *values;
        if ((moved = activeDefragAlloc(array)) != NULL)
            array = (array_container_t *)moved;
        if (array->array != NULL &&
            (values = activeDefragAlloc(array->array)) != NULL)
            array->array = values;
        return (container_t *)moved;
    }
    case BITSET_CONTAINER_TYPE: {
        bitset_container_t *bitset = CAST_bitset(container);
        if ((moved = activeDefragAlloc(bitset)) != NULL)
            bitset = (bitset_container_t *)moved;
        bitmapObjectDefragBitsetWords(bitset);
        return (container_t *)moved;
    }
    case RUN_CONTAINER_TYPE: {
        run_container_t *run = CAST_run(container);
        rle16_t *runs;
        if ((moved = activeDefragAlloc(run)) != NULL)
            run = (run_container_t *)moved;
        if (run->runs != NULL &&
            (runs = activeDefragAlloc(run->runs)) != NULL)
            run->runs = runs;
        return (container_t *)moved;
    }
    default:
        serverPanic("Unknown roaring bitmap container type");
    }
}

size_t bitmapObjectContainerCount(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return (size_t)bitmap->roaring->high_low_container.size;
}

/* Relocate the wrapper struct, roaring struct and container directory, and
 * refresh o->ptr. Container payloads are handled separately so the walk can
 * be split into incremental steps. */
static roaring_array_t *bitmapObjectDefragTopLevel(robj *o) {
    bitmapObject *bitmap = getBitmapObject(o), *newbitmap;
    roaring_bitmap_t *newroaring;
    roaring_array_t *array;

    if ((newbitmap = activeDefragAlloc(bitmap)) != NULL)
        o->ptr = bitmap = newbitmap;
    if ((newroaring = activeDefragAlloc(bitmap->roaring)) != NULL)
        bitmap->roaring = newroaring;

    array = &bitmap->roaring->high_low_container;
    if (array->containers != NULL) {
        container_t **newblock = activeDefragAlloc(array->containers);
        if (newblock != NULL) {
            /* containers, keys and typecodes share one allocation, laid out
             * in that order (see CRoaring's realloc_array()). */
            array->containers = newblock;
            array->keys = (uint16_t *)(array->containers + array->allocation_size);
            array->typecodes = (uint8_t *)(array->keys + array->allocation_size);
        }
    }
    return array;
}

void defragBitmapObject(robj *o) {
    roaring_array_t *array = bitmapObjectDefragTopLevel(o);
    container_t *moved;

    for (int32_t i = 0; i < array->size; i++) {
        moved = bitmapObjectDefragContainer((container_t *)array->containers[i],
                                            array->typecodes[i]);
        if (moved != NULL) array->containers[i] = moved;
    }
}

/* Incremental defrag step for bitmaps queued with defragLater(): 'cursor' is
 * the next container index to process (0 also relocates the top-level
 * allocations). Containers may shift between steps if the bitmap is written
 * concurrently; that only makes the walk skip or revisit a few containers,
 * which is harmless for defrag. Returns the new cursor, 0 when done. */
unsigned long bitmapObjectDefragIncremental(robj *o, unsigned long cursor) {
    roaring_array_t *array;
    container_t *moved;
    long iterations = 0;

    if (cursor == 0)
        array = bitmapObjectDefragTopLevel(o);
    else
        array = &getBitmapObject(o)->roaring->high_low_container;

    while (cursor < (unsigned long)array->size) {
        moved = bitmapObjectDefragContainer(
            (container_t *)array->containers[cursor], array->typecodes[cursor]);
        if (moved != NULL) array->containers[cursor] = moved;
        cursor++;
        if (++iterations >= 128) return cursor;
    }
    return 0;
}

uint64_t bitmapObjectCardinality(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return roaring_bitmap_get_cardinality(bitmap->roaring);
}

uint64_t bitmapObjectRangeCardinality(const robj *o, uint64_t start,
                                      uint64_t end)
{
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t bit_len = (uint64_t)bitmap->byte_len << 3;

    if (start >= end || start >= bit_len) return 0;
    if (end > bit_len) end = bit_len;
    return roaring_bitmap_range_cardinality(bitmap->roaring, start, end);
}

int bitmapObjectCanRepresentBit(uint64_t bitoffset) {
    return bitoffset <= UINT32_MAX;
}

int bitmapObjectGetBit(const robj *o, uint64_t bitoffset) {
    bitmapObject *bitmap = getBitmapObject(o);

    if (!bitmapObjectCanRepresentBit(bitoffset)) return 0;
    if ((bitoffset >> 3) >= bitmap->byte_len) return 0;
    return roaring_bitmap_contains(bitmap->roaring, (uint32_t)bitoffset);
}

int bitmapObjectSetBit(robj *o, uint64_t bitoffset, int on) {
    bitmapObject *bitmap = getBitmapObject(o);
    size_t byte = bitoffset >> 3;

    if (!bitmapObjectCanRepresentBit(bitoffset) || byte >= BITMAP_OBJECT_MAX_BYTES)
        return C_ERR;

    if (byte + 1 > bitmap->byte_len)
        bitmap->byte_len = byte + 1;

    if (on)
        roaring_bitmap_add(bitmap->roaring, (uint32_t)bitoffset);
    else
        roaring_bitmap_remove(bitmap->roaring, (uint32_t)bitoffset);

    return C_OK;
}

sds bitmapObjectMaterialize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    sds raw = sdsnewlen(NULL, bitmap->byte_len);

    roaring_uint32_iterator_t it;
    roaring_iterator_init(bitmap->roaring, &it);
    while (it.has_value) {
        uint32_t offset = it.current_value;
        size_t byte = offset >> 3;
        int bit = 7 - (offset & 7);
        serverAssert(byte < bitmap->byte_len);
        raw[byte] |= (1 << bit);
        roaring_uint32_iterator_advance(&it);
    }

    return raw;
}

sds bitmapObjectSerialize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    size_t len = roaring_bitmap_portable_size_in_bytes(bitmap->roaring);
    sds payload = sdsnewlen(SDS_NOINIT, len);
    size_t written = roaring_bitmap_portable_serialize(bitmap->roaring, payload);
    serverAssert(written == len);
#if (BYTE_ORDER == BIG_ENDIAN)
    serverAssert(bitmapPortableConvertEndian(payload, len, 0) == C_OK);
#endif
    return payload;
}
