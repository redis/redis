/*
 * Gate data type: common definitions shared by every translation unit.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Layering (lower layers never include higher ones):
 *
 *   gate_defs.h      limits, status codes, sparse vectors, allocator hooks
 *   gate_stats.h     e-process certification, error budget, grids, drift window
 *   gate_model.h     linear classifier (logistic / softmax) and SGD
 *   gate_features.h  deterministic feature extraction (TEXT, VECTOR, FEATURES)
 *   gate_core.h      the gate state machine: configuration, generations,
 *                    pending observations, the feedback protocol
 *   gate_codec.h     versioned binary encoding of a whole gate
 *   gate.c           Redis module: commands, replies, persistence, replication
 *
 * Everything below gate.c is plain C99 + libm with no Redis dependency, so
 * it can be unit tested and simulated as ordinary programs.
 */

#ifndef GATE_DEFS_H
#define GATE_DEFS_H

#include <stddef.h>
#include <stdint.h>

/* ----------------------------- Status codes ------------------------------ */

#define GATE_OK 0
#define GATE_ERR (-1)

/* ------------------------------ Hard limits ------------------------------
 * Global caps enforced before any allocation. Per-gate limits (MAXMEMORY,
 * MAXPENDING, ...) are configured at GCREATE time within these bounds. */

#define GATE_MIN_CLASSES 2
#define GATE_MAX_CLASSES 64
#define GATE_MIN_THRESHOLDS 2
#define GATE_MAX_THRESHOLDS 256
#define GATE_MAX_TEXT_DIM (1u << 20)        /* hashed text block buckets */
#define GATE_MAX_VECTOR_DIM 16384u          /* client supplied embedding */
#define GATE_MAX_FEATURE_DIM 65536u         /* hashed named-feature block */
#define GATE_MAX_FEATURES_PER_REQUEST 1024u /* name/value pairs per command */
#define GATE_MAX_TEXT_BYTES 65536u          /* TEXT argument size */
#define GATE_MAX_TOKEN_BYTES 64u            /* longer tokens are truncated */
#define GATE_MAX_LABEL_BYTES 64u
#define GATE_MAX_NAME_BYTES 128u            /* feature names */
#define GATE_MAX_VERSION_BYTES 128u         /* TEACHER / SCHEMAVERSION */
#define GATE_MAX_FEATURE_VALUE 1e6          /* |named feature value| bound */
#define GATE_MAX_VECTOR_VALUE 1e6           /* |vector component| bound */
#define GATE_MAX_WEIGHT 1e6f                /* weights are clamped to +-this */
#define GATE_MAX_LR 10.0
#define GATE_MAX_PENDING_LIMIT 1048576u
#define GATE_MAX_MEMORY_LIMIT (256ull << 20) /* keeps a gate below the 512 MB
                                              * bulk limit when serialized */
#define GATE_MAX_TTL_MS (10ll * 365 * 86400 * 1000)

/* ------------------------------- Defaults -------------------------------- */

#define GATE_DEFAULT_DELTA 0.01
#define GATE_DEFAULT_AUDIT 0.05
#define GATE_DEFAULT_THRESHOLDS 32
#define GATE_DEFAULT_TEXT_DIM 8192u
#define GATE_DEFAULT_FEATURE_DIM 256u
#define GATE_DEFAULT_LR 0.1
#define GATE_DEFAULT_L2 0.0
#define GATE_DEFAULT_MAX_PENDING 4096u
#define GATE_DEFAULT_OBSERVATION_TTL_MS 300000ll
#define GATE_DEFAULT_MAX_MEMORY (64ull << 20)
#define GATE_DEFAULT_CERT_TTL_MS (30ll * 86400 * 1000)
#define GATE_DEFAULT_VERSION "dev"          /* TEACHER / SCHEMAVERSION when
                                             * not given: development only */

/* ----------------------------- Sparse vector -----------------------------
 * Coalesced sparse feature vector: idx strictly increasing, no zero values.
 * Values are stored as float32 (pending observations keep one of these per
 * request, so size matters); all arithmetic is carried out in double. */

typedef struct gateSparse {
    uint32_t n;      /* number of non-zero entries */
    uint32_t *idx;   /* n ascending feature indices, each < model dimension */
    float *val;      /* n values */
} gateSparse;

/* Bytes of heap memory attributed to a sparse vector with n entries. */
#define GATE_SPARSE_BYTES(n) ((size_t)(n) * (sizeof(uint32_t) + sizeof(float)))

/* Free the arrays of 'x' (not 'x' itself) and reset it to the empty vector. */
void gateSparseFree(gateSparse *x);
/* Deep copy; returns GATE_OK or GATE_ERR on allocation failure. */
int gateSparseCopy(gateSparse *dst, const gateSparse *src);

/* ------------------------------ Name policy ------------------------------
 * The gate's UTF-8 policy for labels, feature names and version strings:
 * returns 1 iff 's' is non-empty, at most 'maxlen' bytes, well-formed UTF-8
 * (no overlong forms, no surrogates, nothing above U+10FFFF) and contains
 * no C0/C1 control character or DEL. Raw TEXT sections are NOT subject to
 * this policy: they are tokenized as bytes. */
int gateValidName(const char *s, size_t len, size_t maxlen);

/* ---------------------------- Allocator hooks ----------------------------
 * The library allocates through these wrappers. They default to libc; the
 * Redis module installs the Redis allocator at load time (the same pattern
 * modules/vector-sets uses for hnsw.c). Never call malloc/free directly. */

void gateSetAllocator(void *(*alloc_fn)(size_t),
                      void *(*calloc_fn)(size_t, size_t),
                      void *(*realloc_fn)(void *, size_t),
                      void (*free_fn)(void *));
void *gate_malloc(size_t n);
void *gate_calloc(size_t nmemb, size_t size);
void *gate_realloc(void *p, size_t n);
void gate_free(void *p);

#endif /* GATE_DEFS_H */
