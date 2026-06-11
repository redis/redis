#include "server.h"
#include "bitmap_roaring.h"

#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/run.h>
#include <roaring/memory.h>
#include <roaring/roaring.h>

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

static roaring_bitmap_t *bitmapObjectRoaringFromString(const unsigned char *buf,
                                                       size_t len)
{
    roaring_bitmap_t *roaring = roaring_bitmap_create();
    serverAssert(roaring != NULL);

    for (size_t byte = 0; byte < len; byte++) {
        unsigned char value = buf[byte];
        while (value) {
            int bit = __builtin_ctz(value);
            uint32_t offset = (uint32_t)(byte * 8 + (7 - bit));
            roaring_bitmap_add(roaring, offset);
            value &= value - 1;
        }
    }

    roaring_bitmap_run_optimize(roaring);
    roaring_bitmap_shrink_to_fit(roaring);
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

    bitmapObject *bitmap = zmalloc(sizeof(*bitmap));
    bitmap->byte_len = len;
    bitmap->roaring = bitmapObjectRoaringFromString(buf, len);

    robj *o = createObject(OBJ_BITMAP, bitmap);
    o->encoding = OBJ_ENCODING_BITMAP_ROARING;
    return o;
}

robj *createBitmapObjectFromPortable(size_t byte_len, const char *buf, size_t len, int deep_validate) {
    if (byte_len > BITMAP_OBJECT_MAX_BYTES) return NULL;

    roaring_bitmap_t *roaring = roaring_bitmap_portable_deserialize_safe(buf, len);
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
        shared_container_t *shared = CAST_shared(container);
        bitmapObjectDismissContainer(shared->container, shared->typecode);
        dismissMemory(shared, sizeof(*shared));
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

    UNUSED(size_hint);

    for (int32_t i = 0; i < array->size; i++) {
        bitmapObjectDismissContainer(
            (container_t *)array->containers[i], array->typecodes[i]);
    }

    dismissMemory(array->containers,
                  (size_t)array->allocation_size *
                      (sizeof(*array->containers) + sizeof(*array->keys) +
                       sizeof(*array->typecodes)));
    dismissMemory(bitmap->roaring, sizeof(*bitmap->roaring));
    dismissMemory(bitmap, sizeof(*bitmap));
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

static long long bitmapObjectFirstSetBit(bitmapObject *bitmap, uint64_t start,
                                         uint64_t end)
{
    roaring_uint32_iterator_t it;

    if (start >= end || start > UINT32_MAX) return -1;

    roaring_iterator_init(bitmap->roaring, &it);
    if (!roaring_uint32_iterator_move_equalorlarger(&it, (uint32_t)start))
        return -1;
    if ((uint64_t)it.current_value >= end) return -1;
    return it.current_value;
}

static long long bitmapObjectFirstClearBit(bitmapObject *bitmap,
                                           uint64_t start, uint64_t end,
                                           int end_given)
{
    roaring_uint32_iterator_t it;
    uint64_t pos = start;

    if (start >= end) return -1;
    if (start > UINT32_MAX) return end_given ? -1 : (long long)start;

    roaring_iterator_init(bitmap->roaring, &it);
    if (!roaring_uint32_iterator_move_equalorlarger(&it, (uint32_t)start))
        return (long long)start;

    while (pos < end && it.has_value) {
        roaring_uint32_range_closed_t range;

        if ((uint64_t)it.current_value >= end) return (long long)pos;
        if ((uint64_t)it.current_value > pos) return (long long)pos;
        if (roaring_uint32_iterator_read_ranges(&it, &range, 1) == 0)
            break;
        if ((uint64_t)range.min > pos) return (long long)pos;

        uint64_t range_end = (uint64_t)range.max + 1;
        if (range_end > pos) pos = range_end;
    }

    if (pos < end) return (long long)pos;
    return end_given ? -1 : (long long)end;
}

long long bitmapObjectBitpos(const robj *o, int bit, uint64_t start,
                             uint64_t end, int end_given)
{
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t bit_len = (uint64_t)bitmap->byte_len << 3;

    if (start >= end || start >= bit_len) return -1;
    if (end > bit_len) end = bit_len;

    return bit ? bitmapObjectFirstSetBit(bitmap, start, end) :
                 bitmapObjectFirstClearBit(bitmap, start, end, end_given);
}

int bitmapObjectCanRepresentBit(uint64_t bitoffset) {
    return bitoffset <= UINT32_MAX;
}

unsigned char bitmapObjectGetByte(const robj *o, size_t byte) {
    bitmapObject *bitmap = getBitmapObject(o);
    uint64_t bitoffset = (uint64_t)byte << 3;
    unsigned char value = 0;

    if (byte >= bitmap->byte_len) return 0;
    if (!bitmapObjectCanRepresentBit(bitoffset)) return 0;

    for (int bit = 0; bit < 8; bit++) {
        uint32_t offset = (uint32_t)(bitoffset + bit);
        if (roaring_bitmap_contains(bitmap->roaring, offset))
            value |= (1 << (7 - bit));
    }
    return value;
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

static sds bitmapObjectMaterializeRoaring(const roaring_bitmap_t *roaring,
                                          size_t byte_len)
{
    sds raw = sdsnewlen(NULL, byte_len);
    roaring_uint32_iterator_t it;

    roaring_iterator_init(roaring, &it);
    while (it.has_value) {
        uint32_t offset = it.current_value;
        size_t byte = offset >> 3;
        int bit = 7 - (offset & 7);
        serverAssert(byte < byte_len);
        raw[byte] |= (1 << bit);
        roaring_uint32_iterator_advance(&it);
    }

    return raw;
}

sds bitmapObjectMaterialize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return bitmapObjectMaterializeRoaring(bitmap->roaring, bitmap->byte_len);
}

typedef struct bitmapBitopSource {
    const roaring_bitmap_t *roaring;
    roaring_bitmap_t *owned;
} bitmapBitopSource;

static void bitmapObjectReleaseBitopSources(bitmapBitopSource *sources,
                                            size_t numkeys)
{
    for (size_t i = 0; i < numkeys; i++) {
        if (sources[i].owned != NULL)
            roaring_bitmap_free(sources[i].owned);
    }
}

static int bitmapObjectPrepareBitopSources(robj **objects,
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
                                              sdslen(o->ptr));
            if (sources[i].owned == NULL) return C_ERR;
            sources[i].roaring = sources[i].owned;
        }
    }
    return C_OK;
}

