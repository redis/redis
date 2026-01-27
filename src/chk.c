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

#include "chk.h"
#include "redisassert.h"
#include "zmalloc.h"
#include "xxhash.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Lobby to heavy item promotion threshold */
#define LOBBY_PROMOTION_THRESHOLD 16

#ifndef static_assert
#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]
#endif

static_assert(LOBBY_PROMOTION_THRESHOLD < CHK_LUT_SIZE,
              "Lobby promotion threshold should be less then the LUT size to "
              "ensure constant operations during decayCounter!");

/* After a heavy item is demoted is starts recursively kicking out other heavy
 * items in the case it should stay heavy (defined by isHeavyHitter). In
 * principle this process could go over all the items in the chkTopK's tables
 * so it's artificially limited by this constant. */
#define MAX_KICKS 16

/* An item is defined as heavy hitter if its count is more or equal to x * N
 * where x is a threshold constant (HEAVY_RATIO) and N is the total count the
 * chkTopK structure has accumulated. See the paper for more info. */
#define HEAVY_RATIO 0.008

/* A unique seed for the items when storing them in the heap so it's not related
 * to the cuckoo's hashes. Also, we don't need the less-bit hash here as the
 * heap does not take much memory so we avoid needless possible collisions. */
#define HEAP_SEED 1919

typedef struct {
    size_t idx[CHK_NUM_TABLES];
    fingerprint_t fp;
} fpAndIdx;

#define min(a, b) ((a) < (b) ? (a) : (b))

/* Heap operations */
static chkHeapBucket *chkCheckExistInHeap(chkTopK *topk, const char *item, int itemlen, uint64_t fp) {
    for (int32_t i = topk->k - 1; i >= 0; --i) {
        chkHeapBucket *bucket = topk->heap + i;
        if (bucket->fp == fp && bucket->item &&
            sdslen(bucket->item) == (size_t)itemlen &&
            memcmp(bucket->item, item, itemlen) == 0)
        {
            return bucket;
        }
    }
    return NULL;
}

void chkHeapifyDown(chkHeapBucket *array, size_t len, size_t start) {
    size_t child = start;

    if (len < 2 || (len - 2) / 2 < child) {
        return;
    }
    child = 2 * child + 1;
    if ((child + 1) < len && (array[child].count > array[child + 1].count)) {
        ++child;
    }
    if (array[child].count > array[start].count) {
        return;
    }

    chkHeapBucket top = {0};
    top = array[start];
    do {
        memcpy(&array[start], &array[child], sizeof(chkHeapBucket));
        start = child;

        if ((len - 2) / 2 < child) {
            break;
        }
        child = 2 * child + 1;

        if ((child + 1) < len && (array[child].count > array[child + 1].count)) {
            ++child;
        }
    } while (array[child].count < top.count);
    memcpy(&array[start], &top, sizeof(chkHeapBucket));
}

/*-----------------------------------------------------------------------------
 * chkTopK operations
 *----------------------------------------------------------------------------*/

/* Create the chkTopK structure. Note, CHK paper recommends decay=1.08.
 * numbuckets must be a power of 2. Recommended size for numbuckets is at least
 * 7 or 8 times k. */
chkTopK *chkTopKCreate(int k, int numbuckets, double decay) {
    /* Number of buckets need to be a power of 2 for better performance - we
     * have better cache locality of the tables and faster table indices
     * calculations. */
    assert(k > 0 && (numbuckets & (numbuckets - 1)) == 0);

    size_t usable = 0;
    chkTopK *topk = zcalloc_usable(sizeof(chkTopK), &usable);
    topk->alloc_size += usable;

    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        topk->tables[i] = zcalloc_usable(sizeof(chkBucket) * numbuckets, &usable);
        topk->alloc_size += usable;
    }

    topk->heap = zcalloc_usable(sizeof(chkHeapBucket) * k, &usable);
    topk->alloc_size += usable;

    topk->decay = decay;
    topk->inv_decay = 1. / decay;
    topk->k = k;
    topk->numbuckets = numbuckets;

    topk->lut_decay_exp[0] = 0;
    topk->lut_min_decay[0] = 0;
    topk->lut_decay_prob[0] = 0;
    for (int i = 1; i < CHK_LUT_SIZE + 1; ++i) {
        topk->lut_decay_exp[i] = topk->lut_decay_exp[i - 1] + pow(topk->decay, i - 1);
        topk->lut_min_decay[i] = topk->lut_decay_exp[i] - topk->lut_decay_exp[i - 1];
        topk->lut_decay_prob[i] = pow(topk->inv_decay, i);
    }

    return topk;
}

