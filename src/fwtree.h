/*
 * Copyright (c) 2011-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 * 
 *
 * FENWICK TREE (Binary Indexed Tree)
 * ----------------------------------
 * A Fenwick tree is a data structure that efficiently supports:
 * - Point updates: Add/subtract values at specific indices in O(log n) time
 * - Prefix sum queries: Calculate cumulative sums from index 0 to any index in O(log n) time
 * - Range queries: Calculate sums over any range [i,j] in O(log n) time
 * - Space complexity: O(n)
 *
 * USAGE IN REDIS
 * --------------
 * This implementation is used by KVSTORE and ESTORE to efficiently track:
 * - Cumulative key counts across dictionary slots (KVSTORE)
 * - Cumulative item counts across expiration buckets (ESTORE)
 *
 * This enables efficient operations like:
 * - Finding which dictionary/bucket contains the Nth key/item
 * - Iterating through non-empty dictionaries/buckets
 * - Load balancing and random key selection
 *
 * IMPLEMENTATION NOTES
 * -------------------
 * - The tree uses 1-based indexing internally for mathematical convenience
 * - The public API uses 0-based indexing for consistency with Redis codebase
 * - Tree size must be a power of 2 (specified as sizeBits where size = 2^sizeBits)
 * - All operations have O(log n) time complexity where n is the tree size
 *
 * REFERENCES
 * ----------
 * For more details on Fenwick trees: https://en.wikipedia.org/wiki/Fenwick_tree
 */

#ifndef __FWTREE_H
#define __FWTREE_H

#include <stdint.h>

/* Forward declaration of the fenwick tree structure */
typedef struct _fenwickTree fenwickTree;

/* Fenwick Tree API */

fenwickTree *fwTreeCreate(int sizeBits);

void fwTreeDestroy(fenwickTree *ft);

unsigned long long fwTreePrefixSum(fenwickTree *ft, int idx);

void fwTreeUpdate(fenwickTree *ft, int idx, long long delta);

int fwTreeFindIndex(fenwickTree *ft, unsigned long long target);

int fwTreeFindFirstNonEmpty(fenwickTree *ft);

int fwTreeFindNextNonEmpty(fenwickTree *ft, int idx);

void fwTreeClear(fenwickTree *ft);

#ifdef REDIS_TEST
int fwtreeTest(int argc, char *argv[], int flags);
#endif

#endif /* __FWTREE_H */
