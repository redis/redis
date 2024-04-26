/*
 * Index-based KV store implementation
 * This file implements a KV store comprised of an array of dicts (see dict.c)
 * The purpose of this KV store is to have easy access to all keys that belong
 * in the same dict (i.e. are in the same dict-index)
 *
 * For example, when Redis is running in cluster mode, we use kvstore to save
 * all keys that map to the same hash-slot in a separate dict within the kvstore
 * struct.
 * This enables us to easily access all keys that map to a specific hash-slot.
 *
 * Copyright (c) 2011-Present, Redis Ltd. and contributors.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */
#include "fmacros.h"

#include <string.h>
#include <stddef.h>

#include "zmalloc.h"
#include "kvstore.h"
#include "redisassert.h"
#include "monotonic.h"

#define UNUSED(V) ((void) V)

struct _kvstore {
    int flags;
    dictType dtype;
    dict **dicts;
    long long num_dicts;
    long long num_dicts_bits;
    list *rehashing;                       /* List of dictionaries in this kvstore that are currently rehashing. */
    int resize_cursor;                     /* Cron job uses this cursor to gradually resize dictionaries (only used if num_dicts > 1). */
    int allocated_dicts;                   /* The number of allocated dicts. */
    int non_empty_dicts;                   /* The number of non-empty dicts. */
    unsigned long long key_count;          /* Total number of keys in this kvstore. */
    unsigned long long bucket_count;       /* Total number of buckets in this kvstore across dictionaries. */
    unsigned long long *dict_size_index;   /* Binary indexed tree (BIT) that describes cumulative key frequencies up until given dict-index. */
    size_t overhead_hashtable_lut;         /* The overhead of all dictionaries. */
    size_t overhead_hashtable_rehashing;   /* The overhead of dictionaries rehashing. */
};

/* Structure for kvstore iterator that allows iterating across multiple dicts. */
struct _kvstoreIterator {
    kvstore *kvs;
    long long didx;
    long long next_didx;
    dictIterator di;
};

/* Structure for kvstore dict iterator that allows iterating the corresponding dict. */
struct _kvstoreDictIterator {
    kvstore *kvs;
    long long didx;
    dictIterator di;
};

/* Dict metadata for database, used for record the position in rehashing list. */
typedef struct {
    listNode *rehashing_node;   /* list node in rehashing list */
} kvstoreDictMetadata;

/**********************************/
/*** Helpers **********************/
/**********************************/

/* Get the dictionary pointer based on dict-index. */
static dict *kvstoreGetDict(kvstore *kvs, int didx) {
    return kvs->dicts[didx];
}

static dict **kvstoreGetDictRef(kvstore *kvs, int didx) {
    return &kvs->dicts[didx];
}

static int kvstoreDictIsRehashingPaused(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    return d ? dictIsRehashingPaused(d) : 0;
}

/* Returns total (cumulative) number of keys up until given dict-index (inclusive).
 * Time complexity is O(log(kvs->num_dicts)). */
