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
#include "server.h"
#include "t_cuckoo.h"
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

size_t lazyfreeGetFreeEffort(robj *key, robj *obj, int dbid);
size_t kvobjComputeSize(robj *key, kvobj *o, size_t sample_size, int dbid);

/* Force relocation to catch stale pointers even without allocator defrag support. */
static void *cuckooTestRelocate(void *ptr) {
    size_t bytes = zmalloc_size(ptr);
    void *moved = zmalloc(bytes);
    memcpy(moved, ptr, bytes);
    zfree(ptr);
    return moved;
}

uint64_t moduleTypeEncodeId(const char *name, int encver);

/* Synthetic historical module payloads: versions before 4 omitted geometry
 * options and stored equal-sized subfilters. */
static void testCuckooLegacyRdb(void) {
    CuckooFilter cf;
    assert(CuckooFilter_Init(&cf, 64, 2, 20, 1) == 0);
    for (uint64_t i = 0; i < 10; i++)
        assert(CuckooFilter_Insert(&cf, CUCKOO_GEN_HASH(&i, sizeof(i))) == CuckooInsert_Inserted);
    for (int version = 0; version <= 4; version++) {
        rio out, in;
        rioInitWithBuffer(&out, sdsempty());
        uint64_t id = moduleTypeEncodeId("MBbloomCF", version);
        assert(cuckooIsLegacyRdbType(id));
        rdbSaveType(&out, RDB_TYPE_MODULE_2);
        rdbSaveLen(&out, id);
        uint64_t fields[] = {cf.numFilters, cf.numBuckets, cf.numItems, cf.numDeletes,
                             cf.bucketSize, cf.maxIterations, cf.expansion};
        size_t count = version < 4 ? 3 : 7;
        for (size_t i = 0; i < count; i++) {
            rdbSaveLen(&out, RDB_MODULE_OPCODE_UINT);
            rdbSaveLen(&out, fields[i]);
        }
        for (size_t i = 0; i < cf.numFilters; i++) {
            if (version >= 4) {
                rdbSaveLen(&out, RDB_MODULE_OPCODE_UINT);
                rdbSaveLen(&out, cf.filters[i].numBuckets);
            }
            rdbSaveLen(&out, RDB_MODULE_OPCODE_STRING);
            rdbSaveRawString(&out, cf.filters[i].data,
                            cf.filters[i].numBuckets * cf.bucketSize);
        }
        rdbSaveLen(&out, RDB_MODULE_OPCODE_EOF);
        rioInitWithBuffer(&in, out.io.buffer.ptr);
        int type = rdbLoadObjectType(&in), err;
        robj *o = rdbLoadObject(type, &in, NULL, 0, RDB_VERSION, &err);
        int matches = o && o->type == OBJ_CUCKOO && getObjectLength(o) == cf.numItems;
        if (o) {
            for (uint64_t i = 0; i < 10; i++)
                matches &= CuckooFilter_Check(o->ptr, CUCKOO_GEN_HASH(&i, sizeof(i)));
            decrRefCount(o);
        }
        test_cond("Historical Cuckoo module RDB loads as a native filter", matches);
        /* Exercise the loader itself without a DUMP trailer or checksum. */
        int truncated_ok = 1;
        for (size_t cut = 10; cut < sdslen(out.io.buffer.ptr); cut++) {
            sds partial = sdsnewlen(out.io.buffer.ptr, cut);
            rioInitWithBuffer(&in, partial);
            rdbLoadObjectType(&in);
            uint64_t partial_id = rdbLoadLen(&in, NULL);
            int corrupt;
            o = rdbLoadCuckooObject(&in, partial_id, &corrupt);
            if (o) { truncated_ok = 0; decrRefCount(o); }
            sdsfree(partial);
        }
        test_cond("Truncated historical Cuckoo fields are rejected", truncated_ok);
        sdsfree(out.io.buffer.ptr);
    }
    CuckooFilter_Free(&cf);
}

