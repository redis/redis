/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Ported from RedisBloom tests/unit/test-cuckoo.c. */
#ifdef REDIS_TEST
#include "cuckoo.h"
#include "zmalloc.h"
#include "testhelp.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_BUCKETSIZE 2


static void testBasicOps(void) {

    CuckooFilter ck;
    CuckooFilter_Init(&ck, 50, DEFAULT_BUCKETSIZE, 500, 1);
    assert((0) == (ck.numItems));
    assert((1) == (ck.numFilters));
    // assert((16) == (ck.numBuckets));

    CuckooHash kfoo = CUCKOO_GEN_HASH("foo", 3);
    CuckooHash kbar = CUCKOO_GEN_HASH("bar", 3);
    CuckooHash kbaz = CUCKOO_GEN_HASH("baz", 3);

    assert((CuckooInsert_Inserted) == (CuckooFilter_Insert(&ck, kfoo)));
    assert((CuckooInsert_Inserted) == (CuckooFilter_Insert(&ck, kbar)));
    assert((1) == (CuckooFilter_Check(&ck, kfoo)));
    assert((1) == (CuckooFilter_Check(&ck, kbar)));
    assert((1) != (CuckooFilter_Check(&ck, kbaz)));
    assert((2) == (ck.numItems));

    assert((CuckooInsert_Inserted) == (CuckooFilter_InsertUnique(&ck, kbaz)));
    assert((3) == (ck.numItems));

    assert((CuckooInsert_Exists) == (CuckooFilter_InsertUnique(&ck, kfoo)));

    // Try deleting items
    assert((0) != (CuckooFilter_Delete(&ck, kfoo)));
    assert((2) == (ck.numItems));
    assert((0) == (CuckooFilter_Check(&ck, kfoo)));

    assert((0) == (CuckooFilter_Delete(&ck, kfoo)));
    assert((2) == (ck.numItems));

    CuckooFilter_Free(&ck);

    // Try capacity < numBuckets == 1
    CuckooFilter_Init(&ck, 8, 32, 500, 1);
    assert((1) == (ck.numBuckets));
    CuckooFilter_Free(&ck);
}

static void testCount(void) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, 10, DEFAULT_BUCKETSIZE, 500, 1);
    CuckooHash kfoo = CUCKOO_GEN_HASH("foo", 3);

    assert((0) == (CuckooFilter_Count(&ck, kfoo)));

    assert((0) == (ck.numItems));
    assert((CuckooInsert_Inserted) == (CuckooFilter_Insert(&ck, kfoo)));

    assert((1) == (ck.numItems));
    assert((1) == (CuckooFilter_Count(&ck, kfoo)));

    assert((CuckooInsert_Inserted) == (CuckooFilter_Insert(&ck, kfoo)));
    assert((2) == (CuckooFilter_Count(&ck, kfoo)));

    for (size_t ii = 0; ii < 8; ++ii) {
        assert((CuckooInsert_Inserted) == (CuckooFilter_Insert(&ck, kfoo)));
        assert((3 + ii) == (CuckooFilter_Count(&ck, kfoo)));
    }
    assert((10) == (ck.numItems));
    CuckooFilter_Free(&ck);
}

#define NUM_BULK 10000

static void testRelocations(void) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 2, 4, 5, 1);
    assert((0) == (ck.numItems));
    assert((1) == (ck.numFilters));

    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        size_t jj;
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        assert((CuckooInsert_Inserted) == (CuckooFilter_Insert(&ck, hash)));
        for(jj = 0; jj < ii; ++jj) {
            CuckooHash hashjj = CUCKOO_GEN_HASH(&jj, sizeof jj);
            assert((0) != (CuckooFilter_Check(&ck, hashjj)));
        }
    }

    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        assert((0) != (CuckooFilter_Check(&ck, hash)));
    }

    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        assert((CuckooInsert_Exists) == (CuckooFilter_InsertUnique(&ck, hash)));
        assert((CuckooInsert_Inserted) == (CuckooFilter_Insert(&ck, hash)));
    }

    CuckooFilter_Free(&ck);
}

static void doFill(CuckooFilter *ck) {
    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        CuckooFilter_Insert(ck, hash);
    }
}

