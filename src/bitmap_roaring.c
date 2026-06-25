#include "fmacros.h"
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

/* Native bitmaps use Roaring internally. Client-visible command limits are
 * enforced by command handlers; the object cap protects encoding invariants. */
typedef struct bitmapObject {
    uint64_t byte_len;
    size_t alloc_size;          /* Total memory used by this bitmap object. */
    roaring64_bitmap_t *roaring;
} bitmapObject;

static size_t bitmapRoaringAllocSize(const roaring64_bitmap_t *r);

static bitmapObject *getBitmapObject(const robj *o) {
    serverAssert(o->type == OBJ_BITMAP);
    serverAssert(o->encoding == OBJ_ENCODING_BITMAP_ROARING);
    return o->ptr;
}

static __thread size_t *bitmapRoaringAllocSizeTracker = NULL;

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

static size_t *bitmapRoaringPushAllocSizeTracker(size_t *alloc_size) {
    size_t *prev = bitmapRoaringAllocSizeTracker;
    bitmapRoaringAllocSizeTracker = alloc_size;
    return prev;
}

static void bitmapRoaringPopAllocSizeTracker(size_t *prev) {
    bitmapRoaringAllocSizeTracker = prev;
}

static void *bitmapRoaringMalloc(size_t size) {
    if (bitmapRoaringAllocSizeTracker != NULL) {
        size_t usable = 0;
        void *ptr = zmalloc_usable(size, &usable);
        bitmapObjectAdjustAllocSize(bitmapRoaringAllocSizeTracker, 0, usable);
        return ptr;
    }
    return zmalloc(size);
}

static void *bitmapRoaringRealloc(void *ptr, size_t size) {
    if (bitmapRoaringAllocSizeTracker != NULL) {
        size_t usable = 0, old_usable = 0;
        void *newptr = zrealloc_usable(ptr, size, &usable, &old_usable);
        bitmapObjectAdjustAllocSize(bitmapRoaringAllocSizeTracker,
                                    old_usable, usable);
        return newptr;
    }
    return zrealloc(ptr, size);
}

static void *bitmapRoaringCalloc(size_t nmemb, size_t size) {
    if (bitmapRoaringAllocSizeTracker != NULL) {
        if ((size == 0) || (nmemb > SIZE_MAX / size))
            return zcalloc_num(nmemb, size);
        size_t usable = 0;
        void *ptr = zcalloc_usable(nmemb * size, &usable);
        bitmapObjectAdjustAllocSize(bitmapRoaringAllocSizeTracker, 0, usable);
        return ptr;
    }
    return zcalloc_num(nmemb, size);
}

