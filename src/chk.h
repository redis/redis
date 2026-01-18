/* Implementation of a topK structure using CuckooHeavyKeeper algorithm
 *
 * Implementation is based on the paper "Cuckoo Heavy Keeper and the balancing
 * act of maintaining heavy hitters in stream processing" by Vinh Quang Ngo and
 * Marina Papatriantafilou. Also, the accompanying C++ implementation was used
 * as a reference point: https://github.com/vinhqngo5/Cuckoo_Heavy_Keeper
 * Main changes are addition of a min-heap so we can keep names of the top K
 * elements - idea comes from RedisBloom's TopK structure.
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#pragma once

#include "sds.h"

#include <stddef.h>
#include <stdint.h>

#define CHK_LUT_SIZE 256
#define CHK_HEAVY_ENTRIES_PER_BUCKET 2
#define CHK_NUM_TABLES 2

typedef uint64_t counter_t;
typedef uint16_t fingerprint_t;
typedef uint8_t lobby_counter_t;

typedef struct {
    counter_t count;
    fingerprint_t fp;
} chkHeavyEntry;

typedef struct {
    fingerprint_t fp;
    lobby_counter_t count;
} chkLobbyEntry;

typedef struct {
    chkHeavyEntry heavy_entries[CHK_HEAVY_ENTRIES_PER_BUCKET];
    chkLobbyEntry lobby_entry;
} chkBucket;

typedef struct {
    counter_t count;
    sds item;
    uint64_t fp; /* Fingerprint used to identify the item. Internal use only */
} chkHeapBucket;

typedef struct chkTopK {
    chkBucket *tables[CHK_NUM_TABLES]; /* Cuckoo tables */
    chkHeapBucket *heap; /* Min-heap for storing top-K item's names */

    size_t alloc_size; /* Used for memory tracking only */

    /* Expected number of operations to decay count i to 0 */
    double lut_decay_exp[CHK_LUT_SIZE + 1];

    /* Minimum number of decay operations to decay count i with 1 */
    double lut_min_decay[CHK_LUT_SIZE + 1];

    /* Probability of decaying i with 1. As per paper probability is decay^-i
     * but we actually store (1/decay)^i for faster computation. */
    double lut_decay_prob[CHK_LUT_SIZE + 1];

    double decay; /* Decay constant */
    double inv_decay; /* Cache 1/decay for faster computations */

    counter_t total; /* Total recorded count for all updates */

    int k;
    int numbuckets;
} chkTopK;

chkTopK *chkTopKCreate(int k, int numbuckets, double decay);
void chkTopKRelease(chkTopK *topk);
sds chkTopKUpdate(chkTopK *topk, char *item, int itemlen, counter_t weight);
chkHeapBucket *chkTopKList(chkTopK *topk);
size_t chkTopKGetMemoryUsage(chkTopK *topk);

#ifdef REDIS_TEST

int chkTopKTest(int argc, char *argv[], int flags);

#endif /* REDIS_TEST */