static size_t countColls(CuckooFilter *ck) {
    size_t ret = 0;
    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        size_t count = CuckooFilter_Count(ck, hash);
        assert((count) != (0));
        if (count > 1) {
            ret++;
        }
    }
    return ret;
}

static void testFPR(void) {
    // We should never expect > 3% FPR (False positive rate) on a single filter.
    // The basic idea is that the false positive rate doubles with each
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK, DEFAULT_BUCKETSIZE, 500, 1);
    assert((0) == (ck.numItems));
    assert((1) == (ck.numFilters));
    assert((16384) == (ck.numBuckets * ck.bucketSize));

    doFill(&ck);
    assert((1) == (ck.numFilters));
    assert((NUM_BULK) == (ck.numItems));
    assert(((double)countColls(&ck)) <= ((double)NUM_BULK * 0.015));
    CuckooFilter_Free(&ck);

    // Try again
    CuckooFilter_Init(&ck, NUM_BULK / 2, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    assert((NUM_BULK) == (ck.numItems));
    assert(((double)countColls(&ck)) <= ((double)NUM_BULK * 0.03));
    CuckooFilter_Free(&ck);

    CuckooFilter_Init(&ck, NUM_BULK / 4, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    assert((NUM_BULK) == (ck.numItems));
    assert(((double)countColls(&ck)) <= ((double)NUM_BULK * 0.06));
    CuckooFilter_Free(&ck);

    CuckooFilter_Init(&ck, NUM_BULK / 8, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    assert((NUM_BULK) == (ck.numItems));
    assert(((double)countColls(&ck)) <= ((double)NUM_BULK * 0.08));
    CuckooFilter_Free(&ck);
}

static void testBulkDel(void) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 8, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        assert((1) == (CuckooFilter_Delete(&ck, CUCKOO_GEN_HASH(&ii, sizeof ii))));
    }
    assert((0) == (ck.numItems));
    CuckooFilter_Free(&ck);
}

static void testBulkDelwithExpansion(void) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 8, DEFAULT_BUCKETSIZE, 500, 2);
    doFill(&ck);
    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        assert((1) == (CuckooFilter_Delete(&ck, CUCKOO_GEN_HASH(&ii, sizeof ii))));
    }
    assert((0) == (ck.numItems));
    CuckooFilter_Free(&ck);
}

static void testBucketSize(void) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 10, 1, 50, 1);
    doFill(&ck);
    assert((1) == (ck.bucketSize));
    assert((12) == (ck.numFilters));
    CuckooFilter_Free(&ck);
    CuckooFilter_Init(&ck, NUM_BULK / 10, 2, 50, 1);
    doFill(&ck);
    assert((2) == (ck.bucketSize));
    assert((11) == (ck.numFilters));
    CuckooFilter_Free(&ck);
    CuckooFilter_Init(&ck, NUM_BULK / 10, 4, 50, 1);
    doFill(&ck);
    assert((4) == (ck.bucketSize));
    assert((10) == (ck.numFilters));
    CuckooFilter_Free(&ck);
}

static void testValidationSecurity(void) {
    // Test the security vulnerability fix for expansion=0 with multiple filters
    CuckooFilter ck;

    // Test case 1: Valid filter should pass validation
    memset(&ck, 0, sizeof(ck));
    ck.bucketSize = 2;
    ck.numFilters = 2;
    ck.maxIterations = 20;
    ck.expansion = 1;  // Valid expansion
    ck.numBuckets = 16; // Power of 2

    assert((0) == (CuckooFilter_ValidateIntegrity(&ck)));  // Should pass

    // Test case 2: Invalid filter with expansion=0 and numFilters>1 should fail
    ck.expansion = 0;  // This causes pow(0, 1) = 0 buckets
    ck.numFilters = 2; // Multiple filters trigger the vulnerability

    assert((1) == (CuckooFilter_ValidateIntegrity(&ck)));  // Should fail

    // Test case 3: expansion=0 with numFilters=1 should be OK (no pow calculation issue)
    ck.expansion = 0;
    ck.numFilters = 1;

    assert((0) == (CuckooFilter_ValidateIntegrity(&ck)));  // Should pass

    // Test case 4: Other validation failures
    ck.expansion = 1;
    ck.numFilters = 2;
    ck.numBuckets = 0;  // Invalid - zero buckets

    assert((1) == (CuckooFilter_ValidateIntegrity(&ck)));  // Should fail

    // Test case 5: numBuckets not power of 2
    ck.numBuckets = 15;  // Not power of 2

    assert((1) == (CuckooFilter_ValidateIntegrity(&ck)));  // Should fail

    // Test case 6: Null pointer
    assert((1) == (CuckooFilter_ValidateIntegrity(NULL)));  // Should fail
}

