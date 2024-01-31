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
 * Copyright (c) Redis contributors.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "fmacros.h"

#include <string.h>
#include <stddef.h>

#include "zmalloc.h"
#include "kvstore.h"
#include "redisassert.h"
#include "monotonic.h"

#define UNUSED(V) ((void) V)

/**********************************/
/*** Helpers **********************/
/**********************************/

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
    kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
    listAddNodeTail(metadata->kvs->rehashing, d);
    metadata->rehashing_node = listLast(metadata->kvs->rehashing);

    if (metadata->kvs->num_dicts == 1)
        return;
    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    metadata->kvs->bucket_count += to; /* Started rehashing (Add the new ht size) */
}

/* Remove dictionary from the rehashing list.
 *
 * Updates the bucket count for the given dictionary in a DB. It removes
 * the old ht size of the dictionary from the total sum of buckets for a DB.  */
static void kvstoreDictRehashingCompleted(dict *d) {
    kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
    if (metadata->rehashing_node) {
        listDelNode(metadata->kvs->rehashing, metadata->rehashing_node);
        metadata->rehashing_node = NULL;
    }

    if (metadata->kvs->num_dicts == 1)
        return;
    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    metadata->kvs->bucket_count -= from; /* Finished rehashing (Remove the old ht size) */
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
kvstore *kvstoreCreate(dictType *type, int num_dicts_bits) {
    /* We can't support more than 2^16 dicts because we want to save 48 bits
     * for the dict cursor, see kvstoreScan */
    assert(num_dicts_bits <= 16);

    kvstore *kvs = zcalloc(sizeof(*kvs));
    memcpy(&kvs->dtype, type, sizeof(kvs->dtype));

    /* kvstore must be the one to set this callbacks, so we make sure the
     * caller didn't do it */
    assert(!type->dictMetadataBytes);
    assert(!type->rehashingStarted);
    assert(!type->rehashingCompleted);
    kvs->dtype.dictMetadataBytes = kvstoreDictMetadataSize;
    kvs->dtype.rehashingStarted = kvstoreDictRehashingStarted;
    kvs->dtype.rehashingCompleted = kvstoreDictRehashingCompleted;

    kvs->num_dicts_bits = num_dicts_bits;
    kvs->num_dicts = 1 << kvs->num_dicts_bits;
    kvs->dicts = zmalloc(sizeof(dict*) * kvs->num_dicts);
    for (int i = 0; i < kvs->num_dicts; i++) {
        kvs->dicts[i] = dictCreate(&kvs->dtype);
        kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(kvs->dicts[i]);
        metadata->kvs = kvs;
    }

    kvs->rehashing = listCreate();
    kvs->key_count = 0;
    kvs->non_empty_dicts = 0;
    kvs->resize_cursor = 0;
    kvs->dict_size_index = kvs->num_dicts > 1? zcalloc(sizeof(unsigned long long) * (kvs->num_dicts + 1)) : NULL;
    kvs->bucket_count = 0;

    return kvs;
}

void kvstoreEmpty(kvstore *kvs, void(callback)(dict*)) {
    for (int didx = 0; didx < kvs->num_dicts; didx++) {
        dict *d = kvstoreGetDict(kvs, didx);
        kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        dictEmpty(d, callback);
    }

    listEmpty(kvs->rehashing);

    kvs->key_count = 0;
    kvs->non_empty_dicts = 0;
    kvs->resize_cursor = 0;
    kvs->bucket_count = 0;
    if (kvs->dict_size_index)
        memset(kvs->dict_size_index, 0, sizeof(unsigned long long) * (kvs->num_dicts + 1));
}

void kvstoreRelease(kvstore *kvs) {
    for (int didx = 0; didx < kvs->num_dicts; didx++) {
        dict *d = kvstoreGetDict(kvs, didx);
        kvstoreDictMetadata *metadata = (kvstoreDictMetadata *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        dictRelease(kvstoreGetDict(kvs, didx));
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
        return dictSize(kvs->dicts[0]);
    }
}

/* This method provides the cumulative sum of all the dictionary buckets
 * across dictionaries in a database. */
unsigned long kvstoreBuckets(kvstore *kvs) {
    if (kvs->num_dicts != 1) {
        return kvs->bucket_count;
    } else {
        return dictBuckets(kvs->dicts[0]);
    }
}

size_t kvstoreMemUsage(kvstore *kvs) {
    size_t mem = sizeof(*kvs);

    unsigned long long keys_count = kvstoreSize(kvs);
    mem += keys_count * dictEntryMemUsage() +
           kvstoreBuckets(kvs) * sizeof(dictEntry*) +
           kvs->num_dicts * (sizeof(dict) + dictMetadataSize(kvstoreGetDict(kvs, 0)));

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

    int skip = skip_cb && skip_cb(d);
    if (!skip) {
        _cursor = dictScan(d, cursor, scan_cb, privdata);
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
 * `DICT_OK` response is for successful expansion. However ,`DICT_ERR` response signifies failure in allocation in
 * `dictTryExpand` call and in case of `dictExpand` call it signifies no expansion was performed.
 */
int kvstoreExpand(kvstore *kvs, uint64_t newsize, int try_expand, kvstoreExpandShouldSkipDictIndex *skip_cb) {
    for (int i = 0; i < kvs->num_dicts; i++) {
        if (skip_cb && skip_cb(i))
            continue;
        dict *d = kvstoreGetDict(kvs, i);
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

dict *kvstoreGetDict(kvstore *kvs, int didx) {
    return kvs->dicts[didx];
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

/* Returns next non-empty dict index strictly after given one, or -1 if provided didx is the last one. */
int kvstoreGetNextNonEmptyDictIndex(kvstore *kvs, int didx) {
    unsigned long long next_key = cumulativeKeyCountRead(kvs, didx) + 1;
    return next_key <= kvstoreSize(kvs) ? kvstoreFindDictIndexByKeyIndex(kvs, next_key) : -1;
}

int kvstoreNonEmptyDicts(kvstore *kvs) {
    return kvs->non_empty_dicts;
}

/* Returns kvstore iterator that can be used to iterate through sub-dictionaries.
 *
 * The caller should free the resulting kvs_it with kvstoreIteratorRelease. */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs) {
    kvstoreIterator *kvs_it = zmalloc(sizeof(*kvs_it));
    kvs_it->kvs = kvs;
    kvs_it->didx = -1;
    kvs_it->next_didx = kvstoreFindDictIndexByKeyIndex(kvs_it->kvs, 1); /* Finds first non-empty dict index. */
    dictInitSafeIterator(&kvs_it->di, NULL);
    return kvs_it;
}

/* Free the dbit returned by dbIteratorInit. */
void kvstoreIteratorRelease(kvstoreIterator *kvs_it) {
    dictIterator *iter = &kvs_it->di;
    dictResetIterator(iter);

    zfree(kvs_it);
}

/* Returns next dictionary from the iterator, or NULL if iteration is complete. */
dict *kvstoreIteratorNextDict(kvstoreIterator *kvs_it) {
    if (kvs_it->next_didx == -1)
        return NULL;
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
        dict *d = kvstoreIteratorNextDict(kvs_it);
        if (!d)
            return NULL;
        if (kvs_it->di.d) {
            /* Before we move to the next dict, reset the iter of the previous dict. */
            dictIterator *iter = &kvs_it->di;
            dictResetIterator(iter);
        }
        dictInitSafeIterator(&kvs_it->di, d);
        de = dictNext(&kvs_it->di);
    }
    return de;
}

/* This method traverses through kvstore dictionaries and triggers a resize .
 * It first tries to shrink if needed, and if it isn't, it tries to expand. */
void kvstoreTryResizeDicts(kvstore *kvs, int limit) {
    if (limit > kvs->num_dicts)
        limit = kvs->num_dicts;

    for (int i = 0; i < limit; i++) {
        int didx = kvs->resize_cursor;
        dict *d = kvstoreGetDict(kvs, didx);
        if (dictShrinkIfNeeded(d) == DICT_ERR) {
            dictExpandIfNeeded(d);
        }
        kvs->resize_cursor = (didx + 1) % kvs->num_dicts;
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns the amount of microsecs spent if some rehashing was
 * performed, otherwise 0 is returned. */
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us) {
    if (listLength(kvs->rehashing) == 0)
        return 0;

    /* Our goal is to rehash as many dictionaries as we can before reaching predefined threshold,
     * after each dictionary completes rehashing, it removes itself from the list. */
    listNode *node;
    monotime timer;
    uint64_t elapsed_us = UINT64_MAX;
    elapsedStart(&timer);
    while ((node = listFirst(kvs->rehashing))) {
        elapsed_us = elapsedUs(timer);
        if (elapsed_us >= threshold_us) {
            break;  /* Reached the time limit. */
        }
        dictRehashMicroseconds(listNodeValue(node), threshold_us - elapsed_us);
    }
    assert(elapsed_us != UINT64_MAX);
    return elapsed_us;
}

dictEntry *kvstoreDictFind(kvstore *kvs, int didx, void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    return dictFind(d, key);
}

dictEntry *kvstoreDictAddRaw(kvstore *kvs, int didx, void *key, dictEntry **existing) {
    dictEntry *ret = dictAddRaw(kvstoreGetDict(kvs, didx), key, existing);
    if (ret)
        cumulativeKeyCountAdd(kvs, didx, 1);
    return ret;
}

void kvstoreDictSetKey(kvstore *kvs, int didx, dictEntry* de, void *key) {
    dictSetKey(kvstoreGetDict(kvs, didx), de, key);
}

void kvstoreDictSetVal(kvstore *kvs, int didx, dictEntry *de, void *val) {
    dictSetVal(kvstoreGetDict(kvs, didx), de, val);
}

dictEntry *kvstoreDictTwoPhaseUnlinkFind(kvstore *kvs, int didx, const void *key, dictEntry ***plink, int *table_index) {
    return dictTwoPhaseUnlinkFind(kvstoreGetDict(kvs, didx), key, plink, table_index);
}

void kvstoreDictTwoPhaseUnlinkFree(kvstore *kvs, int didx, dictEntry *he, dictEntry **plink, int table_index) {
    dictTwoPhaseUnlinkFree(kvstoreGetDict(kvs, didx), he, plink, table_index);
    cumulativeKeyCountAdd(kvs, didx, -1);
}

int kvstoreDictDelete(kvstore *kvs, int didx, const void *key) {
    int ret = dictDelete(kvstoreGetDict(kvs, didx), key);
    if (ret == DICT_OK)
        cumulativeKeyCountAdd(kvs, didx, -1);
    return ret;
}
