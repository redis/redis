/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#pragma once

#include <stdint.h> // uint32_t

#include <stddef.h>
#include "zmalloc.h"
#define CMS_CALLOC(count, size) zcalloc_num(count, size)
#define CMS_TRYCALLOC(count, size) ztrycalloc((count) * (size))
#define CMS_FREE(ptr) zfree(ptr)

#define CMS_DEFAULT_CELL_SIZE 4

#define CMS_IS_VALID_CELL_SIZE(cellSize)                                                           \
    ((cellSize) == 1 || (cellSize) == 2 || (cellSize) == 4 || (cellSize) == 8)

#define CMS_CELL_MAX(cellSize)                                                                     \
    ((cellSize) == 8 ? (uint64_t)INT64_MAX : ((UINT64_C(1) << (8 * (uint64_t)(cellSize))) - 1))

typedef struct CMS {
    size_t width;
    size_t depth;
    void *array;
    uint64_t counter;
    uint8_t cellSize; // bytes per cell: 1, 2, 4 or 8
} CMSketch;

typedef struct {
    CMSketch *dest;
    long long numKeys;
    CMSketch **cmsArray;
    long long *weights;
} mergeParams;

typedef enum {
    CMS_STATUS_OK = 0,
    CMS_STATUS_OVERFLOW,
    CMS_STATUS_UNDERFLOW,
} CMSStatus;

/*  Creates a new Count-Min Sketch with dimensions of width * depth,
    with cellSize bytes per cell */
CMSketch *NewCMSketch(size_t width, size_t depth, uint8_t cellSize);

/*  Recommends width & depth for expected n different items,
    with probability of an error  - prob and over estimation
    error - overEst (use 1 for max accuracy) */
void CMS_DimFromProb(double overEst, double prob, size_t *width, size_t *depth);

void CMS_Destroy(CMSketch *cms);

/*  Changes item count by value. A negative value decrements the count.

    On success returns CMS_STATUS_OK and stores the new estimate in count.
    If the change would take any of the item's cells above the maximum value a
    cell can hold, or below zero, the sketch is left untouched and
    CMS_STATUS_OVERFLOW / CMS_STATUS_UNDERFLOW is returned. */
CMSStatus CMS_IncrBy(CMSketch *cms, const char *item, size_t strlen, int64_t value,
                     uint64_t *count);

/* Returns an estimate counter for item */
uint64_t CMS_Query(CMSketch *cms, const char *item, size_t strlen);

/*  Checks a deserialized sketch for values no command could have produced: a cell
    or a total count above what the cell size allows. RESTORE payloads are caller
    controlled, and the headroom arithmetic in CMS_IncrBy relies on both bounds.

    Returns non-zero if the sketch must be rejected. */
int CMS_ValidateLoaded(const CMSketch *cms);

/*  Merges multiple CMSketches into a single one.
    All sketches must have identical width, depth and cell size.
    dest must be already initialized.

    Returns non-zero if overflow validation fails. In this case,
    merge operation will be aborted with no side effects.
*/
int CMS_Merge(CMSketch *dest, size_t quantity, const CMSketch **src, const long long *weights);
int CMS_MergeParams(mergeParams params);

/* Help function */
void CMS_Print(const CMSketch *cms);
