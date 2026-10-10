
#include "cuckoo.h"
#include "../bloom/unit_support.h"
#include "bloom_murmur.h"


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define DEFAULT_BUCKETSIZE 2


TEST_CLASS(cuckoo)
TEST_DEFINE_GLOBALS();

TEST_F(cuckoo, testBasicOps) {

    CuckooFilter ck;
    CuckooFilter_Init(&ck, 50, DEFAULT_BUCKETSIZE, 500, 1);
    ASSERT_EQ(0, ck.numItems);
    ASSERT_EQ(1, ck.numFilters);
    // ASSERT_EQ(16, ck.numBuckets);

    CuckooHash kfoo = CUCKOO_GEN_HASH("foo", 3);
    CuckooHash kbar = CUCKOO_GEN_HASH("bar", 3);
    CuckooHash kbaz = CUCKOO_GEN_HASH("baz", 3);

    ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_Insert(&ck, kfoo));
    ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_Insert(&ck, kbar));
    ASSERT_EQ(1, CuckooFilter_Check(&ck, kfoo));
    ASSERT_EQ(1, CuckooFilter_Check(&ck, kbar));
    ASSERT_NE(1, CuckooFilter_Check(&ck, kbaz));
    ASSERT_EQ(2, ck.numItems);

    ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_InsertUnique(&ck, kbaz));
    ASSERT_EQ(3, ck.numItems);

    ASSERT_EQ(CuckooInsert_Exists, CuckooFilter_InsertUnique(&ck, kfoo));

    // Try deleting items
    ASSERT_NE(0, CuckooFilter_Delete(&ck, kfoo));
    ASSERT_EQ(2, ck.numItems);
    ASSERT_EQ(0, CuckooFilter_Check(&ck, kfoo));

    ASSERT_EQ(0, CuckooFilter_Delete(&ck, kfoo));
    ASSERT_EQ(2, ck.numItems);

    CuckooFilter_Free(&ck);

    // Try capacity < numBuckets == 1
    CuckooFilter_Init(&ck, 8, 32, 500, 1);
    ASSERT_EQ(1, ck.numBuckets);
    CuckooFilter_Free(&ck);
}

TEST_F(cuckoo, testCount) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, 10, DEFAULT_BUCKETSIZE, 500, 1);
    CuckooHash kfoo = CUCKOO_GEN_HASH("foo", 3);

    ASSERT_EQ(0, CuckooFilter_Count(&ck, kfoo));

    ASSERT_EQ(0, ck.numItems);
    ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_Insert(&ck, kfoo));

    ASSERT_EQ(1, ck.numItems);
    ASSERT_EQ(1, CuckooFilter_Count(&ck, kfoo));

    ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_Insert(&ck, kfoo));
    ASSERT_EQ(2, CuckooFilter_Count(&ck, kfoo));

    for (size_t ii = 0; ii < 8; ++ii) {
        ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_Insert(&ck, kfoo));
        ASSERT_EQ(3 + ii, CuckooFilter_Count(&ck, kfoo));
    }
    ASSERT_EQ(10, ck.numItems);
    CuckooFilter_Free(&ck);
}

#define NUM_BULK 10000

TEST_F(cuckoo, testRelocations) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 2, 4, 5, 1);
    ASSERT_EQ(0, ck.numItems);
    ASSERT_EQ(1, ck.numFilters);

    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        size_t jj;
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_Insert(&ck, hash));
        for(jj = 0; jj < ii; ++jj) {
            CuckooHash hashjj = CUCKOO_GEN_HASH(&jj, sizeof jj);
            ASSERT_NE(0, CuckooFilter_Check(&ck, hashjj));
        }
    }

    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        ASSERT_NE(0, CuckooFilter_Check(&ck, hash));
    }

    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        CuckooHash hash = CUCKOO_GEN_HASH(&ii, sizeof ii);
        ASSERT_EQ(CuckooInsert_Exists, CuckooFilter_InsertUnique(&ck, hash));
        ASSERT_EQ(CuckooInsert_Inserted, CuckooFilter_Insert(&ck, hash));
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
        ASSERT_NE(count, 0);
        if (count > 1) {
            ret++;
        }
    }
    return ret;
}

