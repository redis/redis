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

static void bitmapRoaringAlignedFree(void *ptr) {
    if (ptr == NULL) return;
    zfree(((void **)ptr)[-1]);
}

static size_t bitmapRoaringAlignedAllocSize(void *ptr) {
    if (ptr == NULL) return 0;
    return zmalloc_size(((void **)ptr)[-1]);
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

robj *createBitmapObjectFromPortable(size_t byte_len, const char *buf, size_t len) {
    if (byte_len > BITMAP_OBJECT_MAX_BYTES) return NULL;

    roaring_bitmap_t *roaring = roaring_bitmap_portable_deserialize_safe(buf, len);
    if (roaring == NULL) return NULL;

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

robj *dupBitmapObject(const robj *o) {
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

size_t bitmapObjectCardinality(const robj *o) {
    bitmapObject *bitmap = getBitmapObject(o);
    return (size_t)roaring_bitmap_get_cardinality(bitmap->roaring);
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
    return payload;
}
