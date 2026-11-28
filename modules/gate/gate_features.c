/*
 * Gate data type: clues. Text and named features -> hashed sparse vector.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * The rules are in gate_features.h. They are applied in four steps:
 *
 *   1. one pass over the lowercased prefix hashes every clue of the text;
 *   2. the hashes are sorted, repeats are skipped, and each clue that is
 *      left becomes a term: +1 or -1 for its bucket;
 *   3. the terms of each bucket are summed and the norm is capped (the
 *      named features go through this step on their own);
 *   4. the text part and the named part are merged into the output.
 *
 * THE COST IS THAT OF THE PREFIX. Nothing past the first
 * GATE_TEXT_READ_BYTES bytes of a text is read, and the scratch memory, one
 * block per call, is sized by the prefix. A megabyte of text costs what its
 * first 4096 bytes cost.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gate_features.h"

/* The first byte fed to the hash tells the kinds of clue apart. */
#define GATE_CLUE_WORD 0x01
#define GATE_CLUE_PAIR 0x02
#define GATE_CLUE_CHUNK 0x03
#define GATE_CLUE_HINT 0x04
#define GATE_CLUE_NAMED 0x05

#define GATE_CHUNK_BYTES 4
#define GATE_MAX_HINTS 3 /* a length, "link" and "code" */

/* ---------------------------------- Hash ---------------------------------- */

#define GATE_FNV_BASIS 0xcbf29ce484222325ull
#define GATE_FNV_PRIME 0x100000001b3ull

/* FNV-1a takes its input one byte at a time, so a clue made of several
 * pieces (a pair is two tokens and a separator) is hashed piece by piece and
 * never has to be put together in a buffer. */
static uint64_t gateFnvByte(uint64_t h, unsigned char c) {
    return (h ^ c) * GATE_FNV_PRIME;
}

static uint64_t gateFnvBytes(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = data;
    size_t i;

    for (i = 0; i < len; i++) h = gateFnvByte(h, p[i]);
    return h;
}

/* The MurmurHash3 finalizer. */
static uint64_t gateFmix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

uint64_t gateFnv1a64(const void *data, size_t len) {
    return gateFnvBytes(GATE_FNV_BASIS, data, len);
}

uint64_t gateHash(uint8_t domain, const void *data, size_t len) {
    return gateFmix64(gateFnvBytes(gateFnvByte(GATE_FNV_BASIS, domain), data, len));
}

/* ------------------------------- Text clues ------------------------------- */

static int gateIsWordByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
}

/* Hash the word, pair and chunk clues of the prefix 'low' (already
 * lowercased) into 'clues' and return how many there are. Never more than
 * 2 * len: a token gives a word clue and a pair clue, and a word of n > 4
 * bytes gives n - 3 chunks more, which is still fewer than 2 per byte. */
static size_t gateTextClues(const unsigned char *low, size_t len, uint64_t *clues) {
    const unsigned char *prev = NULL; /* the token before this one */
    size_t prevlen = 0, n = 0, i = 0;

    while (i < len) {
        const unsigned char *tok = low + i;
        size_t toklen, nchunks, k;
        uint64_t h;

        if (gateIsWordByte(low[i])) {
            while (i < len && gateIsWordByte(low[i])) i++;
            toklen = (size_t)(low + i - tok);
            /* A long word is cut, and what is cut off is not a token. */
            if (toklen > GATE_MAX_TOKEN_BYTES) toklen = GATE_MAX_TOKEN_BYTES;
        } else if (low[i] == '?' || low[i] == '!' || low[i] == '@') {
            toklen = 1;
            i++;
        } else {
            i++;
            continue;
        }

        clues[n++] = gateHash(GATE_CLUE_WORD, tok, toklen);
        if (prev != NULL) {
            h = gateFnvByte(GATE_FNV_BASIS, GATE_CLUE_PAIR);
            h = gateFnvBytes(h, prev, prevlen);
            h = gateFnvByte(h, 0x00);
            h = gateFnvBytes(h, tok, toklen);
            clues[n++] = gateFmix64(h);
        }
        /* Only a word can be longer than a chunk. */
        nchunks = toklen > GATE_CHUNK_BYTES ? toklen - GATE_CHUNK_BYTES + 1 : 0;
        for (k = 0; k < nchunks; k++) {
            unsigned char tag = k == 0 ? '^' : (k == nchunks - 1 ? '$' : '.');

            h = gateFnvByte(GATE_FNV_BASIS, GATE_CLUE_CHUNK);
            h = gateFnvByte(h, tag);
            h = gateFnvBytes(h, tok + k, GATE_CHUNK_BYTES);
            clues[n++] = gateFmix64(h);
        }
        prev = tok;
        prevlen = toklen;
    }
    return n;
}

