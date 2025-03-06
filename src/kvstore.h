/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
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
    uint64_t keysizes_hist[MAX_KEYSIZES_TYPES][MAX_KEYSIZES_BINS];
} kvstoreMetadata;

/* Like kvstoreMetadata, this one per dict */
typedef struct {
    uint64_t keysizes_hist[MAX_KEYSIZES_TYPES][MAX_KEYSIZES_BINS];
} kvstoreDictMetadata;

typedef struct _kvstore kvstore;
typedef struct _kvstoreIterator kvstoreIterator;
typedef struct _kvstoreDictIterator kvstoreDictIterator;

typedef int (kvstoreScanShouldSkipDict)(dict *d);
typedef int (kvstoreExpandShouldSkipDictIndex)(int didx);

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
int kvstoreGetFairRandomDictIndex(kvstore *kvs);
void kvstoreGetStats(kvstore *kvs, char *buf, size_t bufsize, int full);

int kvstoreFindDictIndexByKeyIndex(kvstore *kvs, unsigned long target);
int kvstoreGetFirstNonEmptyDictIndex(kvstore *kvs);
int kvstoreGetNextNonEmptyDictIndex(kvstore *kvs, int didx);
int kvstoreNumNonEmptyDicts(kvstore *kvs);
int kvstoreNumAllocatedDicts(kvstore *kvs);
int kvstoreNumDicts(kvstore *kvs);
uint64_t kvstoreGetHash(kvstore *kvs, const void *key);

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
kvstoreDictIterator *kvstoreGetDictIterator(kvstore *kvs, int didx);
kvstoreDictIterator *kvstoreGetDictSafeIterator(kvstore *kvs, int didx);
void kvstoreReleaseDictIterator(kvstoreDictIterator *kvs_id);
dictEntry *kvstoreDictIteratorNext(kvstoreDictIterator *kvs_di);
dictEntry *kvstoreDictGetRandomKey(kvstore *kvs, int didx);
dictEntry *kvstoreDictGetFairRandomKey(kvstore *kvs, int didx);
dictEntry *kvstoreDictFindByHashAndPtr(kvstore *kvs, int didx, const void *oldptr, uint64_t hash);
unsigned int kvstoreDictGetSomeKeys(kvstore *kvs, int didx, dictEntry **des, unsigned int count);
int kvstoreDictExpand(kvstore *kvs, int didx, unsigned long size);
unsigned long kvstoreDictScanDefrag(kvstore *kvs, int didx, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata);
typedef dict *(kvstoreDictLUTDefragFunction)(dict *d);
unsigned long kvstoreDictLUTDefrag(kvstore *kvs, unsigned long cursor, kvstoreDictLUTDefragFunction *defragfn);
void *kvstoreDictFetchValue(kvstore *kvs, int didx, const void *key);
dictEntry *kvstoreDictFind(kvstore *kvs, int didx, void *key);
dictEntry *kvstoreDictAddRaw(kvstore *kvs, int didx, void *key, dictEntry **existing);
void kvstoreDictSetKey(kvstore *kvs, int didx, dictEntry* de, void *key);
void kvstoreDictSetVal(kvstore *kvs, int didx, dictEntry *de, void *val);
dictEntry *kvstoreDictTwoPhaseUnlinkFind(kvstore *kvs, int didx, const void *key, dictEntry ***plink, int *table_index);
void kvstoreDictTwoPhaseUnlinkFree(kvstore *kvs, int didx, dictEntry *he, dictEntry **plink, int table_index);
int kvstoreDictDelete(kvstore *kvs, int didx, const void *key);
kvstoreDictMetadata *kvstoreGetDictMetadata(kvstore *kvs, int didx);
kvstoreMetadata *kvstoreGetMetadata(kvstore *kvs);

#ifdef REDIS_TEST
int kvstoreTest(int argc, char *argv[], int flags);
#endif

#endif /* DICTARRAY_H_ */