static unsigned long long cumulativeKeyCountRead(kvstore *kvs, int didx) {
    if (kvs->num_dicts == 1) {
        assert(didx == 0);
        return kvstoreSize(kvs);
    }
    int idx = didx + 1;
    unsigned long long sum = 0;
    while (idx > 0) {
        sum += kvs->dict_size_index[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

static void addDictIndexToCursor(kvstore *kvs, int didx, unsigned long long *cursor) {
    if (kvs->num_dicts == 1)
        return;
    /* didx can be -1 when iteration is over and there are no more dicts to visit. */
    if (didx < 0)
        return;
    *cursor = (*cursor << kvs->num_dicts_bits) | didx;
}

static int getAndClearDictIndexFromCursor(kvstore *kvs, unsigned long long *cursor) {
    if (kvs->num_dicts == 1)
        return 0;
    int didx = (int) (*cursor & (kvs->num_dicts-1));
    *cursor = *cursor >> kvs->num_dicts_bits;
    return didx;
}

/* Updates binary index tree (also known as Fenwick tree), increasing key count for a given dict.
 * You can read more about this data structure here https://en.wikipedia.org/wiki/Fenwick_tree
 * Time complexity is O(log(kvs->num_dicts)). */
static void cumulativeKeyCountAdd(kvstore *kvs, int didx, long delta) {
    kvs->key_count += delta;

    dict *d = kvstoreGetDict(kvs, didx);
    size_t dsize = dictSize(d);
    int non_empty_dicts_delta = dsize == 1? 1 : dsize == 0? -1 : 0;
    kvs->non_empty_dicts += non_empty_dicts_delta;

    /* BIT does not need to be calculated when there's only one dict. */
    if (kvs->num_dicts == 1)
        return;

    /* Update the BIT */
    int idx = didx + 1; /* Unlike dict indices, BIT is 1-based, so we need to add 1. */
    while (idx <= kvs->num_dicts) {
        if (delta < 0) {
            assert(kvs->dict_size_index[idx] >= (unsigned long long)labs(delta));
        }
        kvs->dict_size_index[idx] += delta;
        idx += (idx & -idx);
    }
}

/* Create the dict if it does not exist and return it. */
static dict *createDictIfNeeded(kvstore *kvs, int didx) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (d) return d;

    kvs->dicts[didx] = dictCreate(&kvs->dtype);
    kvs->allocated_dicts++;
    return kvs->dicts[didx];
}

/* Called when the dict will delete entries, the function will check
 * KVSTORE_FREE_EMPTY_DICTS to determine whether the empty dict needs
 * to be freed.
 *
 * Note that for rehashing dicts, that is, in the case of safe iterators
 * and Scan, we won't delete the dict. We will check whether it needs
 * to be deleted when we're releasing the iterator. */
static void freeDictIfNeeded(kvstore *kvs, int didx) {
    if (!(kvs->flags & KVSTORE_FREE_EMPTY_DICTS) ||
        !kvstoreGetDict(kvs, didx) ||
        kvstoreDictSize(kvs, didx) != 0 ||
        kvstoreDictIsRehashingPaused(kvs, didx))
        return;
    dictRelease(kvs->dicts[didx]);
    kvs->dicts[didx] = NULL;
    kvs->allocated_dicts--;
}

/**********************************/
/*** dict callbacks ***************/
/**********************************/

/* Adds dictionary to the rehashing list, which allows us
 * to quickly find rehash targets during incremental rehashing.
 *
 * If there are multiple dicts, updates the bucket count for the given dictionary
 * in a DB, bucket count incremented with the new ht size during the rehashing phase.
 * If there's one dict, bucket count can be retrieved directly from single dict bucket. */
static void kvstoreDictRehashingStarted(dict *d) {
    kvstore *kvs = d->type->userdata;
    kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
    listAddNodeTail(kvs->rehashing, d);
    metadata->rehashing_node = listLast(kvs->rehashing);

    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    kvs->bucket_count += to; /* Started rehashing (Add the new ht size) */
    kvs->overhead_hashtable_lut += to;
    kvs->overhead_hashtable_rehashing += from;
}

/* Remove dictionary from the rehashing list.
 *
 * Updates the bucket count for the given dictionary in a DB. It removes
 * the old ht size of the dictionary from the total sum of buckets for a DB.  */
static void kvstoreDictRehashingCompleted(dict *d) {
    kvstore *kvs = d->type->userdata;
    kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
    if (metadata->rehashing_node) {
        listDelNode(kvs->rehashing, metadata->rehashing_node);
        metadata->rehashing_node = NULL;
    }

    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    kvs->bucket_count -= from; /* Finished rehashing (Remove the old ht size) */
    kvs->overhead_hashtable_lut -= from;
    kvs->overhead_hashtable_rehashing -= from;
}

/* Returns the size of the DB dict metadata in bytes. */
static size_t kvstoreDictMetadataSize(dict *d) {
    UNUSED(d);
    return sizeof(kvstoreDictMetadata);
}

/**********************************/
/*** API **************************/
/**********************************/

/* Create an array of dictionaries
 * num_dicts_bits is the log2 of the amount of dictionaries needed (e.g. 0 for 1 dict,
 * 3 for 8 dicts, etc.) */
kvstore *kvstoreCreate(dictType *type, int num_dicts_bits, int flags) {
    /* We can't support more than 2^16 dicts because we want to save 48 bits
     * for the dict cursor, see kvstoreScan */
    assert(num_dicts_bits <= 16);

    kvstore *kvs = zcalloc(sizeof(*kvs));
    memcpy(&kvs->dtype, type, sizeof(kvs->dtype));
    kvs->flags = flags;

    /* kvstore must be the one to set these callbacks, so we make sure the
     * caller didn't do it */
    assert(!type->userdata);
    assert(!type->dictMetadataBytes);
    assert(!type->rehashingStarted);
    assert(!type->rehashingCompleted);
    kvs->dtype.userdata = kvs;
    kvs->dtype.dictMetadataBytes = kvstoreDictMetadataSize;
    kvs->dtype.rehashingStarted = kvstoreDictRehashingStarted;
    kvs->dtype.rehashingCompleted = kvstoreDictRehashingCompleted;

    kvs->num_dicts_bits = num_dicts_bits;
    kvs->num_dicts = 1 << kvs->num_dicts_bits;
    kvs->dicts = zcalloc(sizeof(dict*) * kvs->num_dicts);
    if (!(kvs->flags & KVSTORE_ALLOCATE_DICTS_ON_DEMAND)) {
        for (int i = 0; i < kvs->num_dicts; i++)
            createDictIfNeeded(kvs, i);
    }

    kvs->rehashing = listCreate();
    kvs->key_count = 0;
    kvs->non_empty_dicts = 0;
    kvs->resize_cursor = 0;
    kvs->dict_size_index = kvs->num_dicts > 1? zcalloc(sizeof(unsigned long long) * (kvs->num_dicts + 1)) : NULL;
    kvs->bucket_count = 0;
    kvs->overhead_hashtable_lut = 0;
    kvs->overhead_hashtable_rehashing = 0;

    return kvs;
}

void kvstoreEmpty(kvstore *kvs, void(callback)(dict*)) {
    for (int didx = 0; didx < kvs->num_dicts; didx++) {
        dict *d = kvstoreGetDict(kvs, didx);
        if (!d)
            continue;
        kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        dictEmpty(d, callback);
        freeDictIfNeeded(kvs, didx);
    }

    listEmpty(kvs->rehashing);

    kvs->key_count = 0;
    kvs->non_empty_dicts = 0;
    kvs->resize_cursor = 0;
    kvs->bucket_count = 0;
    if (kvs->dict_size_index)
        memset(kvs->dict_size_index, 0, sizeof(unsigned long long) * (kvs->num_dicts + 1));
    kvs->overhead_hashtable_lut = 0;
    kvs->overhead_hashtable_rehashing = 0;
}

void kvstoreRelease(kvstore *kvs) {
    for (int didx = 0; didx < kvs->num_dicts; didx++) {
        dict *d = kvstoreGetDict(kvs, didx);
        if (!d)
            continue;
        kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        dictRelease(d);
    }
    zfree(kvs->dicts);

    listRelease(kvs->rehashing);
    if (kvs->dict_size_index)
        zfree(kvs->dict_size_index);

    zfree(kvs);
}

unsigned long long int kvstoreSize(kvstore *kvs) {
    if (kvs->num_dicts != 1) {
        return kvs->key_count;
    } else {
        return kvs->dicts[0]? dictSize(kvs->dicts[0]) : 0;
    }
}

/* This method provides the cumulative sum of all the dictionary buckets
 * across dictionaries in a database. */
unsigned long kvstoreBuckets(kvstore *kvs) {
    if (kvs->num_dicts != 1) {
        return kvs->bucket_count;
    } else {
        return kvs->dicts[0]? dictBuckets(kvs->dicts[0]) : 0;
    }
}

size_t kvstoreMemUsage(kvstore *kvs) {
    size_t mem = sizeof(*kvs);

    unsigned long long keys_count = kvstoreSize(kvs);
    mem += keys_count * dictEntryMemUsage() +
           kvstoreBuckets(kvs) * sizeof(dictEntry*) +
           kvs->allocated_dicts * (sizeof(dict) + kvstoreDictMetadataSize(NULL));

    /* Values are dict* shared with kvs->dicts */
    mem += listLength(kvs->rehashing) * sizeof(listNode);

    if (kvs->dict_size_index)
        mem += sizeof(unsigned long long) * (kvs->num_dicts + 1);

    return mem;
}

/*
 * This method is used to iterate over the elements of the entire kvstore specifically across dicts.
 * It's a three pronged approach.
 *
 * 1. It uses the provided cursor `cursor` to retrieve the dict index from it.
 * 2. If the dictionary is in a valid state checked through the provided callback `dictScanValidFunction`,
 *    it performs a dictScan over the appropriate `keyType` dictionary of `db`.
 * 3. If the dict is entirely scanned i.e. the cursor has reached 0, the next non empty dict is discovered.
 *    The dict information is embedded into the cursor and returned.
 *
 * To restrict the scan to a single dict, pass a valid dict index as
 * 'onlydidx', otherwise pass -1.
 */
unsigned long long kvstoreScan(kvstore *kvs, unsigned long long cursor,
                               int onlydidx, dictScanFunction *scan_cb,
                               kvstoreScanShouldSkipDict *skip_cb,
                               void *privdata)
{
    unsigned long long _cursor = 0;
    /* During dictionary traversal, 48 upper bits in the cursor are used for positioning in the HT.
     * Following lower bits are used for the dict index number, ranging from 0 to 2^num_dicts_bits-1.
     * Dict index is always 0 at the start of iteration and can be incremented only if there are
     * multiple dicts. */
    int didx = getAndClearDictIndexFromCursor(kvs, &cursor);
    if (onlydidx >= 0) {
        if (didx < onlydidx) {
            /* Fast-forward to onlydidx. */
            assert(onlydidx < kvs->num_dicts);
            didx = onlydidx;
            cursor = 0;
        } else if (didx > onlydidx) {
            /* The cursor is already past onlydidx. */
            return 0;
        }
    }

    dict *d = kvstoreGetDict(kvs, didx);

    int skip = !d || (skip_cb && skip_cb(d));
    if (!skip) {
        _cursor = dictScan(d, cursor, scan_cb, privdata);
        /* In dictScan, scan_cb may delete entries (e.g., in active expire case). */
        freeDictIfNeeded(kvs, didx);
    }
    /* scanning done for the current dictionary or if the scanning wasn't possible, move to the next dict index. */
    if (_cursor == 0 || skip) {
        if (onlydidx >= 0)
            return 0;
        didx = kvstoreGetNextNonEmptyDictIndex(kvs, didx);
    }
    if (didx == -1) {
        return 0;
    }
    addDictIndexToCursor(kvs, didx, &_cursor);
    return _cursor;
}

/*
 * This functions increases size of kvstore to match desired number.
 * It resizes all individual dictionaries, unless skip_cb indicates otherwise.
 *
 * Based on the parameter `try_expand`, appropriate dict expand API is invoked.
 * if try_expand is set to 1, `dictTryExpand` is used else `dictExpand`.
 * The return code is either `DICT_OK`/`DICT_ERR` for both the API(s).
 * `DICT_OK` response is for successful expansion. However, `DICT_ERR` response signifies failure in allocation in
 * `dictTryExpand` call and in case of `dictExpand` call it signifies no expansion was performed.
 */
int kvstoreExpand(kvstore *kvs, uint64_t newsize, int try_expand, kvstoreExpandShouldSkipDictIndex *skip_cb) {
    for (int i = 0; i < kvs->num_dicts; i++) {
        dict *d = kvstoreGetDict(kvs, i);
        if (!d || (skip_cb && skip_cb(i)))
            continue;
        int result = try_expand ? dictTryExpand(d, newsize) : dictExpand(d, newsize);
        if (try_expand && result == DICT_ERR)
            return 0;
    }

    return 1;
}

/* Returns fair random dict index, probability of each dict being returned is proportional to the number of elements that dictionary holds.
 * This function guarantees that it returns a dict-index of a non-empty dict, unless the entire kvstore is empty.
 * Time complexity of this function is O(log(kvs->num_dicts)). */
int kvstoreGetFairRandomDictIndex(kvstore *kvs) {
    unsigned long target = kvstoreSize(kvs) ? (randomULong() % kvstoreSize(kvs)) + 1 : 0;
    return kvstoreFindDictIndexByKeyIndex(kvs, target);
}

void kvstoreGetStats(kvstore *kvs, char *buf, size_t bufsize, int full) {
    buf[0] = '\0';

    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;
    dictStats *mainHtStats = NULL;
    dictStats *rehashHtStats = NULL;
    dict *d;
    kvstoreIterator *kvs_it = kvstoreIteratorInit(kvs);
    while ((d = kvstoreIteratorNextDict(kvs_it))) {
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
    kvstoreIteratorRelease(kvs_it);

    if (mainHtStats && bufsize > 0) {
        l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
        dictFreeStats(mainHtStats);
        buf += l;
        bufsize -= l;
    }

    if (rehashHtStats && bufsize > 0) {
        l = dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
        buf += l;
        bufsize -= l;
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
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
 * The return value is 0 based dict-index, and the range of the target is [1..kvstoreSize], kvstoreSize inclusive.
 *
 * To find the dict, we start with the root node of the binary index tree and search through its children
 * from the highest index (2^num_dicts_bits in our case) to the lowest index. At each node, we check if the target
 * value is greater than the node's value. If it is, we remove the node's value from the target and recursively
 * search for the new target using the current node as the parent.
 * Time complexity of this function is O(log(kvs->num_dicts))
 */
int kvstoreFindDictIndexByKeyIndex(kvstore *kvs, unsigned long target) {
    if (kvs->num_dicts == 1 || kvstoreSize(kvs) == 0)
        return 0;
    assert(target <= kvstoreSize(kvs));

    int result = 0, bit_mask = 1 << kvs->num_dicts_bits;
    for (int i = bit_mask; i != 0; i >>= 1) {
        int current = result + i;
        /* When the target index is greater than 'current' node value the we will update
         * the target and search in the 'current' node tree. */
        if (target > kvs->dict_size_index[current]) {
            target -= kvs->dict_size_index[current];
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

/* Wrapper for kvstoreFindDictIndexByKeyIndex to get the first non-empty dict index in the kvstore. */
int kvstoreGetFirstNonEmptyDictIndex(kvstore *kvs) {
    return kvstoreFindDictIndexByKeyIndex(kvs, 1);
}

/* Returns next non-empty dict index strictly after given one, or -1 if provided didx is the last one. */
int kvstoreGetNextNonEmptyDictIndex(kvstore *kvs, int didx) {
    if (kvs->num_dicts == 1) {
        assert(didx == 0);
        return -1;
    }
    unsigned long long next_key = cumulativeKeyCountRead(kvs, didx) + 1;
    return next_key <= kvstoreSize(kvs) ? kvstoreFindDictIndexByKeyIndex(kvs, next_key) : -1;
}

int kvstoreNumNonEmptyDicts(kvstore *kvs) {
    return kvs->non_empty_dicts;
}

int kvstoreNumAllocatedDicts(kvstore *kvs) {
    return kvs->allocated_dicts;
}

int kvstoreNumDicts(kvstore *kvs) {
    return kvs->num_dicts;
}

/* Returns kvstore iterator that can be used to iterate through sub-dictionaries.
 *
 * The caller should free the resulting kvs_it with kvstoreIteratorRelease. */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs) {
    kvstoreIterator *kvs_it = zmalloc(sizeof(*kvs_it));
    kvs_it->kvs = kvs;
    kvs_it->didx = -1;
    kvs_it->next_didx = kvstoreGetFirstNonEmptyDictIndex(kvs_it->kvs); /* Finds first non-empty dict index. */
    dictInitSafeIterator(&kvs_it->di, NULL);
    return kvs_it;
}

/* Free the kvs_it returned by kvstoreIteratorInit. */
void kvstoreIteratorRelease(kvstoreIterator *kvs_it) {
    dictIterator *iter = &kvs_it->di;
    dictResetIterator(iter);
    /* In the safe iterator context, we may delete entries. */
    freeDictIfNeeded(kvs_it->kvs, kvs_it->didx);
    zfree(kvs_it);
}


/* Returns next dictionary from the iterator, or NULL if iteration is complete.
 *
 * - Takes care to reset the iter of the previous dict before moved to the next dict.
 */
dict *kvstoreIteratorNextDict(kvstoreIterator *kvs_it) {
    if (kvs_it->next_didx == -1)
        return NULL;

    /* The dict may be deleted during the iteration process, so here need to check for NULL. */
    if (kvs_it->didx != -1 && kvstoreGetDict(kvs_it->kvs, kvs_it->didx)) {
        /* Before we move to the next dict, reset the iter of the previous dict. */
        dictIterator *iter = &kvs_it->di;
        dictResetIterator(iter);
        /* In the safe iterator context, we may delete entries. */
        freeDictIfNeeded(kvs_it->kvs, kvs_it->didx);
    }

    kvs_it->didx = kvs_it->next_didx;
    kvs_it->next_didx = kvstoreGetNextNonEmptyDictIndex(kvs_it->kvs, kvs_it->didx);
    return kvs_it->kvs->dicts[kvs_it->didx];
}

int kvstoreIteratorGetCurrentDictIndex(kvstoreIterator *kvs_it) {
    assert(kvs_it->didx >= 0 && kvs_it->didx < kvs_it->kvs->num_dicts);
    return kvs_it->didx;
}

/* Returns next entry. */
dictEntry *kvstoreIteratorNext(kvstoreIterator *kvs_it) {
    dictEntry *de = kvs_it->di.d ? dictNext(&kvs_it->di) : NULL;
    if (!de) { /* No current dict or reached the end of the dictionary. */

        /* Before we move to the next dict, function kvstoreIteratorNextDict()
         * reset the iter of the previous dict & freeDictIfNeeded(). */
        dict *d = kvstoreIteratorNextDict(kvs_it);

        if (!d)
            return NULL;

        dictInitSafeIterator(&kvs_it->di, d);
        de = dictNext(&kvs_it->di);
    }
    return de;
}

/* This method traverses through kvstore dictionaries and triggers a resize.
 * It first tries to shrink if needed, and if it isn't, it tries to expand. */
void kvstoreTryResizeDicts(kvstore *kvs, int limit) {
    if (limit > kvs->num_dicts)
        limit = kvs->num_dicts;

    for (int i = 0; i < limit; i++) {
        int didx = kvs->resize_cursor;
        dict *d = kvstoreGetDict(kvs, didx);
        if (d && dictShrinkIfNeeded(d) == DICT_ERR) {
            dictExpandIfNeeded(d);
        }
        kvs->resize_cursor = (didx + 1) % kvs->num_dicts;
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use threshold_us
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns the amount of microsecs spent if some rehashing was
 * performed, otherwise 0 is returned. */
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us) {
    if (listLength(kvs->rehashing) == 0)
        return 0;

    /* Our goal is to rehash as many dictionaries as we can before reaching threshold_us,
     * after each dictionary completes rehashing, it removes itself from the list. */
    listNode *node;
    monotime timer;
    uint64_t elapsed_us = 0;
    elapsedStart(&timer);
    while ((node = listFirst(kvs->rehashing))) {
        dictRehashMicroseconds(listNodeValue(node), threshold_us - elapsed_us);

        elapsed_us = elapsedUs(timer);
        if (elapsed_us >= threshold_us) {
            break;  /* Reached the time limit. */
        }
    }
    return elapsed_us;
}

size_t kvstoreOverheadHashtableLut(kvstore *kvs) {
    return kvs->overhead_hashtable_lut * sizeof(dictEntry *);
}

size_t kvstoreOverheadHashtableRehashing(kvstore *kvs) {
    return kvs->overhead_hashtable_rehashing * sizeof(dictEntry *);
}

unsigned long kvstoreDictRehashingCount(kvstore *kvs) {
    return listLength(kvs->rehashing);
}

unsigned long kvstoreDictSize(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return 0;
    return dictSize(d);
}

kvstoreDictIterator *kvstoreGetDictIterator(kvstore *kvs, int didx)
{
    kvstoreDictIterator *kvs_di = zmalloc(sizeof(*kvs_di));
    kvs_di->kvs = kvs;
    kvs_di->didx = didx;
    dictInitIterator(&kvs_di->di, kvstoreGetDict(kvs, didx));
    return kvs_di;
}

kvstoreDictIterator *kvstoreGetDictSafeIterator(kvstore *kvs, int didx)
{
    kvstoreDictIterator *kvs_di = zmalloc(sizeof(*kvs_di));
    kvs_di->kvs = kvs;
    kvs_di->didx = didx;
    dictInitSafeIterator(&kvs_di->di, kvstoreGetDict(kvs, didx));
    return kvs_di;
}

/* Free the kvs_di returned by kvstoreGetDictIterator and kvstoreGetDictSafeIterator. */
void kvstoreReleaseDictIterator(kvstoreDictIterator *kvs_di)
{
    /* The dict may be deleted during the iteration process, so here need to check for NULL. */
    if (kvstoreGetDict(kvs_di->kvs, kvs_di->didx)) {
        dictResetIterator(&kvs_di->di);
        /* In the safe iterator context, we may delete entries. */
        freeDictIfNeeded(kvs_di->kvs, kvs_di->didx);
    }

    zfree(kvs_di);
}

/* Get the next element of the dict through kvstoreDictIterator and dictNext. */
dictEntry *kvstoreDictIteratorNext(kvstoreDictIterator *kvs_di)
{
    /* The dict may be deleted during the iteration process, so here need to check for NULL. */
    dict *d = kvstoreGetDict(kvs_di->kvs, kvs_di->didx);
    if (!d) return NULL;

    return dictNext(&kvs_di->di);
}

dictEntry *kvstoreDictGetRandomKey(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictGetRandomKey(d);
}

dictEntry *kvstoreDictGetFairRandomKey(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictGetFairRandomKey(d);
}

dictEntry *kvstoreDictFindEntryByPtrAndHash(kvstore *kvs, int didx, const void *oldptr, uint64_t hash)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictFindEntryByPtrAndHash(d, oldptr, hash);
}

unsigned int kvstoreDictGetSomeKeys(kvstore *kvs, int didx, dictEntry **des, unsigned int count)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return 0;
    return dictGetSomeKeys(d, des, count);
}

int kvstoreDictExpand(kvstore *kvs, int didx, unsigned long size)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return DICT_ERR;
    return dictExpand(d, size);
}

unsigned long kvstoreDictScanDefrag(kvstore *kvs, int didx, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return 0;
    return dictScanDefrag(d, v, fn, defragfns, privdata);
}

/* Unlike kvstoreDictScanDefrag(), this method doesn't defrag the data(keys and values)
 * within dict, it only reallocates the memory used by the dict structure itself using 
 * the provided allocation function. This feature was added for the active defrag feature.
 *
 * The 'defragfn' callback is called with a reference to the dict
 * that callback can reallocate. */
void kvstoreDictLUTDefrag(kvstore *kvs, kvstoreDictLUTDefragFunction *defragfn) {
    for (int didx = 0; didx < kvs->num_dicts; didx++) {
        dict **d = kvstoreGetDictRef(kvs, didx), *newd;
        if (!*d)
            continue;
        if ((newd = defragfn(*d))) {
            *d = newd;

            /* After defragmenting the dict, update its corresponding
             * rehashing node in the kvstore's rehashing list. */
            kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(*d);
            if (metadata->rehashing_node)
                metadata->rehashing_node->value = *d;
        }
    }
}

uint64_t kvstoreGetHash(kvstore *kvs, const void *key)
{
    return kvs->dtype.hashFunction(key);
}

void *kvstoreDictFetchValue(kvstore *kvs, int didx, const void *key)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictFetchValue(d, key);
}

dictEntry *kvstoreDictFind(kvstore *kvs, int didx, void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictFind(d, key);
}

dictEntry *kvstoreDictAddRaw(kvstore *kvs, int didx, void *key, dictEntry **existing) {
    dict *d = createDictIfNeeded(kvs, didx);
    dictEntry *ret = dictAddRaw(d, key, existing);
    if (ret)
        cumulativeKeyCountAdd(kvs, didx, 1);
    return ret;
}

void kvstoreDictSetKey(kvstore *kvs, int didx, dictEntry* de, void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    dictSetKey(d, de, key);
}

void kvstoreDictSetVal(kvstore *kvs, int didx, dictEntry *de, void *val) {
    dict *d = kvstoreGetDict(kvs, didx);
    dictSetVal(d, de, val);
}

dictEntry *kvstoreDictTwoPhaseUnlinkFind(kvstore *kvs, int didx, const void *key, dictEntry ***plink, int *table_index) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictTwoPhaseUnlinkFind(kvstoreGetDict(kvs, didx), key, plink, table_index);
}

void kvstoreDictTwoPhaseUnlinkFree(kvstore *kvs, int didx, dictEntry *he, dictEntry **plink, int table_index) {
    dict *d = kvstoreGetDict(kvs, didx);
    dictTwoPhaseUnlinkFree(d, he, plink, table_index);
    cumulativeKeyCountAdd(kvs, didx, -1);
    freeDictIfNeeded(kvs, didx);
}

int kvstoreDictDelete(kvstore *kvs, int didx, const void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return DICT_ERR;
    int ret = dictDelete(d, key);
    if (ret == DICT_OK) {
        cumulativeKeyCountAdd(kvs, didx, -1);
        freeDictIfNeeded(kvs, didx);
    }
    return ret;
}

#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

#define TEST(name) printf("test — %s\n", name);

uint64_t hashTestCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

void freeTestCallback(dict *d, void *val) {
    UNUSED(d);
    zfree(val);
}

void* defragAllocTest(void *ptr) {
    size_t size = zmalloc_size(ptr);
    void *newptr = zmalloc(size);
    memcpy(newptr, ptr, size);
    zfree(ptr);
    return newptr;
}

dict *defragLUTTestCallback(dict *d) {
    /* handle the dict struct */
    d = defragAllocTest(d);
    /* handle the first hash table */
    d->ht_table[0] = defragAllocTest(d->ht_table[0]);
    /* handle the second hash table */
    if (d->ht_table[1])
        d->ht_table[1] = defragAllocTest(d->ht_table[1]);
    return d; 
}

dictType KvstoreDictTestType = {
    hashTestCallback,
    NULL,
    NULL,
    NULL,
    freeTestCallback,
    NULL,
    NULL
};

char *stringFromInt(int value) {
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf, sizeof(buf), "%d",value);
    s = zmalloc(len+1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

/* ./redis-server test kvstore */
int kvstoreTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    void *key;
    dictEntry *de;
    kvstoreIterator *kvs_it;
    kvstoreDictIterator *kvs_di;

    int didx = 0;
    int curr_slot = 0;
    kvstore *kvs1 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    kvstore *kvs2 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_FREE_EMPTY_DICTS);

    TEST("Add 16 keys") {
        for (i = 0; i < 16; i++) {
            de = kvstoreDictAddRaw(kvs1, didx, stringFromInt(i), NULL);
            assert(de != NULL);
            de = kvstoreDictAddRaw(kvs2, didx, stringFromInt(i), NULL);
            assert(de != NULL);
        }
        assert(kvstoreDictSize(kvs1, didx) == 16);
        assert(kvstoreSize(kvs1) == 16);
        assert(kvstoreDictSize(kvs2, didx) == 16);
        assert(kvstoreSize(kvs2) == 16);
    }

    TEST("kvstoreIterator case 1: removing all keys does not delete the empty dict") {
        kvs_it = kvstoreIteratorInit(kvs1);
        while((de = kvstoreIteratorNext(kvs_it)) != NULL) {
            curr_slot = kvstoreIteratorGetCurrentDictIndex(kvs_it);
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs1, curr_slot, key) == DICT_OK);
        }
        kvstoreIteratorRelease(kvs_it);

        dict *d = kvstoreGetDict(kvs1, didx);
        assert(d != NULL);
        assert(kvstoreDictSize(kvs1, didx) == 0);
        assert(kvstoreSize(kvs1) == 0);
    }

    TEST("kvstoreIterator case 2: removing all keys will delete the empty dict") {
        kvs_it = kvstoreIteratorInit(kvs2);
        while((de = kvstoreIteratorNext(kvs_it)) != NULL) {
            curr_slot = kvstoreIteratorGetCurrentDictIndex(kvs_it);
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs2, curr_slot, key) == DICT_OK);
        }
        kvstoreIteratorRelease(kvs_it);

        /* Make sure the dict was removed from the rehashing list. */
        while (kvstoreIncrementallyRehash(kvs2, 1000)) {}

        dict *d = kvstoreGetDict(kvs2, didx);
        assert(d == NULL);
        assert(kvstoreDictSize(kvs2, didx) == 0);
        assert(kvstoreSize(kvs2) == 0);
    }

    TEST("Add 16 keys again") {
        for (i = 0; i < 16; i++) {
            de = kvstoreDictAddRaw(kvs1, didx, stringFromInt(i), NULL);
            assert(de != NULL);
            de = kvstoreDictAddRaw(kvs2, didx, stringFromInt(i), NULL);
            assert(de != NULL);
        }
        assert(kvstoreDictSize(kvs1, didx) == 16);
        assert(kvstoreSize(kvs1) == 16);
        assert(kvstoreDictSize(kvs2, didx) == 16);
        assert(kvstoreSize(kvs2) == 16);
    }

    TEST("kvstoreDictIterator case 1: removing all keys does not delete the empty dict") {
        kvs_di = kvstoreGetDictSafeIterator(kvs1, didx);
        while((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs1, didx, key) == DICT_OK);
        }
        kvstoreReleaseDictIterator(kvs_di);

        dict *d = kvstoreGetDict(kvs1, didx);
        assert(d != NULL);
        assert(kvstoreDictSize(kvs1, didx) == 0);
        assert(kvstoreSize(kvs1) == 0);
    }

    TEST("kvstoreDictIterator case 2: removing all keys will delete the empty dict") {
        kvs_di = kvstoreGetDictSafeIterator(kvs2, didx);
        while((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs2, didx, key) == DICT_OK);
        }
        kvstoreReleaseDictIterator(kvs_di);

        dict *d = kvstoreGetDict(kvs2, didx);
        assert(d == NULL);
        assert(kvstoreDictSize(kvs2, didx) == 0);
        assert(kvstoreSize(kvs2) == 0);
    }

    TEST("Verify that a rehashing dict's node in the rehashing list is correctly updated after defragmentation") {
        kvstore *kvs = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
        for (i = 0; i < 256; i++) {
            de = kvstoreDictAddRaw(kvs, 0, stringFromInt(i), NULL);
            if (listLength(kvs->rehashing)) break;
        }
        assert(listLength(kvs->rehashing));
        kvstoreDictLUTDefrag(kvs, defragLUTTestCallback);
        while (kvstoreIncrementallyRehash(kvs, 1000)) {}
        kvstoreRelease(kvs);
    }

    kvstoreRelease(kvs1);
    kvstoreRelease(kvs2);
    return 0;
}
#endif