/* 1 if the 'len' bytes at 's' contain 'needle'. A text may hold NUL bytes,
 * which rules out strstr(). */
static int gateContains(const unsigned char *s, size_t len, const char *needle) {
    size_t n = strlen(needle), i;

    for (i = 0; i + n <= len; i++)
        if (memcmp(s + i, needle, n) == 0) return 1;
    return 0;
}

static uint64_t gateHintHash(const char *name) {
    return gateHash(GATE_CLUE_HINT, name, strlen(name));
}

/* Hash the structural hints into 'clues' (room for GATE_MAX_HINTS) and
 * return how many there are. The length is that of the whole text; a link
 * or a code block counts only if it lies inside the prefix 'low'. */
static size_t gateHintClues(const unsigned char *low, size_t len, size_t text_len,
                            uint64_t *clues)
{
    const char *length = text_len <= 16 ? "len0" : text_len <= 64 ? "len1" :
                         text_len <= 256 ? "len2" : text_len <= 1024 ? "len3" : "len4";
    size_t n = 0;

    clues[n++] = gateHintHash(length);
    if (gateContains(low, len, "http://") || gateContains(low, len, "https://"))
        clues[n++] = gateHintHash("link");
    if (gateContains(low, len, "```")) clues[n++] = gateHintHash("code");
    return n;
}

/* Order clue hashes by bucket, then by value. One sort does two jobs: the
 * buckets come out ascending and the repeats of a clue end up next to each
 * other. */
static int gateClueCompare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    if (x % GATE_DIM != y % GATE_DIM) return x % GATE_DIM < y % GATE_DIM ? -1 : 1;
    return (x > y) - (x < y);
}

/* ---------------------------------- Terms --------------------------------- */

/* One addition to the vector: 'val' goes to bucket 'idx'. */
typedef struct gateTerm {
    uint32_t idx;
    double val;
} gateTerm;

/* Replace the terms of each bucket by their sum, in place, and return how
 * many are left: a bucket that sums to zero is dropped. 't' must be sorted
 * by bucket. If the squared norm of the result exceeds 'cap' it is scaled
 * down to exactly that norm. */
static size_t gateSumBuckets(gateTerm *t, size_t n, double cap) {
    double norm2 = 0.0, scale;
    size_t i = 0, m = 0;

    while (i < n) {
        gateTerm sum = {t[i].idx, 0.0};

        for (; i < n && t[i].idx == sum.idx; i++) sum.val += t[i].val;
        if (sum.val == 0.0) continue;
        norm2 += sum.val * sum.val;
        t[m++] = sum;
    }
    if (norm2 > cap) {
        scale = sqrt(cap / norm2);
        for (i = 0; i < m; i++) t[i].val *= scale;
    }
    return m;
}

/* 1 if two features have the same name. Names are compared byte for byte:
 * different names that hash alike share a bucket, they are not an error. */
static int gateNameIsRepeated(const gateNamedFeature *f, uint32_t n) {
    uint32_t i, j;

    for (i = 1; i < n; i++)
        for (j = 0; j < i; j++)
            if (f[i].name_len == f[j].name_len &&
                memcmp(f[i].name, f[j].name, f[i].name_len) == 0) return 1;
    return 0;
}

/* Build the named part of the vector in 't' (room for 'n' terms) and return
 * how many terms it has. */
static size_t gateNamedTerms(const gateNamedFeature *f, uint32_t n, gateTerm *t) {
    uint32_t i, j;

    for (i = 0; i < n; i++) {
        uint64_t h = gateHash(GATE_CLUE_NAMED, f[i].name, f[i].name_len);
        gateTerm term = {(uint32_t)(h % GATE_DIM), f[i].value};

        if (term.val > GATE_FEATURE_CLAMP) term.val = GATE_FEATURE_CLAMP;
        if (term.val < -GATE_FEATURE_CLAMP) term.val = -GATE_FEATURE_CLAMP;
        /* Insertion sort. Names that share a bucket stay in the order of
         * the request, so their sum is rounded the same way wherever this
         * runs; qsort() makes no promise about equal keys. */
        for (j = i; j > 0 && t[j - 1].idx > term.idx; j--) t[j] = t[j - 1];
        t[j] = term;
    }
    return gateSumBuckets(t, n, GATE_FEATURE_NORM2_CAP);
}

