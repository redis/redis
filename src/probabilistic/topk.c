/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Implementation by Ariel Shtul
 *
 */

#include "topk.h"
#include "bloom_murmur.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define TOPK_HASH(item, itemlen, i) MurmurHash2(item, itemlen, i)
#define GA 1919

static inline uint32_t max(uint32_t a, uint32_t b) { return a > b ? a : b; }

static inline char *topKStrndup(const char *s, size_t n) {
    char *ret = TOPK_CALLOC(n + 1, sizeof(char));
    if (ret)
        memcpy(ret, s, n);
    return ret;
}

void heapifyDown(HeapBucket *array, size_t len, size_t start) {
    size_t child = start;

    // check whether larger than children
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

    // swap while larger than child
    HeapBucket top = {0};
    memcpy(&top, &array[start], sizeof(HeapBucket));
    do {
        memcpy(&array[start], &array[child], sizeof(HeapBucket));
        start = child;

        if ((len - 2) / 2 < child) {
            break;
        }
        child = 2 * child + 1;

        if ((child + 1) < len && (array[child].count > array[child + 1].count)) {
            ++child;
        }
    } while (array[child].count < top.count);
    memcpy(&array[start], &top, sizeof(HeapBucket));
}

TopK *TopK_Create(uint32_t k, uint32_t width, uint32_t depth, double decay) {
    assert(k > 0);
    assert(width > 0);
    assert(depth > 0);
    assert(decay > 0 && decay <= 1);

    if (depth > SIZE_MAX / width || (size_t)depth * width > SIZE_MAX / sizeof(Bucket) ||
        (uint64_t)k * sizeof(HeapBucket) > SIZE_MAX) {
        return NULL;
    }

    TopK *topk = (TopK *)TOPK_CALLOC(1, sizeof(TopK));
    topk->k = k;
    topk->width = width;
    topk->depth = depth;
    topk->decay = decay;
    topk->data = TOPK_TRYCALLOC(((size_t)width) * depth, sizeof(Bucket));
    if (!topk->data) {
        TOPK_FREE(topk);
        return NULL;
    }

    topk->heap = TOPK_TRYCALLOC(k, sizeof(HeapBucket));
    if (!topk->heap) {
        TOPK_FREE(topk->data);
        TOPK_FREE(topk);
        return NULL;
    }

    for (uint32_t i = 0; i < TOPK_DECAY_LOOKUP_TABLE; ++i) {
        topk->lookupTable[i] = pow(decay, i);
    }

    return topk;
}

void TopK_Destroy(TopK *topk) {
    if (!topk) {
        return;
    }

    if (topk->heap) {
        for (uint32_t i = 0; i < topk->k; ++i) {
            if (topk->heap[i].item) {
                TOPK_FREE(topk->heap[i].item);
            }
        }

        TOPK_FREE(topk->heap);
        topk->heap = NULL;
    }
    if (topk->data) {
        TOPK_FREE(topk->data);
        topk->data = NULL;
    }
    TOPK_FREE(topk);
}

// Complexity O(k + strlen)
static HeapBucket *checkExistInHeap(TopK *topk, const char *item, size_t itemlen) {
    uint32_t fp = TOPK_HASH(item, itemlen, GA);
    HeapBucket *runner = topk->heap;

    for (uint32_t i = 0; i < topk->k; ++i)
        if ((runner + i)->item && fp == (runner + i)->fp && itemlen == (runner + i)->itemlen &&
            memcmp((runner + i)->item, item, itemlen) == 0) {
            return runner + i;
        }
    return NULL;
}

