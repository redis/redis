/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "sb.h"

#include "zmalloc.h"

#include <limits.h>
#include <math.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// Core                                                                     ///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
#define ERROR_TIGHTENING_RATIO 0.5
#define CUR_FILTER(sb) ((sb)->filters + ((sb)->nfilters - 1))
static int SBChain_AddLink(SBChain *chain, uint64_t size, double error_rate) {
    if (chain->nfilters >= INT_MAX ||
        chain->nfilters >= SIZE_MAX / sizeof(*chain->filters)) return SB_INVALID;
    SBLink *filters = ztryrealloc(chain->filters,
                                 sizeof(*chain->filters) * (chain->nfilters + 1));
    if (!filters) return SB_OOM;
    chain->filters = filters;

    SBLink *newlink = chain->filters + chain->nfilters;
    *newlink = (SBLink){
        .size = 0,
    };
    int rc = bloom_init(&newlink->inner, size, error_rate, chain->options);
    if (rc != 0) {
        return rc == 1 ? SB_INVALID : SB_OOM;
    }
    chain->nfilters++;

    return SB_SUCCESS;
}

void SBChain_Free(SBChain *sb) {
    if (!sb) {
        return;
    }

    if (sb->filters) {
        for (size_t ii = 0; ii < sb->nfilters; ++ii) {
            if (sb->filters[ii].inner.bf) {
                zfree(sb->filters[ii].inner.bf);
            }
        }
        zfree(sb->filters);
    }

    zfree(sb);
}

static int SBChain_AddToLink(SBLink *lb, bloom_hashval hash) {
    if (!bloom_add_h(&lb->inner, hash)) {
        // Element not previously present?
        lb->size++;
        return 1;
    } else {
        return 0;
    }
}

static bloom_hashval SBChain_GetHash(const SBChain *chain, const void *buf, size_t len) {
    if (chain->options & BLOOM_OPT_FORCE64) {
        return bloom_calc_hash64(buf, len);
    } else {
        return bloom_calc_hash(buf, len);
    }
}

int SBChain_Add(SBChain *sb, const void *data, size_t len) {
    // Does it already exist?
    bloom_hashval h = SBChain_GetHash(sb, data, len);
    for (int ii = sb->nfilters - 1; ii >= 0; --ii) {
        if (bloom_check_h(&sb->filters[ii].inner, h)) {
            return 0;
        }
    }

    // Determine if we need to add more items?
    SBLink *cur = CUR_FILTER(sb);
    if (cur->size >= cur->inner.entries) {
        if (sb->options & BLOOM_OPT_NO_SCALING) {
            return -2;
        }
        if (sb->growth == 0 || cur->inner.entries > UINT64_MAX / sb->growth) {
            return -1;
        }
        double error = cur->inner.error * ERROR_TIGHTENING_RATIO;
        if (SBChain_AddLink(sb, cur->inner.entries * (uint64_t)sb->growth, error) != 0) {
            return -1;
        }
        cur = CUR_FILTER(sb);
    }

    if (cur->size == SIZE_MAX || sb->size == SIZE_MAX) {
        return -1;
    }
    int rv = SBChain_AddToLink(cur, h);
    if (rv) {
        sb->size++;
    }
    return rv;
}

int SBChain_Check(const SBChain *sb, const void *data, size_t len) {
    bloom_hashval hv = SBChain_GetHash(sb, data, len);
    for (int ii = sb->nfilters - 1; ii >= 0; --ii) {
        if (bloom_check_h(&sb->filters[ii].inner, hv)) {
            return 1;
        }
    }
    return 0;
}

SBChain *SB_NewChain(uint64_t initsize, double error_rate, unsigned options, unsigned growth,
                     int *err) {
    if (initsize == 0 || !isfinite(error_rate) || error_rate <= 0 || error_rate >= 1 ||
        (!(options & BLOOM_OPT_NO_SCALING) && growth == 0)) {
        *err = SB_INVALID;
        return NULL;
    }
    SBChain *sb = ztrycalloc(sizeof(*sb));
    if (!sb) {
        *err = SB_OOM;
        return NULL;
    }
    sb->growth = growth;
    sb->options = options;
    double tightening = (options & BLOOM_OPT_NO_SCALING) ? 1 : ERROR_TIGHTENING_RATIO;
    *err = SBChain_AddLink(sb, initsize, error_rate * tightening);
    if (*err != SB_SUCCESS) {
        SBChain_Free(sb);
        return NULL;
    }
    return sb;
}

