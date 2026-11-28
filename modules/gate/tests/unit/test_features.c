/*
 * Gate data type: unit tests for clue extraction (gate_features.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * WHERE THE EXPECTATIONS COME FROM. Nothing below is copied from the output
 * of the code under test.
 *
 *   - The reference vectors, the hash values and RANDOM_TEXTS_DIGEST were
 *     computed by a second implementation of the rules in gate_features.h,
 *     written in Python from that header alone: a regular expression for
 *     the tokens, a set of clues, a dictionary of buckets, numpy float32
 *     for the stored values. It shares nothing with gate_features.c.
 *   - The words and names that share a bucket ("ec" and "je", "f139", ...)
 *     were found by brute force with that reference. The tests that rely on
 *     them first check with gateHash() that they do collide.
 *   - refNamed() states the rules for named features the naive way, on a
 *     dense array of GATE_DIM buckets.
 *
 * TOLERANCES. An expected value that is a whole number (clues that were not
 * scaled) must be matched exactly. Any other value is compared to 1e-6; the
 * reference prints 9 significant digits, enough to tell float32 values
 * apart.
 */

#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "testutil.h"
#include "gate_features.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------ Test helpers ----------------------------- */

/* One entry of an expected vector. */
typedef struct entry {
    uint32_t idx;
    float val;
} entry;

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "test_features: out of memory\n");
        abort();
    }
    return p;
}

/* A heap copy of exactly 'len' bytes, so that valgrind and the address
 * sanitizer report any read past the end of a text. An empty text becomes
 * NULL, which gateFeaturize() must accept. */
static char *exactCopy(const char *text, size_t len) {
    char *copy;

    if (len == 0) return NULL;
    copy = xmalloc(len);
    memcpy(copy, text, len);
    return copy;
}

/* buf[0..len) = 'pattern', repeated. */
static void fill(char *buf, size_t len, const char *pattern) {
    size_t plen = strlen(pattern), i;

    for (i = 0; i < len; i++) buf[i] = pattern[i % plen];
}

/* Overwrite the bytes at offset 'at' with the string 's'. */
static void put(char *buf, size_t at, const char *s) {
    memcpy(buf + at, s, strlen(s));
}

/* What gate_defs.h promises about a sparse vector, plus finite values. */
static int wellFormed(const gateSparse *x) {
    uint32_t i;

    for (i = 0; i < x->n; i++) {
        if (x->idx[i] >= GATE_DIM) return 0;
        if (i > 0 && x->idx[i] <= x->idx[i - 1]) return 0;
        if (x->val[i] == 0.0f || !isfinite(x->val[i])) return 0;
    }
    return 1;
}

/* Bit for bit. */
static int sameSparse(const gateSparse *a, const gateSparse *b) {
    if (a->n != b->n) return 0;
    if (a->n == 0) return 1;
    return memcmp(a->idx, b->idx, a->n * sizeof(*a->idx)) == 0 &&
           memcmp(a->val, b->val, a->n * sizeof(*a->val)) == 0;
}

static double squaredNorm(const gateSparse *x) {
    double sum = 0.0;
    uint32_t i;

    for (i = 0; i < x->n; i++) sum += (double)x->val[i] * (double)x->val[i];
    return sum;
}

/* The vector of a text without named features. */
static gateSparse textVector(const char *text, size_t len) {
    gateSparse x = {0, NULL, NULL};

    CHECK(gateFeaturize(text, len, NULL, 0, &x, NULL, 0) == GATE_OK);
    return x;
}

/* 1 if two texts (C strings) give the same vector. */
static int sameVector(const char *a, const char *b) {
    gateSparse x = textVector(a, strlen(a)), y = textVector(b, strlen(b));
    int same = sameSparse(&x, &y);

    gateSparseFree(&x);
    gateSparseFree(&y);
    return same;
}

/* Featurize one request and compare the whole result with the reference.
 * 'label' names the case when a check fails: the line is the same for all
 * of them. */
static void expectVector(const char *label, const char *text, size_t len,
                         const gateNamedFeature *f, uint32_t nf,
                         const entry *want, size_t n)
{
    char *copy = exactCopy(text, len);
    gateSparse x = {0, NULL, NULL};
    int failures = gate_test_failures;
    size_t i;

    CHECK(gateFeaturize(copy, len, f, nf, &x, NULL, 0) == GATE_OK);
    CHECK(wellFormed(&x));
    CHECK_EQ_INT(x.n, n);
    for (i = 0; i < n && i < x.n; i++) {
        /* See TOLERANCES at the top of this file. */
        double tol = want[i].val == rintf(want[i].val) ? 0.0 : 1e-6;

        CHECK_EQ_INT(x.idx[i], want[i].idx);
        CHECK_NEAR(x.val[i], want[i].val, tol);
    }
    if (gate_test_failures != failures) printf("    ^ case: %s\n", label);
    gateSparseFree(&x);
    free(copy);
}

#define WANT(name) want_##name, ARRAY_LEN(want_##name)

