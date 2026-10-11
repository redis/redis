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

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "zmalloc.h"
#define TOPK_CALLOC(count, size) zcalloc_num(count, size)
#define TOPK_TRYCALLOC(count, size) ztrycalloc((size_t)(count) * (size))
#define TOPK_FREE(ptr) zfree(ptr)

#define TOPK_DECAY_LOOKUP_TABLE 256

typedef uint32_t counter_t;

typedef struct HeapBucket {
    uint32_t fp;
    uint32_t itemlen;
    char *item;
    counter_t count;
} HeapBucket;

typedef struct Bucket {
    uint32_t fp; //  fingerprint
    counter_t count;
} Bucket;

typedef struct topk {
    uint32_t k;
    uint32_t width;
    uint32_t depth;
    double decay;

    Bucket *data;
    HeapBucket *heap;
    double lookupTable[TOPK_DECAY_LOOKUP_TABLE];
    //  TODO: add function pointers for fast vs accurate
} TopK;

/*  Returns a new Top-K DS which will keep to 'k' heavyhitter, using
    'depth' arrays of 'width' counters at 'decay' rate.
    Complexity - O(1) */
TopK *TopK_Create(uint32_t k, uint32_t width, uint32_t depth, double decay);

/*  Releases resources of a Top-K DS.
    Complexity - O(k) */
void TopK_Destroy(TopK *topk);

/*  Inserts an 'item' with length 'itemlen' into 'topk' DS.
    Return value is NULL if no change to Top-K list occurred else,
    it returns the item expelled from list. If returned pointer
    is not NULL, it should be free()d.
    Complexity - O(k) */
char *TopK_Add(TopK *topk, const char *item, size_t itemlen, uint32_t increment);

/*  Checks whether an 'item' is in Top-K list of 'topk'.
    Complexity - O(k) */
bool TopK_Query(TopK *topk, const char *item, size_t itemlen);

/*  Returns count for an 'item' in 'topk' DS.
    This number can be significantly lower than real count.
    Complexity - O(k) */
size_t TopK_Count(TopK *topk, const char *item, size_t itemlen);

/*  Returns full 'heapList' of items in 'topk' DS. */
HeapBucket *TopK_List(TopK *topk);