/* Merge the text part and the named part (each sorted by bucket, one term
 * per bucket) into 'out'. A bucket is left out when the float stored for it
 * would be zero: the two parts cancel, or the value is too small for a
 * float, and a sparse vector holds no explicit zero. */
static int gateMerge(const gateTerm *a, size_t na, const gateTerm *b, size_t nb,
                     gateSparse *out)
{
    uint32_t *idx = gate_malloc((na + nb) * sizeof(*idx));
    float *val = gate_malloc((na + nb) * sizeof(*val));
    size_t i = 0, j = 0;
    uint32_t n = 0;

    if (idx == NULL || val == NULL) {
        gate_free(idx);
        gate_free(val);
        return GATE_ERR;
    }
    while (i < na || j < nb) {
        /* The lowest bucket not merged yet: one part has it, or both. */
        uint32_t bucket = (j == nb || (i < na && a[i].idx < b[j].idx)) ? a[i].idx : b[j].idx;
        double sum = 0.0;
        float v;

        if (i < na && a[i].idx == bucket) sum += a[i++].val;
        if (j < nb && b[j].idx == bucket) sum += b[j++].val;
        v = (float)sum;
        if (v == 0.0f) continue;
        idx[n] = bucket;
        val[n] = v;
        n++;
    }
    out->n = n;
    out->idx = idx;
    out->val = val;
    return GATE_OK;
}

/* ------------------------------- Featurize -------------------------------- */

static int gateFeaturizeFailed(char *err, size_t errlen, const char *reason) {
    if (err != NULL && errlen > 0) snprintf(err, errlen, "%s", reason);
    return GATE_ERR;
}

int gateFeaturize(const char *text, size_t text_len,
                  const gateNamedFeature *features, uint32_t nfeatures,
                  gateSparse *out, char *err, size_t errlen)
{
    size_t len = text_len < GATE_TEXT_READ_BYTES ? text_len : GATE_TEXT_READ_BYTES;
    size_t maxclues = 2 * len + GATE_MAX_HINTS;
    size_t nclues, nterms = 0, nnamed, i;
    gateTerm named[GATE_MAX_FEATURES], *terms;
    unsigned char *low;
    uint64_t *clues;
    int ret;

    if (nfeatures > GATE_MAX_FEATURES)
        return gateFeaturizeFailed(err, errlen, "too many features");
    if (gateNameIsRepeated(features, nfeatures))
        return gateFeaturizeFailed(err, errlen, "feature name given twice");

    /* One block of scratch memory, sized by the prefix: the clue hashes,
     * the terms they become, and the lowercased prefix. */
    clues = gate_malloc(maxclues * (sizeof(*clues) + sizeof(*terms)) + len);
    if (clues == NULL) return gateFeaturizeFailed(err, errlen, "out of memory");
    terms = (gateTerm *)(clues + maxclues);
    low = (unsigned char *)(terms + maxclues);

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        low[i] = (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
    }
    nclues = gateTextClues(low, len, clues);
    nclues += gateHintClues(low, len, text_len, clues + nclues);

    /* A clue counts once however often it occurs. */
    qsort(clues, nclues, sizeof(*clues), gateClueCompare);
    for (i = 0; i < nclues; i++) {
        if (i > 0 && clues[i] == clues[i - 1]) continue;
        terms[nterms].idx = (uint32_t)(clues[i] % GATE_DIM);
        terms[nterms].val = (clues[i] >> 63) ? -1.0 : 1.0;
        nterms++;
    }
    nterms = gateSumBuckets(terms, nterms, GATE_TEXT_NORM2_CAP);
    nnamed = gateNamedTerms(features, nfeatures, named);

    ret = gateMerge(terms, nterms, named, nnamed, out);
    gate_free(clues);
    if (ret != GATE_OK) return gateFeaturizeFailed(err, errlen, "out of memory");
    return GATE_OK;
}