char *TopK_Add(TopK *topk, const char *item, size_t itemlen, uint32_t increment) {
    assert(topk);
    assert(item);

    Bucket *runner;
    counter_t *countPtr;
    counter_t maxCount = 0;
    uint32_t fp = TOPK_HASH(item, itemlen, GA);

    counter_t heapMin = topk->heap->count;

    // get max item count
    for (uint32_t i = 0; i < topk->depth; ++i) {
        uint32_t loc = TOPK_HASH(item, itemlen, i) % topk->width;
        runner = topk->data + (size_t)i * topk->width + loc;
        countPtr = &runner->count;
        if (*countPtr == 0) {
            runner->fp = fp;
            *countPtr = increment;
            maxCount = max(maxCount, *countPtr);
        } else if (runner->fp == fp) {
            *countPtr += increment;
            maxCount = max(maxCount, *countPtr);
        } else {
            uint32_t local_incr = increment;
            for (; local_incr > 0; --local_incr) {
                double decay;
                if (*countPtr < TOPK_DECAY_LOOKUP_TABLE) {
                    decay = topk->lookupTable[*countPtr];
                } else {
                    //  using precalculate lookup table to save cpu
                    decay = pow(topk->lookupTable[TOPK_DECAY_LOOKUP_TABLE - 1],
                                (*countPtr / (TOPK_DECAY_LOOKUP_TABLE - 1))) *
                            topk->lookupTable[*countPtr % (TOPK_DECAY_LOOKUP_TABLE - 1)];
                }
                double chance = rand() / (double)RAND_MAX;
                if (chance < decay) {
                    --*countPtr;
                    if (*countPtr == 0) {
                        runner->fp = fp;
                        *countPtr = local_incr;
                        maxCount = max(maxCount, *countPtr);
                        break;
                    }
                }
            }
        }
    }

    // update heap
    if (maxCount >= heapMin) {
        HeapBucket *itemHeapPtr = checkExistInHeap(topk, item, itemlen);
        if (itemHeapPtr != NULL) {
            itemHeapPtr->count = maxCount; // Not max of the two, as it might have been decayed
            heapifyDown(topk->heap, topk->k, itemHeapPtr - topk->heap);
        } else {
            // TOPK_FREE(topk->heap[0].item);
            char *expelled = topk->heap[0].item;

            topk->heap[0].count = maxCount;
            topk->heap[0].fp = fp;
            topk->heap[0].item = topKStrndup(item, itemlen);
            topk->heap[0].itemlen = itemlen;
            heapifyDown(topk->heap, topk->k, 0);
            return expelled;
        }
    }
    return NULL;
}

bool TopK_Query(TopK *topk, const char *item, size_t itemlen) {
    return checkExistInHeap(topk, item, itemlen) != NULL;
}

size_t TopK_Count(TopK *topk, const char *item, size_t itemlen) {
    assert(topk);
    assert(item);

    Bucket *runner = NULL;
    uint32_t fp = TOPK_HASH(item, itemlen, GA);
    // TODO: The optimization of >heapMin should be revisited for performance
    counter_t heapMin = topk->heap->count;
    HeapBucket *heapPtr = checkExistInHeap(topk, item, itemlen);
    counter_t res = 0;

    for (uint32_t i = 0; i < topk->depth; ++i) {
        uint32_t loc = TOPK_HASH(item, itemlen, i) % topk->width;
        runner = topk->data + (size_t)i * topk->width + loc;
        if (runner->fp == fp && (heapPtr == NULL || runner->count >= heapMin)) {
            res = max(res, runner->count);
        }
    }
    return res;
}

int cmpHeapBucket(const void *tmp1, const void *tmp2) {
    const HeapBucket *res1 = tmp1;
    const HeapBucket *res2 = tmp2;
    return res1->count < res2->count ? 1 : res1->count > res2->count ? -1 : 0;
}

HeapBucket *TopK_List(TopK *topk) {
    HeapBucket *heapList = TOPK_CALLOC(topk->k, (sizeof(*heapList)));
    memcpy(heapList, topk->heap, topk->k * sizeof(HeapBucket));
    qsort(heapList, topk->k, sizeof(*heapList), cmpHeapBucket);
    return heapList;
}
