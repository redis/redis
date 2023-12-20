/*
 * Copyright (c) 2009-2023, Redis Ltd.
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

#ifndef DICTARRAY_H_
#define DICTARRAY_H_

#include "dict.h"
#include "adlist.h"

typedef struct {
    list *rehashing;                       /* List of dictionaries in this dictarray that are currently rehashing. */
    int resize_cursor;                     /* Cron job uses this cursor to gradually resize dictionaries. */
    int non_empty_dicts;                   /* The number of non-empty dicts. */
    unsigned long long key_count;          /* Total number of keys in this dictarray. */
    unsigned long long bucket_count;       /* Total number of buckets in this dictarray across dictionaries. */
    unsigned long long *dict_size_index;   /* Binary indexed tree (BIT) that describes cumulative key frequencies up until given dict-index. */
} daState;

typedef struct {
    dictType dtype;
    dict **dicts;
    long long num_dicts;
    long long num_dicts_bits;
    daState state;
} dictarray;

/* Dict metadata for database, used for record the position in rehashing list. */
typedef struct {
    dictarray *da;
    listNode *rehashing_node;   /* list node in rehashing list */
} daDictMetadata;

/* Structure for dictarray iterator that allows iterating across multiple dicts. */
typedef struct {
    dictarray *da;
    long long didx;
    long long next_didx;
    dictIterator di;
} daIterator;

typedef int (dictarrayScanShouldSkipDict)(dict *d);
typedef int (dictarrayExpandShouldSkipDictIndex)(int didx);

dictarray *daCreate(dictType *type, int num_dicts_bits);
void daEmpty(dictarray *da, void(callback)(dict*));
void daRelease(dictarray *da);
unsigned long long daSize(dictarray *da);
unsigned long daBuckets(dictarray *da);
size_t daMemUsage(dictarray *da);
unsigned long long daScan(dictarray *da, unsigned long long cursor,
                          int onlydidx, dictScanFunction *scan_cb,
                          dictarrayScanShouldSkipDict *skip_cb,
                          void *privdata);
int daExpand(dictarray *da, uint64_t newsize, int try_expand, dictarrayExpandShouldSkipDictIndex *skip_cb);
int daGetFairRandomDictIndex(dictarray *da);
void daGetStats(dictarray *da, char *buf, size_t bufsize, int full);
dict *daGetDict(dictarray *da, int didx);

int daFindDictIndexByKeyIndex(dictarray *da, unsigned long target);
int daGetNextNonEmptyDictIndex(dictarray *da, int didx);
int daNonEmptyDicts(dictarray *da);

/* dictarray iterator specific functions */
daIterator *daIteratorInit(dictarray *da);
void daReleaseIterator(daIterator *dait);
dict *daIteratorNextDict(daIterator *dait);
int daIteratorGetCurrentDictIndex(daIterator *dait);
dictEntry *daIteratorNext(daIterator *dait);
dict *daGetDictFromIterator(daIterator *dait);

/* Rehashing */
void daTryResizeHashTables(dictarray *da, int limit);
uint64_t daIncrementallyRehash(dictarray *da, uint64_t threshold_ms);

/* dict wrappers */
dictEntry *daDictFind(dictarray *da, int didx, void *key);
dictEntry *daDictAddRaw(dictarray *da, int didx, void *key, dictEntry **existing);
void daDictSetKey(dictarray *da, int didx, dictEntry* de, void *key);
void daDictSetVal(dictarray *da, int didx, dictEntry *de, void *val);
dictEntry *daDictTwoPhaseUnlinkFind(dictarray *da, int didx, const void *key, dictEntry ***plink, int *table_index);
void daDictTwoPhaseUnlinkFree(dictarray *da, int didx, dictEntry *he, dictEntry **plink, int table_index);
int daDictDelete(dictarray *da, int didx, const void *key);
int daDictDelete(dictarray *da, int didx, const void *key);

#endif /* DICTARRAY_H_ */
