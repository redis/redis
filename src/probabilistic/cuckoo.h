/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#pragma once

#include "murmurhash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define CUCKOO_BKTSIZE 2
#define CUCKOO_NULLFP 0

typedef uint8_t CuckooFingerprint;
typedef uint64_t CuckooHash;
typedef uint8_t CuckooBucket[1];
typedef uint8_t MyCuckooBucket;

#define CF_MAX_BUCKET_SIZE 255
#define CF_MAX_EXPANSION 32768

#define CF_MAX_NUM_BUCKETS (0x00FFFFFFFFFFFFFFULL) // 56 bits, see struct SubCF

typedef struct {
    uint64_t numBuckets : 56;
    uint64_t bucketSize : 8;
    MyCuckooBucket *data;
} SubCF;

typedef struct {
    uint64_t numBuckets;
    uint64_t numItems;
    uint64_t numDeletes;
    uint16_t numFilters;
    uint16_t bucketSize;
    uint16_t maxIterations;
    uint16_t expansion;
    SubCF *filters;
} CuckooFilter;

#define CUCKOO_GEN_HASH(s, n) MurmurHash64A(s, n, 0)

typedef struct {
    uint64_t i1;
    uint64_t i2;
    CuckooFingerprint fp;
} CuckooKey;

typedef enum {
    CuckooInsert_Inserted = 1,
    CuckooInsert_Exists = 0,
    CuckooInsert_NoSpace = -1,
    CuckooInsert_MemAllocFailed = -2
} CuckooInsertStatus;

enum CuckooRc {
    CUCKOO_OK = 0,
    CUCKOO_ERR = -1,
    CUCKOO_OOM = -2,
};

int CuckooFilter_Init(CuckooFilter *filter, uint64_t capacity, uint16_t bucketSize,
                      uint16_t maxIterations, uint16_t expansion);
void CuckooFilter_Free(CuckooFilter *filter);
CuckooInsertStatus CuckooFilter_InsertUnique(CuckooFilter *filter, CuckooHash hash);
CuckooInsertStatus CuckooFilter_Insert(CuckooFilter *filter, CuckooHash hash);
int CuckooFilter_Delete(CuckooFilter *filter, CuckooHash hash);
int CuckooFilter_Check(const CuckooFilter *filter, CuckooHash hash);
uint64_t CuckooFilter_Count(const CuckooFilter *filter, CuckooHash);
void CuckooFilter_Compact(CuckooFilter *filter, bool cont);
void CuckooFilter_GetInfo(const CuckooFilter *cf, CuckooHash hash, CuckooKey *out);
int CuckooFilter_ValidateIntegrity(const CuckooFilter *cf);

#ifdef REDIS_TEST
int cuckooTest(int argc, char **argv, int flags);
#endif
