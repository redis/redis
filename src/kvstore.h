/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * KVSTORE
 * -------
 * Index-based KV store implementation. This file implements a KV store comprised
 * of an array of dicts (see dict.c) The purpose of this KV store is to have easy
 * access to all keys that belong in the same dict (i.e. are in the same dict-index)
 *
 * For example, when Redis is running in cluster mode, we use kvstore to save
 * all keys that map to the same hash-slot in a separate dict within the kvstore
 * struct.
 * This enables us to easily access all keys that map to a specific hash-slot.
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#ifndef DICTARRAY_H_
#define DICTARRAY_H_

#include "dict.h"
#include "adlist.h"

/* maximum number of bins of keysizes histogram */
#define MAX_KEYSIZES_BINS 60
#define MAX_KEYSIZES_TYPES 5 /* static_assert at db.c verifies == OBJ_TYPE_BASIC_MAX */

/* When creating kvstore with flag `KVSTORE_ALLOC_META_KEYS_HIST`, then kvstore 
 * alloc and memset struct kvstoreMetadata on init, yet, managed outside kvstore */
typedef struct {
    int64_t keysizes_hist[MAX_KEYSIZES_TYPES][MAX_KEYSIZES_BINS];
} kvstoreMetadata;

/* Like kvstoreMetadata, this one per dict */
typedef struct {
    size_t alloc_size; /* total memory used (in bytes) by this dict */
    int64_t keysizes_hist[MAX_KEYSIZES_TYPES][MAX_KEYSIZES_BINS];
} kvstoreDictMetadata;

typedef struct _kvstore kvstore;
typedef struct _kvstoreIterator kvstoreIterator;
typedef struct _kvstoreDictIterator kvstoreDictIterator;

typedef int (kvstoreScanShouldSkipDict)(dict *d, int didx);
typedef int (kvstoreExpandShouldSkipDictIndex)(int didx);
typedef int (kvstoreRandomShouldSkipDictIndex)(int didx);

#define KVSTORE_ALLOCATE_DICTS_ON_DEMAND (1<<0)
#define KVSTORE_FREE_EMPTY_DICTS (1<<1)
#define KVSTORE_ALLOC_META_KEYS_HIST (1<<2) /* Alloc keysizes histogram */
kvstore *kvstoreCreate(dictType *type, int num_dicts_bits, int flags);
void kvstoreEmpty(kvstore *kvs, void(callback)(dict*));
void kvstoreRelease(kvstore *kvs);
unsigned long long kvstoreSize(kvstore *kvs);
unsigned long kvstoreBuckets(kvstore *kvs);
size_t kvstoreMemUsage(kvstore *kvs);
unsigned long long kvstoreScan(kvstore *kvs, unsigned long long cursor,
                               int onlydidx, dictScanFunction *scan_cb,
                               kvstoreScanShouldSkipDict *skip_cb,
                               void *privdata);
int kvstoreExpand(kvstore *kvs, uint64_t newsize, int try_expand, kvstoreExpandShouldSkipDictIndex *skip_cb);
int kvstoreGetFairRandomDictIndex(kvstore *kvs, kvstoreExpandShouldSkipDictIndex *skip_cb,
                                  int fair_attempts, int slow_fallback);
void kvstoreGetStats(kvstore *kvs, char *buf, size_t bufsize, int full);

int kvstoreFindDictIndexByKeyIndex(kvstore *kvs, unsigned long target);
int kvstoreGetFirstNonEmptyDictIndex(kvstore *kvs);
int kvstoreGetNextNonEmptyDictIndex(kvstore *kvs, int didx);
int kvstoreNumNonEmptyDicts(kvstore *kvs);
int kvstoreNumAllocatedDicts(kvstore *kvs);
int kvstoreNumDicts(kvstore *kvs);
void kvstoreMoveDict(kvstore *kvs, kvstore *dst, int didx);

/* kvstore iterator specific functions */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs);
void kvstoreIteratorRelease(kvstoreIterator *kvs_it);
dict *kvstoreIteratorNextDict(kvstoreIterator *kvs_it);
int kvstoreIteratorGetCurrentDictIndex(kvstoreIterator *kvs_it);
dictEntry *kvstoreIteratorNext(kvstoreIterator *kvs_it);

/* Rehashing */
void kvstoreTryResizeDicts(kvstore *kvs, int limit);
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us);
size_t kvstoreOverheadHashtableLut(kvstore *kvs);
size_t kvstoreOverheadHashtableRehashing(kvstore *kvs);
unsigned long kvstoreDictRehashingCount(kvstore *kvs);

/* Specific dict access by dict-index */
unsigned long kvstoreDictSize(kvstore *kvs, int didx);
size_t kvstoreDictAllocSize(kvstore *kvs, int didx);
kvstoreDictIterator *kvstoreGetDictIterator(kvstore *kvs, int didx);
kvstoreDictIterator *kvstoreGetDictSafeIterator(kvstore *kvs, int didx);
void kvstoreReleaseDictIterator(kvstoreDictIterator *kvs_id);
dictEntry *kvstoreDictIteratorNext(kvstoreDictIterator *kvs_di);
dictEntry *kvstoreDictGetRandomKey(kvstore *kvs, int didx);
dictEntry *kvstoreDictGetFairRandomKey(kvstore *kvs, int didx);
unsigned int kvstoreDictGetSomeKeys(kvstore *kvs, int didx, dictEntry **des, unsigned int count);
int kvstoreDictExpand(kvstore *kvs, int didx, unsigned long size);
unsigned long kvstoreDictScanDefrag(kvstore *kvs, int didx, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata);
typedef dict *(kvstoreDictLUTDefragFunction)(dict *d);
unsigned long kvstoreDictLUTDefrag(kvstore *kvs, unsigned long cursor, kvstoreDictLUTDefragFunction *defragfn);
void *kvstoreDictFetchValue(kvstore *kvs, int didx, const void *key);
dictEntry *kvstoreDictFind(kvstore *kvs, int didx, void *key);
dictEntry *kvstoreDictAddRaw(kvstore *kvs, int didx, void *key, dictEntry **existing);
dictEntryLink kvstoreDictTwoPhaseUnlinkFind(kvstore *kvs, int didx, const void *key, int *table_index);
void kvstoreDictTwoPhaseUnlinkFree(kvstore *kvs, int didx, dictEntryLink plink, int table_index);
int kvstoreDictDelete(kvstore *kvs, int didx, const void *key);
dict *kvstoreGetDict(kvstore *kvs, int didx);
kvstoreDictMetadata *kvstoreGetDictMetadata(kvstore *kvs, int didx);
kvstoreMetadata *kvstoreGetMetadata(kvstore *kvs);

dictEntryLink kvstoreDictFindLink(kvstore *kvs, int didx, void *key, dictEntryLink *bucket);
void kvstoreDictSetAtLink(kvstore *kvs, int didx, void *kv, dictEntryLink *link, int newItem);
void kvstoreTrackDeallocation(dict *d, void *kv);

/* dict with distinct key & value (no_value=1) currently is used only by pubsub. */
void kvstoreDictSetKey(kvstore *kvs, int didx, dictEntry* de, void *key);
void kvstoreDictSetVal(kvstore *kvs, int didx, dictEntry *de, void *val);

#ifdef REDIS_TEST
int kvstoreTest(int argc, char *argv[], int flags);
#endif

#endif /* DICTARRAY_H_ */