/* Release chkTopK resources */
void chkTopKRelease(chkTopK *topk) {
    size_t usable;
    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        zfree_usable(topk->tables[i], &usable);
        topk->alloc_size -= usable;
    }
    for (int i = 0; i < topk->k; ++i) {
        if (topk->heap[i].item) {
            topk->alloc_size -= sdsAllocSize(topk->heap[i].item);
            sdsfree(topk->heap[i].item);
        }
    }
    zfree_usable(topk->heap, &usable);
    topk->alloc_size -= usable;
    debugAssert(topk->alloc_size == zmalloc_usable_size(topk));

    zfree(topk);
}

static inline int generateAltIdx(fingerprint_t fp, int idx, int numbuckets) {
    return (idx ^ (0x5bd1e995 * (size_t)fp)) & (numbuckets - 1);
}

fpAndIdx generateItemFpAndIdxs(chkTopK *topk, char *item, int itemlen) {
    uint64_t hash = XXH3_64bits_withSeed(item, itemlen, 0);

    fpAndIdx res;
    res.fp = (hash & 0xFFFF); /* Only use 16 bits for fingerprint */

    /* Note numbuckets are a power of 2 so we don't use modulo for index calc */
    res.idx[0] = (hash >> 32) & (topk->numbuckets - 1);
    for (int i = 1; i < CHK_NUM_TABLES; ++i) {
        res.idx[i] = generateAltIdx(res.fp, res.idx[i-1], topk->numbuckets);
    }

    return res;
}

typedef struct {
    int table_idx;
    int pos;
} checkEntryRes;

/* Check if `item` is a heavy entry. If so we bump its count. If not - we make
 * it a heavy entry immediately if there is an empty spot, thus skipping the
 * lobby as an optimization. */
checkEntryRes checkHeavyEntries(chkTopK *topk, fpAndIdx item, counter_t weight) {
    int empty_table_idx = -1;
    int empty_pos = -1;

    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        int idx = item.idx[i];

        chkBucket *bucket = &topk->tables[i][idx];
        for (int j = 0; j < CHK_HEAVY_ENTRIES_PER_BUCKET; ++j) {
            chkHeavyEntry *e = &bucket->heavy_entries[j];
            if (e->count > 0) {
                if (e->fp == item.fp) {
                    e->count += weight;

                    checkEntryRes res = { i, j };
                    return res;
                }
            } else if (empty_table_idx == -1) {
                empty_table_idx = i;
                empty_pos = j;
            }
        }
    }

    if (empty_table_idx == -1) {
        checkEntryRes res = { -1, -1 };
        return res;
    }

    /* If there is an empty slot in the heavy entries just put the item there
     * instead of going through the lobby first (optimization as per the paper) */
    int idx = item.idx[empty_table_idx];
    chkHeavyEntry *e = &topk->tables[empty_table_idx][idx].heavy_entries[empty_pos];
    e->fp = item.fp;
    e->count = weight;

    checkEntryRes res = {empty_table_idx, empty_pos};
    return res;
}

/* A heavy hitter is defined by the paper as an item with counter more or equal
 * to phi * N, where phi is a constant and N is the total count the structure
 * has recorded up to that point */
int isHeavyHitter(chkTopK *topk, counter_t cnt) {
    return cnt >= (topk->total * HEAVY_RATIO);
}

/* After a lobby item is promoted it may be placed on a heavy item's spot. The
 * latter is kicked out, but it may recursively kick out another heavy item.
 * The process is limited by MAX_KICKS and also by the fact that during updates
 * one of the kicked out items may have its counter decayed so much - it's not
 * passing the heavy item threshold (see isHeavyHitter). */
