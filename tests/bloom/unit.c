#include "bloom.h"
#include "unit_support.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#define BF_DEFAULT_GROWTH 2

TEST_DEFINE_GLOBALS();

TEST_CLASS(basic)


TEST_F(basic, sbValidation) {
    int err;
    SBChain *chain = SB_NewChain(1, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    ASSERT_NE(chain, NULL);
    ASSERT_EQ(0, chain->size);
    SBChain_Free(chain);

    ASSERT_EQ(NULL, SB_NewChain(0, 0.01, 0, BF_DEFAULT_GROWTH, &err));
    ASSERT_EQ(NULL, SB_NewChain(1, 0, 0, BF_DEFAULT_GROWTH, &err));
    ASSERT_EQ(NULL, SB_NewChain(100, 1.1, 0, BF_DEFAULT_GROWTH, &err));
    ASSERT_EQ(NULL, SB_NewChain(100, -4.4, 0, BF_DEFAULT_GROWTH, &err));
}

TEST_F(basic, sbBasic) {
    int err;
    SBChain *chain = SB_NewChain(100, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    ASSERT_NE(NULL, chain);

    const char *k1 = "hello";
    const size_t n1 = strlen(k1);

    ASSERT_EQ(0, SBChain_Check(chain, k1, n1));
    // Add the item once:
    ASSERT_NE(0, SBChain_Add(chain, k1, n1));
    ASSERT_EQ(1, chain->size);
    ASSERT_NE(0, SBChain_Check(chain, k1, n1));
    // Add the item again:
    ASSERT_EQ(0, SBChain_Add(chain, k1, n1));

    SBChain_Free(chain);
}

TEST_F(basic, sbEmptyChainCreationError) {
    const char *errmsg = NULL;

    static char header[20] = {0};
    SBChain *sb = SB_NewChainFromHeader(header, sizeof(header), &errmsg);
    ASSERT_EQ(NULL, sb);
}

TEST_F(basic, sbExpansion) {
    int err;
    // Note that the chain auto-expands to 6 items by default with the given
    // error ratio. If you modify the error ratio, the expansion may change.
    SBChain *chain = SB_NewChain(6, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    ASSERT_NE(NULL, chain);

    // Add the first item
    ASSERT_NE(0, SBChain_Add(chain, "abc", 3));
    ASSERT_EQ(1, chain->nfilters);

    // Insert 6 items
    for (size_t ii = 0; ii < 16; ++ii) {
        ASSERT_EQ(0, SBChain_Check(chain, &ii, sizeof ii));
        ASSERT_NE(0, SBChain_Add(chain, &ii, sizeof ii));
    }
    ASSERT_GT(chain->nfilters, 1);
    SBChain_Free(chain);
}
/*
// Disabled due to issue 178
TEST_F(basic, testIssue6_Overflow) {
    SBChain *chain = SB_NewChain(1000000000000, 0.00001, 0, BF_DEFAULT_GROWTH);
    if (chain != NULL) {
        SBChain_Free(chain);
    } else {
        ASSERT_EQ(ENOMEM, errno);
    }

    chain = SB_NewChain(4294967296, 0.00001, 0, BF_DEFAULT_GROWTH);
    ASSERT_EQ(NULL, chain);
} */

TEST_F(basic, testIssue7_Overflow) {
    if (!getenv("BLOOM_LARGE_TESTS") || strcmp(getenv("BLOOM_LARGE_TESTS"), "1")) {
        puts("SKIP: set BLOOM_LARGE_TESTS=1 for 1 GiB allocation"); return;
    }
    int err;
    // Try with a bit count of 33:
    SBChain *chain = SB_NewChain(33, 0.000025, BLOOM_OPT_ENTS_IS_BITS, BF_DEFAULT_GROWTH, &err);
    if (chain == NULL) {
        ASSERT_EQ(ENOMEM, errno);
        return;
    }

    ASSERT_NE(0, SBChain_Add(chain, "foo", 3));
    ASSERT_NE(0, SBChain_Add(chain, "bar", 3));
    ASSERT_EQ(2, chain->size);
    ASSERT_EQ(2, chain->filters[0].size);

    struct bloom *inner = &chain->filters[0].inner;
    ASSERT_EQ(33, inner->n2);
    /* Scalable chains halve the requested rate for the first link. The
     * upstream integer assertion silently truncated both doubles to zero. */
    ASSERT_EQ(1, fabs(inner->error - 0.0000125) < 1e-15);
    ASSERT_EQ(1073741824, inner->bytes);
    ASSERT_EQ(365557102, inner->entries);

    SBChain_Free(chain);
}

TEST_F(basic, testIssue9) {
    if (!getenv("BLOOM_LARGE_TESTS") || strcmp(getenv("BLOOM_LARGE_TESTS"), "1")) {
        puts("SKIP: set BLOOM_LARGE_TESTS=1 for large allocation"); return;
    }
    int err;
    SBChain *chain = SB_NewChain(350000000, 0.01, 0, BF_DEFAULT_GROWTH, &err);
    if (chain == NULL) {
        ASSERT_EQ(ENOMEM, errno);
        return;
    }

    ASSERT_NE(0, SBChain_Add(chain, "asdf", 4));
    ASSERT_NE(0, SBChain_Add(chain, "a", 1));
    ASSERT_NE(0, SBChain_Add(chain, "s", 1));
    ASSERT_NE(0, SBChain_Add(chain, "d", 1));
    ASSERT_NE(0, SBChain_Add(chain, "f", 1));

    SBChain_Free(chain);
}

TEST_F(basic, testNoRound) {
    int err;
    SBChain *chain = SB_NewChain(100, 0.01, BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND, 2, &err);
    if (chain == NULL) {
        ASSERT_EQ(ENOMEM, errno);
        return;
    }
    ASSERT_EQ(100, chain->filters[0].inner.entries);
    ASSERT_NE(0, SBChain_Add(chain, "asdf", 4));
    ASSERT_NE(0, SBChain_Add(chain, "a", 1));
    ASSERT_NE(0, SBChain_Add(chain, "s", 1));
    ASSERT_NE(0, SBChain_Add(chain, "d", 1));
    ASSERT_NE(0, SBChain_Add(chain, "f", 1));
    ASSERT_EQ(0, SBChain_Add(chain, "asdf", 4));
    ASSERT_EQ(0, SBChain_Add(chain, "a", 1));
    ASSERT_EQ(0, SBChain_Add(chain, "s", 1));
    ASSERT_EQ(0, SBChain_Add(chain, "d", 1));
    ASSERT_EQ(0, SBChain_Add(chain, "f", 1));
    ASSERT_NE(0, SBChain_Check(chain, "asdf", 4));
    ASSERT_NE(0, SBChain_Check(chain, "a", 1));
    ASSERT_NE(0, SBChain_Check(chain, "s", 1));
    ASSERT_NE(0, SBChain_Check(chain, "d", 1));
    ASSERT_NE(0, SBChain_Check(chain, "f", 1));

    SBChain_Free(chain);
}


/**
 *      self.cmd('bf.reserve', 'myBloom', '0.0001', '100')
        def do_verify():
            for x in xrange(1000):
                self.cmd('bf.add', 'myBloom', x)
                rv = self.cmd('bf.exists', 'myBloom', x)
                self.assertTrue(rv)
                rv = self.cmd('bf.exists', 'myBloom', 'nonexist_{}'.format(x))
                self.assertFalse(rv, x)

 */
TEST_F(basic, test64BitHash) {
    int err;
    SBChain *chain = SB_NewChain(100, 0.0001, BLOOM_OPT_FORCE64, BF_DEFAULT_GROWTH, &err);
    for (size_t ii = 0; ii < 1000; ++ii) {
        size_t val_exist = ii;
        size_t val_nonexist = ~ii;
        // Add the item
        int rc = SBChain_Add(chain, &val_exist, sizeof val_exist);
        ASSERT_NE(0, rc);
        ASSERT_NE(0, SBChain_Check(chain, &val_exist, sizeof val_exist));
        ASSERT_EQ(0, SBChain_Check(chain, &val_nonexist, sizeof val_nonexist));
    }
    SBChain_Free(chain);
}

typedef struct {
    const char *buf;
    size_t nbuf;
    long long iter;
} encodedInfo;

TEST_CLASS(encoding)

TEST_F(encoding, testEncodingSimple) {
    int err;
    SBChain *chain = SB_NewChain(1000, 0.001, 0, BF_DEFAULT_GROWTH, &err);
    ASSERT_NE(NULL, chain);

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

    ASSERT_EQ(94, nColls);

    // Dump the header
    size_t len = 0;
    char *hdr = SBChain_GetEncodedHeader(chain, &len);
    ASSERT_NE(NULL, hdr);
    ASSERT_NE(0, len);

    encodedInfo *encs = malloc(sizeof(*encs));
    size_t numEncs = 0;
    long long iter = SB_CHUNKITER_INIT;

    while (iter != SB_CHUNKITER_DONE) {
        encs = realloc(encs, sizeof(*encs) * (numEncs + 1));
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
    ASSERT_NE(NULL, chain2);
    ASSERT_EQ(chain->size, chain2->size);
    ASSERT_EQ(chain->growth, chain2->growth);
    ASSERT_EQ(chain->options, chain2->options);
    ASSERT_EQ(chain->nfilters, chain2->nfilters);

    for (size_t ii = 0; ii < numEncs; ++ii) {
        ASSERT_EQ(0, SBChain_LoadEncodedChunk(chain2, encs[ii].iter, encs[ii].buf, encs[ii].nbuf,
                                              &errmsg));
    }

    ASSERT_EQ(chain->nfilters, chain2->nfilters);
    for (size_t ii = 0; ii < chain->nfilters; ++ii) {
        const SBLink *link1 = chain->filters + ii;
        const SBLink *link2 = chain2->filters + ii;
        ASSERT_EQ(link1->inner.bytes, link2->inner.bytes);
        ASSERT_EQ(0, memcmp(link1->inner.bf, link2->inner.bf, link2->inner.bytes));
    }

    size_t nColls_2 = 0;
    for (size_t ii = 1; ii < 100000; ++ii) {
        ASSERT_EQ(1, SBChain_Check(chain2, &ii, sizeof ii));
        size_t iiFlipped = ii << 31;
        if (SBChain_Check(chain2, &iiFlipped, sizeof iiFlipped) != 0) {
            nColls_2++;
        }
    }

    ASSERT_EQ(nColls, nColls_2);

    SB_FreeEncodedHeader(hdr);
    SBChain_Free(chain);
    SBChain_Free(chain2);
    free(encs);
}

int main(void) {
    test__abort_on_fail = 1;
    TEST_RUN_ALL_TESTS();
    return 0;
}

/* Standalone equivalent of the core assertion hook used by zmalloc. */
void _serverAssert(const char *expression, const char *file, int line) {
    fprintf(stderr, "%s:%d: %s\n", file, line, expression);
    abort();
}
