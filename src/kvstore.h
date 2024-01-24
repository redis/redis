#ifndef DICTARRAY_H_
#define DICTARRAY_H_

#include "dict.h"
#include "adlist.h"

typedef struct {
    list *rehashing;                       /* List of dictionaries in this kvstore that are currently rehashing. */
    int resize_cursor;                     /* Cron job uses this cursor to gradually resize dictionaries. */
    int non_empty_dicts;                   /* The number of non-empty dicts. */
    unsigned long long key_count;          /* Total number of keys in this kvstore. */
    unsigned long long bucket_count;       /* Total number of buckets in this kvstore across dictionaries. */
    unsigned long long *dict_size_index;   /* Binary indexed tree (BIT) that describes cumulative key frequencies up until given dict-index. */
} kvstoreState;

typedef struct {
    dictType dtype;
    dict **dicts;
    long long num_dicts;
    long long num_dicts_bits;
    kvstoreState state;
} kvstore;

/* Dict metadata for database, used for record the position in rehashing list. */
typedef struct {
    kvstore *kvs;
    listNode *rehashing_node;   /* list node in rehashing list */
} kvstoreDictMetadata;

/* Structure for kvstore iterator that allows iterating across multiple dicts. */
typedef struct {
    kvstore *kvs;
    long long didx;
    long long next_didx;
    dictIterator di;
} kvstoreIterator;

typedef int (kvstoreScanShouldSkipDict)(dict *d);
typedef int (kvstoreExpandShouldSkipDictIndex)(int didx);

kvstore *kvstoreCreate(dictType *type, int num_dicts_bits);
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
dict *kvstoreGetDict(kvstore *kvs, int didx);

int kvstoreFindDictIndexByKeyIndex(kvstore *kvs, unsigned long target);
int kvstoreGetNextNonEmptyDictIndex(kvstore *kvs, int didx);
int kvstoreNonEmptyDicts(kvstore *kvs);

/* kvstore iterator specific functions */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs);
void kvstoreIteratorRelease(kvstoreIterator *kvs_it);
dict *kvstoreIteratorNextDict(kvstoreIterator *kvs_it);
int kvstoreIteratorGetCurrentDictIndex(kvstoreIterator *kvs_it);
dictEntry *kvstoreIteratorNext(kvstoreIterator *kvs_it);

/* Rehashing */
void kvstoreTryShrinkHashTables(kvstore *kvs, int limit);
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_ms);

/* Specific dict access by dict-index */
dictEntry *kvstoreDictFind(kvstore *kvs, int didx, void *key);
dictEntry *kvstoreDictAddRaw(kvstore *kvs, int didx, void *key, dictEntry **existing);
void kvstoreDictSetKey(kvstore *kvs, int didx, dictEntry* de, void *key);
void kvstoreDictSetVal(kvstore *kvs, int didx, dictEntry *de, void *val);
dictEntry *kvstoreDictTwoPhaseUnlinkFind(kvstore *kvs, int didx, const void *key, dictEntry ***plink, int *table_index);
void kvstoreDictTwoPhaseUnlinkFree(kvstore *kvs, int didx, dictEntry *he, dictEntry **plink, int table_index);
int kvstoreDictDelete(kvstore *kvs, int didx, const void *key);

#endif /* DICTARRAY_H_ */
