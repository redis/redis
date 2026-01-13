/*
 * fwtree.c -- FENWICK TREE (Binary Indexed Tree)
 * 
 * Copyright (c) 2011-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "fwtree.h"
#include "zmalloc.h"
#include "redisassert.h"
#include <string.h>

struct _fenwickTree {
    unsigned long long *tree;
    int size_bits;
    int size;
    uint64_t total;
};

/* Create a new Fenwick tree with 2^sizeBits elements (all initialized to 0) */
fenwickTree *fwTreeCreate(int sizeBits) {
    fenwickTree *ft = zmalloc(sizeof(fenwickTree));
    ft->size_bits = sizeBits;
    ft->size = 1 << sizeBits;
    /* Fenwick tree is 1-based, so we need size + 1 elements */
    ft->tree = zcalloc(sizeof(unsigned long long) * (ft->size + 1));
    ft->total = 0;
    return ft;
}

void fwTreeDestroy(fenwickTree *ft) {
    if (!ft) return;
    zfree(ft->tree);
    zfree(ft);
}

/* Query cumulative sum from index 0 to idx (inclusive, 0-based) */
unsigned long long fwTreePrefixSum(fenwickTree *ft, int idx) {
    if (!ft || idx < 0) return 0;
    if (idx >= ft->size) idx = ft->size - 1;

    /* Convert to 1-based indexing */
    idx++;

    unsigned long long sum = 0;
    while (idx > 0) {
        sum += ft->tree[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

/* Update the tree by adding delta to the element at idx (0-based) */
void fwTreeUpdate(fenwickTree *ft, int idx, long long delta) {
    if (!ft || idx < 0 || idx >= ft->size) return;

    /* Convert to 1-based indexing */
    idx++;
    ft->total += delta;

    while (idx <= ft->size) {
        if (delta < 0) {
            assert(ft->tree[idx] >= (unsigned long long)(-delta));
        }
        ft->tree[idx] += delta;
        idx += (idx & -idx);
    }
    debugAssert(ft->total == fwTreePrefixSum(ft, ft->size - 1));
}

/* Find the 0-based index where the cumulative sum first reaches or exceeds target.
 * target should be in range [1..total].
 * Returns the 0-based index, or 0 if target <= 0 or tree is empty.
 */
int fwTreeFindIndex(fenwickTree *ft, unsigned long long target) {
    debugAssert(ft);

    if (target <= 0) return 0;

    int result = 0, bit_mask = 1 << ft->size_bits;
    for (int i = bit_mask; i != 0; i >>= 1) {
        int current = result + i;
        /* When the target index is greater than 'current' node value the we will update
         * the target and search in the 'current' node tree. */
        if (target > ft->tree[current]) {
            target -= ft->tree[current];
            result = current;
        }
    }
    /* Adjust the result to get the correct index:
     * 1. result += 1;
     *    After the calculations, the index of target in the tree should be the next one,
     *    so we should add 1.
     * 2. result -= 1;
     *    Unlike BIT (tree is 1-based), the API uses 0-based indexing, so we need to subtract 1.
     * As the addition and subtraction cancel each other out, we can simply return the result. */
    return result;
}

/* Find the first non-empty index (equivalent to fwTreeFindIndex(ft, 1)) */
int fwTreeFindFirstNonEmpty(fenwickTree *ft) {
    debugAssert(ft);
    return fwTreeFindIndex(ft, 1);
}

/* Find the next non-empty index after idx (0-based).
 * Returns the 0-based index of the next non-empty element, or -1 if no such element exists.
 * If idx is -1, finds the first non-empty index.
 * Time complexity: O(log n)
 */
int fwTreeFindNextNonEmpty(fenwickTree *ft, int idx) {
    if (!ft || idx < 0 || idx >= ft->size) return -1;
    /* Get cumulative sum up to current index */
    unsigned long long next_sum = fwTreePrefixSum(ft, idx) + 1;
    /* Find the index that contains the next key (curr_sum + 1) */
    return (next_sum <= ft->total) ? fwTreeFindIndex(ft, next_sum) : -1;
}

/* Clear all values in the tree */
void fwTreeClear(fenwickTree *ft) {
    debugAssert(ft);
    memset(ft->tree, 0, sizeof(unsigned long long) * (ft->size + 1));
    ft->total = 0;
}

#ifdef REDIS_TEST
#include <stdio.h>

#define TEST(name) printf("%s\n", name);

int fwtreeTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    /* Test basic operations */
    int sizeBits = 3; /*size = 8*/
    fenwickTree *ft = fwTreeCreate(sizeBits);
    assert(ft != NULL);

    TEST("estore - Test updates and queries") {
        fwTreeUpdate(ft, 0, 5);  /* index 0 += 5 */
        fwTreeUpdate(ft, 2, 3);  /* index 2 += 3 */
        fwTreeUpdate(ft, 4, 7);  /* index 4 += 7 */
        fwTreeUpdate(ft, 6, 2);  /* index 6 += 2 */
    }

    TEST("estore - Test cumulative queries") {
        assert(fwTreePrefixSum(ft, 0) == 5);   /* sum[0..0] = 5 */
        assert(fwTreePrefixSum(ft, 1) == 5);   /* sum[0..1] = 5 */
        assert(fwTreePrefixSum(ft, 2) == 8);   /* sum[0..2] = 5+3 = 8 */
        assert(fwTreePrefixSum(ft, 3) == 8);   /* sum[0..3] = 8 */
        assert(fwTreePrefixSum(ft, 4) == 15);  /* sum[0..4] = 5+3+7 = 15 */
        assert(fwTreePrefixSum(ft, 5) == 15);  /* sum[0..5] = 15 */
        assert(fwTreePrefixSum(ft, 6) == 17);  /* sum[0..6] = 5+3+7+2 = 17 */
        assert(fwTreePrefixSum(ft, 7) == 17);  /* sum[0..7] = 17 */
    }



    TEST("estore - Test find_index functionality") {
        assert(fwTreeFindIndex(ft, 1) == 0);  /* target 1 -> index 0 */
        assert(fwTreeFindIndex(ft, 5) == 0);  /* target 5 -> index 0 */
        assert(fwTreeFindIndex(ft, 6) == 2);  /* target 6 -> index 2 */
        assert(fwTreeFindIndex(ft, 8) == 2);  /* target 8 -> index 2 */
        assert(fwTreeFindIndex(ft, 9) == 4);  /* target 9 -> index 4 */
        assert(fwTreeFindIndex(ft, 15) == 4); /* target 15 -> index 4 */
        assert(fwTreeFindIndex(ft, 16) == 6); /* target 16 -> index 6 */
        assert(fwTreeFindIndex(ft, 17) == 6); /* target 17 -> index 6 */
    }

    TEST("estore - Test fwTreeFindNextNonEmpty functionality") {
        /* Current state: indices 0, 2, 4, 6 have values 5, 3, 7, 2 respectively */
        assert(fwTreeFindNextNonEmpty(ft, -1) == -1);  /* Invalid index */
        assert(fwTreeFindNextNonEmpty(ft, 0) == 2);   /* Next after 0 is index 2 */
        assert(fwTreeFindNextNonEmpty(ft, 1) == 2);   /* Next after 1 is index 2 */
        assert(fwTreeFindNextNonEmpty(ft, 2) == 4);   /* Next after 2 is index 4 */
        assert(fwTreeFindNextNonEmpty(ft, 3) == 4);   /* Next after 3 is index 4 */
        assert(fwTreeFindNextNonEmpty(ft, 4) == 6);   /* Next after 4 is index 6 */
        assert(fwTreeFindNextNonEmpty(ft, 5) == 6);   /* Next after 5 is index 6 */
        assert(fwTreeFindNextNonEmpty(ft, 6) == -1);  /* No next after 6 */
        assert(fwTreeFindNextNonEmpty(ft, 7) == -1);  /* No next after 7 */
    }

    TEST("estore - Test negative updates") {
        fwTreeUpdate(ft, 2, -1);  /* index 2 -= 1 */
        assert(fwTreePrefixSum(ft, 2) == 7);   /* sum[0..2] = 5+2 = 7 */
        assert(fwTreePrefixSum(ft, 7) == 16);  /* total = 16 */
    }

    TEST("estore - Test making an index empty") {
        fwTreeUpdate(ft, 2, -2);  /* index 2 -= 2, should become empty */
        assert(fwTreePrefixSum(ft, 2) == 5);   /* sum[0..2] = 5+0 = 5 */
    }
    
    TEST("estore - Test fwTreeFindNextNonEmpty after making index 2 empty") {
        /* Current state: indices 0, 4, 6 have values 5, 7, 2 respectively (index 2 is now empty) */
        assert(fwTreeFindNextNonEmpty(ft, 0) == 4);   /* Next after 0 is now index 4 (skipping empty 2) */
        assert(fwTreeFindNextNonEmpty(ft, 1) == 4);   /* Next after 1 is index 4 */
        assert(fwTreeFindNextNonEmpty(ft, 2) == 4);   /* Next after 2 is index 4 */
        assert(fwTreeFindNextNonEmpty(ft, 3) == 4);   /* Next after 3 is index 4 */
    }

    TEST("estore - Operations on empty tree") {
        fwTreeClear(ft);
        assert(fwTreePrefixSum(ft, 7) == 0);
    
        /* Test fwTreeFindNextNonEmpty on empty tree */
        assert(fwTreeFindNextNonEmpty(ft, -1) == -1);  /* Empty tree */
        assert(fwTreeFindNextNonEmpty(ft, 0) == -1);   /* Empty tree */
    }

    fwTreeDestroy(ft);

    TEST("estore - misc") {
        ft = fwTreeCreate(0);
        
        fwTreeUpdate(ft, 0, 10);  /* add 10 to index 0 */
        assert(fwTreePrefixSum(ft, 0) == 10);
    
        assert(fwTreeFindIndex(ft, 5) == 0);
    
        /* Test fwTreeFindNextNonEmpty on single element tree */
        assert(fwTreeFindNextNonEmpty(ft, -1) == -1);  /* Invalid index */
        assert(fwTreeFindNextNonEmpty(ft, 0) == -1);   /* No next after 0 in single element tree */
    
        fwTreeDestroy(ft);
    }
    
    return 0;
}

#endif