void kickout(chkTopK *topk, chkHeavyEntry entry, int idx, int table_idx) {
    for (int i = 0; i < MAX_KICKS; ++i) {
        /* Do not try to swap with any entries if we don't reach the heavy
         * hitter threshold */
        if (!isHeavyHitter(topk, entry.count)) return;

        /* Find the heavy entry in the alt bucket in the other table with
         * minimum count. If there is empty entry there just occupy it, else
         * recursively kick the minimal one out.
         * To find the alt bucket we need to compute the alt index from the
         * fingerprint of the kicked-out entry. */
        table_idx = 1 - table_idx;
        idx = generateAltIdx(entry.fp, idx, topk->numbuckets);

        chkBucket *bucket = &topk->tables[table_idx][idx];
        counter_t min = (counter_t)-1;
        int min_pos = -1;
        for (int j = 0; j < CHK_HEAVY_ENTRIES_PER_BUCKET; ++j) {
            chkHeavyEntry *e = &bucket->heavy_entries[j];
            if (e->count == 0) {
                *e = entry;
                return;
            }
            if (e->count < min) {
                min = e->count;
                min_pos = j;
            }
        }

        chkHeavyEntry old_entry = bucket->heavy_entries[min_pos];
        bucket->heavy_entries[min_pos] = entry;
        entry = old_entry;
    }
}

/* When a lobby entry's counter passes the promotion threshold we try to promote
 * it with some probability. See the paper for more details. If promotion is
 * successful the lobby entry may kick out a heavy one - see kickout() */
int tryPromoteAndKickout(chkTopK *topk, fpAndIdx item, counter_t new_count,
                         int table_idx)
{
    int idx = item.idx[table_idx];
    chkBucket *bucket = &topk->tables[table_idx][idx];
    counter_t min = (counter_t)-1; /* counter_t is unsigned */
    int min_idx = -1;

    /* We search for heavy item bucket of the promoted lobby entry. We may have
     * an empty space which we immediately occupy. Otherwise we choose the
     * bucket with lowest counter */
    for (int i = 0; i < CHK_HEAVY_ENTRIES_PER_BUCKET; ++i) {
        if (bucket->heavy_entries[i].count == 0) {
            bucket->heavy_entries[i].fp = item.fp;
            bucket->heavy_entries[i].count = new_count;
            return i;
        }
        if (bucket->heavy_entries[i].count < min) {
            min = bucket->heavy_entries[i].count;
            min_idx = i;
        }
    }

    /* If the heavy entry that is going to be kicked out has a counter lower
     * than the lobby's one we always kick it out */
    if (min > new_count) {
        double prob = (new_count - LOBBY_PROMOTION_THRESHOLD) /
                      (double)(min - LOBBY_PROMOTION_THRESHOLD);

        if ((rand() / (double)RAND_MAX) >= prob) return -1;
    }

    chkHeavyEntry to_kickout = bucket->heavy_entries[min_idx];
    /* Note, that here the promoted item keeps the old count as per the paper */
    bucket->heavy_entries[min_idx].fp =  bucket->lobby_entry.fp;

    bucket->lobby_entry.count = 0;
    bucket->lobby_entry.fp = 0;

    kickout(topk, to_kickout, idx, table_idx);

    return min_idx;
}

/* Check if an item is a lobby entry */
checkEntryRes checkLobbyEntries(chkTopK *topk, fpAndIdx item, counter_t weight) {
    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        int idx = item.idx[i];

        chkBucket *bucket = &topk->tables[i][idx];
        chkLobbyEntry *e = &bucket->lobby_entry;

        /* No match or empty lobby entry */
        if (e->fp != item.fp || e->count == 0) continue;

        /* If we don't cross the threshold just update the counter */
        uint64_t new_count = (uint64_t)e->count + weight;
        if (new_count < LOBBY_PROMOTION_THRESHOLD) {
            e->count = (uint16_t)new_count;

            checkEntryRes res = { i, -1 };
            return res;
        }

        /* Try to promote the entry to heavy entry if we crossed the threshold.
         * Else just set the counter to the value of the threshold */
        int kickout_pos = tryPromoteAndKickout(topk, item, new_count, i);
        if (kickout_pos != -1) {
            checkEntryRes res = {i, kickout_pos};
            return res;
        }

        e->count = LOBBY_PROMOTION_THRESHOLD;
        checkEntryRes res = { i, -1 };
        return res;
    }

    checkEntryRes res = { -1, -1 };
    return res;
}

/* Probability to decay cnt with 1.
 * Equal to pow(decay, -cnt) */
