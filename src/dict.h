/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0
#define DICT_ERR 1

/* Hash table parameters */
#define HASHTABLE_MIN_FILL        8      /* Minimal hash table fill 12.5%(100/8) */

typedef struct dictEntry dictEntry; /* opaque */
typedef struct dict dict;
typedef dictEntry **dictEntryLink; /* See description of dictFindLink() */

/* Searching for a key in a dict may involve few comparisons.
 * If extracting the looked-up key is expensive (e.g., sdslen(), kvobjGetKey()),  
 * caching can be used to reduce those repetitive computations.  
 *  
 * This struct, passed to the comparison function as temporary caching, if 
 * needed by the function across comparison of a given lookup. 
 * for the looked-up key and resets before each new lookup. */
typedef struct dictCmpCache {
    int useCache;
    
    union {
        uint64_t u64;
        int64_t i64;
        int i;
        size_t sz;
        void *p;
    } data[2];
} dictCmpCache;

typedef struct dictType {
    /* Callbacks */
    uint64_t (*hashFunction)(const void *key);
    void *(*keyDup)(dict *d, const void *key);
    void *(*valDup)(dict *d, const void *obj);
    int (*keyCompare)(dictCmpCache *cache, const void *key1, const void *key2);
    void (*keyDestructor)(dict *d, void *key);
    void (*valDestructor)(dict *d, void *obj);
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of dict initialization/rehashing (old and new ht are already created) */
    void (*rehashingStarted)(dict *d);
    /* Invoked at the end of dict initialization/rehashing of all the entries from old to new ht. Both ht still exists
     * and are cleaned up after this callback.  */
    void (*rehashingCompleted)(dict *d);
    /* Invoked when the size of the dictionary changes.
     * The `delta` parameter can be positive (size increase) or negative (size decrease). */
    void (*bucketChanged)(dict *d, long long delta);
    /* Allow a dict to carry extra caller-defined metadata. The
     * extra memory is initialized to 0 when a dict is allocated. */
    size_t (*dictMetadataBytes)(dict *d);

    /* Data */
    void *userdata;

    /* Flags */
    /* The 'no_value' flag, if set, indicates that values are not used, i.e. the
     * dict is a set. When this flag is set, it's not possible to access the
     * value of a dictEntry and it's also impossible to use dictSetKey(). It 
     * enables an optimization to store a key directly without an allocating 
     * dictEntry in between, if it is the only key in the bucket. */
    unsigned int no_value:1;
    /* This flag is required for `no_value` optimization since the optimization
     * reuses LSB bits as metadata */ 
    unsigned int keys_are_odd:1;

    /* Ensures that the entire hash table is rehashed at once if set. */
    unsigned int force_full_rehash:1;

    /* Sometimes we want the ability to store a key in a given way inside the hash
     * function, and lookup it in some other way without resorting to any kind of
     * conversion. For instance the key may be stored as a structure also
     * representing other things, but the lookup happens via just a pointer to a
     * null terminated string. Optionally providing additional hash/cmp functions,
     * dict supports such usage. In that case we'll have a hashFunction() that will
     * expect a null terminated C string, and a storedHashFunction() that will
     * instead expect the structure. Similarly, the two comparison functions will
     * work differently. The keyCompare() will treat the first argument as a pointer
     * to a C string and the other as a structure (this way we can directly lookup
     * the structure key using the C string). While the storedKeyCompare() will
     * check if two pointers to the key in structure form are the same.
     *
     * However, functions of dict that gets key as argument (void *key) don't get
     * any indication whether it is a lookup or stored key. To indicate that
     * you intend to use key of type stored-key, and, consequently, use
     * dedicated compare and hash functions of stored-key, is by calling
     * dictUseStoredKeyApi(1) before using any of the dict functions that gets
     * key as a parameter and then call again  dictUseStoredKeyApi(0) once done.
     *
     * Set to NULL both functions, if you don't want to support this feature. */
    uint64_t (*storedHashFunction)(const void *key);
    int (*storedKeyCompare)(dictCmpCache *cache, const void *key1, const void *key2);

    /* Optional callback called when the dict is destroyed. */
    void (*onDictRelease)(dict *d);
} dictType;

#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1<<(exp))
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp))-1)

struct dict {
    dictType *type;

    dictEntry **ht_table[2];
    unsigned long ht_used[2];

    long rehashidx; /* rehashing not in progress if rehashidx == -1 */

    /* Keep small vars at end for optimal (minimal) struct padding */
    unsigned pauserehash : 15; /* If >0 rehashing is paused */

    unsigned useStoredKeyApi : 1; /* See comment of storedHashFunction above */
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) */
    int16_t pauseAutoResize;  /* If >0 automatic resizing is disallowed (<0 indicates coding error) */
    void *metadata[];
};

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;
    long index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    unsigned long long fingerprint;
} dictIterator;