int cuckooObjectTest(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    int before_failed = __failed_tests;
    size_t before_memory = zmalloc_used_memory();
    testCuckooLegacyRdb();
    CuckooFilter *cf = zmalloc(sizeof(*cf));
    assert(CuckooFilter_Init(cf, 8, 2, 20, 2) == 0);
    robj *o = createCuckooObject(cf);
    for (uint64_t i = 0; i < 100; i++)
        assert(CuckooFilter_Insert(cf, CUCKOO_GEN_HASH(&i, sizeof(i))) == CuckooInsert_Inserted);
    test_cond("Cuckoo object type, encoding, length and free effort",
              o->type == OBJ_CUCKOO && o->encoding == OBJ_ENCODING_CUCKOO &&
              !strcmp(strEncoding(o->encoding), "cuckoo") &&
              getObjectLength(o) == 100 && cf->numFilters > 1 &&
              lazyfreeGetFreeEffort(NULL, o, 0) == (size_t)cf->numFilters + 2);

    unsigned char original[20] = {0}, copied[20] = {0};
    cuckooMixDigest(original, o);
    robj *copy = cuckooDup(o);
    cuckooMixDigest(copied, copy);
    test_cond("Cuckoo copy preserves data and metadata", memcmp(original, copied, 20) == 0);
    CuckooFilter *other = copy->ptr;
    int independent = other != cf && other->filters != cf->filters;
    for (size_t i = 0; i < cf->numFilters; i++)
        independent &= other->filters[i].data != cf->filters[i].data;
    other->filters[0].data[0] ^= 1;
    memset(copied, 0, sizeof(copied));
    cuckooMixDigest(copied, copy);
    test_cond("Cuckoo copy has independent allocations and digest detects data changes",
              independent && memcmp(original, copied, 20) != 0);
    other->filters[0].data[0] ^= 1;
    other->numDeletes++;
    memset(copied, 0, sizeof(copied));
    cuckooMixDigest(copied, copy);
    test_cond("Cuckoo digest detects metadata changes", memcmp(original, copied, 20) != 0);
    decrRefCount(copy);

    for (int compression = 0; compression <= 1; compression++) {
        int old_compression = server.rdb_compression;
        server.rdb_compression = compression;
        rio out, in;
        rioInitWithBuffer(&out, sdsempty());
        test_cond("Save native Cuckoo RDB type and value",
                  rdbSaveObjectType(&out, o) > 0 && rdbSaveObject(&out, o, NULL, 0) > 0);
        rioInitWithBuffer(&in, out.io.buffer.ptr);
        int type = rdbLoadObjectType(&in), err;
        copy = rdbLoadObject(type, &in, NULL, 0, RDB_VERSION, &err);
        test_cond("Load native Cuckoo RDB value", type == RDB_TYPE_CUCKOO && copy != NULL);
        if (copy) {
            memset(copied, 0, sizeof(copied));
            cuckooMixDigest(copied, copy);
            int matches = memcmp(original, copied, 20) == 0;
            for (uint64_t i = 0; i < 100; i++)
                matches &= CuckooFilter_Check(copy->ptr, CUCKOO_GEN_HASH(&i, sizeof(i)));
            test_cond("Cuckoo RDB round trip preserves membership and metadata", matches);
            decrRefCount(copy);
        }
        int truncated_ok = 1;
        for (size_t cut = 1; cut < sdslen(out.io.buffer.ptr); cut++) {
            sds partial = sdsnewlen(out.io.buffer.ptr, cut);
            rioInitWithBuffer(&in, partial);
            rdbLoadObjectType(&in);
            int corrupt;
            copy = rdbLoadCuckooObject(&in, CUCKOO_RDB_NATIVE_ID, &corrupt);
            if (copy) { truncated_ok = 0; decrRefCount(copy); }
            sdsfree(partial);
        }
        test_cond("Truncated native Cuckoo fields are rejected", truncated_ok);
        sdsfree(out.io.buffer.ptr);
        server.rdb_compression = old_compression;
    }

    size_t alloc_size = cuckooAllocSize(o);
    test_cond("Cuckoo allocation size includes every allocation",
              alloc_size + zmalloc_size(o) == zmalloc_used_memory() - before_memory);
    sds key = sdsnew("cuckoo");
    copy = cuckooDup(o);
    kvobj *kv = kvobjSet(key, copy, 0);
    test_cond("Cuckoo database memory accounting includes filter allocations",
              kvobjAllocSize(kv) >= cuckooAllocSize(kv) + sizeof(kvobj) &&
              kvobjComputeSize(NULL, kv, 0, 0) == kvobjAllocSize(kv));
    decrRefCount(kv);
    sdsfree(key);

    unsigned long cursor = 0;
    do {
        cursor = cuckooDefrag(o, cursor, 1, cuckooTestRelocate);
    } while (cursor);
    memset(copied, 0, sizeof(copied));
    cuckooMixDigest(copied, o);
    test_cond("Incremental Cuckoo defrag preserves contents and allocation size",
              memcmp(original, copied, 20) == 0 && cuckooAllocSize(o) == alloc_size);
    cuckooDefrag(o, 0, SIZE_MAX, cuckooTestRelocate);
    memset(copied, 0, sizeof(copied));
    cuckooMixDigest(copied, o);
    test_cond("Full Cuckoo defrag preserves contents", memcmp(original, copied, 20) == 0);

    /* Deletion may compact away subfilters between incremental defrag calls. */
    cursor = cuckooDefrag(o, 0, 1, cuckooTestRelocate);
    cf = o->ptr;
    for (uint64_t i = 0; i < 100; i++)
        assert(CuckooFilter_Delete(cf, CUCKOO_GEN_HASH(&i, sizeof(i))) == 1);
    CuckooFilter_Compact(cf, true);
    memset(original, 0, sizeof(original));
    cuckooMixDigest(original, o);
    do {
        cursor = cuckooDefrag(o, cursor, 1, cuckooTestRelocate);
    } while (cursor);
    memset(copied, 0, sizeof(copied));
    cuckooMixDigest(copied, o);
    test_cond("Cuckoo defrag handles compaction between steps",
              getObjectLength(o) == 0 && memcmp(original, copied, 20) == 0);
    decrRefCount(o);
    test_cond("Cuckoo object operations release all allocations",
              zmalloc_used_memory() == before_memory);
    return __failed_tests != before_failed;
}
#endif