static inline double getDecayProb(chkTopK *topk, counter_t cnt) {
    if (cnt < CHK_LUT_SIZE) {
        return topk->lut_decay_prob[cnt];
    }

    return pow(topk->lut_decay_prob[CHK_LUT_SIZE],
               ((double)cnt / (CHK_LUT_SIZE))) *
           topk->lut_decay_prob[cnt % (CHK_LUT_SIZE)];
}

/* Expected decay steps to decay cnt to 0.
 * Equal to sum(pow(decay, i)) for i in [0; cnt] */
static inline double getExpDecayCount(chkTopK *topk, lobby_counter_t cnt) {
    return topk->lut_decay_exp[cnt];
}

/* Expected minimum decay steps to decay cnt with 1. Since probability is
 * pow(decay, -cnt) it's equal to pow(decay, cnt) */
static inline double getMinDecayCount(chkTopK *topk, counter_t cnt) {
    if (cnt < CHK_LUT_SIZE) {
        return topk->lut_min_decay[cnt];
    }

    return pow(topk->lut_min_decay[CHK_LUT_SIZE],
               ((double)cnt / (CHK_LUT_SIZE))) *
           topk->lut_min_decay[cnt % (CHK_LUT_SIZE)];
}

/* When there is a hash-collission between lobby entries we decay the existing
 * lobby entry with the weight of the new one. Return the counter after decaying. */
lobby_counter_t chkDecayCounter(chkTopK *topk, lobby_counter_t cnt, counter_t weight) {
    if (weight == 0) return cnt;

    /* Unweighted update - just decay with probability pow(decay, -cnt) */
    if (weight == 1) {
        double prob = getDecayProb(topk, (counter_t)cnt);
        if ((rand() / (double)RAND_MAX) < prob) {
            return cnt - 1;
        }
        return cnt;
    }

    /* For weighted updates we simulate multiple unweighted ones */

    /* Weight is smaller than the minimum amount of decay steps required to
     * decay the counter with probability of 100% so again we roll the dice */
    double min_decay = getMinDecayCount(topk, cnt);
    if (weight < (counter_t)min_decay) {
        double prob = weight / min_decay;
        if ((rand() / (double)RAND_MAX) < prob) {
            return cnt - 1;
        }
        return cnt;
    }

    /* Weight is more than the expected amount of decay steps to decay the
     * counter to 0. */
    double exp_decays = getExpDecayCount(topk, cnt);
    if (weight >= (counter_t)exp_decays)
        return 0;

    /* Weight is large enough to decay the counter to cnt - X where 0 < X < cnt.
     * We binary search for the largest value `C` such that:
     *
     * (expected decay ops for `C`) >= (expected decay ops for `cnt`) - `weight`
     * i.e lut_decay_exp[C] + weight >= lut_decay_exp[cnt]
     *
     * Note that since cnt is a lobby counter it will necessarily be less or
     * equal than LOBBY_PROMOTION_THRESHOLD, so although we binary search this
     * is a O(1) operation */
    int left = 0;
    int right = cnt;
    while (left < right) {
        int mid = left + (right - left) / 2;

        if (topk->lut_decay_exp[mid] + weight >= topk->lut_decay_exp[cnt]) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }

    return left;
}

/* Update weighted item. If another one was expelled from the topK list -
 * return it. Caller is responsible for releasing it */