typedef struct dictStats {
    int htidx;
    unsigned long buckets;
    unsigned long maxChainLen;
    unsigned long totalChainLen;
    unsigned long htSize;
    unsigned long htUsed;
    unsigned long *clvector;
} dictStats;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de, dictEntry **plink);
typedef void *(dictDefragAllocFunction)(void *ptr);
typedef struct {
    dictDefragAllocFunction *defragAlloc; /* Used for entries etc. */
    dictDefragAllocFunction *defragKey;   /* Defrag-realloc keys (optional) */
    dictDefragAllocFunction *defragVal;   /* Defrag-realloc values (optional) */
} dictDefragFunctions;

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP      2
#define DICT_HT_INITIAL_SIZE     (1<<(DICT_HT_INITIAL_EXP))

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) do {                     \
    if ((d)->type->valDestructor)                      \
        (d)->type->valDestructor((d), dictGetVal(entry)); \
   } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d), dictGetKey(entry))

#define dictMetadata(d) (&(d)->metadata)
#define dictMetadataSize(d) ((d)->type->dictMetadataBytes \
                             ? (d)->type->dictMetadataBytes(d) : 0)

#define dictBuckets(d) (DICTHT_SIZE((d)->ht_size_exp[0])+DICTHT_SIZE((d)->ht_size_exp[1]))
#define dictSize(d) ((d)->ht_used[0]+(d)->ht_used[1])
#define dictIsEmpty(d) ((d)->ht_used[0] == 0 && (d)->ht_used[1] == 0)
#define dictIsRehashing(d) ((d)->rehashidx != -1)
#define dictPauseRehashing(d) ((d)->pauserehash++)
#define dictResumeRehashing(d) ((d)->pauserehash--)
#define dictIsRehashingPaused(d) ((d)->pauserehash > 0)
#define dictPauseAutoResize(d) ((d)->pauseAutoResize++)
#define dictResumeAutoResize(d) ((d)->pauseAutoResize--)
#define dictUseStoredKeyApi(d, flag) ((d)->useStoredKeyApi = (flag))

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long) genrand64_int64())
#else
#define randomULong() random()
#endif

typedef enum {
    DICT_RESIZE_ENABLE,
    DICT_RESIZE_AVOID,
    DICT_RESIZE_FORBID,
} dictResizeEnable;

/* API */
dict *dictCreate(dictType *type);
void dictTypeAddMeta(dict **d, dictType *typeWithMeta);
int dictExpand(dict *d, unsigned long size);
int dictTryExpand(dict *d, unsigned long size);
int dictShrink(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *dictAddOrFind(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
int dictDelete(dict *d, const void *key);
dictEntry *dictUnlink(dict *d, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
dictEntryLink dictTwoPhaseUnlinkFind(dict *d, const void *key, int *table_index);
void dictTwoPhaseUnlinkFree(dict *d, dictEntryLink llink, int table_index);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
dictEntry *dictFindByHashAndPtr(dict *d, const void *oldptr, const uint64_t hash);
int dictShrinkIfNeeded(dict *d);
int dictExpandIfNeeded(dict *d);
void *dictGetKey(const dictEntry *de);
int dictEntryIsKey(const dictEntry *de);
int dictCompareKeys(dict *d, const void *key1, const void *key2);
size_t dictMemUsage(const dict *d);
size_t dictEntryMemUsage(int noValueDict);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
void dictInitIterator(dictIterator *iter, dict *d);
void dictInitSafeIterator(dictIterator *iter, dict *d);
void dictResetIterator(dictIterator *iter);
dictEntry *dictNext(dictIterator *iter);
dictEntry *dictGetNext(const dictEntry *de);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d, int full);
uint64_t dictGenHashFunction(const void *key, size_t len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);
void dictEmpty(dict *d, void(callback)(dict*));
void dictSetResizeEnabled(dictResizeEnable enable);
int dictRehash(dict *d, int n);
int dictRehashMicroseconds(dict *d, uint64_t us);
void dictSetHashFunctionSeed(uint8_t *seed);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);
unsigned long dictScanDefrag(dict *d, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
void dictRehashingInfo(dict *d, unsigned long long *from_size, unsigned long long *to_size);

size_t dictGetStatsMsg(char *buf, size_t bufsize, dictStats *stats, int full);
dictStats* dictGetStatsHt(dict *d, int htidx, int full);
void dictCombineStats(dictStats *from, dictStats *into);
void dictFreeStats(dictStats *stats);

dictEntryLink dictFindLink(dict *d, const void *key, dictEntryLink *bucket);
void dictSetKeyAtLink(dict *d, void *key, dictEntryLink *link, int newItem);

/* API relevant only when dict is used as a hash-map (no_value=0) */ 
void dictSetKey(dict *d, dictEntry* de, void *key);
void dictSetVal(dict *d, dictEntry *de, void *val);
void *dictGetVal(const dictEntry *de);
void dictSetDoubleVal(dictEntry *de, double val);
double dictGetDoubleVal(const dictEntry *de);
double *dictGetDoubleValPtr(dictEntry *de);
void *dictFetchValue(dict *d, const void *key);
void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val);
uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val);
uint64_t dictGetUnsignedIntegerVal(const dictEntry *de);

#define dictForEach(d, ty, m, ...) do { \
    dictIterator di; \
    dictEntry *de; \
    dictInitIterator(&di, d); \
    while ((de = dictNext(&di)) != NULL) { \
        ty *m = dictGetVal(de); \
        do { \
            __VA_ARGS__ \
        } while(0); \
    } \
    dictResetIterator(&di); \
} while(0);

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);
#endif

#endif /* __DICT_H */