static void testMalformedHeaderProtection(void) {
    // Test protection against malformed headers that could cause crashes

    // Simulate malformed header values that previously caused crashes
    CuckooFilter ck;
    memset(&ck, 0, sizeof(ck));

    // These are the dangerous values from the security vulnerability
    ck.bucketSize = 1;      // Within valid range [1, 255]
    ck.numFilters = 2;      // Within valid range [1, 65536]
    ck.maxIterations = 1;   // Within valid range [1, 65535]
    ck.expansion = 0;       // DANGEROUS: causes pow(0, 1) = 0
    ck.numBuckets = 1;      // Power of 2, > 0

    // This should now be caught by our security fix
    assert((1) == (CuckooFilter_ValidateIntegrity(&ck)));  // Should fail

    // Test with various other dangerous expansion/numFilters combinations
    for (int filters = 2; filters <= 5; filters++) {
        ck.numFilters = filters;
        ck.expansion = 0;
        assert((1) == (CuckooFilter_ValidateIntegrity(&ck)));  // Should always fail
    }
}


/* Golden outputs from RedisBloom f83f0ad, MurmurHash64A_Bloom with seed zero.
 * Cover complete blocks and every tail length using unaligned binary input. */
static void testHashCompatibility(void) {
    static const uint64_t expected[] = {
        UINT64_C(0x0000000000000000),
        UINT64_C(0x5825f5f3bd962979),
        UINT64_C(0xfce4b24e84cc521b),
        UINT64_C(0x0b7a77597d5318d7),
        UINT64_C(0xeebf5cd927142ace),
        UINT64_C(0x56f25b3e60942526),
        UINT64_C(0x38b5d88a56c0bd13),
        UINT64_C(0xb4fdd1625e45f654),
        UINT64_C(0x732b8fbb28a06e1b),
        UINT64_C(0xe3695200d1865a01),
        UINT64_C(0xc6908059faf3762e),
        UINT64_C(0x27dfae7c1efe02eb),
        UINT64_C(0x46874e3fef014ba1),
        UINT64_C(0x6df048e59430ced5),
        UINT64_C(0xe35e974299429660),
        UINT64_C(0xbc9f16e650a6111b),
        UINT64_C(0xad39d486ca323b18),
    };
    unsigned char data[18];
    for (size_t i = 0; i < 17; i++) data[i + 1] = (unsigned char)(i * 37);
    for (size_t len = 0; len < 17; len++) {
        assert(CUCKOO_GEN_HASH(data + 1, len) == expected[len]);
    }
}

int cuckooTest(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    int before_failed = __failed_tests;
    size_t before_memory = zmalloc_used_memory();
    testHashCompatibility();
    test_cond("Cuckoo hash matches RedisBloom", 1);
    testBasicOps();
    test_cond("RedisBloom testBasicOps", 1);
    testCount();
    test_cond("RedisBloom testCount", 1);
    testRelocations();
    test_cond("RedisBloom testRelocations", 1);
    testFPR();
    test_cond("RedisBloom testFPR", 1);
    testBulkDel();
    test_cond("RedisBloom testBulkDel", 1);
    testBulkDelwithExpansion();
    test_cond("RedisBloom testBulkDelwithExpansion", 1);
    testBucketSize();
    test_cond("RedisBloom testBucketSize", 1);
    testValidationSecurity();
    test_cond("RedisBloom testValidationSecurity", 1);
    testMalformedHeaderProtection();
    test_cond("RedisBloom testMalformedHeaderProtection", 1);
    test_cond("Cuckoo tests release all allocations", zmalloc_used_memory() == before_memory);
    return __failed_tests != before_failed;
}
#endif