sds chkTopKUpdate(chkTopK *topk, char *item, int itemlen, counter_t weight)
{
    if (weight == 0) return NULL;

    topk->total += weight;

    /* Generate a fingerprint and indices for both cuckoo tables. */
    fpAndIdx itemFpIdx = generateItemFpAndIdxs(topk, item, itemlen);

    /* Check if the item is amongst the heavy entries. If so we just update its
     * counter. */
    checkEntryRes res = checkHeavyEntries(topk, itemFpIdx, weight);
    if (res.table_idx != -1) {
        goto update_heap;
    }

    /* If the item is not already heavy it may be in the lobby. If so we'll
     * increase its counter and promote it to a heavy entry if it passes the
     * threshold */
    res = checkLobbyEntries(topk, itemFpIdx, weight);
    if (res.table_idx != -1) {
        goto update_heap;
    }

    /* Item is not tracked at all. Check for empty lobby entries - if there is
     * any - place the item there. The weight may be higher than the promotional
     * threshold in which case we'll try to promote it. */
    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        int idx = itemFpIdx.idx[i];
        chkBucket *bucket = &topk->tables[i][idx];
        if (bucket->lobby_entry.count == 0) {
            bucket->lobby_entry.fp = itemFpIdx.fp;

            res.table_idx = i;
            res.pos = -1;

            if (weight < LOBBY_PROMOTION_THRESHOLD) {
                bucket->lobby_entry.count = weight;
            } else {
                int kickout_pos = tryPromoteAndKickout(topk, itemFpIdx, weight, i);
                if (kickout_pos != -1) {
                    res.pos = kickout_pos;
                } else {
                    bucket->lobby_entry.count = LOBBY_PROMOTION_THRESHOLD;
                }
            }

            goto update_heap;
        }
    }
 
    /* If there are no empty lobby entries choose a table deterministically,
     * decay its lobby counter and update */
    int table_idx = itemFpIdx.fp & 1;
    int idx = itemFpIdx.idx[table_idx];

    chkLobbyEntry *e = &topk->tables[table_idx][idx].lobby_entry;

    /* new_count is the count of `e` after decaying it with weight */
    lobby_counter_t new_count = chkDecayCounter(topk, e->count, weight);

    /* if the chosen lobby entry has decayed its counter to 0, it's replaced by
     * the new entry. Note, in that case the new entry has it's weight
     * decreased by the approximate amount of decay operations needed to decay
     * the old entry. */
    if (new_count == 0) {
        e->fp = itemFpIdx.fp;
        counter_t exp_decay_cnt = getExpDecayCount(topk, e->count);
        e->count = exp_decay_cnt >= weight ?
            1 : (lobby_counter_t)min(255, weight - exp_decay_cnt);
    } else {
        e->count = new_count;
    }

    if (e->count >= LOBBY_PROMOTION_THRESHOLD) {
        int kickout_pos = tryPromoteAndKickout(topk, itemFpIdx, e->count, table_idx);
        if (kickout_pos != -1) {
            res.table_idx = table_idx;
            res.pos = kickout_pos;
        }
    }

    /* After a change in the structure has occurred we check if we also need to
     * update the heap - i.e bump a new item in it, or reorder an old item if
     * it's counter went up. */
update_heap:
    if (res.table_idx == -1 || res.pos == -1)
        return NULL;

    table_idx = res.table_idx;
    idx = itemFpIdx.idx[table_idx];

    counter_t heap_min = topk->heap[0].count;
    chkHeavyEntry *entry = &topk->tables[table_idx][idx].heavy_entries[res.pos];
 
    if (entry->count < heap_min)
        return NULL;
 
    /* Heap uses different hash than the cuckoo tables */
    uint64_t fp = XXH3_64bits_withSeed(item, itemlen, HEAP_SEED);
    chkHeapBucket *itemHeapPtr = chkCheckExistInHeap(topk, item, itemlen, fp);
    if (itemHeapPtr != NULL) {
        itemHeapPtr->count = entry->count;
        chkHeapifyDown(topk->heap, topk->k, itemHeapPtr - topk->heap);
    } else {
        /* We know the new entry has bigger count than the min-element so it's
         * safe to expel it. */
        sds expelled = topk->heap[0].item;
        if (expelled) topk->alloc_size -= sdsAllocSize(expelled);

        topk->heap[0].count = entry->count;
        topk->heap[0].fp = fp;
        topk->heap[0].item = sdsnewlen(item, itemlen);
        topk->alloc_size += sdsAllocSize(topk->heap[0].item);

        chkHeapifyDown(topk->heap, topk->k, 0);
        return expelled;
    }

    return NULL;
}

int cmpchkHeapBucket(const void *tmp1, const void *tmp2) {
    const chkHeapBucket *res1 = tmp1;
    const chkHeapBucket *res2 = tmp2;
    return res1->count < res2->count ? 1 : res1->count > res2->count ? -1 : 0;
}

/* Get an ordered by count list of topk->k elements inside the topk object.
 *
 * NOTE, the returned array is a copy of the internal heap stored by `topk`. The
 * caller is responsible for releasing it after use. The elements of the array
 * share their `item` pointers with the internal topk->heap buckets so one must
 * not use it after `topk` is released. */