static roaring_bitmap_t *bitmapObjectCopyBitopSource(bitmapBitopSource *source) {
    roaring_bitmap_t *copy = source->roaring != NULL ?
        roaring_bitmap_copy(source->roaring) : roaring_bitmap_create();
    serverAssert(copy != NULL);
    return copy;
}

static roaring_bitmap_t *bitmapObjectUnionBitopSources(bitmapBitopSource *sources,
                                                       size_t start,
                                                       size_t numkeys)
{
    roaring_bitmap_t *result = roaring_bitmap_create();
    serverAssert(result != NULL);

    for (size_t i = start; i < numkeys; i++) {
        if (sources[i].roaring != NULL)
            roaring_bitmap_or_inplace(result, sources[i].roaring);
    }
    return result;
}

static roaring_bitmap_t *bitmapObjectExactlyOneBitopSources(bitmapBitopSource *sources,
                                                            size_t numkeys)
{
    roaring_bitmap_t *result = bitmapObjectCopyBitopSource(&sources[0]);
    roaring_bitmap_t *multiple = roaring_bitmap_create();
    serverAssert(multiple != NULL);

    for (size_t i = 1; i < numkeys; i++) {
        roaring_bitmap_t *both;

        if (sources[i].roaring == NULL) continue;

        both = roaring_bitmap_and(result, sources[i].roaring);
        serverAssert(both != NULL);
        roaring_bitmap_or_inplace(multiple, both);
        roaring_bitmap_free(both);

        roaring_bitmap_xor_inplace(result, sources[i].roaring);
        roaring_bitmap_andnot_inplace(result, multiple);
    }

    roaring_bitmap_free(multiple);
    return result;
}

sds bitmapObjectsBitop(bitmapBitop op, robj **objects, size_t numkeys,
                       size_t maxlen)
{
    bitmapBitopSource *sources;
    roaring_bitmap_t *result = NULL;
    sds raw;

    serverAssert(numkeys > 0);
    if (maxlen > BITMAP_OBJECT_MAX_BYTES) return NULL;

    sources = zcalloc(sizeof(*sources) * numkeys);
    if (bitmapObjectPrepareBitopSources(objects, sources, numkeys) != C_OK) {
        bitmapObjectReleaseBitopSources(sources, numkeys);
        zfree(sources);
        return NULL;
    }

    switch (op) {
    case BITMAP_BITOP_AND:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring_bitmap_and_inplace(result, sources[i].roaring);
            else
                roaring_bitmap_clear(result);
        }
        break;
    case BITMAP_BITOP_OR:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring_bitmap_or_inplace(result, sources[i].roaring);
        }
        break;
    case BITMAP_BITOP_XOR:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring_bitmap_xor_inplace(result, sources[i].roaring);
        }
        break;
    case BITMAP_BITOP_NOT:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        roaring_bitmap_flip_inplace(result, 0, (uint64_t)maxlen << 3);
        break;
    case BITMAP_BITOP_DIFF:
        result = bitmapObjectCopyBitopSource(&sources[0]);
        for (size_t i = 1; i < numkeys; i++) {
            if (sources[i].roaring != NULL)
                roaring_bitmap_andnot_inplace(result, sources[i].roaring);
        }
        break;
    case BITMAP_BITOP_DIFF1:
        result = bitmapObjectUnionBitopSources(sources, 1, numkeys);
        if (sources[0].roaring != NULL)
            roaring_bitmap_andnot_inplace(result, sources[0].roaring);
        break;
    case BITMAP_BITOP_ANDOR:
        result = bitmapObjectUnionBitopSources(sources, 1, numkeys);
        if (sources[0].roaring != NULL)
            roaring_bitmap_and_inplace(result, sources[0].roaring);
        else
            roaring_bitmap_clear(result);
        break;
    case BITMAP_BITOP_ONE:
        result = bitmapObjectExactlyOneBitopSources(sources, numkeys);
        break;
    default:
        serverPanic("Unknown native bitmap BITOP");
    }

    roaring_bitmap_run_optimize(result);
    raw = bitmapObjectMaterializeRoaring(result, maxlen);
    roaring_bitmap_free(result);
    bitmapObjectReleaseBitopSources(sources, numkeys);
    zfree(sources);
    return raw;
}

size_t bitmapObjectSerializedSize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return roaring_bitmap_portable_size_in_bytes(bitmap->roaring);
}

sds bitmapObjectSerialize(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    size_t len = roaring_bitmap_portable_size_in_bytes(bitmap->roaring);
    sds payload = sdsnewlen(SDS_NOINIT, len);
    size_t written = roaring_bitmap_portable_serialize(bitmap->roaring, payload);
    serverAssert(written == len);
    return payload;
}
