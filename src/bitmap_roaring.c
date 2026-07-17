#include "fmacros.h"
/* CRoaring's x64 (AVX2/AVX512) and NEON acceleration is disabled, for two
 * reasons. First, build correctness: CRoaring exposes target-specific inline
 * helpers from its public headers, so this translation unit's SIMD
 * configuration must match the one deps/Makefile uses to build libcroaring
 * (CROARING_CFLAGS defines the same two flags); mixing settings would compile
 * incompatible inline definitions on the two sides of the library boundary.
 * Second, portability: with the SIMD paths compiled out, the binary needs no
 * -march flags or CPU runtime dispatch, matching how Redis builds its own
 * bit-manipulation code. Revisit if benchmarks justify wiring CRoaring's
 * runtime dispatch into the Redis build. */
#ifndef ROARING_DISABLE_X64
#define ROARING_DISABLE_X64 1
#endif
#ifndef DISABLENEON
#define DISABLENEON 1
#endif
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

#ifdef STRINGIFY
#undef STRINGIFY
#endif

#include "server.h"
#include "bitmap_roaring.h"
#include "lzf.h"
#include "rdb.h"

/* Reuse the established string BITOP vector kernels after bounded roaring
 * operands have been materialized. Their scalar tail remains local below. */
#ifdef HAVE_AVX2
unsigned long bitopCommandAVX(unsigned char **keys, unsigned char *res,
                              bitmapBitop op, unsigned long numkeys,
                              unsigned long minlen);
#endif
#ifdef HAVE_AVX512
unsigned long bitopCommandAVX512(unsigned char **keys, unsigned char *res,
                                 bitmapBitop op, unsigned long numkeys,
                                 unsigned long minlen);
#endif

/* Derived from the byte cap in bitmap_roaring.h; only this file needs the
 * bit-offset form. */
#define BITMAP_OBJECT_MAX_BITOFFSET (BITMAP_OBJECT_MAX_BYTES * 8 - 1)

/* Bitmap objects use Roaring internally. Client-visible command limits are
 * enforced by command handlers; the object cap protects encoding invariants. */
typedef struct bitmapObject {
    uint64_t byte_len;
    size_t alloc_size;          /* Total memory used by this bitmap object. */
    roaring64_bitmap_t *roaring;
} bitmapObject;

static size_t bitmapRoaringAllocSize(const roaring64_bitmap_t *r);
static size_t bitmapRoaringRangeAllocSize(const roaring64_bitmap_t *r,
                                          uint64_t start, uint64_t end);
static void bitmapObjectRefreshAllocSize(bitmapObject *bitmap);
static void bitmapObjectRefreshRangeAllocSize(bitmapObject *bitmap,
                                              uint64_t start, uint64_t end,
                                              size_t old_size);
static void bitmapArtKeyFromHigh48(uint64_t high48,
                                   art_key_chunk_t key[ART_KEY_BYTES]);

static bitmapObject *getBitmapObject(const robj *o) {
    serverAssert(o->type == OBJ_BITMAP);
    serverAssert(o->encoding == OBJ_ENCODING_BITMAP_ROARING);
    return o->ptr;
}

static void bitmapObjectAdjustAllocSize(size_t *alloc_size, size_t old_size,
                                        size_t new_size)
{
    if (new_size >= old_size) {
        serverAssert(SIZE_MAX - *alloc_size >= new_size - old_size);
        *alloc_size += new_size - old_size;
    } else {
        serverAssert(*alloc_size >= old_size - new_size);
        *alloc_size -= old_size - new_size;
    }
}