/* A text of the reference section, by name. */
#define EXPECT_TEXT(name) \
    expectVector(#name, text_##name, sizeof(text_##name) - 1, NULL, 0, WANT(name))

/* A text the test has built: first make sure the reference was given the
 * very same bytes. */
#define EXPECT_BUILT(name, text, len) do { \
    CHECK(gateFnv1a64((text), (len)) == FNV_##name); \
    expectVector(#name, (text), (len), NULL, 0, WANT(name)); \
} while (0)

/* A text (string literal or array) with the named features feat_<name>. */
#define EXPECT_NAMED(name, text) \
    expectVector(#name, (text), sizeof(text) - 1, feat_##name, \
                 (uint32_t)ARRAY_LEN(feat_##name), WANT(name))

/* ---------------------------- Random requests ---------------------------- */

#define RANDOM_TEXT_MAX 5000

/* A random text. Most are short, one in twenty is long enough to reach the
 * end of the prefix. There are four kinds: every byte value (invalid UTF-8,
 * NULs); words of some fifty letters out of four (words that get cut, many
 * repeated chunks); and twice an alphabet in which word bytes, separators,
 * the three punctuation tokens, NULs and backticks are all common, once
 * with a link or a code block (or a near miss) dropped somewhere. The
 * reference generates the same texts from the same seed: do not change
 * this function without recomputing RANDOM_TEXTS_DIGEST. */
static size_t randText(uint64_t *rng, unsigned char *buf) {
    static const unsigned char alphabet[] = {
        'a', 'b', 'c', 'd', 'e', 'A', 'B', 'Z', '0', '7', ' ', ' ', ' ', '\n',
        '.', ',', '-', '_', '\'', '?', '!', '@', '`', ':', '/', 0x00, 0x80,
        0xc3, 0xa9, 0xe2, 0xff
    };
    static const char *const snippets[] = {
        "http://", "HTTPS://", "hTtP://", "```", "``", "https:/"
    };
    uint64_t r = gate_test_rand64(rng);
    size_t len = r % 20 == 0 ? 3500 + (r >> 8) % 1500 : (r >> 8) % 160;
    unsigned kind = (unsigned)((r >> 40) % 4);
    size_t i;

    for (i = 0; i < len; i++) {
        uint64_t b = gate_test_rand64(rng);

        if (kind == 0)
            buf[i] = (unsigned char)b;
        else if (kind == 3)
            buf[i] = b % 50 == 0 ? ' ' : (unsigned char)('a' + (b >> 8) % 4);
        else
            buf[i] = alphabet[b % sizeof(alphabet)];
    }
    if (kind == 2 && len >= 8) {
        const char *s = snippets[gate_test_rand64(rng) % ARRAY_LEN(snippets)];
        size_t at = gate_test_rand64(rng) % (len - 7);

        memcpy(buf + at, s, strlen(s));
    }
    return len;
}

/* Fold a vector into a running digest: FNV-1a over 64-bit words, the entry
 * count first, then (bucket << 32 | float bits) for each entry. */
static uint64_t digestVector(uint64_t d, const gateSparse *x) {
    uint32_t i, bits;

    d = (d ^ x->n) * 0x100000001b3ull;
    for (i = 0; i < x->n; i++) {
        memcpy(&bits, &x->val[i], sizeof(bits));
        d = (d ^ ((uint64_t)x->idx[i] << 32 | bits)) * 0x100000001b3ull;
    }
    return d;
}

/* The named feature rules of gate_features.h written the naive way: the
 * named part of a vector as a dense array of GATE_DIM buckets. */
static void refNamed(const gateNamedFeature *f, uint32_t nf, double *dense) {
    double norm2 = 0.0, scale;
    uint32_t i;

    for (i = 0; i < GATE_DIM; i++) dense[i] = 0.0;
    for (i = 0; i < nf; i++) {
        double v = f[i].value;

        if (v > GATE_FEATURE_CLAMP) v = GATE_FEATURE_CLAMP;
        if (v < -GATE_FEATURE_CLAMP) v = -GATE_FEATURE_CLAMP;
        dense[gateHash(0x05, f[i].name, f[i].name_len) % GATE_DIM] += v;
    }
    for (i = 0; i < GATE_DIM; i++) norm2 += dense[i] * dense[i];
    if (norm2 > GATE_FEATURE_NORM2_CAP) {
        scale = sqrt(GATE_FEATURE_NORM2_CAP / norm2);
        for (i = 0; i < GATE_DIM; i++) dense[i] *= scale;
    }
}

/* --------------------------- Counting allocator -------------------------- */

static int allocCalls;    /* allocations asked for since the last reset */
static int allocFailAt;   /* the allocation that fails (1 = the next), 0 = none */
static int allocLive;     /* blocks handed out and not yet freed */
static size_t allocBytes; /* bytes asked for since the last reset */

static void *countingMalloc(size_t n) {
    void *p;

    if (++allocCalls == allocFailAt) return NULL;
    p = xmalloc(n);
    allocLive++;
    allocBytes += n;
    return p;
}

static void *countingCalloc(size_t nmemb, size_t size) {
    void *p = countingMalloc(nmemb * size);

    if (p != NULL) memset(p, 0, nmemb * size);
    return p;
}

static void *countingRealloc(void *p, size_t n) {
    void *q;

    if (p == NULL) return countingMalloc(n);
    if (++allocCalls == allocFailAt) return NULL;
    q = realloc(p, n);
    if (q == NULL) abort();
    allocBytes += n;
    return q;
}

static void countingFree(void *p) {
    allocLive--;
    free(p);
}

static void countAllocations(void) {
    allocCalls = allocFailAt = allocLive = 0;
    allocBytes = 0;
    gateSetAllocator(countingMalloc, countingCalloc, countingRealloc, countingFree);
}

static void stopCounting(void) {
    gateSetAllocator(malloc, calloc, realloc, free);
}

/* ---------------------------- Reference vectors --------------------------
 * Generated by the reference implementation (see the top of this file):
 * the text or the named features of a case, then the vector the rules of
 * gate_features.h give for it. */

/* The empty text: nothing but the "len0" hint. */
static const char text_empty[] = "";
static const entry want_empty[] = {
    {6403, 1}
};

/* One word of 5 bytes: itself, "^hell", "$ello", "len0". */
static const char text_hello[] = "hello";
static const entry want_hello[] = {
    {3386, 1}, {3668, -1}, {6403, 1}, {7931, 1}
};

/* ASCII upper case is folded: same vector as "hello world". */
static const char text_mixed_case[] = "Hello WORLD";
static const entry want_mixed_case[] = {
    {1610, 1}, {3386, 1}, {3668, -1}, {4404, 1}, {5240, 1}, {5791, 1},
    {6403, 1}, {7931, 1}
};

/* Digits are word bytes. */
static const char text_digits[] = "route 66 b2b 2024";
static const entry want_digits[] = {
    {1350, 1}, {2025, -1}, {2251, -1}, {2741, -1}, {3150, -1}, {4282, 1},
    {4301, 1}, {5945, -1}, {6892, 1}, {7811, 1}
};

/* Bytes >= 0x80 are word bytes and are not case folded: the tokens are
 * "caf\303\251", "\303\211t\303\211" and "\377\376\200". */
static const char text_high_bytes[] =
    "Caf\303\251 \303\211T\303\211 \377\376\200";
static const entry want_high_bytes[] = {
    {135, -1}, {297, -1}, {902, -1}, {2812, -1}, {3711, 1}, {4049, -1},
    {4302, -1}, {4955, -1}, {6403, 1}, {7294, -1}
};

/* A lone "?" is a token. */
static const char text_question[] = "\?";
static const entry want_question[] = {
    {2391, -1}, {6403, 1}
};

/* Repeated punctuation: words "?" and "!", pairs "?""?", "?""!" and
 * "!""!", each once. */
static const char text_punct_repeated[] = "\?\? !!!";
static const entry want_punct_repeated[] = {
    {2356, -1}, {2391, -1}, {5747, 1}, {6403, 1}, {7138, 1}, {7346, -1}
};

/* Tokens "what", "?", "!": 3 words, 2 pairs, no chunk. */
static const char text_punct_adjacent[] = "what\?!";
static const entry want_punct_adjacent[] = {
    {2356, -1}, {2391, -1}, {2857, 1}, {5829, -1}, {6403, 1}, {7346, -1}
};

/* Tokens "me", "@", "example", "com". */
static const char text_at_sign[] = "me@example.com";
static const entry want_at_sign[] = {
    {226, 1}, {865, -1}, {952, -1}, {2666, 1}, {2813, -1}, {2977, 1},
    {3980, -1}, {4365, 1}, {4854, -1}, {5732, -1}, {6041, -1}, {6403, 1}
};

/* No token at all: only the length hint (32 bytes, "len1"). */
static const char text_separators[] =
    " \011\015\012.,;:-_()[]{}<>\"'#$%^&*+=/\\|~";
static const entry want_separators[] = {
    {1350, 1}
};

/* Other punctuation only separates: same vector as "a b c d". */
static const char text_other_punct[] = "a.b,c;d";
static const entry want_other_punct[] = {
    {51, 1}, {114, 1}, {1656, 1}, {2343, -1}, {5306, -1}, {5484, -1},
    {5524, 1}, {6403, 1}
};

/* Apostrophe, hyphen and underscore separate too: "don", "t", "re", "use",
 * "snake", "case". */
static const char text_inner_punct[] = "don't re-use snake_case";
static const entry want_inner_punct[] = {
    {986, -1}, {1080, 1}, {1145, 1}, {1350, 1}, {2917, 1}, {2975, -1},
    {3148, -1}, {4261, -1}, {4490, 1}, {4813, 1}, {7683, -1}, {7765, -1},
    {7861, -1}, {7872, 1}
};

/* NUL bytes are separators like any other. */
static const char text_nul_bytes[] = "a\000b\000\000c";
static const entry want_nul_bytes[] = {
    {51, 1}, {114, 1}, {1656, 1}, {5306, -1}, {5524, 1}, {6403, 1}
};

/* A repeated word and a repeated pair count once. */
static const char text_repeats[] = "the the the the";
static const entry want_repeats[] = {
    {4200, 1}, {4380, 1}, {6403, 1}
};

/* A word of 4 bytes has no chunk. */
static const char text_word4[] = "abcd";
static const entry want_word4[] = {
    {6403, 1}, {8102, -1}
};

/* A word of 5 bytes has two chunks: "^abcd" and "$bcde". */
static const char text_word5[] = "abcde";
static const entry want_word5[] = {
    {5, 1}, {222, 1}, {3452, -1}, {6403, 1}
};

/* A word of 6 bytes has three: "^abcd", ".bcde", "$cdef". */
static const char text_word6[] = "abcdef";
static const entry want_word6[] = {
    {222, 1}, {944, -1}, {2692, 1}, {6333, 1}, {6403, 1}
};

/* "shipped" and "shipping" share "^ship" and ".hipp". */
static const char text_shipped[] = "shipped";
static const entry want_shipped[] = {
    {171, -1}, {2294, -1}, {2973, -1}, {4526, 1}, {5697, 1}, {6403, 1}
};

/* See above. */
static const char text_shipping[] = "shipping";
static const entry want_shipping[] = {
    {765, -1}, {2973, -1}, {4940, 1}, {5264, 1}, {5697, 1}, {6313, -1},
    {6403, 1}
};

/* "http://" gives the "link" hint. */
static const char text_link_http[] = "see http://example.com/x";
static const entry want_link_http[] = {
    {226, 1}, {435, 1}, {482, -1}, {865, -1}, {952, -1}, {1285, 1},
    {1350, 1}, {2556, -1}, {2813, -1}, {2977, 1}, {3304, 1}, {4497, -1},
    {5191, 1}, {5732, -1}, {6041, -1}
};

/* "https://" too, in any case. */
static const char text_link_https[] = "HTTPS://Example.COM";
static const entry want_link_https[] = {
    {204, -1}, {226, 1}, {482, -1}, {693, -1}, {865, -1}, {878, 1},
    {952, -1}, {1350, 1}, {2216, -1}, {2813, -1}, {2977, 1}, {5732, -1},
    {6041, -1}
};

/* Near misses: no "link" hint. */
static const char text_no_link[] = "http:/x https:x httpx://y ftp://z";
static const entry want_no_link[] = {
    {204, -1}, {581, 1}, {693, -1}, {1285, 1}, {1350, 1}, {1573, 1},
    {2216, -1}, {3291, -1}, {3517, -1}, {4874, -1}, {5011, 1}, {5191, 1},
    {5436, -1}, {6487, 1}, {6757, 1}, {6993, -1}, {7380, 1}, {7800, -1}
};

/* Three backticks in a row give the "code" hint. */
static const char text_code[] = "run ```ls -l``` now";
static const entry want_code[] = {
    {1167, 1}, {1350, 1}, {2087, 1}, {2536, 1}, {3340, 1}, {4741, -1},
    {6071, -1}, {6438, -1}, {7463, 1}
};

/* Five backticks, never three in a row: no "code" hint. */
static const char text_no_code[] = "``x`` `y`";
static const entry want_no_code[] = {
    {1573, 1}, {4609, -1}, {5191, 1}, {6403, 1}
};

/* Words, "@", "?", "!", a link and a code block in one text. */
static const char text_everything[] =
    "Hey @bob, is https://ex.io/a\?b=1 OK\?! See ```x```";
static const entry want_everything[] = {
    {85, 0.956182897f}, {204, -0.956182897f}, {210, -0.956182897f},
    {482, -0.956182897f}, {693, -0.956182897f}, {1350, 0.956182897f},
    {1720, -0.956182897f}, {1761, 0.956182897f}, {2057, 0.956182897f},
    {2216, -0.956182897f}, {2356, -0.956182897f}, {2391, -0.956182897f},
    {2400, -0.956182897f}, {2666, 0.956182897f}, {2771, -0.956182897f},
    {3414, -0.956182897f}, {3609, 0.956182897f}, {3643, 0.956182897f},
    {3988, 0.956182897f}, {4424, 0.956182897f}, {4497, -0.956182897f},
    {4805, -0.956182897f}, {4936, 0.956182897f}, {5191, 0.956182897f},
    {5306, -0.956182897f}, {5433, 0.956182897f}, {5524, 0.956182897f},
    {5664, 0.956182897f}, {6172, 0.956182897f}, {6419, -0.956182897f},
    {6438, -0.956182897f}, {6802, 0.956182897f}, {7346, -0.956182897f},
    {7607, -0.956182897f}, {7823, -0.956182897f}
};

/* 16 words, 15 pairs and a hint in 32 different buckets: the squared norm
 * is exactly 32, which is not above the cap. */
static const char text_clues32[] = "a b c d e f g h i j k l m n o p";
static const entry want_clues32[] = {
    {51, 1}, {114, 1}, {208, 1}, {290, -1}, {462, 1}, {565, -1}, {1220, 1},
    {1350, 1}, {1656, 1}, {2343, -1}, {2856, 1}, {3390, 1}, {4333, -1},
    {4363, 1}, {5091, 1}, {5116, 1}, {5275, 1}, {5306, -1}, {5360, 1},
    {5484, -1}, {5524, 1}, {5875, 1}, {6099, -1}, {6199, -1}, {6341, 1},
    {6613, -1}, {7035, -1}, {7230, 1}, {7463, 1}, {7559, 1}, {7655, -1},
    {8117, -1}
};

/* One more pair makes 33 clues: every value becomes +-sqrt(32/33). */
static const char text_clues33[] = "a b c d e f g h i j k l m n o p a";
static const entry want_clues33[] = {
    {51, 0.984731913f}, {114, 0.984731913f}, {208, 0.984731913f},
    {290, -0.984731913f}, {344, 0.984731913f}, {462, 0.984731913f},
    {565, -0.984731913f}, {1220, 0.984731913f}, {1350, 0.984731913f},
    {1656, 0.984731913f}, {2343, -0.984731913f}, {2856, 0.984731913f},
    {3390, 0.984731913f}, {4333, -0.984731913f}, {4363, 0.984731913f},
    {5091, 0.984731913f}, {5116, 0.984731913f}, {5275, 0.984731913f},
    {5306, -0.984731913f}, {5360, 0.984731913f}, {5484, -0.984731913f},
    {5524, 0.984731913f}, {5875, 0.984731913f}, {6099, -0.984731913f},
    {6199, -0.984731913f}, {6341, 0.984731913f}, {6613, -0.984731913f},
    {7035, -0.984731913f}, {7230, 0.984731913f}, {7463, 0.984731913f},
    {7559, 0.984731913f}, {7655, -0.984731913f}, {8117, -0.984731913f}
};

/* 52 clues: 14 words, 14 pairs, 23 chunks and "len2". The vector is scaled
 * down to a squared norm of 32. */
static const char text_pangram[] =
    "The quick brown fox jumps over the lazy dog while seven wizards "
    "quietly juggle boxes";
static const entry want_pangram[] = {
    {77, -0.784464538f}, {110, -0.784464538f}, {165, -0.784464538f},
    {375, -0.784464538f}, {435, 0.784464538f}, {641, -0.784464538f},
    {966, 0.784464538f}, {1112, -0.784464538f}, {1347, -0.784464538f},
    {1528, 0.784464538f}, {2010, -0.784464538f}, {2338, -0.784464538f},
    {2462, 0.784464538f}, {2539, 0.784464538f}, {2688, -0.784464538f},
    {2769, 0.784464538f}, {2900, -0.784464538f}, {2906, 0.784464538f},
    {3094, 0.784464538f}, {3436, 0.784464538f}, {3485, -0.784464538f},
    {3533, -0.784464538f}, {3807, 0.784464538f}, {4017, -0.784464538f},
    {4186, -0.784464538f}, {4200, 0.784464538f}, {4215, 0.784464538f},
    {4265, 0.784464538f}, {4425, 0.784464538f}, {4458, 0.784464538f},
    {4549, -0.784464538f}, {4555, 0.784464538f}, {4690, -0.784464538f},
    {4807, -0.784464538f}, {4909, -0.784464538f}, {4940, 0.784464538f},
    {5194, 0.784464538f}, {5311, -0.784464538f}, {5556, 0.784464538f},
    {5770, -0.784464538f}, {6044, -0.784464538f}, {6417, -0.784464538f},
    {6503, 0.784464538f}, {6725, -0.784464538f}, {6770, -0.784464538f},
    {6848, 0.784464538f}, {7051, -0.784464538f}, {7521, 0.784464538f},
    {7651, -0.784464538f}, {7663, 0.784464538f}, {7701, -0.784464538f},
    {7942, 0.784464538f}
};

/* The words "ec" and "je" hash to bucket 6139 with the same sign: the
 * bucket holds 2. */
static const char text_same_sign[] = "ec je";
static const entry want_same_sign[] = {
    {3721, -1}, {6139, 2}, {6403, 1}
};

/* The words "al" and "di" hash to bucket 549 with opposite signs: the
 * bucket is dropped, the pair and "len0" remain. */
static const char text_opposite_sign[] = "al di";
static const entry want_opposite_sign[] = {
    {1686, 1}, {6403, 1}
};

/* --- Texts the test builds with fill() and put(). FNV_x is the FNV-1a
 * hash of the text the reference was given, so a text built differently
 * is reported as such. --- */

/* 16 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len0". */
#define FNV_len16 0xf7fe1856a6772605ull
static const entry want_len16[] = {
    {1656, 1}, {5306, -1}, {5524, 1}, {6278, -1}, {6403, 1}
};

/* 17 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len1". */
#define FNV_len17 0xdbe9bf3cdc763becull
static const entry want_len17[] = {
    {1350, 1}, {1656, 1}, {5306, -1}, {5524, 1}, {6278, -1}
};

/* 64 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len1". */
#define FNV_len64 0x5e44007e3c8d44a5ull
static const entry want_len64[] = {
    {1350, 1}, {1656, 1}, {5306, -1}, {5524, 1}, {6278, -1}
};

/* 65 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len2". */
#define FNV_len65 0xbad19a80e40bd90cull
static const entry want_len65[] = {
    {1656, 1}, {5306, -1}, {5524, 1}, {5770, -1}, {6278, -1}
};

/* 256 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len2". */
#define FNV_len256 0x5f0a0b0d9157a925ull
static const entry want_len256[] = {
    {1656, 1}, {5306, -1}, {5524, 1}, {5770, -1}, {6278, -1}
};

/* 257 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len3". */
#define FNV_len257 0xd5ba0c0df7f49e8cull
static const entry want_len257[] = {
    {1656, 1}, {2159, -1}, {5306, -1}, {5524, 1}, {6278, -1}
};

/* 1024 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len3". */
#define FNV_len1024 0xe6e2bbce41883b25ull
static const entry want_len1024[] = {
    {1656, 1}, {2159, -1}, {5306, -1}, {5524, 1}, {6278, -1}
};

/* 1025 bytes of "a b a b ...": words "a" and "b", pairs "a""b" and "b""a",
 * and "len4". */
#define FNV_len1025 0xdb8063795a7cb48cull
static const entry want_len1025[] = {
    {1656, 1}, {3491, -1}, {5306, -1}, {5524, 1}, {6278, -1}
};

/* A word of 70 bytes and "end". The word is cut to 64 bytes: its clue, the
 * pair with "end" and 61 windows ("^abcd", "$abcd" and 10 different "."
 * ones) use the cut word; the 6 bytes cut off are not a token. */
#define FNV_word70 0x4a37c1a1da777c8full
static const entry want_word70[] = {
    {10, -1}, {222, 1}, {2752, 1}, {3117, 1}, {3409, -1}, {3611, 1},
    {3620, -1}, {5553, -1}, {5594, -1}, {5770, -1}, {6004, -1}, {6275, 1},
    {6333, 1}, {6535, 1}, {6833, -1}, {7855, -1}
};

/* A word of exactly 64 bytes is not cut ("len1"). */
#define FNV_word64 0xc41b04dc59962f49ull
static const entry want_word64[] = {
    {222, 1}, {1350, 1}, {2752, 1}, {3117, 1}, {3409, -1}, {3611, 1},
    {3620, -1}, {5594, -1}, {6004, -1}, {6275, 1}, {6333, 1}, {6535, 1},
    {6833, -1}, {7855, -1}
};

/* A word of 65 bytes is cut to the same 64: only the length hint differs
 * ("len2"). */
#define FNV_word65 0xd0186e6c3a3227c4ull
static const entry want_word65[] = {
    {222, 1}, {2752, 1}, {3117, 1}, {3409, -1}, {3611, 1}, {3620, -1},
    {5594, -1}, {5770, -1}, {6004, -1}, {6275, 1}, {6333, 1}, {6535, 1},
    {6833, -1}, {7855, -1}
};

/* 5000 bytes. The word "abcdefghij" starts at offset 4090, so the prefix
 * ends inside it: the token is "abcdef" (chunks "^abcd", ".bcde",
 * "$cdef"), and "ghij" and "klm" are never seen. Hint "len4". */
#define FNV_mid_word 0xbbf97e5be5dfb405ull
static const entry want_mid_word[] = {
    {222, 1}, {714, 1}, {944, -1}, {1656, 1}, {2692, 1}, {3491, -1},
    {5306, -1}, {5524, 1}, {6278, -1}, {6333, 1}
};

/* A link and a code block right after the prefix (offset 4096): neither
 * hint. */
#define FNV_beyond 0xec8d8c4364336d7aull
static const entry want_beyond[] = {
    {1656, 1}, {3491, -1}, {5306, -1}, {5524, 1}, {6278, -1}
};

/* "http://" at offset 4090: its last byte is outside the prefix. No "link"
 * hint; "http" is a word. */
#define FNV_link_cut 0x39e6dde0b27ca179ull
static const entry want_link_cut[] = {
    {1285, 1}, {1656, 1}, {2732, 1}, {3491, -1}, {5306, -1}, {5524, 1},
    {6278, -1}
};

/* "http://" at offset 4089 ends with the prefix: "link" hint. It follows
 * an "a", so the word is "ahttp". */
#define FNV_link_end 0x7b6419cb242b3588ull
static const entry want_link_end[] = {
    {192, 1}, {482, -1}, {1656, 1}, {3491, -1}, {4052, 1}, {4851, 1},
    {5306, -1}, {5524, 1}, {6278, -1}, {7449, -1}
};

/* Three backticks at offset 4094: only two are in the prefix. No "code"
 * hint. */
#define FNV_code_cut 0x461fd629bd58dea6ull
static const entry want_code_cut[] = {
    {1656, 1}, {3491, -1}, {5306, -1}, {5524, 1}, {6278, -1}
};

/* Three backticks at offset 4093, the last 3 bytes of the prefix. */
#define FNV_code_end 0x86982e857d00d327ull
static const entry want_code_end[] = {
    {1656, 1}, {3491, -1}, {5306, -1}, {5524, 1}, {6278, -1}, {6438, -1}
};

/* 4096 question marks: 8191 clues before repeats are dropped, 3 after
 * ("?", "?""?", "len4"). */
#define FNV_all_question 0x96d7b490f997f325ull
static const entry want_all_question[] = {
    {2391, -1}, {3491, -1}, {5747, 1}
};

/* "a?a?..." for 4096 bytes: every byte is a token. */
#define FNV_all_tokens 0x9873c551c0c1a325ull
static const entry want_all_tokens[] = {
    {548, -1}, {2391, -1}, {3491, -1}, {3643, 1}, {5524, 1}
};

/* --- Named features. --- */

/* Text "hi". Values pass through unchanged. */
static const gateNamedFeature feat_pass_through[] = {
    {"mentioned", 9, 1.0}, {"thread_depth", 12, 0.3}
};
static const entry want_pass_through[] = {
    {2341, 0.300000012f}, {4565, 1}, {6403, 1}, {6657, -1}
};

/* Text "". A named feature has no sign of its own: the value is added as
 * given. */
static const gateNamedFeature feat_negative[] = {
    {"score", 5, -0.75}
};
static const entry want_negative[] = {
    {4592, -0.75f}, {6403, 1}
};

/* Text "". 100 is clamped to 4 before the norm is capped: (4, 1) scaled by
 * sqrt(8/17). Without the clamp "small" would come out as 0.028. */
static const gateNamedFeature feat_clamp_high[] = {
    {"big", 3, 100.0}, {"small", 5, 1.0}
};
static const entry want_clamp_high[] = {
    {5955, 0.685994327f}, {6403, 1}, {8024, 2.74397731f}
};

/* Text "". -1e9 is clamped to -4. */
static const gateNamedFeature feat_clamp_low[] = {
    {"neg", 3, -1000000000.0}, {"small", 5, 1.0}
};
static const entry want_clamp_low[] = {
    {5955, 0.685994327f}, {6403, 1}, {8125, -2.74397731f}
};

/* Text "". A squared norm of exactly 8 is not above the cap. */
static const gateNamedFeature feat_at_cap[] = {
    {"a", 1, 2.0}, {"b", 1, 2.0}
};
static const entry want_at_cap[] = {
    {1892, 2}, {5878, 2}, {6403, 1}
};

/* Text "". A squared norm of 12 is scaled down to 8: three times 2
 * sqrt(8/12). */
static const gateNamedFeature feat_above_cap[] = {
    {"a", 1, 2.0}, {"b", 1, 2.0}, {"c", 1, 2.0}
};
static const entry want_above_cap[] = {
    {1892, 1.63299322f}, {2286, 1.63299322f}, {5878, 1.63299322f}, {6403, 1}
};

/* Text "". A value of zero leaves no entry. */
static const gateNamedFeature feat_zero_value[] = {
    {"zero", 4, 0.0}, {"half", 4, 0.5}
};
static const entry want_zero_value[] = {
    {2854, 0.5f}, {6403, 1}
};

/* Text "". Neither does a value too small for a float. */
static const gateNamedFeature feat_tiny_value[] = {
    {"tiny", 4, 1e-60}, {"half", 4, 0.5}
};
static const entry want_tiny_value[] = {
    {2854, 0.5f}, {6403, 1}
};

/* Text text_pangram. The text part (squared norm 32) and the named part
 * (squared norm 8) are scaled separately. */
static const gateNamedFeature feat_both_capped[] = {
    {"a", 1, 3.0}, {"b", 1, -3.0}, {"c", 1, 3.0}
};
static const entry want_both_capped[] = {
    {77, -0.784464538f}, {110, -0.784464538f}, {165, -0.784464538f},
    {375, -0.784464538f}, {435, 0.784464538f}, {641, -0.784464538f},
    {966, 0.784464538f}, {1112, -0.784464538f}, {1347, -0.784464538f},
    {1528, 0.784464538f}, {1892, -1.63299322f}, {2010, -0.784464538f},
    {2286, 1.63299322f}, {2338, -0.784464538f}, {2462, 0.784464538f},
    {2539, 0.784464538f}, {2688, -0.784464538f}, {2769, 0.784464538f},
    {2900, -0.784464538f}, {2906, 0.784464538f}, {3094, 0.784464538f},
    {3436, 0.784464538f}, {3485, -0.784464538f}, {3533, -0.784464538f},
    {3807, 0.784464538f}, {4017, -0.784464538f}, {4186, -0.784464538f},
    {4200, 0.784464538f}, {4215, 0.784464538f}, {4265, 0.784464538f},
    {4425, 0.784464538f}, {4458, 0.784464538f}, {4549, -0.784464538f},
    {4555, 0.784464538f}, {4690, -0.784464538f}, {4807, -0.784464538f},
    {4909, -0.784464538f}, {4940, 0.784464538f}, {5194, 0.784464538f},
    {5311, -0.784464538f}, {5556, 0.784464538f}, {5770, -0.784464538f},
    {5878, 1.63299322f}, {6044, -0.784464538f}, {6417, -0.784464538f},
    {6503, 0.784464538f}, {6725, -0.784464538f}, {6770, -0.784464538f},
    {6848, 0.784464538f}, {7051, -0.784464538f}, {7521, 0.784464538f},
    {7651, -0.784464538f}, {7663, 0.784464538f}, {7701, -0.784464538f},
    {7942, 0.784464538f}
};

/* Text "hello". The name "f13762" lands in bucket 3668, where the word
 * "hello" adds -1: the bucket holds -0.75. */
static const gateNamedFeature feat_shared_bucket[] = {
    {"f13762", 6, 0.25}
};
static const entry want_shared_bucket[] = {
    {3386, 1}, {3668, -0.75f}, {6403, 1}, {7931, 1}
};

/* Text "". The name "f139" lands in bucket 6403 like "len0" (+1): with a
 * value of -1 nothing is left, the vector is empty. */
static const gateNamedFeature feat_cancelled[] = {
    {"f139", 4, -1.0}
};

/* Text "". The names "f16" and "f58" share bucket 3581: their values add
 * up. */
static const gateNamedFeature feat_two_names[] = {
    {"f16", 3, 1.5}, {"f58", 3, 0.25}
};
static const entry want_two_names[] = {
    {3581, 1.75f}, {6403, 1}
};

/* Text "". The same two names with opposite values leave no entry. */
static const gateNamedFeature feat_two_names_cancel[] = {
    {"f16", 3, 1.5}, {"f58", 3, -1.5}, {"half", 4, 0.5}
};
static const entry want_two_names_cancel[] = {
    {2854, 0.5f}, {6403, 1}
};

/* Text "". GATE_MAX_FEATURES features, "k0" to "k63", of value 1. Two of
 * them share bucket 6085, so the squared norm is 66 before it is capped. */
static const entry want_max_features[] = {
    {22, 0.34815532f}, {98, 0.34815532f}, {208, 0.34815532f},
    {282, 0.34815532f}, {368, 0.34815532f}, {872, 0.34815532f},
    {1004, 0.34815532f}, {1061, 0.34815532f}, {1121, 0.34815532f},
    {1130, 0.34815532f}, {1145, 0.34815532f}, {1247, 0.34815532f},
    {1420, 0.34815532f}, {1503, 0.34815532f}, {1568, 0.34815532f},
    {1663, 0.34815532f}, {1809, 0.34815532f}, {1903, 0.34815532f},
    {1935, 0.34815532f}, {2066, 0.34815532f}, {2333, 0.34815532f},
    {2791, 0.34815532f}, {2839, 0.34815532f}, {2998, 0.34815532f},
    {3023, 0.34815532f}, {3027, 0.34815532f}, {3216, 0.34815532f},
    {3229, 0.34815532f}, {3239, 0.34815532f}, {3265, 0.34815532f},
    {3303, 0.34815532f}, {3322, 0.34815532f}, {3549, 0.34815532f},
    {3636, 0.34815532f}, {3767, 0.34815532f}, {3945, 0.34815532f},
    {3946, 0.34815532f}, {4037, 0.34815532f}, {4046, 0.34815532f},
    {4434, 0.34815532f}, {4499, 0.34815532f}, {4759, 0.34815532f},
    {5150, 0.34815532f}, {5206, 0.34815532f}, {5385, 0.34815532f},
    {5727, 0.34815532f}, {5787, 0.34815532f}, {6085, 0.696310639f},
    {6200, 0.34815532f}, {6281, 0.34815532f}, {6331, 0.34815532f},
    {6403, 1}, {6702, 0.34815532f}, {6826, 0.34815532f},
    {6841, 0.34815532f}, {7006, 0.34815532f}, {7030, 0.34815532f},
    {7051, 0.34815532f}, {7783, 0.34815532f}, {7951, 0.34815532f},
    {8025, 0.34815532f}, {8066, 0.34815532f}, {8071, 0.34815532f},
    {8155, 0.34815532f}
};

/* --- gateHash(domain, bytes). --- */
static const struct {
    uint8_t domain;
    const char *bytes;
    size_t len;
    uint64_t hash;
} hashes[] = {
    {1, "hello", 5, 0xd6a20fc8f914ae54ull},
    {1, "\?", 1, 0xe8532f34d5702957ull},
    {1, "", 0, 0x0d7cea42b5057e4cull},
    {2, "hello\000world", 11, 0x66c463749d929134ull},
    {2, "\?\000!", 3, 0xb32421bd3215a934ull},
    {3, "^ship", 5, 0xfe0e3ae5c58f4b9dull},
    {3, ".hipp", 5, 0x7e57fb02d1fb7641ull},
    {3, "$ping", 5, 0x0a34dbe25991534cull},
    {4, "len0", 4, 0x5613edb7cfde1903ull},
    {4, "len4", 4, 0xa3638e305c8a4da3ull},
    {4, "link", 4, 0xf586c48da59ba1e2ull},
    {4, "code", 4, 0xa62c7228cceaf926ull},
    {5, "mentioned", 9, 0xda0f4928d311d1d5ull},
    {5, "thread_depth", 12, 0x093edc2acd2f0925ull},
    {0, "hello", 5, 0x8a5df5c78e233bb6ull},
    {255, "hello", 5, 0x6baf5f8e9e9b30a6ull},
    {5, "caf\303\251", 5, 0x90566bd8078a1884ull},
};

/* --------------------------------- Hashes -------------------------------- */

TEST(fnv1a64_known_values) {
    /* The vectors published with the hash. */
    CHECK(gateFnv1a64("", 0) == 0xcbf29ce484222325ull);
    CHECK(gateFnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(gateFnv1a64("foobar", 6) == 0x85944171f73967e8ull);
    /* From the reference: a NUL and two bytes >= 0x80, which must be hashed
     * as the unsigned bytes they are. */
    CHECK(gateFnv1a64("\000\200\377", 3) == 0xd79a37186a9dda16ull);
}

TEST(hash_known_values) {
    size_t i;

    for (i = 0; i < ARRAY_LEN(hashes); i++) {
        uint64_t h = gateHash(hashes[i].domain, hashes[i].bytes, hashes[i].len);

        CHECK(h == hashes[i].hash);
        if (h != hashes[i].hash)
            printf("    ^ hashes[%d]: got 0x%016llx\n", (int)i, (unsigned long long)h);
    }
}

TEST(bucket_and_sign) {
    /* Worked by hand from two rows of hashes[]. H(word, "?") ends in 0x2957,
     * whose low 13 bits are 0x0957 = 2391, and starts with 0xe: bit 63 is
     * set, the clue subtracts 1. H(hint, "len0") ends in 0x1903 = 6403 and
     * starts with 0x5: bit 63 is clear, the clue adds 1. */
    CHECK(gateHash(0x01, "?", 1) % GATE_DIM == 2391 && gateHash(0x01, "?", 1) >> 63 == 1);
    CHECK(gateHash(0x04, "len0", 4) % GATE_DIM == 6403 && gateHash(0x04, "len0", 4) >> 63 == 0);
    EXPECT_TEXT(question);
}

/* ---------------------------------- Text --------------------------------- */

TEST(empty_text) {
    gateSparse x = textVector(NULL, 0), y = textVector("", 0);

    EXPECT_TEXT(empty);
    CHECK(x.n == 1 && sameSparse(&x, &y));
    gateSparseFree(&x);
    gateSparseFree(&y);
}

TEST(words_and_chunks) {
    EXPECT_TEXT(hello);
    EXPECT_TEXT(word4);
    EXPECT_TEXT(word5);
    EXPECT_TEXT(word6);
    EXPECT_TEXT(shipped);
    EXPECT_TEXT(shipping);
}

TEST(case_folding) {
    EXPECT_TEXT(mixed_case);
    CHECK(sameVector("Hello WORLD", "hello world"));
    CHECK(sameVector("SHIPPING", "shipping"));
    CHECK(!sameVector("hello world", "hello word"));
}

TEST(word_bytes) {
    EXPECT_TEXT(digits);
    EXPECT_TEXT(high_bytes);
    /* U+00C9 and U+00E9, an upper and a lower case letter outside ASCII,
     * stay different. */
    CHECK(!sameVector("\303\211", "\303\251"));
}

TEST(punctuation_tokens) {
    EXPECT_TEXT(question);
    EXPECT_TEXT(punct_repeated);
    EXPECT_TEXT(punct_adjacent);
    EXPECT_TEXT(at_sign);
}

TEST(separators) {
    EXPECT_TEXT(separators);
    EXPECT_TEXT(other_punct);
    EXPECT_TEXT(inner_punct);
    EXPECT_TEXT(nul_bytes);
    /* Which separator, and how many, makes no difference (all three texts
     * are in the same length bucket). */
    CHECK(sameVector("a.b,c;d", "a b c d"));
    CHECK(sameVector("a.b,c;d", "  a\tb\n\nc -- d  "));
    /* The order of the tokens does: the pairs change. */
    CHECK(!sameVector("a b c d", "a b d c"));
}

TEST(repeats_count_once) {
    EXPECT_TEXT(repeats);
    CHECK(sameVector("the the the the", "the the"));
    CHECK(sameVector("what?! what?!", "what?! what?"));
    CHECK(!sameVector("the the", "the"));
}

TEST(long_words_are_cut) {
    char buf[74];

    fill(buf, 70, "abcdefghij");
    put(buf, 70, " end");
    EXPECT_BUILT(word70, buf, sizeof(buf));
    EXPECT_BUILT(word64, buf, 64);
    EXPECT_BUILT(word65, buf, 65);
}

TEST(length_buckets) {
    char buf[1025];

    /* On either side of a boundary the tokens are the same, only the hint
     * moves to another bucket. */
    fill(buf, sizeof(buf), "a b ");
    EXPECT_BUILT(len16, buf, 16);
    EXPECT_BUILT(len17, buf, 17);
    EXPECT_BUILT(len64, buf, 64);
    EXPECT_BUILT(len65, buf, 65);
    EXPECT_BUILT(len256, buf, 256);
    EXPECT_BUILT(len257, buf, 257);
    EXPECT_BUILT(len1024, buf, 1024);
    EXPECT_BUILT(len1025, buf, 1025);
}

TEST(links_and_code) {
    EXPECT_TEXT(link_http);
    EXPECT_TEXT(link_https);
    EXPECT_TEXT(no_link);
    EXPECT_TEXT(code);
    EXPECT_TEXT(no_code);
    EXPECT_TEXT(everything);
}

#define LONG_TEXT 5000

/* LONG_TEXT bytes of "a b a b ..." with 's' written at offset 'at'. */
static void longText(char *buf, size_t at, const char *s) {
    fill(buf, LONG_TEXT, "a b ");
    put(buf, at, s);
}

TEST(only_the_prefix_counts) {
    char buf[LONG_TEXT];
    gateSparse x, y;

    longText(buf, 4090, "abcdefghij klm");
    EXPECT_BUILT(mid_word, buf, LONG_TEXT);
    /* Whatever follows the prefix changes nothing. */
    x = textVector(buf, LONG_TEXT);
    memset(buf + GATE_TEXT_READ_BYTES, '?', LONG_TEXT - GATE_TEXT_READ_BYTES);
    y = textVector(buf, LONG_TEXT);
    CHECK(sameSparse(&x, &y));
    gateSparseFree(&x);
    gateSparseFree(&y);

    longText(buf, 4096, "http://x ```");
    EXPECT_BUILT(beyond, buf, LONG_TEXT);
    longText(buf, 4090, "http://x");
    EXPECT_BUILT(link_cut, buf, LONG_TEXT);
    longText(buf, 4089, "http://");
    EXPECT_BUILT(link_end, buf, LONG_TEXT);
    longText(buf, 4094, "```");
    EXPECT_BUILT(code_cut, buf, LONG_TEXT);
    longText(buf, 4093, "```");
    EXPECT_BUILT(code_end, buf, LONG_TEXT);
}

TEST(every_byte_a_token) {
    char buf[GATE_TEXT_READ_BYTES];

    /* Two clues per byte before the repeats are dropped: the most a prefix
     * can produce, and what the scratch memory must hold. */
    memset(buf, '?', sizeof(buf));
    EXPECT_BUILT(all_question, buf, sizeof(buf));
    fill(buf, sizeof(buf), "a?");
    EXPECT_BUILT(all_tokens, buf, sizeof(buf));
}

TEST(clues_that_share_a_bucket) {
    uint64_t ec = gateHash(0x01, "ec", 2), je = gateHash(0x01, "je", 2);
    uint64_t al = gateHash(0x01, "al", 2), di = gateHash(0x01, "di", 2);

    CHECK(ec % GATE_DIM == je % GATE_DIM && ec >> 63 == je >> 63);
    EXPECT_TEXT(same_sign);
    CHECK(al % GATE_DIM == di % GATE_DIM && al >> 63 != di >> 63);
    EXPECT_TEXT(opposite_sign);
}

/* Squared norm of the vector of a text. */
static double textNorm2(const char *text, size_t len) {
    gateSparse x = textVector(text, len);
    double norm2 = squaredNorm(&x);

    gateSparseFree(&x);
    return norm2;
}

TEST(norm_cap) {
    char words[4000];
    uint64_t rng = 32;
    gateSparse x;
    size_t i;

    /* 32 clues in 32 buckets are at the cap, not above it: every value is
     * still +1 or -1. */
    EXPECT_TEXT(clues32);
    CHECK(textNorm2(text_clues32, sizeof(text_clues32) - 1) == 32.0);
    /* One clue more and the vector is scaled down to the cap. */
    EXPECT_TEXT(clues33);
    CHECK_NEAR(textNorm2(text_clues33, sizeof(text_clues33) - 1), 32.0, 1e-4);
    EXPECT_TEXT(pangram);
    CHECK_NEAR(textNorm2(text_pangram, sizeof(text_pangram) - 1), 32.0, 1e-4);

    /* Random words, a few thousand clues. */
    for (i = 0; i < sizeof(words); i++) {
        uint64_t r = gate_test_rand64(&rng);

        words[i] = r % 7 == 0 ? ' ' : (char)('a' + (r >> 8) % 26);
    }
    x = textVector(words, sizeof(words));
    CHECK(x.n > 1000 && wellFormed(&x));
    CHECK_NEAR(squaredNorm(&x), 32.0, 1e-4);
    gateSparseFree(&x);
}

/* ----------------------------- Named features ---------------------------- */

TEST(named_values) {
    EXPECT_NAMED(pass_through, "hi");
    EXPECT_NAMED(negative, "");
    EXPECT_NAMED(zero_value, "");
    EXPECT_NAMED(tiny_value, "");
}

TEST(named_clamp_and_cap) {
    gateSparse x = {0, NULL, NULL};

    EXPECT_NAMED(clamp_high, "");
    EXPECT_NAMED(clamp_low, "");
    EXPECT_NAMED(at_cap, "");
    EXPECT_NAMED(above_cap, "");
    /* Each part has its own cap: 32 for the text, 8 for the names. */
    EXPECT_NAMED(both_capped, text_pangram);
    CHECK(gateFeaturize(text_pangram, sizeof(text_pangram) - 1, feat_both_capped, 3, &x,
                        NULL, 0) == GATE_OK);
    CHECK_NEAR(squaredNorm(&x), 40.0, 1e-4);
    gateSparseFree(&x);
}

TEST(named_shared_buckets) {
    uint32_t hello = (uint32_t)(gateHash(0x01, "hello", 5) % GATE_DIM);
    uint32_t len0 = (uint32_t)(gateHash(0x04, "len0", 4) % GATE_DIM);

    /* A name in the bucket of a text clue. */
    CHECK(gateHash(0x05, "f13762", 6) % GATE_DIM == hello);
    EXPECT_NAMED(shared_bucket, "hello");
    /* Two names in one bucket. */
    CHECK(gateHash(0x05, "f16", 3) % GATE_DIM == gateHash(0x05, "f58", 3) % GATE_DIM);
    EXPECT_NAMED(two_names, "");
    EXPECT_NAMED(two_names_cancel, "");
    /* A name that cancels the only clue of the empty text: no entry is
     * left, and that is not an error. */
    CHECK(gateHash(0x05, "f139", 4) % GATE_DIM == len0);
    expectVector("cancelled", "", 0, feat_cancelled, 1, NULL, 0);
}

/* A request that must be refused: GATE_ERR, the reason in 'err', nothing in
 * 'out', and no trouble from a missing or short 'err'. */
static void expectError(const gateNamedFeature *f, uint32_t nf, const char *reason) {
    gateSparse x = {0, NULL, NULL};
    char err[64];

    memset(err, '#', sizeof(err));
    err[sizeof(err) - 1] = '\0';
    CHECK(gateFeaturize("some text", 9, f, nf, &x, err, sizeof(err)) == GATE_ERR);
    CHECK_EQ_STR(err, reason);

    CHECK(gateFeaturize("some text", 9, f, nf, &x, NULL, 0) == GATE_ERR);
    CHECK(gateFeaturize("some text", 9, f, nf, &x, NULL, sizeof(err)) == GATE_ERR);
    /* No room at all: nothing is written. */
    memset(err, '#', sizeof(err));
    CHECK(gateFeaturize("some text", 9, f, nf, &x, err, 0) == GATE_ERR);
    CHECK(err[0] == '#');
    /* Room for 7 characters: the reason is cut, the buffer is not overrun. */
    CHECK(gateFeaturize("some text", 9, f, nf, &x, err, 8) == GATE_ERR);
    CHECK(err[7] == '\0' && strlen(err) == 7 && strncmp(err, reason, 7) == 0);
    CHECK(err[8] == '#');

    CHECK(x.n == 0 && x.idx == NULL && x.val == NULL);
}

TEST(repeated_name) {
    static const gateNamedFeature twice[] = {
        {"depth", 5, 1.0}, {"mentioned", 9, 1.0}, {"depth", 5, 2.0}
    };
    /* A value of zero adds nothing, the name is repeated all the same. */
    static const gateNamedFeature zeros[] = {{"depth", 5, 0.0}, {"depth", 5, 0.0}};
    /* Names are compared byte for byte: these four are different. */
    static const gateNamedFeature different[] = {
        {"depth", 5, 0.5}, {"Depth", 5, 0.5}, {"dept", 4, 0.5}, {"depth ", 6, 0.5}
    };
    gateSparse x = {0, NULL, NULL};

    expectError(twice, 3, "feature name given twice");
    expectError(zeros, 2, "feature name given twice");
    CHECK(gateFeaturize("", 0, different, 4, &x, NULL, 0) == GATE_OK);
    CHECK_EQ_INT(x.n, 5); /* four names and "len0" */
    gateSparseFree(&x);
}

TEST(feature_count_limit) {
    char names[GATE_MAX_FEATURES + 1][8];
    gateNamedFeature f[GATE_MAX_FEATURES + 1];
    uint32_t i;

    for (i = 0; i <= GATE_MAX_FEATURES; i++) {
        f[i].name = names[i];
        f[i].name_len = (size_t)snprintf(names[i], sizeof(names[i]), "k%u", i);
        f[i].value = 1.0;
    }
    expectVector("max_features", "", 0, f, GATE_MAX_FEATURES, WANT(max_features));
    expectError(f, GATE_MAX_FEATURES + 1, "too many features");
}

TEST(allocation_failure) {
    static const gateNamedFeature f[] = {{"depth", 5, 0.5}};
    int failed = 0, ret;

    /* Fail the first allocation of a request, then the second, and so on
     * until the request goes through. */
    countAllocations();
    do {
        gateSparse x = {0, NULL, NULL};
        char err[64] = "";

        allocCalls = 0;
        allocFailAt = failed + 1;
        ret = gateFeaturize("some text?", 10, f, 1, &x, err, sizeof(err));
        allocFailAt = 0;
        if (ret == GATE_OK) {
            /* All that is left is the result: its two arrays. */
            CHECK_EQ_INT(allocLive, 2);
            CHECK(x.n > 0 && wellFormed(&x));
            gateSparseFree(&x);
        } else {
            failed++;
            CHECK(x.n == 0 && x.idx == NULL && x.val == NULL);
            CHECK_EQ_STR(err, "out of memory");
        }
        CHECK_EQ_INT(allocLive, 0);
    } while (ret != GATE_OK && failed < 8);
    stopCounting();
    /* One block of scratch memory and the two arrays of the result. */
    CHECK_EQ_INT(failed, 3);
}

/* ----------------------------- Random requests --------------------------- */

#define RANDOM_TEXTS 2000
#define RANDOM_TEXTS_SEED 0x67617465ull
#define RANDOM_TEXTS_DIGEST 0x3adc9f51c31bb582ull

TEST(random_texts) {
    unsigned char *text = xmalloc(RANDOM_TEXT_MAX);
    uint64_t rng = RANDOM_TEXTS_SEED, digest = 0xcbf29ce484222325ull;
    int r;

    for (r = 0; r < RANDOM_TEXTS; r++) {
        size_t len = randText(&rng, text);
        char *copy = exactCopy((char *)text, len);
        gateSparse x = textVector(copy, len), again = textVector((char *)text, len);

        CHECK(wellFormed(&x));
        /* The same bytes at another address give the same bits. */
        CHECK(sameSparse(&x, &again));
        digest = digestVector(digest, &x);
        gateSparseFree(&x);
        gateSparseFree(&again);
        free(copy);
    }
    /* Every one of them is, bit for bit, what the reference computes. */
    CHECK(digest == RANDOM_TEXTS_DIGEST);
    free(text);
}

TEST(random_requests_with_features) {
    static double want[GATE_DIM];
    unsigned char *text = xmalloc(RANDOM_TEXT_MAX);
    char names[GATE_MAX_FEATURES][24];
    gateNamedFeature f[GATE_MAX_FEATURES];
    uint64_t rng = 0x6e616d6564ull;
    unsigned r;

    for (r = 0; r < 400; r++) {
        size_t len = randText(&rng, text);
        uint32_t nf = (uint32_t)(gate_test_rand64(&rng) % (GATE_MAX_FEATURES + 1)), i, j = 0;
        gateSparse t = textVector((char *)text, len), x = {0, NULL, NULL};
        int ok;

        for (i = 0; i < nf; i++) {
            uint64_t kind = gate_test_rand64(&rng) % 8;
            /* Zero, too small for a float, far beyond the clamp, the size
             * of a clue (so that some cancel one), or anything. */
            static const double special[] = {0.0, 1e-50, 1e9, -1e9, 1.0, -1.0};

            f[i].name = names[i];
            f[i].name_len = (size_t)snprintf(names[i], sizeof(names[i]), "n%u.%u", r, i);
            f[i].value = kind < ARRAY_LEN(special) ? special[kind]
                                                   : 12.0 * gate_test_uniform(&rng) - 6.0;
        }
        CHECK(gateFeaturize((char *)text, len, f, nf, &x, NULL, 0) == GATE_OK);

        /* The named part on its own, plus the text part on its own. */
        refNamed(f, nf, want);
        for (i = 0; i < t.n; i++) want[t.idx[i]] += (double)t.val[i];
        ok = wellFormed(&x);
        for (i = 0; i < GATE_DIM; i++) {
            double have = (j < x.n && x.idx[j] == i) ? (double)x.val[j++] : 0.0;

            if (!(fabs(have - want[i]) <= 1e-6)) ok = 0;
        }
        CHECK(ok && j == x.n);
        gateSparseFree(&t);
        gateSparseFree(&x);
    }
    free(text);
}

/* ------------------------------ Long texts ------------------------------- */

TEST(nothing_past_the_prefix_is_read) {
    /* What the prefix ends with, and the byte that follows it: each pair
     * tempts a careless reader to look one byte too far. */
    static const struct {
        const char *tail, *next;
    } ends[] = {
        {"http:/", "/"}, {"https:/", "/"}, {"``", "`"}, {"abcdefgh", "i"}, {"?", "?"}
    };
    const size_t len = GATE_MAX_TEXT_BYTES;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t span = (GATE_TEXT_READ_BYTES + page - 1) / page * page;
    char *map = mmap(NULL, span + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    char *guarded, *plain;
    size_t i;

    CHECK(map != MAP_FAILED);
    if (map == MAP_FAILED) return;
    /* The prefix ends where a page that cannot be read begins: touching
     * anything past it kills the test. */
    CHECK(mprotect(map + span, page, PROT_NONE) == 0);
    guarded = map + span - GATE_TEXT_READ_BYTES;
    plain = xmalloc(len);

    for (i = 0; i < ARRAY_LEN(ends); i++) {
        gateSparse x, y;

        fill(plain, len, "so many words, so little time! ");
        put(plain, GATE_TEXT_READ_BYTES - strlen(ends[i].tail), ends[i].tail);
        put(plain, GATE_TEXT_READ_BYTES, ends[i].next);
        memcpy(guarded, plain, GATE_TEXT_READ_BYTES);
        x = textVector(guarded, len);
        y = textVector(plain, len);
        CHECK(x.n > 0 && sameSparse(&x, &y));
        gateSparseFree(&x);
        gateSparseFree(&y);
    }
    free(plain);
    munmap(map, span + page);
}

TEST(megabyte) {
    const size_t len = GATE_MAX_TEXT_BYTES;
    char *text = xmalloc(len);
    double pass = HUGE_VAL, best = HUGE_VAL, limit;
    uint64_t rng = 20, hash;
    size_t i, bytes;
    gateSparse x, y;
    int r;

    /* Random words: nearly every clue of the prefix is a different one,
     * which is the expensive case. */
    for (i = 0; i < len; i++) {
        uint64_t v = gate_test_rand64(&rng);

        text[i] = v % 7 == 0 ? ' ' : (char)('a' + (v >> 8) % 26);
    }

    /* Memory: the megabyte asks for exactly as many bytes as its first
     * 4096 (both are "len4", so the vectors are the same too). */
    countAllocations();
    x = textVector(text, len);
    bytes = allocBytes;
    allocBytes = 0;
    y = textVector(text, GATE_TEXT_READ_BYTES);
    CHECK(sameSparse(&x, &y));
    CHECK_EQ_INT(bytes, allocBytes);
    gateSparseFree(&x);
    gateSparseFree(&y);
    stopCounting();

    /* Time: well under 5 ms of CPU, best of five. Under valgrind or a
     * sanitizer the clock means something else, so the limit is stretched
     * by how much longer than usual one FNV-1a pass over the megabyte
     * takes here (it takes about a millisecond; 2 ms is generous). */
    hash = gateFnv1a64(text, len);
    for (r = 0; r < 5; r++) {
        clock_t start = clock();
        double t;

        CHECK(gateFnv1a64(text, len) == hash);
        t = (double)(clock() - start) / CLOCKS_PER_SEC;
        if (t < pass) pass = t;
    }
    for (r = 0; r < 5; r++) {
        clock_t start = clock();
        double t;

        for (i = 0; i < 20; i++) {
            x = textVector(text, len);
            gateSparseFree(&x);
        }
        t = (double)(clock() - start) / CLOCKS_PER_SEC / 20;
        if (t < best) best = t;
    }
    limit = 0.005 * fmax(1.0, pass / 0.002);
    CHECK(best < limit);
    if (!(best < limit))
        printf("    ^ %.0f us for a megabyte, the limit is %.0f us\n", best * 1e6, limit * 1e6);
    free(text);
}

int main(void) {
    RUN(fnv1a64_known_values);
    RUN(hash_known_values);
    RUN(bucket_and_sign);
    RUN(empty_text);
    RUN(words_and_chunks);
    RUN(case_folding);
    RUN(word_bytes);
    RUN(punctuation_tokens);
    RUN(separators);
    RUN(repeats_count_once);
    RUN(long_words_are_cut);
    RUN(length_buckets);
    RUN(links_and_code);
    RUN(only_the_prefix_counts);
    RUN(every_byte_a_token);
    RUN(clues_that_share_a_bucket);
    RUN(norm_cap);
    RUN(named_values);
    RUN(named_clamp_and_cap);
    RUN(named_shared_buckets);
    RUN(repeated_name);
    RUN(feature_count_limit);
    RUN(allocation_failure);
    RUN(random_texts);
    RUN(random_requests_with_features);
    RUN(nothing_past_the_prefix_is_read);
    RUN(megabyte);
    return TEST_REPORT();
}
