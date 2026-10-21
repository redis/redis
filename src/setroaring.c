/* Experimental set storage: sparse integer buckets promote into bitroar's
 * merged Redis Roaring64 bitmap; non-integers stay in a hash table. */
#include "server.h"
#include "bitroar.h"
#include "setroaring.h"

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

static uint64_t encodeSigned(int64_t value) {
    return ((uint64_t)value) ^ (1ULL << 63);
}

int64_t setRoaringDecodeValue(uint64_t value) {
    value ^= (1ULL << 63);
    int64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

void setRoaringInit(void) {
    /* bitroarInit() installs the shared CRoaring allocator during startup. */
}

setRoaring *setRoaringCreate(void) {
    setRoaring *set = zcalloc(sizeof(*set));
    set->integers = bitroarCreate();
    set->sparse = dictCreate(&sparseDictType);
    set->strings = dictCreate(&setDictType);
    if (!set->integers || !set->sparse || !set->strings) {
        setRoaringFree(set);
        return NULL;
    }
    return set;
}

setRoaring *setRoaringDup(const setRoaring *src) {
    setRoaring *copy = setRoaringCreate();
    if (!copy) return NULL;
    decrRefCount(copy->integers);
    copy->integers = bitroarDup(src->integers);
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
    if (set->integers) decrRefCount(set->integers);
    if (set->sparse) dictRelease(set->sparse);
    if (set->strings) dictRelease(set->strings);
    zfree(set);
}

void setRoaringDismiss(setRoaring *set, size_t size_hint) {
    if (!set) return;
    if (set->integers) {
        bitroarDismiss(set->integers, size_hint);
        dismissMemory(set->integers, sizeof(robj));
    }
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
        dismissMemory(set->sparse, sizeof(dict));
    }
    if (set->strings) {
        if (dictSize(set->strings) && size_hint / dictSize(set->strings) >= server.page_size) {
            dictIterator di;
            dictInitIterator(&di, set->strings);
            dictEntry *de;
            while ((de = dictNext(&di)) != NULL) dismissSds(dictGetKey(de));
            dictResetIterator(&di);
        }
        dismissDictBucketsMemory(set->strings);
        dismissMemory(set->strings, sizeof(dict));
    }
    dismissMemory(set, sizeof(*set));
}

int setRoaringAddInt(setRoaring *set, int64_t value) {
    uint64_t encoded = encodeSigned(value), key = (encoded >> 16) + 1;
    /* A slice can contain promoted values and a new sparse bucket at the
     * same time. Check the bitmap before the bucket so duplicates already in
     * the promoted portion are not inserted into the sparse portion. */
    if (bitroarContainsRawValue(set->integers, encoded)) return 0;
    dictEntry *de = dictFind(set->sparse, (void *)(uintptr_t)key);
    if (!de) {
        setRoaringSparseBucket *bucket = zmalloc(sizeof(*bucket) + 4 * sizeof(uint16_t));
        bucket->count = 1;
        bucket->capacity = 4;
        bucket->values[0] = encoded & 0xffff;
        serverAssert(dictAdd(set->sparse, (void *)(uintptr_t)key, bucket) == DICT_OK);
        set->sparse_count++;
        return 1;
    }
    setRoaringSparseBucket *bucket = dictGetVal(de);
    uint16_t low = encoded & 0xffff;
    for (uint16_t i = 0; i < bucket->count; i++)
        if (bucket->values[i] == low) return 0;
    if (bucket->count + 1 >= SETROARING_PROMOTE_THRESHOLD) {
        for (uint16_t i = 0; i < bucket->count; i++) {
            uint64_t member = (((uint64_t)(uintptr_t)dictGetKey(de) - 1) << 16) | bucket->values[i];
            serverAssert(bitroarAddRawValue(set->integers, member));
        }
        serverAssert(bitroarAddRawValue(set->integers, encoded));
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
    if (de) {
        setRoaringSparseBucket *bucket = dictGetVal(de);
        uint16_t low = encoded & 0xffff;
        for (uint16_t i = 0; i < bucket->count; i++) {
            if (bucket->values[i] != low) continue;
            bucket->values[i] = bucket->values[--bucket->count];
            set->sparse_count--;
            if (!bucket->count) dictDelete(set->sparse, (void *)(uintptr_t)key);
            return 1;
        }
    }
    int removed = bitroarRemoveRawValue(set->integers, encoded);
    set->promoted_count -= removed;
    return removed;
}

int setRoaringFindInt(const setRoaring *set, int64_t value) {
    uint64_t encoded = encodeSigned(value), key = (encoded >> 16) + 1;
    dictEntry *de = dictFind(set->sparse, (void *)(uintptr_t)key);
    if (de) {
        setRoaringSparseBucket *bucket = dictGetVal(de);
        uint16_t low = encoded & 0xffff;
        for (uint16_t i = 0; i < bucket->count; i++)
            if (bucket->values[i] == low) return 1;
    }
    return bitroarContainsRawValue(set->integers, encoded);
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
    uint64_t promoted = bitroarCardinality(set->integers), encoded;
    if (rank < promoted) {
        if (!bitroarSelectRawValue(set->integers, rank, &encoded)) return 0;
        *value = setRoaringDecodeValue(encoded);
        return 1;
    }
    rank -= promoted;
    dictIterator di;
    dictInitIterator(&di, set->sparse);
    dictEntry *de;
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
        sparse += zmalloc_usable_size(bucket);
    }
    dictResetIterator(&di);
    return zmalloc_usable_size((void *)set) + sparse +
           sizeof(robj) + bitroarAllocSize(set->integers) + sizeof(dict) +
           dictMemUsage(set->strings) + *htGetMetadataSize(set->strings);
}

dict *setRoaringSparse(setRoaring *set) { return set->sparse; }
robj *setRoaringIntegers(setRoaring *set) { return set->integers; }

uint64_t setRoaringSparseKey(uint64_t encoded) { return (encoded >> 16) + 1; }

uint64_t setRoaringSparseEncodedValue(const dictEntry *entry, uint16_t low) {
    return (((uint64_t)(uintptr_t)dictGetKey(entry) - 1) << 16) | low;
}

dict *setRoaringStrings(setRoaring *set) { return set->strings; }
