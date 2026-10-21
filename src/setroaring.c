#ifndef DISABLENEON
#define DISABLENEON 1
#endif
#ifndef ROARING_DISABLE_X64
#define ROARING_DISABLE_X64 1
#endif
#include "server.h"
#include "setroaring.h"
#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/containers.h>
#include <roaring/containers/run.h>
#include <roaring/memory.h>
#include <roaring/roaring_array.h>

static uint64_t sparseKeyHash(const void *key) {
    uintptr_t value = (uintptr_t)key;
    return dictGenHashFunction(&value, sizeof(value));
}

static int sparseKeyCompare(dictCmpCache *cache, const void *key1, const void *key2) {
    UNUSED(cache);
    return key1 == key2;
}

static void sparseBucketDestructor(dict *d, void *bucket) {
    UNUSED(d);
    zfree(bucket);
}

static dictType sparseDictType = {
    .hashFunction = sparseKeyHash,
    .keyCompare = sparseKeyCompare,
    .valDestructor = sparseBucketDestructor,
};

static size_t setRoaringAllocationSize(const void *ptr) {
    return ptr ? zmalloc_usable_size((void *)ptr) : 0;
}

static size_t setRoaringAlignedAllocationSize(const void *ptr) {
    if (!ptr) return 0;
    return setRoaringAllocationSize(((void **)ptr)[-1]);
}

static size_t setRoaringContainerAllocSize(const roaring_bitmap_t *bitmap,
                                           const container_t *container,
                                           uint8_t type) {
    UNUSED(bitmap);
    if (!container) return 0;
    size_t size = setRoaringAllocationSize(container);
    switch (type) {
    case ARRAY_CONTAINER_TYPE: {
        const array_container_t *array = const_CAST_array(container);
        return size + setRoaringAllocationSize(array->array);
    }
    case BITSET_CONTAINER_TYPE: {
        const bitset_container_t *bitset = const_CAST_bitset(container);
        return size + setRoaringAlignedAllocationSize(bitset->words);
    }
    case RUN_CONTAINER_TYPE: {
        const run_container_t *run = const_CAST_run(container);
        return size + setRoaringAllocationSize(run->runs);
    }
    default:
        serverPanic("Unknown Roaring container type");
    }
}

static size_t setRoaringBitmapAllocSize(const roaring_bitmap_t *bitmap) {
    size_t size = setRoaringAllocationSize(bitmap);
    const roaring_array_t *array = &bitmap->high_low_container;
    {
        size += setRoaringAllocationSize(array->containers);
        size += setRoaringAllocationSize(array->keys);
        size += setRoaringAllocationSize(array->typecodes);
    }
    for (int i = 0; i < array->size; i++) {
        size += setRoaringContainerAllocSize(bitmap,
            array->containers[i], array->typecodes[i]);
    }
    return size;
}

/* CRoaring's aligned allocator must keep its base pointer for zmalloc. */
static void *setRoaringAlignedMalloc(size_t alignment, size_t size) {
    uintptr_t raw, aligned;
    void *base = zmalloc(size + alignment + sizeof(void *));
    if (!base) return NULL;
    raw = (uintptr_t)base + sizeof(void *);
    aligned = (raw + alignment - 1) & ~(uintptr_t)(alignment - 1);
    ((void **)aligned)[-1] = base;
    return (void *)aligned;
}

static void setRoaringAlignedFree(void *ptr) {
    if (ptr) zfree(((void **)ptr)[-1]);
}

static void *setRoaringCalloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    return zcalloc(count * size);
}

void setRoaringInit(void) {
    roaring_memory_t hook = {
        .malloc = zmalloc,
        .realloc = zrealloc,
        .calloc = setRoaringCalloc,
        .free = zfree,
        .aligned_malloc = setRoaringAlignedMalloc,
        .aligned_free = setRoaringAlignedFree,
    };
    roaring_init_memory_hook(hook);
}

static uint64_t encodeSigned(int64_t value) {
    return ((uint64_t)value) ^ (1ULL << 63);
}

static void promotedBitmapDestructor(dict *d, void *bitmap) {
    UNUSED(d);
    roaring_bitmap_free(bitmap);
}

static dictType promotedDictType = {
    .hashFunction = sparseKeyHash,
    .keyCompare = sparseKeyCompare,
    .valDestructor = promotedBitmapDestructor,
};