typedef struct __attribute__((packed)) {
    uint64_t bytes;
    uint64_t bits;
    uint64_t size;
    double error;
    double bpe;
    uint32_t hashes;
    uint64_t entries;
    uint8_t n2;
} dumpedChainLink;

// X-Macro uses to convert between encoded and decoded SBLink
#define X_ENCODED_LINK(X, enc, link)                                                               \
    X((enc)->bytes, (link)->inner.bytes)                                                           \
    X((enc)->bits, (link)->inner.bits)                                                             \
    X((enc)->size, (link)->size)                                                                   \
    X((enc)->error, (link)->inner.error)                                                           \
    X((enc)->hashes, (link)->inner.hashes)                                                         \
    X((enc)->bpe, (link)->inner.bpe)                                                               \
    X((enc)->entries, (link)->inner.entries)                                                       \
    X((enc)->n2, (link)->inner.n2)

typedef struct __attribute__((packed)) {
    uint64_t size;
    uint32_t nfilters;
    uint32_t options;
    uint32_t growth;
    dumpedChainLink links[];
} dumpedChainHeader;

static SBLink *getLinkPos(const SBChain *sb, long long curIter, size_t *offset) {
    if (curIter < 1) {
        return NULL;
    }

    curIter--;
    SBLink *link = NULL;

    // Read iterator
    size_t seekPos = 0;

    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        if (seekPos + sb->filters[ii].inner.bytes > (uint64_t)curIter) {
            link = sb->filters + ii;
            break;
        } else {
            seekPos += sb->filters[ii].inner.bytes;
        }
    }
    if (!link) {
        return NULL;
    }

    curIter -= seekPos;
    *offset = curIter;
    return link;
}

const char *SBChain_GetEncodedChunk(const SBChain *sb, long long *curIter, size_t *len,
                                    size_t maxChunkSize) {
    // See into the offset.
    size_t offset = 0;
    SBLink *link = getLinkPos(sb, *curIter, &offset);

    if (!link) {
        *curIter = 0;
        return NULL;
    }

    *len = maxChunkSize;
    size_t linkRemaining = link->inner.bytes - offset;
    if (linkRemaining < *len) {
        *len = linkRemaining;
    }

    *curIter += *len;
    // printf("Returning offset=%lu\n", offset);
    return (const char *)(link->inner.bf + offset);
}

char *SBChain_GetEncodedHeader(const SBChain *sb, size_t *hdrlen) {
    *hdrlen = sizeof(dumpedChainHeader) + (sizeof(dumpedChainLink) * sb->nfilters);
    dumpedChainHeader *hdr = zcalloc(*hdrlen);
    hdr->size = sb->size;
    hdr->nfilters = sb->nfilters;
    hdr->options = sb->options;
    hdr->growth = sb->growth;

    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        dumpedChainLink *dstlink = &hdr->links[ii];
        SBLink *srclink = sb->filters + ii;

#define X(encfld, srcfld) encfld = srcfld;
        X_ENCODED_LINK(X, dstlink, srclink)
#undef X
    }
    return (char *)hdr;
}

void SB_FreeEncodedHeader(char *s) { zfree(s); }

// Returns 0 on success
int SB_ValidateIntegrity(const SBChain *sb) {
    if (!sb || sb->nfilters == 0 || sb->nfilters > INT_MAX || !sb->filters ||
        (!(sb->options & BLOOM_OPT_NO_SCALING) && sb->growth == 0)) return 1;
    if (sb->options &
        ~(BLOOM_OPT_NOROUND | BLOOM_OPT_ENTS_IS_BITS | BLOOM_OPT_FORCE64 | BLOOM_OPT_NO_SCALING)) {
        return 1;
    }

    size_t total = 0;
    for (size_t i = 0; i < sb->nfilters; i++) {
        if (sb->filters[i].size > sb->filters[i].inner.entries ||
            sb->filters[i].size > SIZE_MAX - total) {
            return 1;
        }
        total += sb->filters[i].size;
    }

    if (sb->size != total) {
        return 1;
    }

    return 0;
}

