/*
 * Gate data type: allocator hooks and sparse vector helpers.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdlib.h>
#include <string.h>
#include "gate_defs.h"

static void *(*gateAllocFn)(size_t) = malloc;
static void *(*gateCallocFn)(size_t, size_t) = calloc;
static void *(*gateReallocFn)(void *, size_t) = realloc;
static void (*gateFreeFn)(void *) = free;

void gateSetAllocator(void *(*alloc_fn)(size_t),
                      void *(*calloc_fn)(size_t, size_t),
                      void *(*realloc_fn)(void *, size_t),
                      void (*free_fn)(void *))
{
    gateAllocFn = alloc_fn;
    gateCallocFn = calloc_fn;
    gateReallocFn = realloc_fn;
    gateFreeFn = free_fn;
}

void *gate_malloc(size_t n) { return gateAllocFn(n ? n : 1); }
void *gate_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) { nmemb = 1; size = 1; }
    return gateCallocFn(nmemb, size);
}
void *gate_realloc(void *p, size_t n) { return gateReallocFn(p, n ? n : 1); }
void gate_free(void *p) { if (p) gateFreeFn(p); }

void gateSparseFree(gateSparse *x) {
    if (x == NULL) return;
    gate_free(x->idx);
    gate_free(x->val);
    x->idx = NULL;
    x->val = NULL;
    x->n = 0;
}

int gateSparseCopy(gateSparse *dst, const gateSparse *src) {
    dst->n = 0;
    dst->idx = NULL;
    dst->val = NULL;
    if (src == NULL || src->n == 0) return GATE_OK;
    dst->idx = gate_malloc(sizeof(uint32_t) * src->n);
    dst->val = gate_malloc(sizeof(float) * src->n);
    if (dst->idx == NULL || dst->val == NULL) {
        gateSparseFree(dst);
        return GATE_ERR;
    }
    memcpy(dst->idx, src->idx, sizeof(uint32_t) * src->n);
    memcpy(dst->val, src->val, sizeof(float) * src->n);
    dst->n = src->n;
    return GATE_OK;
}

/* Strict UTF-8 walk (RFC 3629). Rejects overlong encodings, UTF-16
 * surrogates, code points above U+10FFFF, truncated sequences, and the
 * control ranges U+0000-U+001F, U+007F and U+0080-U+009F. */
int gateValidName(const char *s, size_t len, size_t maxlen) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    if (s == NULL || len == 0 || len > maxlen) return 0;
    while (i < len) {
        unsigned char c = p[i];
        uint32_t cp;
        size_t need;

        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return 0;
            i++;
            continue;
        } else if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            cp = c & 0x07;
        } else {
            return 0; /* continuation byte, 0xc0/0xc1 overlong lead, or > 0xf4 */
        }
        if (len - i - 1 < need) return 0;
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 2 && cp < 0x800) return 0;                  /* overlong */
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
        if (cp >= 0xd800 && cp <= 0xdfff) return 0;             /* surrogates */
        if (cp >= 0x80 && cp <= 0x9f) return 0;                 /* C1 controls */
        i += need + 1;
    }
    return 1;
}