static void bitmapRoaringFree(void *ptr) {
    if (ptr == NULL) return;
    if (bitmapRoaringAllocSizeTracker != NULL) {
        size_t usable = 0;
        zfree_usable(ptr, &usable);
        bitmapObjectAdjustAllocSize(bitmapRoaringAllocSizeTracker, usable, 0);
        return;
    }
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

    if (bitmapRoaringAllocSizeTracker != NULL) {
        size_t usable = 0;
        base = zmalloc_usable(size + extra, &usable);
        bitmapObjectAdjustAllocSize(bitmapRoaringAllocSizeTracker, 0, usable);
    } else {
        base = zmalloc(size + extra);
    }
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
    if (bitmapRoaringAllocSizeTracker != NULL) {
        size_t usable = 0;
        zfree_usable(base, &usable);
        bitmapObjectAdjustAllocSize(bitmapRoaringAllocSizeTracker, usable, 0);
        return;
    }
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
    size_t bitmap_alloc_size = 0;
    bitmapObject *bitmap = zmalloc_usable(sizeof(*bitmap), &bitmap_alloc_size);
    bitmap->byte_len = 0;
    bitmap->roaring = roaring64_bitmap_create();
    bitmap->alloc_size = bitmap_alloc_size +
                         bitmapRoaringAllocSize(bitmap->roaring);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *createBitmapObjectWithLen(uint64_t byte_len) {
    if (byte_len > BITMAP_OBJECT_MAX_BYTES) return NULL;

    robj *o = createBitmapObject();
    getBitmapObject(o)->byte_len = byte_len;
    return o;
}

robj *createBitmapObjectFromString(const unsigned char *buf, size_t len) {
#if SIZE_MAX > BITMAP_OBJECT_MAX_BYTES_RAW
    if ((uint64_t)len > BITMAP_OBJECT_MAX_BYTES) return NULL;
#endif

    size_t bitmap_alloc_size = 0;
    bitmapObject *bitmap = zmalloc_usable(sizeof(*bitmap), &bitmap_alloc_size);
    bitmap->byte_len = len;
    bitmap->roaring = bitmapObjectRoaringFromString(buf, len, 1);
    bitmap->alloc_size = bitmap_alloc_size +
                         bitmapRoaringAllocSize(bitmap->roaring);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *bitmapTypeDup(const robj *o) {
    bitmapObject *src = getBitmapObject(o);
    size_t bitmap_alloc_size = 0;
    bitmapObject *dst = zmalloc_usable(sizeof(*dst), &bitmap_alloc_size);
    dst->byte_len = src->byte_len;
    dst->roaring = roaring64_bitmap_copy(src->roaring);
    dst->alloc_size = bitmap_alloc_size +
                      bitmapRoaringAllocSize(dst->roaring);

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
    newbase = bitmapObjectActiveDefragAlloc(bitmap, base);
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
                                                bitmapObject *bitmap,
                                                uint8_t type) {
    void *moved;

    if (container == NULL) return NULL;

    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        array_container_t *array = CAST_array(container);
        uint16_t *values;
        if ((moved = bitmapObjectActiveDefragAlloc(bitmap, array)) != NULL)
            array = (array_container_t *)moved;
        if (array->array != NULL &&
            (values = bitmapObjectActiveDefragAlloc(bitmap, array->array)) != NULL)
            array->array = values;
        return (container_t *)moved;
    }
    case BITSET_CONTAINER_TYPE: {
        bitset_container_t *bitset = CAST_bitset(container);
        if ((moved = bitmapObjectActiveDefragAlloc(bitmap, bitset)) != NULL)
            bitset = (bitset_container_t *)moved;
        bitmapObjectDefragBitsetWords(bitmap, bitset);
        return (container_t *)moved;
    }
    case RUN_CONTAINER_TYPE: {
        run_container_t *run = CAST_run(container);
        rle16_t *runs;
        if ((moved = bitmapObjectActiveDefragAlloc(bitmap, run)) != NULL)
            run = (run_container_t *)moved;
        if (run->runs != NULL &&
            (runs = bitmapObjectActiveDefragAlloc(bitmap, run->runs)) != NULL)
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

static void bitmapObjectDefragLeafContainer(roaring64_bitmap_t *r,
                                            bitmapObject *bitmap,
                                            roaring64_leaf_t leaf) {
    uint64_t index = roaring64_leaf_index(leaf);
    container_t *moved = bitmapObjectDefragContainer(
        r->containers[index], bitmap, roaring64_leaf_typecode(leaf));
    if (moved != NULL) r->containers[index] = moved;
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

    size_t *prev = bitmapRoaringPushAllocSizeTracker(&bitmap->alloc_size);
    if (on)
        roaring64_bitmap_add(bitmap->roaring, bitoffset);
    else
        roaring64_bitmap_remove(bitmap->roaring, bitoffset);
    bitmapRoaringPopAllocSizeTracker(prev);

    return C_OK;
}

/* Add bits in the half-open range [start,end). */
int bitmapObjectAddRange(robj *o, uint64_t start, uint64_t end) {
    bitmapObject *bitmap = getBitmapObject(o);

    if (start >= end) return C_OK;
    if (!bitmapObjectCanRepresentBit(end - 1))
        return C_ERR;

    uint64_t byte = (end - 1) >> 3;
    if (byte + 1 > bitmap->byte_len)
        bitmap->byte_len = byte + 1;

    size_t *prev = bitmapRoaringPushAllocSizeTracker(&bitmap->alloc_size);
    roaring64_bitmap_add_range(bitmap->roaring, start, end);
    bitmapRoaringPopAllocSizeTracker(prev);

    return C_OK;
}

void bitmapObjectOptimize(robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    size_t *prev = bitmapRoaringPushAllocSizeTracker(&bitmap->alloc_size);
    roaring64_bitmap_run_optimize(bitmap->roaring);
    roaring64_bitmap_shrink_to_fit(bitmap->roaring);
    bitmapRoaringPopAllocSizeTracker(prev);
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

/* RDB raw payloads are persisted data, not client protocol bulk strings. */
sds bitmapObjectMaterializeForRDB(const robj *o) {
    return bitmapObjectMaterializeRaw(o, 0);
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
 * bytes; the BITOP NOT length guard lives at the command layer because
 * complementing is inherently dense. */
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
    case BITOP_AND:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring64_bitmap_and_inplace(result, sources[i].roaring);
            else
                roaring64_bitmap_clear(result);
        }
        break;
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
        serverPanic("Unknown native bitmap BITOP");
    }

    bitmapObjectReleaseBitopSources(sources, numkeys);

    /* Unlike BITOP temporaries, the result is stored in the keyspace, so the
     * container-conversion work pays for itself. */
    roaring64_bitmap_run_optimize(result);
    roaring64_bitmap_shrink_to_fit(result);
    zfree(sources);

    size_t bitmap_alloc_size = 0;
    bitmapObject *bitmap = zmalloc_usable(sizeof(*bitmap), &bitmap_alloc_size);
    bitmap->byte_len = maxlen;
    bitmap->roaring = result;
    bitmap->alloc_size = bitmap_alloc_size + bitmapRoaringAllocSize(result);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}
