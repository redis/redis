/* Unit tests for native Redis Bloom filters and objects.
 *
 * Copyright (c) 2026-present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
#ifdef REDIS_TEST
#include "server.h"
#include "t_bloom.h"
#include "testhelp.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

/* Byte offsets in the packed SCANDUMP header wire format. */
#define BLOOM_DUMP_SIZE_OFFSET 0
#define BLOOM_DUMP_GROWTH_OFFSET 16
#define BLOOM_DUMP_FIRST_HASHES_OFFSET 60

/* Ported from RedisBloom tests/unit/test-basic.c. */
#define BF_DEFAULT_GROWTH 2

static void bloomTest_sbValidation(void) {
    int err;
    SBChain *chain = SB_NewChain(1, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    assert((chain) != (NULL));
    assert((0) == (chain->size));
    SBChain_Free(chain);

    assert((NULL) == (SB_NewChain(0, 0.01, 0, BF_DEFAULT_GROWTH, &err)));
    assert((NULL) == (SB_NewChain(1, 0, 0, BF_DEFAULT_GROWTH, &err)));
    assert((NULL) == (SB_NewChain(100, 1.1, 0, BF_DEFAULT_GROWTH, &err)));
    assert((NULL) == (SB_NewChain(100, -4.4, 0, BF_DEFAULT_GROWTH, &err)));
}

static void bloomTest_bloomValidationRejectsExcessiveHashes(void) {
    struct bloom bloom = {
        .error = 0.01,
        .bpe = 3098164007.0,
        .hashes = INT_MAX,
        .bytes = 1,
        .bits = 8,
    };
    assert((0) != (bloom_validate_integrity(&bloom)));
}

static void bloomTest_bloomValidationEnforcesHashLimit(void) {
    struct bloom bloom = {
        .error = 0.01,
        .bytes = 1,
        .bits = 8,
    };

    // Keep the calculated value away from a ceil boundary.
    bloom.bpe = ((double)BLOOM_MAX_HASHES - 0.5) / log(2.0);
    bloom.hashes = BLOOM_MAX_HASHES;
    assert((0) == (bloom_validate_integrity(&bloom)));

    bloom.bpe = ((double)BLOOM_MAX_HASHES + 0.5) / log(2.0);
    bloom.hashes = BLOOM_MAX_HASHES + 1;
    assert((0) != (bloom_validate_integrity(&bloom)));
}

static void bloomTest_sbBasic(void) {
    int err;
    SBChain *chain = SB_NewChain(100, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    assert((NULL) != (chain));

    const char *k1 = "hello";
    const size_t n1 = strlen(k1);

    assert((0) == (SBChain_Check(chain, k1, n1)));
    // Add the item once:
    assert((0) != (SBChain_Add(chain, k1, n1)));
    assert((1) == (chain->size));
    assert((0) != (SBChain_Check(chain, k1, n1)));
    // Add the item again:
    assert((0) == (SBChain_Add(chain, k1, n1)));

    SBChain_Free(chain);
}

static void bloomTest_sbEmptyChainCreationError(void) {
    const char *errmsg = NULL;

    static char header[20] = {0};
    SBChain *sb = SB_NewChainFromHeader(header, sizeof(header), &errmsg);
    assert((NULL) == (sb));
}

static void bloomTest_sbExpansion(void) {
    int err;
    // Note that the chain auto-expands to 6 items by default with the given
    // error ratio. If you modify the error ratio, the expansion may change.
    SBChain *chain = SB_NewChain(6, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    assert((NULL) != (chain));

    // Add the first item
    assert((0) != (SBChain_Add(chain, "abc", 3)));
    assert((1) == (chain->nfilters));

    // Insert 6 items
    for (size_t ii = 0; ii < 16; ++ii) {
        assert((0) == (SBChain_Check(chain, &ii, sizeof ii)));
        assert((0) != (SBChain_Add(chain, &ii, sizeof ii)));
    }
    assert((chain->nfilters) > (1));
    SBChain_Free(chain);
}


static void bloomTest_testIssue7_Overflow(void) {
    int err;
    // Try with a bit count of 33:
    SBChain *chain = SB_NewChain(33, 0.000025, BLOOM_OPT_ENTS_IS_BITS, BF_DEFAULT_GROWTH, &err);
    if (chain == NULL) {
        assert((ENOMEM) == (errno));
        return;
    }

    assert((0) != (SBChain_Add(chain, "foo", 3)));
    assert((0) != (SBChain_Add(chain, "bar", 3)));
    assert((2) == (chain->size));
    assert((2) == (chain->filters[0].size));

    struct bloom *inner = &chain->filters[0].inner;
    assert((33) == (inner->n2));
    /* Scalable filters reserve half the error budget for later links. */
    assert(0.000025 * 0.5 == inner->error);
    assert((1073741824) == (inner->bytes));
    assert((365557102) == (inner->entries));

    SBChain_Free(chain);
}

static void bloomTest_testIssue9(void) {
    int err;
    SBChain *chain = SB_NewChain(350000000, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    if (chain == NULL) {
        assert((ENOMEM) == (errno));
        return;
    }

    assert((0) != (SBChain_Add(chain, "asdf", 4)));
    assert((0) != (SBChain_Add(chain, "a", 1)));
    assert((0) != (SBChain_Add(chain, "s", 1)));
    assert((0) != (SBChain_Add(chain, "d", 1)));
    assert((0) != (SBChain_Add(chain, "f", 1)));

    SBChain_Free(chain);
}

static void bloomTest_testNoRound(void) {
    int err;
    SBChain *chain = SB_NewChain(100, 0.01, BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND, 2, &err);
    if (chain == NULL) {
        assert((ENOMEM) == (errno));
        return;
    }
    assert((100) == (chain->filters[0].inner.entries));
    assert((0) != (SBChain_Add(chain, "asdf", 4)));
    assert((0) != (SBChain_Add(chain, "a", 1)));
    assert((0) != (SBChain_Add(chain, "s", 1)));
    assert((0) != (SBChain_Add(chain, "d", 1)));
    assert((0) != (SBChain_Add(chain, "f", 1)));
    assert((0) == (SBChain_Add(chain, "asdf", 4)));
    assert((0) == (SBChain_Add(chain, "a", 1)));
    assert((0) == (SBChain_Add(chain, "s", 1)));
    assert((0) == (SBChain_Add(chain, "d", 1)));
    assert((0) == (SBChain_Add(chain, "f", 1)));
    assert((0) != (SBChain_Check(chain, "asdf", 4)));
    assert((0) != (SBChain_Check(chain, "a", 1)));
    assert((0) != (SBChain_Check(chain, "s", 1)));
    assert((0) != (SBChain_Check(chain, "d", 1)));
    assert((0) != (SBChain_Check(chain, "f", 1)));

    SBChain_Free(chain);
}



static void bloomTest_test64BitHash(void) {
    int err;
    SBChain *chain = SB_NewChain(100, 0.0001, BLOOM_OPT_FORCE64, BF_DEFAULT_GROWTH, &err);
    for (size_t ii = 0; ii < 1000; ++ii) {
        size_t val_exist = ii;
        size_t val_nonexist = ~ii;
        // Add the item
        int rc = SBChain_Add(chain, &val_exist, sizeof val_exist);
        assert((0) != (rc));
        assert((0) != (SBChain_Check(chain, &val_exist, sizeof val_exist)));
        assert((0) == (SBChain_Check(chain, &val_nonexist, sizeof val_nonexist)));
    }
    SBChain_Free(chain);
}

typedef struct {
    const char *buf;
    size_t nbuf;
    long long iter;
} encodedInfo;



static void bloomTest_testEncodingSimple(void) {
    int err;
    SBChain *chain = SB_NewChain(1000, 0.001, 0, BF_DEFAULT_GROWTH, &err);
    assert((NULL) != (chain));

    for (size_t ii = 1; ii < 100000; ++ii) {
        SBChain_Add(chain, &ii, sizeof ii);
    }

    size_t nColls = 0;
    for (size_t ii = 1; ii < 100000; ++ii) {
        size_t iiFlipped = ii << 31;
        if (SBChain_Check(chain, &iiFlipped, sizeof iiFlipped) != 0) {
            nColls++;
        }
    }

    assert((94) == (nColls));

    // Dump the header
    size_t len = 0;
    char *hdr = SBChain_GetEncodedHeader(chain, &len);
    assert((NULL) != (hdr));
    assert((0) != (len));

    encodedInfo *encs = zmalloc(sizeof(*encs));
    size_t numEncs = 0;
    long long iter = SB_CHUNKITER_INIT;

    while (iter != SB_CHUNKITER_DONE) {
        encs = zrealloc(encs, sizeof(*encs) * (numEncs + 1));
        encodedInfo *curEnc = encs + numEncs;
        curEnc->buf = SBChain_GetEncodedChunk(chain, &iter, &curEnc->nbuf, 128);
        curEnc->iter = iter;

        if (curEnc->buf) {
            numEncs++;
        } else {
            break;
        }
    }

    const char *errmsg;
    SBChain *chain2 = SB_NewChainFromHeader(hdr, len, &errmsg);
    assert((NULL) != (chain2));
    assert((chain->size) == (chain2->size));
    assert((chain->growth) == (chain2->growth));
    assert((chain->options) == (chain2->options));
    assert((chain->nfilters) == (chain2->nfilters));

    for (size_t ii = 0; ii < numEncs; ++ii) {
        assert(SBChain_LoadEncodedChunk(chain2, encs[ii].iter, encs[ii].buf,
                                        encs[ii].nbuf, &errmsg) == 0);
    }

    assert((chain->nfilters) == (chain2->nfilters));
    for (size_t ii = 0; ii < chain->nfilters; ++ii) {
        const SBLink *link1 = chain->filters + ii;
        const SBLink *link2 = chain2->filters + ii;
        assert((link1->inner.bytes) == (link2->inner.bytes));
        assert((0) == (memcmp(link1->inner.bf, link2->inner.bf, link2->inner.bytes)));
    }

    size_t nColls_2 = 0;
    for (size_t ii = 1; ii < 100000; ++ii) {
        assert((1) == (SBChain_Check(chain2, &ii, sizeof ii)));
        size_t iiFlipped = ii << 31;
        if (SBChain_Check(chain2, &iiFlipped, sizeof iiFlipped) != 0) {
            nColls_2++;
        }
    }

    assert((nColls) == (nColls_2));

    SB_FreeEncodedHeader(hdr);
    SBChain_Free(chain);
    SBChain_Free(chain2);
    zfree(encs);
}


#undef BF_DEFAULT_GROWTH

int bloomTest(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    int before_failed = __failed_tests;
    size_t before_memory = zmalloc_used_memory();
    bloomTest_sbValidation();
    test_cond("RedisBloom sbValidation", 1);
    bloomTest_bloomValidationRejectsExcessiveHashes();
    test_cond("RedisBloom bloomValidationRejectsExcessiveHashes", 1);
    bloomTest_bloomValidationEnforcesHashLimit();
    test_cond("RedisBloom bloomValidationEnforcesHashLimit", 1);
    bloomTest_sbBasic();
    test_cond("RedisBloom sbBasic", 1);
    bloomTest_sbEmptyChainCreationError();
    test_cond("RedisBloom sbEmptyChainCreationError", 1);
    bloomTest_sbExpansion();
    test_cond("RedisBloom sbExpansion", 1);
    if (flags & REDIS_TEST_LARGE_MEMORY) {
        bloomTest_testIssue7_Overflow();
        test_cond("RedisBloom testIssue7_Overflow", 1);
    }
    if (flags & REDIS_TEST_LARGE_MEMORY) {
        bloomTest_testIssue9();
        test_cond("RedisBloom testIssue9", 1);
    }
    bloomTest_testNoRound();
    test_cond("RedisBloom testNoRound", 1);
    bloomTest_test64BitHash();
    test_cond("RedisBloom test64BitHash", 1);
    bloomTest_testEncodingSimple();
    test_cond("RedisBloom testEncodingSimple", 1);
    int err;
    if (flags & REDIS_TEST_ACCURATE) {
        /* RedisBloom tests/unit/test-perf.c repeated add/check workload. */
        SBChain *chain = SB_NewChain(100000, 0.0001, 0, 2, &err);
        assert(chain != NULL);
        for (size_t i = 0; i < 50000000; i++) {
            size_t item = i % 100000;
            SBChain_Add(chain, &item, sizeof(item));
            assert(SBChain_Check(chain, &item, sizeof(item)) == 1);
        }
        SBChain_Free(chain);
        test_cond("RedisBloom repeated add/check workload", 1);
    }
    const unsigned modes[] = {0, BLOOM_OPT_NOROUND, BLOOM_OPT_FORCE64,
                              BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND};
    for (size_t mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++) {
        SBChain *sb = SB_NewChain(8, 0.0001, modes[mode], 2, &err);
        test_cond("Create Bloom chain", sb != NULL && err == SB_SUCCESS);
        if (!sb) continue;
        int ok = SBChain_Check(sb, "", 0) == 0 && SBChain_Add(sb, "", 0) == 1;
        for (uint32_t i = 0; i < 1000; i++) {
            /* Includes zero and high bytes, independent of host endianness. */
            unsigned char value[] = {i & 255, (i >> 8) & 255, 0, 255};
            ok &= SBChain_Add(sb, value, sizeof(value)) >= 0;
        }
        size_t size = sb->size;
        for (uint32_t i = 0; i < 1000; i++) {
            unsigned char value[] = {i & 255, (i >> 8) & 255, 0, 255};
            ok &= SBChain_Check(sb, value, sizeof(value)) == 1;
            ok &= SBChain_Add(sb, value, sizeof(value)) == 0;
        }
        ok &= SBChain_Check(sb, "", 0) == 1 && SBChain_Add(sb, "", 0) == 0;
        test_cond("Bloom membership and duplicates survive scaling",
                  ok && sb->size == size && sb->nfilters > 1 && SB_ValidateIntegrity(sb) == 0);
        test_cond("Bloom expansion grows capacity and tightens error rate",
                  sb->filters[1].inner.entries >= sb->filters[0].inner.entries * 2 &&
                  sb->filters[1].inner.error == sb->filters[0].inner.error * 0.5);

        size_t nfilters = sb->nfilters, lastsize = sb->filters[sb->nfilters - 1].size;
        sb->filters[sb->nfilters - 1].size = sb->filters[sb->nfilters - 1].inner.entries;
        sb->size = SIZE_MAX;
        test_cond("Bloom count overflow does not grow the filter",
                  SBChain_Add(sb, "overflow", 8) == SB_ERR && sb->nfilters == nfilters);
        sb->size = size;
        sb->filters[sb->nfilters - 1].size = lastsize;

        size_t header_len;
        char *header = SBChain_GetEncodedHeader(sb, &header_len);
        const char *errmsg = NULL;
        SBChain *copy = SB_NewChainFromHeader(header, header_len, &errmsg);
        test_cond("Load Bloom header", copy != NULL);
        if (copy) {
            long long iter = SB_CHUNKITER_INIT;
            const char *chunk;
            size_t len;
            ok = 1;
            while ((chunk = SBChain_GetEncodedChunk(sb, &iter, &len, 17)))
                ok &= SBChain_LoadEncodedChunk(copy, iter, chunk, len, &errmsg) == 0;
            for (size_t i = 0; i < sb->nfilters; i++)
                ok &= memcmp(sb->filters[i].inner.bf, copy->filters[i].inner.bf,
                             sb->filters[i].inner.bytes) == 0;
            size_t copy_len;
            char *copy_header = SBChain_GetEncodedHeader(copy, &copy_len);
            test_cond("Bloom chunk round trip preserves every bit and metadata",
                      ok && iter == SB_CHUNKITER_DONE && copy_len == header_len &&
                      memcmp(header, copy_header, header_len) == 0);
            test_cond("Reject invalid chunk offsets and oversized chunks",
                      SBChain_LoadEncodedChunk(copy, 0, "x", 1, &errmsg) != 0 &&
                      SBChain_LoadEncodedChunk(copy, LLONG_MAX, "x", 1, &errmsg) != 0 &&
                      SBChain_LoadEncodedChunk(copy, copy->filters[0].inner.bytes + 2,
                                              "x", copy->filters[0].inner.bytes + 1, &errmsg) != 0);
            SB_FreeEncodedHeader(copy_header);
            SBChain_Free(copy);
        }
        test_cond("Reject truncated Bloom headers",
                  SB_NewChainFromHeader(header, header_len - 1, &errmsg) == NULL);
        uint32_t saved_hashes, hashes = BLOOM_MAX_HASHES + 1;
        memcpy(&saved_hashes, header + BLOOM_DUMP_FIRST_HASHES_OFFSET, sizeof(saved_hashes));
        memcpy(header + BLOOM_DUMP_FIRST_HASHES_OFFSET, &hashes, sizeof(hashes));
        test_cond("Reject excessive hash counts",
                  SB_NewChainFromHeader(header, header_len, &errmsg) == NULL);
        memcpy(header + BLOOM_DUMP_FIRST_HASHES_OFFSET, &saved_hashes, sizeof(saved_hashes));
        uint64_t count = sb->size + 1;
        memcpy(header + BLOOM_DUMP_SIZE_OFFSET, &count, sizeof(count));
        test_cond("Reject inconsistent Bloom counts",
                  SB_NewChainFromHeader(header, header_len, &errmsg) == NULL);
        count = sb->size;
        memcpy(header + BLOOM_DUMP_SIZE_OFFSET, &count, sizeof(count));
        uint32_t growth = 0;
        memcpy(header + BLOOM_DUMP_GROWTH_OFFSET, &growth, sizeof(growth));
        test_cond("Reject scaling headers with zero growth",
                  SB_NewChainFromHeader(header, header_len, &errmsg) == NULL);
        SB_FreeEncodedHeader(header);
        SBChain_Free(sb);
    }

    SBChain *sb = SB_NewChain(1, 0.0001,
                             BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND | BLOOM_OPT_NO_SCALING,
                             0, &err);
    test_cond("Create non-scaling Bloom chain", sb != NULL);
    if (sb) {
        test_cond("Full non-scaling filter accepts duplicates but rejects new items",
                  SBChain_Add(sb, "a", 1) == 1 && SBChain_Add(sb, "a", 1) == 0 &&
                  SBChain_Check(sb, "b", 1) == 0 && SBChain_Add(sb, "b", 1) == SB_FULL &&
                  sb->size == 1 && sb->nfilters == 1);
        SBChain_Free(sb);
    }
    double invalid_errors[] = {0, -1, 1, NAN, INFINITY};
    for (size_t i = 0; i < sizeof(invalid_errors) / sizeof(*invalid_errors); i++) {
        sb = SB_NewChain(8, invalid_errors[i], 0, 2, &err);
        test_cond("Reject invalid Bloom error rate", sb == NULL && err == SB_INVALID);
        SBChain_Free(sb);
    }
    struct bloom b = {0};
    test_cond("Reject invalid Bloom capacity and overflow before allocation",
              bloom_init(&b, 0, 0.01, 0) == 1 &&
              bloom_init(&b, 64, 0.01, BLOOM_OPT_ENTS_IS_BITS) == 1 &&
              bloom_init(&b, UINT64_MAX, 0.01, BLOOM_OPT_NOROUND) == 1 &&
              bloom_init(&b, UINT64_MAX, 0.01, 0) == 1 &&
              bloom_init(&b, 63, 0.99, BLOOM_OPT_ENTS_IS_BITS) == 1);
    sb = SB_NewChain(8, 0.01, 0, 0, &err);
    test_cond("Reject zero growth for scaling filters", sb == NULL && err == SB_INVALID);
    SBChain_Free(sb);
    test_cond("Bloom tests release all allocations", zmalloc_used_memory() == before_memory);
    return __failed_tests != before_failed;
}

size_t lazyfreeGetFreeEffort(robj *key, robj *obj, int dbid);
size_t kvobjComputeSize(robj *key, kvobj *o, size_t sample_size, int dbid);

/* Move every allocation, even if the allocator would normally leave it alone. */
static void *bloomTestRelocate(void *ptr) {
    size_t len = zmalloc_size(ptr);
    void *moved = zmalloc(len);
    memcpy(moved, ptr, len);
    zfree(ptr);
    return moved;
}

int bloomObjectTest(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    int before_failed = __failed_tests;
    size_t before_memory = zmalloc_used_memory();
    int err;
    SBChain *sb = SB_NewChain(2, 0.0001, BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND, 2, &err);
    test_cond("Create native Bloom object", sb != NULL);
    if (!sb) return 1;
    robj *o = createBloomObject(sb);
    for (unsigned i = 0; i < 100; i++) SBChain_Add(sb, &i, sizeof(i));
    test_cond("Bloom object type, encoding, length and free effort",
              o->type == OBJ_BLOOM && o->encoding == OBJ_ENCODING_BLOOM &&
              getObjectLength(o) == sb->size && lazyfreeGetFreeEffort(NULL, o, 0) == sb->nfilters + 2);
    unsigned char original[20] = {0}, copied[20] = {0};
    bloomMixDigest(original, o);
    robj *copy = bloomDup(o);
    bloomMixDigest(copied, copy);
    test_cond("Bloom copy preserves data and metadata", memcmp(original, copied, 20) == 0);
    SBChain *other = copy->ptr;
    int independent = other != sb && other->filters != sb->filters;
    for (size_t i = 0; i < sb->nfilters; i++)
        independent &= other->filters[i].inner.bf != sb->filters[i].inner.bf;
    other->filters[0].inner.bf[0] ^= 1;
    memset(copied, 0, sizeof(copied));
    bloomMixDigest(copied, copy);
    test_cond("Bloom copy has independent allocations and digest detects bit changes",
              independent && memcmp(original, copied, 20) != 0);
    decrRefCount(copy);

    for (int compression = 0; compression <= 1; compression++) {
        int old_compression = server.rdb_compression;
        server.rdb_compression = compression;
        rio out;
        rioInitWithBuffer(&out, sdsempty());
        test_cond("Save native Bloom RDB type and value",
                  rdbSaveObjectType(&out, o) > 0 && rdbSaveObject(&out, o, NULL, 0) > 0);
        rio in;
        rioInitWithBuffer(&in, out.io.buffer.ptr);
        int type = rdbLoadObjectType(&in);
        copy = rdbLoadObject(type, &in, NULL, 0, RDB_VERSION, &err);
        test_cond("Load native Bloom RDB value", type == RDB_TYPE_BLOOM && copy != NULL);
        if (copy) {
            memset(copied, 0, sizeof(copied));
            bloomMixDigest(copied, copy);
            test_cond("RDB round trip preserves Bloom membership and digest",
                      memcmp(original, copied, 20) == 0 && getObjectLength(copy) == sb->size);
            decrRefCount(copy);
        }
        sdsfree(out.io.buffer.ptr);
        server.rdb_compression = old_compression;
    }

    size_t alloc_size = bloomAllocSize(o);
    sds key = sdsnew("bloom");
    copy = bloomDup(o);
    kvobj *kv = kvobjSet(key, copy, 0);
    test_cond("Bloom database memory accounting includes filter allocations",
              kvobjAllocSize(kv) >= bloomAllocSize(kv) + sizeof(kvobj) &&
              kvobjComputeSize(NULL, kv, 0, 0) == kvobjAllocSize(kv));
    decrRefCount(kv);
    sdsfree(key);

    unsigned long cursor = 0;
    do {
        cursor = bloomDefrag(o, cursor, 1, bloomTestRelocate);
    } while (cursor);
    memset(copied, 0, sizeof(copied));
    bloomMixDigest(copied, o);
    test_cond("Incremental defrag preserves all pointers and filter contents",
              memcmp(original, copied, 20) == 0 && bloomAllocSize(o) >= alloc_size);
    bloomDefrag(o, 0, SIZE_MAX, bloomTestRelocate);
    memset(copied, 0, sizeof(copied));
    bloomMixDigest(copied, o);
    test_cond("Full defrag preserves Bloom contents", memcmp(original, copied, 20) == 0);
    decrRefCount(o);
    test_cond("Bloom object operations release all allocations", zmalloc_used_memory() == before_memory);
    return __failed_tests != before_failed;
}
#endif