chkHeapBucket *chkTopKList(chkTopK *topk) {
    chkHeapBucket *list = zmalloc(sizeof(chkHeapBucket) * topk->k);
    memcpy(list, topk->heap, sizeof(chkHeapBucket) * topk->k);
    qsort(list, topk->k, sizeof(*list), cmpchkHeapBucket);
    return list;
}

size_t chkTopKGetMemoryUsage(chkTopK *topk) {
    if (!topk) return 0;

    return topk->alloc_size;
}

#ifdef REDIS_TEST

#include <stdio.h>
#include "testhelp.h"

#define UNUSED(x) (void)(x)

static int findItemInList(chkHeapBucket *list, int k, const char *item, int itemlen) {
    for (int i = 0; i < k; i++) {
        if (list[i].item != NULL &&
            sdslen(list[i].item) == (size_t)itemlen &&
            memcmp(list[i].item, item, itemlen) == 0) {
            return i;
        }
    }
    return -1;
}

static int verifyListSorted(chkHeapBucket *list, int k) {
    for (int i = 0; i < k - 1; i++) {
        if (list[i].item == NULL) continue;
        if (list[i + 1].item == NULL) continue;
        if (list[i].count < list[i + 1].count) {
            return 0;
        }
    }
    return 1;
}

static void chkTopKUpdateAndFreeExpelled(chkTopK *topk, const char *item, int itemlen, counter_t weight) {
    sds expelled = chkTopKUpdate(topk, (char *)item, itemlen, weight);
    if (expelled) sdsfree(expelled);
}

static void testBasicTopK(void) {
    int k = 5;
    int numbuckets = 64;
    double decay = 0.9;

    chkTopK *topk = chkTopKCreate(k, numbuckets, decay);
    test_cond("Create topk structure", topk != NULL);

    if (topk == NULL) return;

    chkTopKUpdateAndFreeExpelled(topk, "item1", 5, 100);
    chkTopKUpdateAndFreeExpelled(topk, "item2", 5, 200);
    chkTopKUpdateAndFreeExpelled(topk, "item3", 5, 150);
    chkTopKUpdateAndFreeExpelled(topk, "item4", 5, 50);
    chkTopKUpdateAndFreeExpelled(topk, "item5", 5, 300);
    chkTopKUpdateAndFreeExpelled(topk, "item6", 5, 75);

    chkHeapBucket *list = chkTopKList(topk);
    test_cond("chkTopKList returns non-NULL", list != NULL);

    if (list == NULL) {
        chkTopKRelease(topk);
        return;
    }

    test_cond("TopK list is sorted in descending order", verifyListSorted(list, k));

    int idx1 = findItemInList(list, k, "item5", 5);
    int idx2 = findItemInList(list, k, "item2", 5);
    int idx3 = findItemInList(list, k, "item3", 5);

    test_cond("Heaviest items are in the list", idx1 != -1 && idx2 != -1 && idx3 != -1);

    test_cond("item5 has the highest count", idx1 == 0);

    zfree(list);
    chkTopKRelease(topk);
}

