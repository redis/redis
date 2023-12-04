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
    unsigned long long key_count;          /* Total number of keys in this dictarray. */
    unsigned long long bucket_count;       /* Total number of buckets in this dictarray across dictionaries. */
    unsigned long long *slot_size_index;   /* Binary indexed tree (BIT) that describes cumulative key frequencies up until given slot. */
} daState;

typedef struct {
    dict **dicts;
    long long num_slots;
    long long num_slots_bits;
    daState state;
} dictarray;

/* Structure for DB iterator that allows iterating across multiple slot specific dictionaries in cluster mode. */
typedef struct {
    dictarray *da;
    long long slot;
    long long next_slot;
    dictIterator di;
} daIterator;

typedef int (dictarrayScanShouldSkipDict)(dict *d);
typedef int (dictarrayExpandShouldSkipSlot)(int slot);

int daFindSlotByKeyIndex(dictarray *da, unsigned long target);
int daGetNextNonEmptySlot(dictarray *da, int slot);
void daCumulativeKeyCountAdd(dictarray *da, int slot, long delta);
void daGetStats(dictarray *da, char *buf, size_t bufsize, int full);
dictarray *daCreate(dictType *type, int num_slots_bits);
void daEmpty(dictarray *da, void(callback)(dict*));
void daRelease(dictarray *da);
dict *daGetDict(dictarray *da, int slot);
unsigned long long daSize(dictarray *da);
unsigned long daBuckets(dictarray *da);
size_t daMemUsage(dictarray *da);
dictEntry *daFind(dictarray *da, void *key, int slot);
unsigned long long daScan(dictarray *da, unsigned long long cursor,
                          int onlyslot, dictScanFunction *scan_cb,
                          dictarrayScanShouldSkipDict *skip_cb,
                          void *privdata);
int daExpand(dictarray *da, uint64_t newsize, int try_expand, dictarrayExpandShouldSkipSlot *skip_cb);
int daGetFairRandomSlot(dictarray *da);
int daFindSlotByKeyIndex(dictarray *da, unsigned long target);

/* DB iterator specific functions */
daIterator *daIteratorInit(dictarray *da);
void daReleaseIterator(daIterator *dait);
dict *daIteratorNextDict(daIterator *dait);
dict *daGetDictFromIterator(daIterator *dait);
int daIteratorGetCurrentSlot(daIterator *dait);
dictEntry *daIteratorNext(daIterator *dait);

#endif /* DICTARRAY_H_ */
