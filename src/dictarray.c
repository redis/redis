#include "fmacros.h"

#include <string.h>
#include <stddef.h>

#include "zmalloc.h"
#include "dictarray.h"
#include "redisassert.h"
#include "monotonic.h"

#define UNUSED(V) ((void) V)

/**********************************/
/*** Helpers **********************/
/**********************************/

/* Returns total (cumulative) number of keys up until given dict-index (inclusive).
 * Time complexity is O(log(da->num_dicts)). */
static unsigned long long cumulativeKeyCountRead(dictarray *da, int didx) {
    if (da->num_dicts == 1) {
        assert(didx == 0);
        return daSize(da);
    }
    int idx = didx + 1;
    unsigned long long sum = 0;
    while (idx > 0) {
        sum += da->state.dict_size_index[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

static void addDictIndexToCursor(dictarray *da, int didx, unsigned long long *cursor) {
    if (da->num_dicts == 1)
        return;
    /* didx can be -1 when iteration is over and there are no more dicts to visit. */
    if (didx < 0)
        return;
    *cursor = (*cursor << da->num_dicts_bits) | didx;
}

static int getAndClearDictIndexFromCursor(dictarray *da, unsigned long long *cursor) {
    if (da->num_dicts == 1)
        return 0;
    int didx = (int) (*cursor & (da->num_dicts-1));
    *cursor = *cursor >> da->num_dicts_bits;
    return didx;
}

/* Updates binary index tree (also known as Fenwick tree), increasing key count for a given dict.
 * You can read more about this data structure here https://en.wikipedia.org/wiki/Fenwick_tree
 * Time complexity is O(log(da->num_dicts)). */
static void daCumulativeKeyCountAdd(dictarray *da, int didx, long delta) {
    da->state.key_count += delta;

    dict *d = daGetDict(da, didx);
    size_t dsize = dictSize(d);
    int non_empty_dicts_delta = dsize == 1? 1 : dsize == 0? -1 : 0;
    da->state.non_empty_dicts += non_empty_dicts_delta;

    /* BIT does not need to be calculated when the cluster is turned off. */
    if (da->num_dicts == 1)
        return;

    /* Update the BIT */
    int idx = didx + 1; /* Unlike dict indices, BIT is 1-based, so we need to add 1. */
    while (idx <= da->num_dicts) {
        if (delta < 0) {
            assert(da->state.dict_size_index[idx] >= (unsigned long long)labs(delta));
        }
        da->state.dict_size_index[idx] += delta;
        idx += (idx & -idx);
    }
}

/**********************************/
/*** Dict extension ***************/
/**********************************/

/* Adds dictionary to the rehashing list, which allows us
 * to quickly find rehash targets during incremental rehashing.
 *
 * Updates the bucket count in cluster-mode for the given dictionary in a DB, bucket count
 * incremented with the new ht size during the rehashing phase. In non-cluster mode,
 * bucket count can be retrieved directly from single dict bucket. */
void daDictRehashingStarted(dict *d) {
    daDictMetadata *metadata = (daDictMetadata *)dictMetadata(d);
    listAddNodeTail(metadata->da->state.rehashing, d);
    metadata->rehashing_node = listLast(metadata->da->state.rehashing);

    if (metadata->da->num_dicts == 1)
        return;
    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    metadata->da->state.bucket_count += to; /* Started rehashing (Add the new ht size) */
}

/* Remove dictionary from the rehashing list.
 *
 * Updates the bucket count for the given dictionary in a DB. It removes
 * the old ht size of the dictionary from the total sum of buckets for a DB.  */
void daDictRehashingCompleted(dict *d) {
    daDictMetadata *metadata = (daDictMetadata *)dictMetadata(d);
    if (metadata->rehashing_node) {
        listDelNode(metadata->da->state.rehashing, metadata->rehashing_node);
        metadata->rehashing_node = NULL;
    }

    if (metadata->da->num_dicts == 1)
        return;
    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    metadata->da->state.bucket_count -= from; /* Finished rehashing (Remove the old ht size) */
}

/* Returns the size of the DB dict metadata in bytes. */
size_t daDictMetadataSize(dict *d) {
    UNUSED(d);
    /* NOTICE: this also affects overhead_ht_main and overhead_ht_expires in getMemoryOverheadData. */
    return sizeof(daDictMetadata);
}

/**********************************/
/*** API **************************/
/**********************************/

/* Create an array of dictionaries */
dictarray *daCreate(dictType *type, int num_dicts_bits) {
    dictarray *da = zcalloc(sizeof(*da));

    memcpy(&da->dtype, type, sizeof(da->dtype));
    assert(!type->dictMetadataBytes);
    assert(!type->rehashingStarted);
    assert(!type->rehashingCompleted);
    da->dtype.dictMetadataBytes = daDictMetadataSize;
    da->dtype.rehashingStarted = daDictRehashingStarted;
    da->dtype.rehashingCompleted = daDictRehashingCompleted;


    da->num_dicts_bits = num_dicts_bits;
    da->num_dicts = 1 << da->num_dicts_bits;
    da->dicts = zmalloc(sizeof(dict*) * da->num_dicts);
    for (int i = 0; i < da->num_dicts; i++) {
        da->dicts[i] = dictCreate(&da->dtype);
        daDictMetadata *metadata = (daDictMetadata *)dictMetadata(da->dicts[i]);
        metadata->da = da;
    }

    da->state.rehashing = listCreate();
    da->state.key_count = 0;
    da->state.non_empty_dicts = 0;
    da->state.resize_cursor = -1;
    da->state.dict_size_index = da->num_dicts > 1? zcalloc(sizeof(unsigned long long) * (da->num_dicts + 1)) : NULL;
    da->state.bucket_count = 0;

    return da;
}

void daEmpty(dictarray *da, void(callback)(dict*)) {
    for (int didx = 0; didx < da->num_dicts; didx++) {
        dict *d = daGetDict(da, didx);
        daDictMetadata *metadata = (daDictMetadata *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        dictEmpty(d, callback);
    }

    listEmpty(da->state.rehashing);

    da->state.key_count = 0;
    da->state.non_empty_dicts = 0;
    da->state.resize_cursor = -1;
    da->state.bucket_count = -1;
    if (da->state.dict_size_index)
        memset(da->state.dict_size_index, 0, sizeof(unsigned long long) * (da->num_dicts + 1));
}

void daRelease(dictarray *da) {
    for (int didx = 0; didx < da->num_dicts; didx++) {
        dict *d = daGetDict(da, didx);
        daDictMetadata *metadata = (daDictMetadata *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        dictRelease(daGetDict(da, didx));
    }
    zfree(da->dicts);

    listRelease(da->state.rehashing);
    if (da->state.dict_size_index)
        zfree(da->state.dict_size_index);

    zfree(da);
}

unsigned long long int daSize(dictarray *da) {
    if (da->num_dicts != 1) {
        return da->state.key_count;
    } else {
        return dictSize(da->dicts[0]);
    }
}

/* This method provides the cumulative sum of all the dictionary buckets
 * across dictionaries in a database. */
unsigned long daBuckets(dictarray *da) {
    if (da->num_dicts != 1) {
        return da->state.bucket_count;
    } else {
        return dictBuckets(da->dicts[0]);
    }
}

size_t daMemUsage(dictarray *da) {
    size_t mem = 0;
    unsigned long long keys_count = daSize(da);
    mem += keys_count * dictEntryMemUsage() +
           daBuckets(da) * sizeof(dictEntry*) +
           da->num_dicts * (sizeof(dict) + dictMetadataSize(daGetDict(da, 0)));
    return mem;
}

/*
 * This method is used to iterate over the elements of the entire dictarray specifically across dicts.
 * It's a three pronged approach.
 *
 * 1. It uses the provided cursor `cursor` to retrieve the dict index from it.
 * 2. If the dictionary is in a valid state checked through the provided callback `dictScanValidFunction`,
 *    it performs a dictScan over the appropriate `keyType` dictionary of `db`.
 * 3. If the dict is entirely scanned i.e. the cursor has reached 0, the next non empty dict is discovered.
 *    The dict information is embedded into the cursor and returned.
 *
 * To restrict the scan to a single cluster dict, pass a valid dict index as
 * 'onlydidx', otherwise pass -1.
 */
unsigned long long daScan(dictarray *da, unsigned long long cursor,
                          int onlydidx, dictScanFunction *scan_cb,
                          dictarrayScanShouldSkipDict *skip_cb,
                          void *privdata)
{
    unsigned long long _cursor = 0;
    /* During main dictionary traversal in cluster mode, 48 upper bits in the cursor are used for positioning in the HT.
     * Following lower bits are used for the dict index number, ranging from 0 to 2^num_dicts_bits-1.
     * Dict index is always 0 at the start of iteration and can be incremented only if there are multiple dicts. */
    int didx = getAndClearDictIndexFromCursor(da, &cursor);
    if (onlydidx >= 0) {
        if (didx < onlydidx) {
            /* Fast-forward to onlydidx. */
            assert(onlydidx < da->num_dicts);
            didx = onlydidx;
            cursor = 0;
        } else if (didx > onlydidx) {
            /* The cursor is already past onlydidx. */
            return 0;
        }
    }

    dict *d = daGetDict(da, didx);

    int skip = skip_cb && skip_cb(d);
    if (!skip) {
        _cursor = dictScan(d, cursor, scan_cb, privdata);
    }
    /* scanning done for the current dictionary or if the scanning wasn't possible, move to the next dict index. */
    if (_cursor == 0 || skip) {
        if (onlydidx >= 0)
            return 0;
        didx = daGetNextNonEmptyDictIndex(da, didx);
    }
    if (didx == -1) {
        return 0;
    }
    addDictIndexToCursor(da, didx, &_cursor);
    return _cursor;
}

/*
 * This functions increases size of dictarray to match desired number.
 * It resizes all individual dictionaries, unless skip_cb indicates otherwise.
 *
 * Based on the parameter `try_expand`, appropriate dict expand API is invoked.
 * if try_expand is set to 1, `dictTryExpand` is used else `dictExpand`.
 * The return code is either `DICT_OK`/`DICT_ERR` for both the API(s).
 * `DICT_OK` response is for successful expansion. However ,`DICT_ERR` response signifies failure in allocation in
 * `dictTryExpand` call and in case of `dictExpand` call it signifies no expansion was performed.
 */
int daExpand(dictarray *da, uint64_t newsize, int try_expand, dictarrayExpandShouldSkipDictIndex *skip_cb) {
    for (int i = 0; i < da->num_dicts; i++) {
        if (skip_cb && skip_cb(i))
            continue;
        dict *d = daGetDict(da, i);
        int result = try_expand ? dictTryExpand(d, newsize) : dictExpand(d, newsize);
        if (try_expand && result == DICT_ERR)
            return 0;
    }

    return 1;
}

/* Returns fair random dict index, probability of each dict being returned is proportional to the number of elements that dictionary holds.
 * This function guarantees that it returns a dict-index of a non-empty dict, unless the entire dictarray is empty.
 * Time complexity of this function is O(log(da->num_dicts)). */
int daGetFairRandomDictIndex(dictarray *da) {
    unsigned long target = daSize(da) ? (randomULong() % daSize(da)) + 1 : 0;
    return daFindDictIndexByKeyIndex(da, target);
}

void daGetStats(dictarray *da, char *buf, size_t bufsize, int full) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;
    dictStats *mainHtStats = NULL;
    dictStats *rehashHtStats = NULL;
    dict *d;
    daIterator *dait = daIteratorInit(da);
    while ((d = daIteratorNextDict(dait))) {
        dictStats *stats = dictGetStatsHt(d, 0, full);
        if (!mainHtStats) {
            mainHtStats = stats;
        } else {
            dictCombineStats(stats, mainHtStats);
            dictFreeStats(stats);
        }
        if (dictIsRehashing(d)) {
            stats = dictGetStatsHt(d, 1, full);
            if (!rehashHtStats) {
                rehashHtStats = stats;
            } else {
                dictCombineStats(stats, rehashHtStats);
                dictFreeStats(stats);
            }
        }
    }
    daReleaseIterator(dait);
    l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
    dictFreeStats(mainHtStats);
    buf += l;
    bufsize -= l;
    if (rehashHtStats && bufsize > 0) {
        dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
}

dict *daGetDict(dictarray *da, int didx) {
    return da->dicts[didx];
}

/* Finds a dict containing target element in a key space ordered by dict index.
 * Consider this example. Dictionaries are represented by brackets and keys by dots:
 *  #0   #1   #2     #3    #4
 * [..][....][...][.......][.]
 *                    ^
 *                 target
 *
 * In this case dict #3 contains key that we are trying to find.
 *
 * The return value is 0 based dict-index, and the range of the target is [1..daSize], daSize inclusive.
 *
 * To find the dict, we start with the root node of the binary index tree and search through its children
 * from the highest index (2^num_dicts_bits in our case) to the lowest index. At each node, we check if the target
 * value is greater than the node's value. If it is, we remove the node's value from the target and recursively
 * search for the new target using the current node as the parent.
 * Time complexity of this function is O(log(da->num_dicts))
 */
int daFindDictIndexByKeyIndex(dictarray *da, unsigned long target) {
    if (da->num_dicts == 1 || daSize(da) == 0)
        return 0;
    assert(target <= daSize(da));

    int result = 0, bit_mask = 1 << da->num_dicts_bits;
    for (int i = bit_mask; i != 0; i >>= 1) {
        int current = result + i;
        /* When the target index is greater than 'current' node value the we will update
         * the target and search in the 'current' node tree. */
        if (target > da->state.dict_size_index[current]) {
            target -= da->state.dict_size_index[current];
            result = current;
        }
    }
    /* Adjust the result to get the correct dict:
     * 1. result += 1;
     *    After the calculations, the index of target in dict_size_index should be the next one,
     *    so we should add 1.
     * 2. result -= 1;
     *    Unlike BIT(dict_size_index is 1-based), dict indices are 0-based, so we need to subtract 1.
     * As the addition and subtraction cancel each other out, we can simply return the result. */
    return result;
}

/* Returns next non-empty dict index strictly after given one, or -1 if provided didx is the last one. */
int daGetNextNonEmptyDictIndex(dictarray *da, int didx) {
    unsigned long long next_key = cumulativeKeyCountRead(da, didx) + 1;
    return next_key <= daSize(da) ? daFindDictIndexByKeyIndex(da, next_key) : -1;
}

int daNonEmptyDicts(dictarray *da) {
    return da->state.non_empty_dicts;
}

/* Returns dictarray iterator that can be used to iterate through sub-dictionaries.
 *
 * The caller should free the resulting dait with daReleaseIterator. */
daIterator *daIteratorInit(dictarray *da) {
    daIterator *dait = zmalloc(sizeof(*dait));
    dait->da = da;
    dait->didx = -1;
    dait->next_didx = daFindDictIndexByKeyIndex(dait->da, 1); /* Finds first non-empty dict index. */
    dictInitSafeIterator(&dait->di, NULL);
    return dait;
}

/* Free the dbit returned by dbIteratorInit. */
void daReleaseIterator(daIterator *dait) {
    dictIterator *iter = &dait->di;
    dictResetIterator(iter);

    zfree(dait);
}

/* Returns next dictionary from the iterator, or NULL if iteration is complete. */
dict *daIteratorNextDict(daIterator *dait) {
    if (dait->next_didx == -1)
        return NULL;
    dait->didx = dait->next_didx;
    dait->next_didx = daGetNextNonEmptyDictIndex(dait->da, dait->didx);
    return dait->da->dicts[dait->didx];
}

int daIteratorGetCurrentDictIndex(daIterator *dait) {
    assert(dait->didx >= 0 && dait->didx < dait->da->num_dicts);
    return dait->didx;
}

/* Returns next entry. */
dictEntry *daIteratorNext(daIterator *dait) {
    dictEntry *de = dait->di.d ? dictNext(&dait->di) : NULL;
    if (!de) { /* No current dict or reached the end of the dictionary. */
        dict *d = daIteratorNextDict(dait);
        if (!d)
            return NULL;
        if (dait->di.d) {
            /* Before we move to the next dict, reset the iter of the previous dict. */
            dictIterator *iter = &dait->di;
            dictResetIterator(iter);
        }
        dictInitSafeIterator(&dait->di, d);
        de = dictNext(&dait->di);
    }
    return de;
}

dict *daGetDictFromIterator(daIterator *dait) {
    return daGetDict(dait->da, dait->didx);
}

/* Cursor-scan the dictarray and attempt to resize (if needed, handled by dictResize) */
void daTryResizeHashTables(dictarray *da, int limit) {
    if (daSize(da) == 0)
        return;

    if (da->state.resize_cursor == -1)
        da->state.resize_cursor = daFindDictIndexByKeyIndex(da, 1);

    for (int i = 0; i < limit && da->state.resize_cursor != -1; i++) {
        int didx = da->state.resize_cursor;
        dict *d = daGetDict(da, didx);
        dictResize(d);
        da->state.resize_cursor = daGetNextNonEmptyDictIndex(da, didx);
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns the amount of microsecs spent if some rehashing was
 * performed, otherwise -1 is returned. */
int daIncrementallyRehash(dictarray *da, uint64_t threshold_us) {
    if (listLength(da->state.rehashing) == 0)
        return -1;

    /* Our goal is to rehash as many dictionaries as we can before reaching predefined threshold,
     * after each dictionary completes rehashing, it removes itself from the list. */
    listNode *node;
    monotime timer;
    uint64_t elapsed_us;
    elapsedStart(&timer);
    while ((node = listFirst(da->state.rehashing))) {
        elapsed_us = elapsedUs(timer);
        if (elapsed_us >= threshold_us) {
            break;  /* Reached the time limit. */
        }
        dictRehashMicroseconds(listNodeValue(node), threshold_us - elapsed_us);
    }
    assert(elapsed_us != 0);
    return elapsed_us;
}

dictEntry *daDictFind(dictarray *da, int didx, void *key) {
    dict *d = daGetDict(da, didx);
    return dictFind(d, key);
}

dictEntry *daDictAddRaw(dictarray *da, int didx, void *key, dictEntry **existing) {
    dictEntry *ret = dictAddRaw(daGetDict(da, didx), key, existing);
    if (ret)
        daCumulativeKeyCountAdd(da, didx, 1);
    return ret;
}

void daDictSetKey(dictarray *da, int didx, dictEntry* de, void *key) {
    dictSetKey(daGetDict(da, didx), de, key);
}

void daDictSetVal(dictarray *da, int didx, dictEntry *de, void *val) {
    dictSetVal(daGetDict(da, didx), de, val);
}

dictEntry *daDictTwoPhaseUnlinkFind(dictarray *da, int didx, const void *key, dictEntry ***plink, int *table_index) {
    return dictTwoPhaseUnlinkFind(daGetDict(da, didx), key, plink, table_index);
}

void daDictTwoPhaseUnlinkFree(dictarray *da, int didx, dictEntry *he, dictEntry **plink, int table_index) {
    dictTwoPhaseUnlinkFree(daGetDict(da, didx), he, plink, table_index);
    daCumulativeKeyCountAdd(da, didx, -1);
}

int daDictDelete(dictarray *da, int didx, const void *key) {
    int ret = dictDelete(daGetDict(da, didx), key);
    if (ret == DICT_OK)
        daCumulativeKeyCountAdd(da, didx, -1);
    return ret;
}