SBChain *SB_NewChainFromHeader(const char *buf, size_t bufLen, const char **errmsg) {
    SBChain *sb = NULL;

    const dumpedChainHeader *header = (const void *)buf;
    if (bufLen < sizeof(dumpedChainHeader)) {
        goto err;
    }

    size_t nfilters = header->nfilters;
    if (nfilters == 0 || nfilters > INT_MAX ||
        nfilters > (SIZE_MAX - sizeof(*header)) / sizeof(header->links[0]) ||
        nfilters > SIZE_MAX / sizeof(*sb->filters)) {
        goto err;
    }

    if (bufLen != sizeof(*header) + (sizeof(header->links[0]) * header->nfilters)) {
        goto err;
    }

    sb = ztrycalloc(sizeof(*sb));
    if (!sb) goto err;
    sb->filters = ztrycalloc(header->nfilters * sizeof(*sb->filters));
    if (!sb->filters) goto err;
    sb->nfilters = header->nfilters;
    sb->options = header->options;
    sb->size = header->size;
    sb->growth = header->growth;

    for (size_t ii = 0; ii < header->nfilters; ++ii) {
        SBLink *dstlink = sb->filters + ii;
        const dumpedChainLink *srclink = header->links + ii;
#define X(encfld, dstfld) dstfld = encfld;
        X_ENCODED_LINK(X, srclink, dstlink)
#undef X

        if (bloom_validate_integrity(&dstlink->inner) != 0) {
            goto err;
        }

        if (dstlink->inner.bytes > SIZE_MAX) goto err;
        dstlink->inner.bf = ztrycalloc(dstlink->inner.bytes);
        if (!dstlink->inner.bf) {
            goto err;
        }

        if (sb->options & BLOOM_OPT_FORCE64) {
            dstlink->inner.force64 = 1;
        }
    }

    if (SB_ValidateIntegrity(sb) != 0) {
        goto err;
    }

    return sb;

err:
    SBChain_Free(sb);
    *errmsg = "ERR received bad data";
    return NULL;
}

int SBChain_LoadEncodedChunk(SBChain *sb, long long iter, const char *buf, size_t bufLen,
                             const char **errmsg) {
    if (!buf || iter <= 0 || (uint64_t)iter < bufLen) {
        *errmsg = "ERR received bad data";
        return -1;
    }
    // Load the chunk
    size_t offset;
    iter -= bufLen;

    SBLink *link = getLinkPos(sb, iter, &offset);
    if (!link) {
        *errmsg = "ERR invalid offset - no link found"; // LCOV_EXCL_LINE
        return -1;                                      // LCOV_EXCL_LINE
    }

    if (bufLen > link->inner.bytes - offset) {
        *errmsg = "ERR invalid chunk - Too big for current filter"; // LCOV_EXCL_LINE
        return -1;                                                  // LCOV_EXCL_LINE
    }

    // printf("Copying to %p. Offset=%lu, Len=%lu\n", link, offset, bufLen);
    memcpy(link->inner.bf + offset, buf, bufLen);
    return 0;
}

#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

int bloomTest(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    int before_failed = __failed_tests;
    size_t before_memory = zmalloc_used_memory();
    int err;
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
        dumpedChainHeader *h = (void *)header;
        uint32_t saved_hashes = h->links[0].hashes;
        h->links[0].hashes = BLOOM_MAX_HASHES + 1;
        test_cond("Reject excessive hash counts",
                  SB_NewChainFromHeader(header, header_len, &errmsg) == NULL);
        h->links[0].hashes = saved_hashes;
        h->size++;
        test_cond("Reject inconsistent Bloom counts",
                  SB_NewChainFromHeader(header, header_len, &errmsg) == NULL);
        h->size--;
        h->growth = 0;
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
#endif