static void testHeavierElementsReplaceLighter(void) {
    int k = 5;
    int numbuckets = 64;
    double decay = 0.9;

    chkTopK *topk = chkTopKCreate(k, numbuckets, decay);
    test_cond("Create topk structure for replacement test", topk != NULL);

    if (topk == NULL) return;

    chkTopKUpdateAndFreeExpelled(topk, "light1", 6, 50);
    chkTopKUpdateAndFreeExpelled(topk, "light2", 6, 60);
    chkTopKUpdateAndFreeExpelled(topk, "light3", 6, 70);
    chkTopKUpdateAndFreeExpelled(topk, "light4", 6, 80);
    chkTopKUpdateAndFreeExpelled(topk, "light5", 6, 90);

    chkHeapBucket *list1 = chkTopKList(topk);
    test_cond("Initial topk list is not NULL", list1 != NULL);

    if (list1 == NULL) {
        chkTopKRelease(topk);
        return;
    }

    int light1_idx = findItemInList(list1, k, "light1", 6);
    int light2_idx = findItemInList(list1, k, "light2", 6);
    int light3_idx = findItemInList(list1, k, "light3", 6);
    int light4_idx = findItemInList(list1, k, "light4", 6);
    int light5_idx = findItemInList(list1, k, "light5", 6);

    test_cond("light1 is in initial topk list", light1_idx != -1);
    test_cond("light2 is in initial topk list", light2_idx != -1);
    test_cond("light3 is in initial topk list", light3_idx != -1);
    test_cond("light4 is in initial topk list", light4_idx != -1);
    test_cond("light5 is in initial topk list", light5_idx != -1);

    zfree(list1);

    chkTopKUpdateAndFreeExpelled(topk, "heavy1", 6, 500);
    chkTopKUpdateAndFreeExpelled(topk, "heavy2", 6, 600);

    chkHeapBucket *list2 = chkTopKList(topk);
    test_cond("Updated topk list is not NULL", list2 != NULL);

    if (list2 == NULL) {
        chkTopKRelease(topk);
        return;
    }

    int heavy1_idx = findItemInList(list2, k, "heavy1", 6);
    int heavy2_idx = findItemInList(list2, k, "heavy2", 6);

    test_cond("heavy1 is in updated topk list", heavy1_idx != -1);
    test_cond("heavy2 is in updated topk list", heavy2_idx != -1);

    light1_idx = findItemInList(list2, k, "light1", 6);
    light2_idx = findItemInList(list2, k, "light2", 6);
    light3_idx = findItemInList(list2, k, "light3", 6);
    light4_idx = findItemInList(list2, k, "light4", 6);
    light5_idx = findItemInList(list2, k, "light5", 6);

    int light_items_remaining = (light1_idx != -1 ? 1 : 0) +
                                (light2_idx != -1 ? 1 : 0) +
                                (light3_idx != -1 ? 1 : 0) +
                                (light4_idx != -1 ? 1 : 0) +
                                (light5_idx != -1 ? 1 : 0);

    test_cond("Some lighter items remain in the list after adding heavier ones",
              light_items_remaining > 0);

    zfree(list2);
    chkTopKRelease(topk);
}

static void testManySmallWeightUpdates(void) {
    int k = 2;
    int numbuckets = 64;
    double decay = 0.9;

    chkTopK *topk = chkTopKCreate(k, numbuckets, decay);
    test_cond("Create topk structure for small weight updates test", topk != NULL);

    if (topk == NULL) return;

    chkTopKUpdateAndFreeExpelled(topk, "item0", 5, 50);
    chkTopKUpdateAndFreeExpelled(topk, "item1", 5, 100);

    chkHeapBucket *list1 = chkTopKList(topk);
    test_cond("Topk list after adding item0 and item1 is not NULL", list1 != NULL);

    if (list1 == NULL) {
        chkTopKRelease(topk);
        return;
    }

    int item0_idx1 = findItemInList(list1, k, "item0", 5);
    int item1_idx1 = findItemInList(list1, k, "item1", 5);

    test_cond("item0 and item1 are in topk after initial updates",
              item0_idx1 != -1 && item1_idx1 != -1);

    zfree(list1);

    for (int i = 0; i < 100; i++) {
        chkTopKUpdateAndFreeExpelled(topk, "item2", 5, 1);
    }

    chkHeapBucket *list2 = chkTopKList(topk);
    test_cond("Topk list after many small updates is not NULL", list2 != NULL);

    if (list2 == NULL) {
        chkTopKRelease(topk);
        return;
    }

    int item0_idx2 = findItemInList(list2, k, "item0", 5);
    int item1_idx2 = findItemInList(list2, k, "item1", 5);
    int item2_idx2 = findItemInList(list2, k, "item2", 5);

    test_cond("item1 and item2 are in topk, item0 is not",
              item1_idx2 != -1 && item2_idx2 != -1 && item0_idx2 == -1);

    counter_t item1_count = 0;
    counter_t item2_count = 0;
    if (item1_idx2 != -1) item1_count = list2[item1_idx2].count;
    if (item2_idx2 != -1) item2_count = list2[item2_idx2].count;

    test_cond("item1 and item2 have similar weights", item1_count > 0 && item2_count > 0 && 
              (item1_count > item2_count ? item1_count - item2_count : item2_count - item1_count) < 5);

    zfree(list2);
    chkTopKRelease(topk);
}

int chkTopKTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    testBasicTopK();
    testHeavierElementsReplaceLighter();
    testManySmallWeightUpdates();

    return 0;
}

#endif /* REDIS_TEST */