static void bitmapObjectRoaringAppendContainer(roaring64_bitmap_t *roaring,
                                               uint64_t high48,
                                               container_t *container,
                                               uint8_t typecode)
{
    if (roaring->first_free == roaring->capacity) {
        uint64_t new_capacity;

        if (roaring->capacity == 0)
            new_capacity = 2;
        else if (roaring->capacity < 1024)
            new_capacity = roaring->capacity * 2;
        else
            new_capacity = roaring->capacity + roaring->capacity / 4;

        container_t **containers = roaring_realloc(
            roaring->containers, new_capacity * sizeof(*containers));
        serverAssert(containers != NULL);
        memset(containers + roaring->capacity, 0,
               (new_capacity - roaring->capacity) * sizeof(*containers));
        roaring->containers = containers;
        roaring->capacity = new_capacity;
    }

    uint64_t index = roaring->first_free++;
    roaring->containers[index] = container;

    art_key_chunk_t key[ART_KEY_BYTES];
    bitmapArtKeyFromHigh48(high48, key);
    art_insert(&roaring->art, key, (art_val_t)((index << 8) | typecode));
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

static unsigned char bitmapReverseByte(unsigned char value) {
    value = (unsigned char)(((value & 0xf0) >> 4) | ((value & 0x0f) << 4));
    value = (unsigned char)(((value & 0xcc) >> 2) | ((value & 0x33) << 2));
    value = (unsigned char)(((value & 0xaa) >> 1) | ((value & 0x55) << 1));
    return value;
}

static int bitmapRawChunkFitsArray(const unsigned char *buf, size_t len,
                                   int *cardinality)
{
    int count = 0;

    for (size_t i = 0; i < len; i++) {
        count += __builtin_popcount(buf[i]);
        if (count > DEFAULT_MAX_SIZE) return 0;
    }
    *cardinality = count;
    return 1;
}

static void bitmapObjectAppendRawArrayContainer(roaring64_bitmap_t *roaring,
                                                const unsigned char *buf,
                                                size_t len,
                                                int cardinality,
                                                uint64_t high48)
{
    array_container_t *array =
        array_container_create_given_capacity(cardinality);
    serverAssert(array != NULL);
    array->cardinality = cardinality;

    int32_t out = 0;
    for (size_t byte = 0; byte < len; byte++) {
        unsigned char value = buf[byte];
        for (int bit = 0; bit < 8; bit++) {
            if (value & (0x80 >> bit))
                array->array[out++] = (uint16_t)(byte * 8 + bit);
        }
    }
    serverAssert(out == cardinality);

    bitmapObjectRoaringAppendContainer(roaring, high48, (container_t *)array,
                                       ARRAY_CONTAINER_TYPE);
}

static void bitmapObjectAppendRawBitsetContainer(roaring64_bitmap_t *roaring,
                                                 const unsigned char *buf,
                                                 size_t len,
                                                 uint64_t high48)
{
    bitset_container_t *bitset = bitset_container_create();
    serverAssert(bitset != NULL);

    int cardinality = 0;
    size_t byte = 0;
    for (int word = 0; word < BITSET_CONTAINER_SIZE_IN_WORDS; word++) {
        uint64_t bits = 0;

        for (int lane = 0; lane < 8 && byte < len; lane++, byte++) {
            unsigned char value = buf[byte];
            cardinality += __builtin_popcount(value);
            bits |= (uint64_t)bitmapReverseByte(value) << (lane * 8);
        }
        bitset->words[word] = bits;
    }
    bitset->cardinality = cardinality;

    bitmapObjectRoaringAppendContainer(roaring, high48, (container_t *)bitset,
                                       BITSET_CONTAINER_TYPE);
}

/* Build a Roaring bitmap from raw bitmap string bytes by constructing one
 * container per 2^16-bit chunk. This conversion runs on every
 * bitmap-default-roaring write that converts a string and on every string
 * BITOP source, so dense chunks must avoid per-bit roaring64_bitmap_add calls.
 * The optimize pass is only worth paying for bitmaps that are kept
 * (run_optimize/shrink_to_fit walk every container); BITOP operand temporaries
 * are freed within the command, so their callers pass optimize=0. */
static roaring64_bitmap_t *bitmapObjectRoaringFromString(const unsigned char *buf,
                                                         size_t len, int optimize)
{
    roaring64_bitmap_t *roaring = roaring64_bitmap_create();

#define BITMAP_ROARING_CONTAINER_BYTES \
    (BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t))
    for (size_t offset = 0; offset < len;
         offset += BITMAP_ROARING_CONTAINER_BYTES)
    {
        size_t chunk_len = len - offset;
        if (chunk_len > BITMAP_ROARING_CONTAINER_BYTES)
            chunk_len = BITMAP_ROARING_CONTAINER_BYTES;

        uint64_t high48 = offset / BITMAP_ROARING_CONTAINER_BYTES;
        int cardinality;
        if (bitmapRawChunkFitsArray(buf + offset, chunk_len, &cardinality)) {
            if (cardinality == 0) continue;
            bitmapObjectAppendRawArrayContainer(roaring, buf + offset,
                                                chunk_len, cardinality,
                                                high48);
        } else {
            bitmapObjectAppendRawBitsetContainer(roaring, buf + offset,
                                                 chunk_len, high48);
        }
    }
#undef BITMAP_ROARING_CONTAINER_BYTES

    if (optimize) {
        roaring64_bitmap_run_optimize(roaring);
        roaring64_bitmap_shrink_to_fit(roaring);
    }
    return roaring;
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
    return ptr == NULL ? 0 : zmalloc_usable_size((void *)ptr);
}