int64_t setRoaringDecodeValue(uint64_t value) {
    value ^= (1ULL << 63);
    int64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

setRoaring *setRoaringCreate(void) {
    setRoaring *set = zcalloc(sizeof(*set));
    set->promoted = dictCreate(&promotedDictType);
    set->sparse = dictCreate(&sparseDictType);
    set->strings = dictCreate(&setDictType);
    if (!set->promoted || !set->sparse || !set->strings) {
        setRoaringFree(set);
        return NULL;
    }
    return set;
}

setRoaring *setRoaringDup(const setRoaring *src) {
    setRoaring *copy = setRoaringCreate();
    if (!copy) return NULL;
    dictIterator pi;
    dictInitIterator(&pi, src->promoted);
    dictEntry *pde;
    while ((pde = dictNext(&pi)) != NULL) {
        roaring_bitmap_t *bitmap = roaring_bitmap_copy(dictGetVal(pde));
        serverAssert(bitmap != NULL);
        serverAssert(dictAdd(copy->promoted, dictGetKey(pde), bitmap) == DICT_OK);
    }
    dictResetIterator(&pi);
    copy->promoted_count = src->promoted_count;
    dictIterator si;
    dictInitIterator(&si, src->sparse);
    dictEntry *sde;
    while ((sde = dictNext(&si)) != NULL) {
        setRoaringSparseBucket *source = dictGetVal(sde);
        size_t bytes = sizeof(*source) + source->capacity * sizeof(uint16_t);
        setRoaringSparseBucket *bucket = zmalloc(bytes);
        memcpy(bucket, source, bytes);
        serverAssert(dictAdd(copy->sparse, dictGetKey(sde), bucket) == DICT_OK);
        copy->sparse_count += bucket->count;
    }
    dictResetIterator(&si);
    dictIterator di;
    dictInitIterator(&di, src->strings);
    dictEntry *de;
    while ((de = dictNext(&di)) != NULL) {
        sds value = sdsdup(dictGetKey(de));
        serverAssert(dictAdd(copy->strings, value, NULL) == DICT_OK);
        *htGetMetadataSize(copy->strings) += sdsAllocSize(value);
    }
    dictResetIterator(&di);
    return copy;
}

void setRoaringFree(setRoaring *set) {
    if (!set) return;
    if (set->sparse) dictRelease(set->sparse);
    if (set->strings) dictRelease(set->strings);
    if (set->promoted) dictRelease(set->promoted);
    zfree(set);
}

void setRoaringDismiss(setRoaring *set, size_t size_hint) {
    if (!set) return;
    if (set->sparse) {
        dictIterator di;
        dictInitIterator(&di, set->sparse);
        dictEntry *de;
        while ((de = dictNext(&di)) != NULL) {
            setRoaringSparseBucket *bucket = dictGetVal(de);
            dismissMemory(bucket, sizeof(*bucket) + bucket->capacity * sizeof(uint16_t));
        }
        dictResetIterator(&di);
        dismissDictBucketsMemory(set->sparse);
    }
    if (set->strings) {
        if (dictSize(set->strings) && size_hint / dictSize(set->strings) >= server.page_size) {
            dictIterator di;
            dictInitIterator(&di, set->strings);
            dictEntry *de;
            while ((de = dictNext(&di)) != NULL)
                dismissSds(dictGetKey(de));
            dictResetIterator(&di);
        }
        dismissDictBucketsMemory(set->strings);
    }
    dictIterator pi;
    dictInitIterator(&pi, set->promoted);
    dictEntry *pde;
    while ((pde = dictNext(&pi)) != NULL) {
        roaring_bitmap_t *bitmap = dictGetVal(pde);
        roaring_array_t *array = &bitmap->high_low_container;
        uint32_t count = roaring_bitmap_get_cardinality(bitmap);
        if (count && size_hint / count >= server.page_size) {
          for (int i = 0; i < array->size; i++) {
            container_t *container = array->containers[i];
            switch (array->typecodes[i]) {
            case ARRAY_CONTAINER_TYPE: {
                array_container_t *array = CAST_array(container);
                dismissMemory(array->array, (size_t)array->capacity * sizeof(*array->array));
                dismissMemory(array, sizeof(*array));
                break;
            }
            case BITSET_CONTAINER_TYPE: {
                bitset_container_t *bitset = CAST_bitset(container);
                dismissMemory(((void **)bitset->words)[-1], 0);
                dismissMemory(bitset, sizeof(*bitset));
                break;
            }
            case RUN_CONTAINER_TYPE: {
                run_container_t *run = CAST_run(container);
                dismissMemory(run->runs, (size_t)run->capacity * sizeof(*run->runs));
                dismissMemory(run, sizeof(*run));
                break;
            }
            default: serverPanic("Unknown Roaring container type");
            }
          }
        }
        dismissMemory(array->containers, (size_t)array->allocation_size * sizeof(*array->containers));
        dismissMemory(array->keys, (size_t)array->allocation_size * sizeof(*array->keys));
        dismissMemory(array->typecodes, (size_t)array->allocation_size * sizeof(*array->typecodes));
        dismissMemory(bitmap, sizeof(*bitmap));
    }
    dictResetIterator(&pi);
    dismissDictBucketsMemory(set->promoted);
    dismissMemory(set->promoted, sizeof(dict));
    dismissMemory(set->sparse, sizeof(dict));
    dismissMemory(set->strings, sizeof(dict));
    dismissMemory(set, sizeof(*set));
}

int setRoaringAddInt(setRoaring *set, int64_t value) {
    uint64_t encoded = encodeSigned(value), key = (encoded >> 16) + 1;
    dictEntry *de = dictFind(set->sparse, (void *)(uintptr_t)key);
    if (!de) {
        dictEntry *promoted = dictFind(set->promoted, (void *)(uintptr_t)key);
        if (promoted) {
            int added = roaring_bitmap_add_checked(dictGetVal(promoted), encoded & 0xffff);
            set->promoted_count += added;
            return added;
        }
    }
    if (!de) {
        setRoaringSparseBucket *bucket = zmalloc(sizeof(*bucket) + 4 * sizeof(uint16_t));
        bucket->count = 1;
        bucket->capacity = 4;
        bucket->values[0] = encoded & 0xffff;
        if (dictAdd(set->sparse, (void *)(uintptr_t)key, bucket) != DICT_OK) {
            zfree(bucket);
            return 0;
        }
        set->sparse_count++;
        return 1;
    }
    setRoaringSparseBucket *bucket = dictGetVal(de);
    uint16_t low = encoded & 0xffff;
    for (uint16_t i = 0; i < bucket->count; i++)
        if (bucket->values[i] == low) return 0;
    if (bucket->count + 1 >= SETROARING_PROMOTE_THRESHOLD) {
        roaring_bitmap_t *bitmap = roaring_bitmap_create();
        roaring_bitmap_add(bitmap, low);
        for (uint16_t i = 0; i < bucket->count; i++) roaring_bitmap_add(bitmap, bucket->values[i]);
        serverAssert(dictAdd(set->promoted, (void *)(uintptr_t)key, bitmap) == DICT_OK);
        set->promoted_count += bucket->count + 1;
        set->sparse_count -= bucket->count;
        dictDelete(set->sparse, (void *)(uintptr_t)key);
        return 1;
    }
    if (bucket->count == bucket->capacity) {
        bucket->capacity *= 2;
        bucket = zrealloc(bucket, sizeof(*bucket) + bucket->capacity * sizeof(uint16_t));
        dictSetVal(set->sparse, de, bucket);
    }
    bucket->values[bucket->count++] = low;
    set->sparse_count++;
    return 1;
}

int setRoaringRemoveInt(setRoaring *set, int64_t value) {
    uint64_t encoded = encodeSigned(value), key = (encoded >> 16) + 1;
    dictEntry *de = dictFind(set->sparse, (void *)(uintptr_t)key);
    if (!de) {
        dictEntry *promoted = dictFind(set->promoted, (void *)(uintptr_t)key);
        if (!promoted) return 0;
        roaring_bitmap_t *bitmap = dictGetVal(promoted);
        int removed = roaring_bitmap_remove_checked(bitmap, encoded & 0xffff);
        set->promoted_count -= removed;
        if (removed && !roaring_bitmap_get_cardinality(bitmap)) dictDelete(set->promoted, (void *)(uintptr_t)key);
        return removed;
    }
    setRoaringSparseBucket *bucket = dictGetVal(de);
    uint16_t low = encoded & 0xffff;
    for (uint16_t i = 0; i < bucket->count; i++) {
        if (bucket->values[i] != low) continue;
        bucket->values[i] = bucket->values[--bucket->count];
        set->sparse_count--;
        if (!bucket->count) dictDelete(set->sparse, (void *)(uintptr_t)key);
        return 1;
    }
    return 0;
}

int setRoaringFindInt(const setRoaring *set, int64_t value) {
    uint64_t encoded = encodeSigned(value), key = (encoded >> 16) + 1;
    dictEntry *de = dictFind(set->sparse, (void *)(uintptr_t)key);
    if (de) {
        setRoaringSparseBucket *bucket = dictGetVal(de);
        uint16_t low = encoded & 0xffff;
        for (uint16_t i = 0; i < bucket->count; i++)
            if (bucket->values[i] == low) return 1;
        return 0;
    }
    dictEntry *promoted = dictFind(set->promoted, (void *)(uintptr_t)key);
    return promoted && roaring_bitmap_contains(dictGetVal(promoted), encoded & 0xffff);
}

int setRoaringAddString(setRoaring *set, sds value) {
    if (dictAdd(set->strings, value, NULL) != DICT_OK) return 0;
    *htGetMetadataSize(set->strings) += sdsAllocSize(value);
    return 1;
}

int setRoaringRemoveString(setRoaring *set, const char *value, size_t len) {
    sds key = sdsnewlen(value, len);
    int removed = dictDelete(set->strings, key) == DICT_OK;
    sdsfree(key);
    return removed;
}

int setRoaringFindString(const setRoaring *set, const char *value, size_t len) {
    sds key = sdsnewlen(value, len);
    int found = dictFind(set->strings, key) != NULL;
    sdsfree(key);
    return found;
}

uint64_t setRoaringIntCount(const setRoaring *set) {
    return set->promoted_count + set->sparse_count;
}

uint64_t setRoaringSize(const setRoaring *set) {
    return setRoaringIntCount(set) + dictSize(set->strings);
}

int setRoaringSelectInt(const setRoaring *set, uint64_t rank, int64_t *value) {
    uint64_t encoded;
    dictIterator di;
    dictInitIterator(&di, set->promoted);
    dictEntry *de;
    while ((de = dictNext(&di)) != NULL) {
        roaring_bitmap_t *bitmap = dictGetVal(de);
        uint64_t count = roaring_bitmap_get_cardinality(bitmap);
        if (rank < count) {
            uint32_t low;
            int ok = roaring_bitmap_select(bitmap, rank, &low);
            serverAssert(ok);
            encoded = (((uint64_t)(uintptr_t)dictGetKey(de) - 1) << 16) | low;
            dictResetIterator(&di);
            *value = setRoaringDecodeValue(encoded);
            return 1;
        }
        rank -= count;
    }
    dictResetIterator(&di);
    dictInitIterator(&di, set->sparse);
    while ((de = dictNext(&di)) != NULL) {
        setRoaringSparseBucket *bucket = dictGetVal(de);
        if (rank < bucket->count) {
            encoded = (((uint64_t)(uintptr_t)dictGetKey(de) - 1) << 16) | bucket->values[rank];
            dictResetIterator(&di);
            *value = setRoaringDecodeValue(encoded);
            return 1;
        }
        rank -= bucket->count;
    }
    dictResetIterator(&di);
    return 0;
}

uint64_t setRoaringAllocSize(const setRoaring *set) {
    if (!set) return 0;
    uint64_t sparse = sizeof(dict) + dictMemUsage(set->sparse);
    dictIterator di;
    dictInitIterator(&di, set->sparse);
    dictEntry *de;
    while ((de = dictNext(&di)) != NULL) {
        setRoaringSparseBucket *bucket = dictGetVal(de);
        sparse += setRoaringAllocationSize(bucket);
    }
    dictResetIterator(&di);
    uint64_t promoted = sizeof(dict) + dictMemUsage(set->promoted);
    dictInitIterator(&di, set->promoted);
    while ((de = dictNext(&di)) != NULL) promoted += setRoaringBitmapAllocSize(dictGetVal(de));
    dictResetIterator(&di);
    return setRoaringAllocationSize(set) + sparse + promoted +
           sizeof(dict) + dictMemUsage(set->strings) + *htGetMetadataSize(set->strings);
}

dict *setRoaringSparse(setRoaring *set) { return set->sparse; }
dict *setRoaringPromoted(setRoaring *set) { return set->promoted; }


uint64_t setRoaringSparseKey(uint64_t encoded) { return (encoded >> 16) + 1; }

uint64_t setRoaringSparseEncodedValue(const dictEntry *entry, uint16_t low) {
    return (((uint64_t)(uintptr_t)dictGetKey(entry) - 1) << 16) | low;
}

dict *setRoaringStrings(setRoaring *set) {
    return set->strings;
}
