/* zset_btree.h -- memory efficient sorted set implementation.
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef REDIS_ZSET_BTREE_H
#define REDIS_ZSET_BTREE_H

#include "sds.h"
#include <stddef.h>
#include <stdint.h>

typedef struct zbtreeSet zbtreeSet;

/* A ZADD lookup records both index positions. An insertion can reuse the hash
 * position after a miss, while an update can reuse the score position after
 * a hit. The contents are private to zset_btree.c. */
typedef struct {
    void *bucket;
    unsigned int pos;
    void *score_leaf;
    unsigned int score_pos;
    uint32_t hash;
    uint32_t revision;
} zbtreeInsertPosition;

/* Iterators point inside score leaves. They and the member bytes returned by
 * zbtreeIteratorNext() remain valid only until the set is modified. */
typedef struct {
    void *leaf;
    unsigned int pos;
} zbtreeIterator;

/* This state exists only while active defragmentation is visiting a large
 * sorted set. It is kept by the defragmenter, not in every zset. */
typedef struct {
    zbtreeSet *tree;
    uint32_t score_leaf_id;
    uint16_t score_pos;
    uint8_t leaf_moved;
} zbtreeDefragState;

typedef void *zbtreeDefragAllocFunction(void *ptr);
typedef void zbtreeDefragFreeFunction(void *ptr);
typedef void zbtreeScanFunction(void *privdata, const unsigned char *ele,
                                size_t len, double score);

/* Creation, release, and memory accounting. */
zbtreeSet *zbtreeCreate(void);
zbtreeSet *zbtreeDup(const zbtreeSet *zs);
void zbtreeFree(zbtreeSet *zs);
unsigned long zbtreeLength(const zbtreeSet *zs);
size_t zbtreeAllocSize(const zbtreeSet *zs);
void zbtreeDismissMemory(zbtreeSet *zs);

/* Active defragmentation. Start may move the set itself and therefore returns
 * its current address. Step moves at most one leaf, one external member, and
 * the inner pages that finish at that leaf. It returns one when complete. */
zbtreeSet *zbtreeDefragStart(zbtreeSet *zs,
                             zbtreeDefragAllocFunction *defrag_alloc);
void zbtreeDefragStateReset(zbtreeDefragState *state);
int zbtreeDefragStep(zbtreeSet *zs, zbtreeDefragState *state,
                     zbtreeDefragAllocFunction *defrag_alloc,
                     zbtreeDefragFreeFunction *defrag_free,
                     unsigned long *scanned);

/* Lookup and modification. InsertNew* require an absent member. UpdateScore
 * requires the position returned by a successful FindForAdd. Delete returns
 * zero when the member is absent. */
int zbtreeScore(zbtreeSet *zs, sds ele, double *score);
int zbtreeScoreRaw(zbtreeSet *zs, const unsigned char *ele,
                   size_t elelen, double *score);
int zbtreeFindForAdd(zbtreeSet *zs, sds ele, double *score,
                     zbtreeInsertPosition *position);
long zbtreeRank(zbtreeSet *zs, sds ele, int reverse, double *score);
void zbtreeInsertNew(zbtreeSet *zs, double score, sds ele,
                     const zbtreeInsertPosition *position);
void zbtreeInsertNewRaw(zbtreeSet *zs, double score, const unsigned char *ele,
                        size_t elelen, const zbtreeInsertPosition *position);
int zbtreeDelete(zbtreeSet *zs, sds ele);
void zbtreeUpdateScore(zbtreeSet *zs, sds ele, double score,
                       const zbtreeInsertPosition *position);
unsigned long zbtreeDeleteRangeByRank(zbtreeSet *zs, unsigned long start,
                                      unsigned long end);
unsigned long zbtreeDeleteRangeByScore(zbtreeSet *zs, double min, int minex,
                                       double max, int maxex);

/* Ordered access. A successful seek leaves the iterator on its result; Next
 * returns that result and advances in the requested direction. Its member,
 * length, and score output pointers may be NULL. */
int zbtreeIteratorSeekRank(const zbtreeSet *zs, unsigned long rank,
                           zbtreeIterator *iter);
int zbtreeIteratorSeekScore(const zbtreeSet *zs, double score, int exclusive,
                            int reverse, zbtreeIterator *iter,
                            unsigned long *rank);
int zbtreeIteratorSeekLex(const zbtreeSet *zs, sds ele, int exclusive,
                          int reverse, zbtreeIterator *iter,
                          unsigned long *rank);
int zbtreeIteratorStart(const zbtreeSet *zs, int reverse,
                        zbtreeIterator *iter);
int zbtreeIteratorNext(zbtreeIterator *iter, int reverse,
                       const unsigned char **ele, size_t *len, double *score);
unsigned long zbtreeCountByScore(const zbtreeSet *zs, double min, int minex,
                                 double max, int maxex);

/* Incremental unordered access. The cursor is an opaque hash position. */
uint64_t zbtreeScan(zbtreeSet *zs, uint64_t cursor,
                    unsigned long count, zbtreeScanFunction *fn,
                    void *privdata);

#endif