static size_t bitmapRoaringAlignedMallocSize(void *ptr) {
    void *base = bitmapRoaringAlignedAllocBase(ptr);
    return base == NULL ? 0 : zmalloc_usable_size(base);
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
    bitmapObjectRefreshAllocSize(bitmap);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *createBitmapObjectFromString(const unsigned char *buf, size_t len) {
#if SIZE_MAX > BITMAP_OBJECT_MAX_BYTES
    if ((uint64_t)len > BITMAP_OBJECT_MAX_BYTES) return NULL;
#endif

    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = len;
    bitmap->roaring = bitmapObjectRoaringFromString(buf, len, 1);
    bitmapObjectRefreshAllocSize(bitmap);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *bitmapTypeDup(const robj *o) {
    bitmapObject *src = getBitmapObject(o);
    bitmapObject *dst = zmalloc(sizeof(*dst));
    dst->byte_len = src->byte_len;
    dst->roaring = roaring64_bitmap_copy(src->roaring);
    bitmapObjectRefreshAllocSize(dst);

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
/* Keep Roaring BITOP latency competitive for small steady-state results without
 * letting skipped run compression create unbounded memory growth. */
#define BITMAP_BITOP_FAST_RESULT_MAX_BYTES (1024 * 1024)

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
        serverPanic("Unknown Roaring bitmap container type");
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

static size_t bitmapRoaringTopLevelAllocSize(const roaring64_bitmap_t *r) {
    size_t size = bitmapRoaringMallocSize(r);

    if (!bitmapRoaringIsFrozen(r)) {
        for (int i = BITMAP_ART_MIN_NODE_TYPE; i <= BITMAP_ART_MAX_NODE_TYPE; i++)
            size += bitmapRoaringMallocSize(r->art.nodes[i]);
    }
    size += bitmapRoaringMallocSize(r->containers);
    return size;
}

/* Mutating one bit or range can only change the top-level CRoaring arrays and
 * the containers whose high 48 bits fall inside that range. Keep hot
 * accounting updates proportional to the mutation instead of walking every
 * bitmap container. */
static size_t bitmapRoaringRangeAllocSize(const roaring64_bitmap_t *r,
                                          uint64_t start, uint64_t end)
{
    size_t size = bitmapRoaringTopLevelAllocSize(r);

    if (start >= end) return size;

    uint64_t last_high48 = (end - 1) >> 16;
    art_key_chunk_t key[ART_KEY_BYTES];
    bitmapArtKeyFromHigh48(start >> 16, key);
    art_iterator_t it = art_lower_bound((art_t *)&r->art, key);
    while (it.value != NULL) {
        uint64_t high48 = bitmapArtKeyToHigh48(it.key);
        if (high48 > last_high48) break;

        roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
        size += bitmapRoaringContainerAllocSize(
            r, r->containers[roaring64_leaf_index(leaf)],
            roaring64_leaf_typecode(leaf));
        art_iterator_next(&it);
    }
    return size;
}

/* Whole-object refreshes walk all containers and must stay off per-update hot
 * paths. Construction, load/dup, BITOP result materialization and explicit
 * optimization use it after replacing or compacting the entire Roaring value. */
static void bitmapObjectRefreshAllocSize(bitmapObject *bitmap) {
    bitmap->alloc_size = bitmapRoaringMallocSize(bitmap) +
                         bitmapRoaringAllocSize(bitmap->roaring);
}

static void bitmapObjectRefreshRangeAllocSize(bitmapObject *bitmap,
                                              uint64_t start, uint64_t end,
                                              size_t old_size)
{
    size_t new_size = bitmapRoaringRangeAllocSize(bitmap->roaring, start, end);
    bitmapObjectAdjustAllocSize(&bitmap->alloc_size, old_size, new_size);
}

size_t bitmapObjectAllocSize(const robj *o) {
    return getBitmapObject(o)->alloc_size;
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

static void bitmapObjectDismissContainer(container_t *container, uint8_t type) {
    if (container == NULL) return;

    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array = CAST_array(container);
        dismissMemory(array->array, (size_t)array->capacity * sizeof(*array->array));
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
        serverPanic("Unknown Roaring bitmap container type");
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
 * the wrapper struct, the Roaring bitmap struct, the per-type ART node arrays
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

static void *bitmapObjectActiveDefragAlloc(bitmapObject *bitmap, void *ptr) {
    size_t old_size = zmalloc_usable_size(ptr);
    void *newptr = activeDefragAlloc(ptr);
    if (newptr != NULL)
        bitmapObjectAdjustAllocSize(&bitmap->alloc_size, old_size,
                                    zmalloc_usable_size(newptr));
    return newptr;
}

static bitmapObject *bitmapObjectActiveDefragSelf(bitmapObject *bitmap) {
    size_t old_size = zmalloc_usable_size(bitmap);
    bitmapObject *newbitmap = activeDefragAlloc(bitmap);
    if (newbitmap != NULL)
        bitmapObjectAdjustAllocSize(&newbitmap->alloc_size, old_size,
                                    zmalloc_usable_size(newbitmap));
    return newbitmap;
}

static void bitmapObjectDefragBitsetWords(bitmapObject *bitmap,
                                          bitset_container_t *bitset) {
    void *base, *newbase;
    uintptr_t aligned;
    size_t old_off, new_off;

    if (bitset->words == NULL) return;
    base = bitmapRoaringAlignedAllocBase(bitset->words);
    /* activeDefragAlloc() may free base. Capture every relationship to the old
     * allocation before calling it. */
    old_off = (size_t)((char *)bitset->words - (char *)base);
    newbase = bitmapObjectActiveDefragAlloc(bitmap, base);
    if (newbase == NULL) return;

    /* The relocation copied the block verbatim, so the words still sit at
     * their old offset; re-derive the aligned offset for the new base address
     * and slide the words when the two differ. */
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
                                                bitmapObject *bitmap,
                                                uint8_t type) {
    void *new_container;

    if (container == NULL) return NULL;

    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array = CAST_array(container);
        uint16_t *values;
        if ((new_container = bitmapObjectActiveDefragAlloc(bitmap, array)) != NULL)
            array = (array_container_t *)new_container;
        if (array->array != NULL &&
            (values = bitmapObjectActiveDefragAlloc(bitmap, array->array)) != NULL)
            array->array = values;
        return (container_t *)new_container;
    }
    case BITSET_CONTAINER_TYPE: {
        bitset_container_t *bitset = CAST_bitset(container);
        if ((new_container = bitmapObjectActiveDefragAlloc(bitmap, bitset)) != NULL)
            bitset = (bitset_container_t *)new_container;
        bitmapObjectDefragBitsetWords(bitmap, bitset);
        return (container_t *)new_container;
    }
    case RUN_CONTAINER_TYPE: {
        run_container_t *run = CAST_run(container);
        rle16_t *runs;
        if ((new_container = bitmapObjectActiveDefragAlloc(bitmap, run)) != NULL)
            run = (run_container_t *)new_container;
        if (run->runs != NULL &&
            (runs = bitmapObjectActiveDefragAlloc(bitmap, run->runs)) != NULL)
            run->runs = runs;
        return (container_t *)new_container;
    }
    default:
        serverPanic("Unknown Roaring bitmap container type");
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
    if ((newroaring = bitmapObjectActiveDefragAlloc(bitmap, bitmap->roaring)) != NULL)
        bitmap->roaring = newroaring;
    r = bitmap->roaring;

    for (int i = BITMAP_ART_MIN_NODE_TYPE; i <= BITMAP_ART_MAX_NODE_TYPE; i++) {
        void *moved;
        if (r->art.nodes[i] != NULL &&
            (moved = bitmapObjectActiveDefragAlloc(bitmap, r->art.nodes[i])) != NULL)
            r->art.nodes[i] = moved;
    }
    if (r->containers != NULL) {
        container_t **moved = bitmapObjectActiveDefragAlloc(bitmap, r->containers);
        if (moved != NULL) r->containers = moved;
    }
    return r;
}

static void bitmapObjectDefragLeafContainer(roaring64_bitmap_t *r, bitmapObject *bitmap,
                                            roaring64_leaf_t leaf)
{
    uint64_t index = roaring64_leaf_index(leaf);
    container_t *moved = bitmapObjectDefragContainer(
        r->containers[index], bitmap, roaring64_leaf_typecode(leaf));
    if (moved != NULL) r->containers[index] = moved;
    server.stat_active_defrag_scanned++;
}

void bitmapObjectDefrag(robj *o) {
    bitmapObject *bitmap;
    roaring64_bitmap_t *r = bitmapObjectDefragTopLevel(o);
    bitmap = getBitmapObject(o);

    art_iterator_t it = art_init_iterator(&r->art, true);
    while (it.value != NULL) {
        bitmapObjectDefragLeafContainer(r, bitmap, (roaring64_leaf_t)*it.value);
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
    bitmapObject *bitmap;
    roaring64_bitmap_t *r;
    art_iterator_t it;
    long iterations = 0;

    if (cursor == 0) {
        r = bitmapObjectDefragTopLevel(o);
        bitmap = getBitmapObject(o);
        it = art_init_iterator(&r->art, true);
    } else {
        art_key_chunk_t key[ART_KEY_BYTES];
        bitmap = getBitmapObject(o);
        r = bitmap->roaring;
        bitmapArtKeyFromHigh48((uint64_t)cursor, key);
        it = art_lower_bound(&r->art, key);
    }

    while (it.value != NULL) {
        bitmapObjectDefragLeafContainer(r, bitmap, (roaring64_leaf_t)*it.value);
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

void bitmapObjectVisitSetBitRanges(const robj *o,
                                   bitmapObjectRangeCallback *callback,
                                   void *privdata)
{
    bitmapObject *bitmap = getBitmapObject(o);
    roaring64_iterator_t *it = roaring64_iterator_create(bitmap->roaring);
    roaring64_range_closed_t ranges[128];
    size_t count;

    do {
        count = roaring64_iterator_read_ranges(
            it, ranges, sizeof(ranges) / sizeof(ranges[0]));
        for (size_t i = 0; i < count; i++)
            callback(ranges[i].min, ranges[i].max, privdata);
    } while (count == sizeof(ranges) / sizeof(ranges[0]));

    roaring64_iterator_free(it);
}

static long long bitmapObjectFirstSetBit(bitmapObject *bitmap, uint64_t start,
                                         uint64_t end)
{
    roaring64_bitmap_t *r = bitmap->roaring;

    if (start >= end) return -1;

    art_key_chunk_t key[ART_KEY_BYTES];
    bitmapArtKeyFromHigh48(start >> 16, key);
    art_iterator_t it = art_lower_bound(&r->art, key);

    while (it.value != NULL) {
        uint64_t high48 = bitmapArtKeyToHigh48(it.key);
        uint64_t container_base = high48 << 16;
        uint16_t low16;

        if (container_base >= end) break;

        roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
        uint8_t typecode = roaring64_leaf_typecode(leaf);
        container_t *container = r->containers[roaring64_leaf_index(leaf)];

        if (high48 == (start >> 16)) {
            roaring_container_iterator_t container_it = {0};
            if (container_iterator_lower_bound(container, typecode,
                                               &container_it, &low16,
                                               (uint16_t)(start & 0xffff))) {
                uint64_t value = container_base + low16;
                if (value < end) return (long long)value;
            }
        } else {
            uint64_t value = container_base + container_minimum(container, typecode);
            if (value < end) return (long long)value;
        }

        art_iterator_next(&it);
    }
    return -1;
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
        serverPanic("Unknown Roaring bitmap container type");
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
    size_t old_size;

    if (!bitmapObjectCanRepresentBit(bitoffset))
        return C_ERR;

    if (byte + 1 > bitmap->byte_len)
        bitmap->byte_len = byte + 1;

    old_size = bitmapRoaringRangeAllocSize(bitmap->roaring, bitoffset,
                                           bitoffset + 1);
    if (on)
        roaring64_bitmap_add(bitmap->roaring, bitoffset);
    else
        roaring64_bitmap_remove(bitmap->roaring, bitoffset);
    bitmapObjectRefreshRangeAllocSize(bitmap, bitoffset, bitoffset + 1,
                                      old_size);

    return C_OK;
}

uint64_t bitmapObjectGetUnsignedBitfield(const robj *o, uint64_t offset,
                                         uint64_t bits) {
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t bit_len = bitmap->byte_len * 8;
    uint64_t last_bit;
    uint64_t value = 0;
    uint64_t cached_high48 = UINT64_MAX;
    const container_t *container = NULL;
    uint8_t typecode = 0;

    if (bits == 0) return 0;
    if (!bitmapObjectCanRepresentBit(offset) || offset >= bit_len) return 0;
    if (offset <= UINT64_MAX - (bits - 1)) {
        last_bit = offset + bits - 1;
        if (last_bit < bit_len && (offset >> 16) == (last_bit >> 16)) {
            uint64_t high48 = offset >> 16;
            art_key_chunk_t key[ART_KEY_BYTES];
            bitmapArtKeyFromHigh48(high48, key);
            roaring64_leaf_t *leaf =
                (roaring64_leaf_t *)art_find(&bitmap->roaring->art, key);

            if (leaf == NULL) return 0;

            typecode = roaring64_leaf_typecode(*leaf);
            container =
                bitmap->roaring->containers[roaring64_leaf_index(*leaf)];
            if (container_contains_range(container, (uint16_t)offset,
                                         (uint32_t)(uint16_t)last_bit + 1,
                                         typecode)) {
                return bits == 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
            }
            cached_high48 = high48;
        }
    }

    for (uint64_t j = 0; j < bits; j++) {
        if (offset > UINT64_MAX - j) {
            value <<= bits - j;
            break;
        }

        uint64_t bitoffset = offset + j;
        if (!bitmapObjectCanRepresentBit(bitoffset) || bitoffset >= bit_len) {
            value <<= bits - j;
            break;
        }

        uint64_t high48 = bitoffset >> 16;
        if (high48 != cached_high48) {
            art_key_chunk_t key[ART_KEY_BYTES];
            bitmapArtKeyFromHigh48(high48, key);
            roaring64_leaf_t *leaf =
                (roaring64_leaf_t *)art_find(&bitmap->roaring->art, key);
            if (leaf != NULL) {
                typecode = roaring64_leaf_typecode(*leaf);
                container =
                    bitmap->roaring->containers[roaring64_leaf_index(*leaf)];
            } else {
                container = NULL;
                typecode = 0;
            }
            cached_high48 = high48;
        }

        uint64_t bitval = container != NULL &&
            container_contains(container, (uint16_t)bitoffset, typecode);
        value = (value << 1) | bitval;
    }

    return value;
}

int bitmapObjectSetUnsignedBitfield(robj *o, uint64_t offset, uint64_t bits,
                                    uint64_t value) {
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t positions_to_add[64];
    uint64_t positions_to_remove[64];
    size_t add_count = 0, remove_count = 0;
    size_t old_size;

    if (bits == 0) return C_OK;
    if (offset > UINT64_MAX - (bits - 1)) return C_ERR;

    uint64_t last_bit = offset + bits - 1;
    if (!bitmapObjectCanRepresentBit(last_bit)) return C_ERR;

    uint64_t byte = last_bit >> 3;
    if (byte + 1 > bitmap->byte_len)
        bitmap->byte_len = byte + 1;

    for (uint64_t j = 0; j < bits; j++) {
        uint64_t bitoffset = offset + j;
        int bitval = (value & ((uint64_t)1 << (bits - 1 - j))) != 0;
        if (bitval)
            positions_to_add[add_count++] = bitoffset;
        else
            positions_to_remove[remove_count++] = bitoffset;
    }

    old_size = bitmapRoaringRangeAllocSize(bitmap->roaring, offset,
                                           last_bit + 1);
    if (add_count)
        roaring64_bitmap_add_many(bitmap->roaring, add_count,
                                  positions_to_add);
    if (remove_count) {
        /* remove_many can keep same-container state after a removal deletes
         * that container; BITFIELD clears may include already-clear bits after
         * the last set bit. */
        for (size_t j = 0; j < remove_count; j++)
            roaring64_bitmap_remove(bitmap->roaring, positions_to_remove[j]);
    }
    bitmapObjectRefreshRangeAllocSize(bitmap, offset, last_bit + 1, old_size);

    return C_OK;
}

void bitmapObjectOptimize(robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    roaring64_bitmap_run_optimize(bitmap->roaring);
    roaring64_bitmap_shrink_to_fit(bitmap->roaring);
    bitmapObjectRefreshAllocSize(bitmap);
}

static void bitmapObjectMaterializeRawRange(unsigned char *raw,
                                            size_t chunk_len,
                                            uint32_t start, uint32_t end)
{
    uint32_t bit = start;

    while (bit <= end) {
        size_t byte = bit >> 3;
        if (byte >= chunk_len) break;

        if ((bit & 7) == 0 && bit + 7 <= end) {
            size_t fill = ((size_t)end - bit + 1) >> 3;
            size_t remaining = chunk_len - byte;
            if (fill > remaining) fill = remaining;
            memset(raw + byte, 0xff, fill);
            bit += (uint32_t)(fill * 8);
        } else {
            raw[byte] |= 1 << (7 - (bit & 7));
            bit++;
        }
    }
}

static void bitmapObjectMaterializeContainer(unsigned char *raw,
                                             size_t byte_len,
                                             uint64_t high48,
                                             const container_t *container,
                                             uint8_t typecode)
{
    const size_t container_bytes =
        BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
    uint64_t byte_base = high48 * container_bytes;
    if (byte_base >= byte_len) return;

    size_t chunk_len = byte_len - (size_t)byte_base;
    if (chunk_len > container_bytes) chunk_len = container_bytes;
    unsigned char *chunk = raw + (size_t)byte_base;

    container = container_unwrap_shared(container, &typecode);
    switch (typecode) {
    case BITSET_CONTAINER_TYPE: {
        const bitset_container_t *bitset = const_CAST_bitset(container);
        size_t byte = 0;
        for (int word = 0;
             word < BITSET_CONTAINER_SIZE_IN_WORDS && byte < chunk_len;
             word++)
        {
            uint64_t bits = bitset->words[word];
            for (int lane = 0; lane < 8 && byte < chunk_len; lane++, byte++)
                chunk[byte] = bitmapReverseByte((bits >> (lane * 8)) & 0xff);
        }
        break;
    }
    case ARRAY_CONTAINER_TYPE: {
        const array_container_t *array = const_CAST_array(container);
        for (int32_t i = 0; i < array->cardinality; i++) {
            uint16_t offset = array->array[i];
            size_t byte = offset >> 3;
            if (byte >= chunk_len) break;
            chunk[byte] |= 1 << (7 - (offset & 7));
        }
        break;
    }
    case RUN_CONTAINER_TYPE: {
        const run_container_t *run = const_CAST_run(container);
        for (int32_t i = 0; i < run->n_runs; i++) {
            uint32_t start = run->runs[i].value;
            uint32_t end = start + run->runs[i].length;
            bitmapObjectMaterializeRawRange(chunk, chunk_len, start, end);
        }
        break;
    }
    default:
        serverPanic("Unknown Roaring bitmap container type");
    }
}

static sds bitmapObjectMaterializeRoaring(const roaring64_bitmap_t *roaring,
                                          size_t byte_len)
{
    sds raw = sdsnewlen(NULL, byte_len);

    art_iterator_t it = art_init_iterator((art_t *)&roaring->art, true);
    while (it.value != NULL) {
        roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
        bitmapObjectMaterializeContainer(
            (unsigned char *)raw, byte_len, bitmapArtKeyToHigh48(it.key),
            roaring->containers[roaring64_leaf_index(leaf)],
            roaring64_leaf_typecode(leaf));
        art_iterator_next(&it);
    }
    return raw;
}

static sds bitmapObjectMaterializeRaw(const robj *o, int proto_limited) {
    bitmapObject *bitmap = getBitmapObject(o);
    if (proto_limited &&
        bitmap->byte_len > (uint64_t)server.proto_max_bulk_len) return NULL;
    if (bitmap->byte_len > (uint64_t)SIZE_MAX) return NULL;
    return bitmapObjectMaterializeRoaring(bitmap->roaring,
                                          (size_t)bitmap->byte_len);
}

/* Flatten the bitmap into its logical raw string bytes. Returns NULL when the
 * logical length exceeds proto-max-bulk-len. */
sds bitmapObjectMaterialize(const robj *o) {
    return bitmapObjectMaterializeRaw(o, 1);
}

/* RDB raw payloads are persisted data, not client protocol bulk strings.
 * DUMP serialization cannot fail, so Redis' allocator owns OOM handling. */
sds bitmapObjectMaterializeForRDB(const robj *o) {
    return bitmapObjectMaterializeRaw(o, 0);
}

typedef struct bitmapBitopSource {
    const roaring64_bitmap_t *roaring;
    roaring64_bitmap_t *owned;
} bitmapBitopSource;

typedef struct bitmapRawBitopSource {
    const unsigned char *raw;
    sds owned;
    size_t len;
} bitmapRawBitopSource;

/* Dense mixed operands are cheaper to flatten and combine as machine words
 * than to convert every string source into a temporary Roaring bitmap. Keep
 * this path deliberately narrow: Roaring-only and sparse workloads retain
 * Roaring algebra, while the size cap bounds all temporary raw buffers. */
static int bitmapObjectsUseMixedRawBitop(robj **objects, size_t numkeys,
                                         uint64_t maxlen)
{
    int has_string = 0;
    int has_roaring = 0;

    if (maxlen > BITMAP_BITOP_FAST_RESULT_MAX_BYTES) return 0;

    for (size_t i = 0; i < numkeys; i++) {
        robj *o = objects[i];
        if (o == NULL) continue;

        if (o->type == OBJ_BITMAP) {
            has_roaring = 1;
        } else {
            serverAssert(o->type == OBJ_STRING);
            has_string = 1;
        }
    }

    if (!has_string || !has_roaring) return 0;

    /* Cardinality is a full container walk, so only pay it after the cheap
     * type scan proves this is a genuinely mixed operation. Keep the sum of
     * all retained roaring buffers within the same cap as an individual result;
     * otherwise many small operands could make the temporary memory unbounded.
     * At least one set bit per logical byte is a conservative signal that
     * flattening is preferable to sparse container algebra. */
    uint64_t materialized_bytes = 0;
    for (size_t i = 0; i < numkeys; i++) {
        robj *o = objects[i];
        if (o == NULL || o->type != OBJ_BITMAP) continue;

        uint64_t logical_len = bitmapObjectLen(o);
        if (logical_len > BITMAP_BITOP_FAST_RESULT_MAX_BYTES -
                          materialized_bytes)
            return 0;
        materialized_bytes += logical_len;
        if (bitmapObjectCardinality(o) < logical_len) return 0;
    }

    return 1;
}

static uint64_t bitmapRawBitopLoadWord(const bitmapRawBitopSource *source,
                                       size_t offset)
{
    uint64_t word = 0;
    if (offset >= source->len) return 0;

    size_t copy = source->len - offset;
    if (copy > sizeof(word)) copy = sizeof(word);
    memcpy(&word, source->raw + offset, copy);
    return word;
}

static uint64_t bitmapRawBitopWord(bitmapBitop op,
                                   bitmapRawBitopSource *sources,
                                   size_t numkeys, size_t offset)
{
    uint64_t output = bitmapRawBitopLoadWord(&sources[0], offset);

    switch (op) {
    case BITOP_AND:
        for (size_t i = 1; i < numkeys; i++)
            output &= bitmapRawBitopLoadWord(&sources[i], offset);
        break;
    case BITOP_OR:
        for (size_t i = 1; i < numkeys; i++)
            output |= bitmapRawBitopLoadWord(&sources[i], offset);
        break;
    case BITOP_XOR:
        for (size_t i = 1; i < numkeys; i++)
            output ^= bitmapRawBitopLoadWord(&sources[i], offset);
        break;
    case BITOP_NOT:
        output = ~output;
        break;
    case BITOP_DIFF:
    case BITOP_DIFF1:
    case BITOP_ANDOR: {
        uint64_t disjunction = 0;
        for (size_t i = 1; i < numkeys; i++)
            disjunction |= bitmapRawBitopLoadWord(&sources[i], offset);
        if (op == BITOP_DIFF)
            output &= ~disjunction;
        else if (op == BITOP_DIFF1)
            output = ~output & disjunction;
        else
            output &= disjunction;
        break;
    }
    case BITOP_ONE: {
        uint64_t common = 0;
        for (size_t i = 1; i < numkeys; i++) {
            uint64_t word = bitmapRawBitopLoadWord(&sources[i], offset);
            common |= output & word;
            output ^= word;
            output &= ~common;
        }
        break;
    }
    default:
        serverPanic("Unknown Roaring bitmap BITOP");
    }

    return output;
}

static roaring64_bitmap_t *bitmapObjectsMixedRawBitop(bitmapBitop op,
                                                       robj **objects,
                                                       size_t numkeys,
                                                       size_t maxlen)
{
    bitmapRawBitopSource *sources = zcalloc(sizeof(*sources) * numkeys);
    unsigned char **raw_sources = zmalloc(sizeof(*raw_sources) * numkeys);
    size_t minlen = maxlen;

    for (size_t i = 0; i < numkeys; i++) {
        robj *o = objects[i];
        if (o == NULL) {
            raw_sources[i] = NULL;
            minlen = 0;
            continue;
        }

        if (o->type == OBJ_BITMAP) {
            sources[i].len = (size_t)bitmapObjectLen(o);
            /* The mixed-path cap, rather than proto-max-bulk-len, bounds this
             * internal temporary so lowering the protocol limit cannot change
             * whether an otherwise valid Roaring BITOP succeeds. */
            sources[i].owned = bitmapObjectMaterializeRaw(o, 0);
            serverAssert(sources[i].owned != NULL);
            sources[i].raw = (unsigned char *)sources[i].owned;
        } else {
            serverAssert(o->type == OBJ_STRING);
            sources[i].len = sdslen(o->ptr);
            sources[i].raw = (unsigned char *)o->ptr;
        }
        raw_sources[i] = (unsigned char *)sources[i].raw;
        if (sources[i].len < minlen) minlen = sources[i].len;
    }

    sds raw_result = sdsnewlen(SDS_NOINIT, maxlen);
    size_t offset = 0;
    int used_vector = 0;
#ifdef HAVE_AVX512
    if (__builtin_cpu_supports("avx512f") && minlen >= 10000 && numkeys >= 8) {
        offset = bitopCommandAVX512(raw_sources,
                                    (unsigned char *)raw_result, op,
                                    (unsigned long)numkeys,
                                    (unsigned long)minlen);
        used_vector = 1;
    }
#endif
#ifdef HAVE_AVX2
    if (!used_vector && __builtin_cpu_supports("avx2")) {
        offset = bitopCommandAVX(raw_sources, (unsigned char *)raw_result, op,
                                (unsigned long)numkeys,
                                (unsigned long)minlen);
    }
#else
    UNUSED(used_vector);
#endif
    for (; offset + sizeof(uint64_t) <= maxlen; offset += sizeof(uint64_t)) {
        uint64_t output = bitmapRawBitopWord(op, sources, numkeys, offset);
        memcpy(raw_result + offset, &output, sizeof(output));
    }
    if (offset < maxlen) {
        uint64_t output = bitmapRawBitopWord(op, sources, numkeys, offset);
        memcpy(raw_result + offset, &output, maxlen - offset);
    }

    roaring64_bitmap_t *result = bitmapObjectRoaringFromString(
        (unsigned char *)raw_result, maxlen, 0);
    sdsfree(raw_result);
    for (size_t i = 0; i < numkeys; i++) sdsfree(sources[i].owned);
    zfree(raw_sources);
    zfree(sources);
    return result;
}

static int bitmapRoaringHasRunContainers(const roaring64_bitmap_t *roaring) {
    art_iterator_t it = art_init_iterator((art_t *)&roaring->art, true);
    while (it.value != NULL) {
        roaring64_leaf_t leaf = (roaring64_leaf_t)*it.value;
        uint8_t typecode = roaring64_leaf_typecode(leaf);
        if (typecode == RUN_CONTAINER_TYPE) return 1;
        if (typecode == SHARED_CONTAINER_TYPE) {
            const shared_container_t *shared = const_CAST_shared(
                roaring->containers[roaring64_leaf_index(leaf)]);
            if (shared->typecode == RUN_CONTAINER_TYPE) return 1;
        }
        art_iterator_next(&it);
    }
    return 0;
}

static int bitmapBitopSourcesBorrowedWithoutRuns(bitmapBitopSource *sources,
                                                 size_t numkeys)
{
    for (size_t i = 0; i < numkeys; i++) {
        if (sources[i].owned != NULL) return 0;
        if (sources[i].roaring != NULL &&
            bitmapRoaringHasRunContainers(sources[i].roaring))
            return 0;
    }
    return 1;
}

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

/* Compute a BITOP over string and Roaring bitmap sources and return the result
 * as a new Roaring bitmap object whose logical length is 'maxlen', matching the
 * string semantics where the destination length equals the longest source.
 * Sparse, large and Roaring-only operations stay entirely in Roaring space;
 * bounded dense mixed operands use the raw-word path above. The BITOP NOT
 * length guard lives at the command layer because complementing is inherently
 * dense. */
robj *bitmapObjectsBitop(bitmapBitop op, robj **objects, size_t numkeys,
                         uint64_t maxlen)
{
    bitmapBitopSource *sources;
    roaring64_bitmap_t *result = NULL;
    int optimize_result = 1;
    int shrink_result = 1;

    serverAssert(numkeys > 0);
    serverAssert(maxlen <= BITMAP_OBJECT_MAX_BYTES);

    if (bitmapObjectsUseMixedRawBitop(objects, numkeys, maxlen)) {
        result = bitmapObjectsMixedRawBitop(op, objects, numkeys,
                                            (size_t)maxlen);
        optimize_result = 0;
        shrink_result = 0;
        sources = NULL;
        goto bitop_result;
    }

    sources = zcalloc(sizeof(*sources) * numkeys);
    bitmapObjectPrepareBitopSources(objects, sources, numkeys);

    switch (op) {
    case BITOP_AND: {
        int skip_optimize =
            maxlen <= BITMAP_BITOP_FAST_RESULT_MAX_BYTES &&
            bitmapBitopSourcesBorrowedWithoutRuns(sources, numkeys);
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_and_inplace(result, sources[i].roaring);
            else
                roaring64_bitmap_clear(result);
        }
        if (skip_optimize) {
            optimize_result = 0;
            shrink_result = 0;
        }
        break;
    }
    case BITOP_OR:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_or_inplace(result, sources[i].roaring);
        }
        break;
    case BITOP_XOR:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_xor_inplace(result, sources[i].roaring);
        }
        break;
    case BITOP_NOT:
        if (sources[0].owned == NULL && sources[0].roaring != NULL &&
            maxlen <= BITMAP_BITOP_FAST_RESULT_MAX_BYTES) {
            optimize_result = 0;
            shrink_result = 0;
        }
        result = bitmapObjectCopyBitopSource(&sources[0]);
        roaring64_bitmap_flip_inplace(result, 0, maxlen * 8);
        break;
    case BITOP_DIFF:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_andnot_inplace(result, sources[i].roaring);
        }
        break;
    case BITOP_DIFF1:
        result = bitmapObjectUnionBitopSources(sources, 1, numkeys);
        if (sources[0].roaring != NULL)
            roaring64_bitmap_andnot_inplace(result, sources[0].roaring);
        break;
    case BITOP_ANDOR:
        result = bitmapObjectUnionBitopSources(sources, 1, numkeys);
        if (sources[0].roaring != NULL)
            roaring64_bitmap_and_inplace(result, sources[0].roaring);
        else
            roaring64_bitmap_clear(result);
        break;
    case BITOP_ONE:
        result = bitmapObjectExactlyOneBitopSources(sources, numkeys);
        break;
    default:
        serverPanic("Unknown Roaring bitmap BITOP");
    }

    bitmapObjectReleaseBitopSources(sources, numkeys);

bitop_result:
    /* Large or potentially run-friendly results still pay the conversion and
     * compaction cost before being stored; small roaring steady-state results
     * skip both full-result CRoaring walks above. */
    if (optimize_result) roaring64_bitmap_run_optimize(result);
    if (shrink_result) roaring64_bitmap_shrink_to_fit(result);
    zfree(sources);

    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = maxlen;
    bitmap->roaring = result;
    bitmapObjectRefreshAllocSize(bitmap);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}
