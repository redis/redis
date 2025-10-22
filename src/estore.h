/*
 * Copyright (c) 2011-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * estore.h -- Expiration Store implementation
 *
 * ESTORE (Expiration Store)
 * =========================
 * 
 * Index-based expiration store implementation. Similar to kvstore, but built
 * on top of ebuckets instead of dict. Items stored in estore must embed an
 * ExpireMeta, enabling efficient active-expiration.
 *
 * Estore is currently used to manage "subexpiry" only for hash objects with
 * field-level expiration (HFE). Each hash with HFE is registered in estore
 * with the earliest expiration time among its fields.
 *
 * USAGE IN REDIS
 * ==============
 * This implementation is used to efficiently track hash objects that have
 * field-level expiration (HFE):
 * - Each hash with HFE is registered with its earliest field expiration time
 * - Enables efficient active expiration of hash fields
 * - Uses Fenwick tree for efficient iteration through non-empty buckets
 * - Supports cluster mode with per-slot buckets
 *
 * IMPLEMENTATION NOTES
 * ====================
 * - Built on top of ebuckets data structure for expiration management
 * - Uses Fenwick tree to track cumulative item counts across buckets
 * - Supports both single bucket (standalone) and multiple buckets (cluster) modes
 * - All operations have O(log n) time complexity for bucket selection
 *
 * STRUCTURE
 * =========
 * - ebArray: Array of ebuckets (one per slot in cluster mode, or just one)
 * - buckets_sizes: Fenwick tree tracking cumulative counts for efficient iteration
 * - bucket_type: EbucketsType defining callbacks for the stored items
 */

#ifndef __ESTORE_H
#define __ESTORE_H

#include "ebuckets.h"
#include "fwtree.h"

/* Forward declaration of the estore structure */
typedef struct _estore estore;

/* Estore API */

estore *estoreCreate(EbucketsType *type, int num_buckets_bits);

void estoreEmpty(estore *es);

int estoreIsEmpty(estore *es);

void estoreRelease(estore *es);

void estoreActiveExpire(estore *es, int eidx, ExpireInfo *info);

uint64_t estoreRemove(estore *es, int eidx, eItem item);

void estoreAdd(estore *es, int eidx, eItem item, uint64_t when);

void estoreUpdate(estore *es, int eidx, eItem item, uint64_t when);

uint64_t estoreSize(estore *es);

ebuckets *estoreGetBuckets(estore *es, int eidx);

int estoreGetFirstNonEmptyBucket(estore *es);

int estoreGetNextNonEmptyBucket(estore *es, int eidx);

void estoreMoveEbuckets(estore *src, estore *dst, int eidx);

/* Hash-specific function to get ExpireMeta from a hash kvobj. 
 * Once we shall have another data-type with subexpiry, we should refactor
 * ExpireMeta to optionally reside as part of kvobj struct */
ExpireMeta *hashGetExpireMeta(const eItem kvobjHash);

#ifdef REDIS_TEST
int estoreTest(int argc, char *argv[], int flags);
#endif

#endif /* __ESTORE_H */