TEST_F(cuckoo, testFPR) {
    // We should never expect > 3% FPR (False positive rate) on a single filter.
    // The basic idea is that the false positive rate doubles with each
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK, DEFAULT_BUCKETSIZE, 500, 1);
    ASSERT_EQ(0, ck.numItems);
    ASSERT_EQ(1, ck.numFilters);
    ASSERT_EQ(16384, ck.numBuckets * ck.bucketSize);

    doFill(&ck);
    ASSERT_EQ(1, ck.numFilters);
    ASSERT_EQ(NUM_BULK, ck.numItems);
    ASSERT_LE((double)countColls(&ck), (double)NUM_BULK * 0.015);
    CuckooFilter_Free(&ck);

    // Try again
    CuckooFilter_Init(&ck, NUM_BULK / 2, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    ASSERT_EQ(NUM_BULK, ck.numItems);
    ASSERT_LE((double)countColls(&ck), (double)NUM_BULK * 0.03);
    CuckooFilter_Free(&ck);

    CuckooFilter_Init(&ck, NUM_BULK / 4, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    ASSERT_EQ(NUM_BULK, ck.numItems);
    ASSERT_LE((double)countColls(&ck), (double)NUM_BULK * 0.06);
    CuckooFilter_Free(&ck);

    CuckooFilter_Init(&ck, NUM_BULK / 8, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    ASSERT_EQ(NUM_BULK, ck.numItems);
    ASSERT_LE((double)countColls(&ck), (double)NUM_BULK * 0.08);
    CuckooFilter_Free(&ck);
}

TEST_F(cuckoo, testBulkDel) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 8, DEFAULT_BUCKETSIZE, 500, 1);
    doFill(&ck);
    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        ASSERT_EQ(1, CuckooFilter_Delete(&ck, CUCKOO_GEN_HASH(&ii, sizeof ii)));
    }
    ASSERT_EQ(0, ck.numItems);
    CuckooFilter_Free(&ck);
}

TEST_F(cuckoo, testBulkDelwithExpansion) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 8, DEFAULT_BUCKETSIZE, 500, 2);
    doFill(&ck);
    for (size_t ii = 0; ii < NUM_BULK; ++ii) {
        ASSERT_EQ(1, CuckooFilter_Delete(&ck, CUCKOO_GEN_HASH(&ii, sizeof ii)));
    }
    ASSERT_EQ(0, ck.numItems);
    CuckooFilter_Free(&ck);
}

TEST_F(cuckoo, testBucketSize) {
    CuckooFilter ck;
    CuckooFilter_Init(&ck, NUM_BULK / 10, 1, 50, 1);
    doFill(&ck);
    ASSERT_EQ(1, ck.bucketSize);
    ASSERT_EQ(12, ck.numFilters);
    CuckooFilter_Free(&ck);
    CuckooFilter_Init(&ck, NUM_BULK / 10, 2, 50, 1);
    doFill(&ck);
    ASSERT_EQ(2, ck.bucketSize);
    ASSERT_EQ(11, ck.numFilters);
    CuckooFilter_Free(&ck);
    CuckooFilter_Init(&ck, NUM_BULK / 10, 4, 50, 1);
    doFill(&ck);
    ASSERT_EQ(4, ck.bucketSize);
    ASSERT_EQ(10, ck.numFilters);
    CuckooFilter_Free(&ck);
}

TEST_F(cuckoo, testValidationSecurity) {
    // Test the security vulnerability fix for expansion=0 with multiple filters
    CuckooFilter ck;

    // Test case 1: Valid filter should pass validation
    memset(&ck, 0, sizeof(ck));
    ck.bucketSize = 2;
    ck.numFilters = 2;
    ck.maxIterations = 20;
    ck.expansion = 1;  // Valid expansion
    ck.numBuckets = 16; // Power of 2

    ASSERT_EQ(0, CuckooFilter_ValidateIntegrity(&ck));  // Should pass

    // Test case 2: Invalid filter with expansion=0 and numFilters>1 should fail
    ck.expansion = 0;  // This causes pow(0, 1) = 0 buckets
    ck.numFilters = 2; // Multiple filters trigger the vulnerability

    ASSERT_EQ(1, CuckooFilter_ValidateIntegrity(&ck));  // Should fail

    // Test case 3: expansion=0 with numFilters=1 should be OK (no pow calculation issue)
    ck.expansion = 0;
    ck.numFilters = 1;

    ASSERT_EQ(0, CuckooFilter_ValidateIntegrity(&ck));  // Should pass

    // Test case 4: Other validation failures
    ck.expansion = 1;
    ck.numFilters = 2;
    ck.numBuckets = 0;  // Invalid - zero buckets

    ASSERT_EQ(1, CuckooFilter_ValidateIntegrity(&ck));  // Should fail

    // Test case 5: numBuckets not power of 2
    ck.numBuckets = 15;  // Not power of 2

    ASSERT_EQ(1, CuckooFilter_ValidateIntegrity(&ck));  // Should fail

    // Test case 6: Null pointer
    ASSERT_EQ(1, CuckooFilter_ValidateIntegrity(NULL));  // Should fail
}

TEST_F(cuckoo, testMalformedHeaderProtection) {
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
    ASSERT_EQ(1, CuckooFilter_ValidateIntegrity(&ck));  // Should fail

    // Test with various other dangerous expansion/numFilters combinations
    for (int filters = 2; filters <= 5; filters++) {
        ck.numFilters = filters;
        ck.expansion = 0;
        ASSERT_EQ(1, CuckooFilter_ValidateIntegrity(&ck));  // Should always fail
    }
}

int main(void) {
    test__abort_on_fail = 1;

    TEST_RUN_ALL_TESTS();
    return 0;
}

void _serverAssert(const char *expression, const char *file, int line) {
    fprintf(stderr, "%s:%d: %s\n", file, line, expression);
    abort();
}
