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
 *   gate_defs.h      fixed choices, status codes, sparse vectors, allocator
 *   gate_stats.h     betting test, error budget, threshold grid, drift detector
 *   gate_model.h     linear classifier and its online learning
 *   gate_features.h  text and named features -> hashed sparse vector
 *   gate_core.h      the gate itself: snapshots, proof, feedback, lifecycle
 *   gate_codec.h     binary encoding of a whole gate
 *   gate.c           Redis module: commands, replies, persistence, replication
 *
 * Everything below gate.c is plain C99 + libm with no Redis dependency, so
 * it is unit tested and simulated as ordinary programs.
 *
 * The user-facing design (SPEC.md) has almost no settings. The numbers a
 * user would otherwise have to choose live here.
 */

#ifndef GATE_DEFS_H
#define GATE_DEFS_H

#include <stddef.h>
#include <stdint.h>

/* ----------------------------- Status codes ------------------------------ */

#define GATE_OK 0
#define GATE_ERR (-1)

/* ------------------------------ Fixed choices ---------------------------- */

#define GATE_DIM 8192u              /* hash buckets: the model dimension */
#define GATE_THRESHOLDS 32u         /* confidence thresholds per label */
#define GATE_MIN_CLASSES 2
#define GATE_MAX_CLASSES 64
#define GATE_MAX_LABEL_BYTES 64u
#define GATE_MAX_NAME_BYTES 64u     /* named feature */
#define GATE_MAX_FEATURES 64u       /* named features per request */
#define GATE_TEXT_READ_BYTES 4096u  /* only this prefix of a TEXT is tokenized */
#define GATE_MAX_TEXT_BYTES (1u << 20) /* longer TEXT arguments are refused */
#define GATE_MAX_TOKEN_BYTES 64u    /* longer words are cut */
#define GATE_FEATURE_CLAMP 4.0      /* named feature values are clamped to +-this */
#define GATE_TEXT_NORM2_CAP 32.0    /* squared norm cap of the text clues */
#define GATE_FEATURE_NORM2_CAP 8.0  /* squared norm cap of the named features */
#define GATE_MAX_WEIGHT 64.0f       /* weights saturate at +-this */
#define GATE_LEARNING_RATE 0.1

#define GATE_DEFAULT_DELTA 0.01
#define GATE_DEFAULT_AUDIT 0.05
#define GATE_DEFAULT_SNAPSHOT_EXAMPLES 1000u /* gate.snapshot-examples */
#define GATE_DEFAULT_FEEDBACK_TIMEOUT_MS 300000ll /* gate.feedback-timeout */
#define GATE_MAX_FEEDBACK_TIMEOUT_MS (7ll * 86400 * 1000)

#define GATE_MAX_PENDING 4096u      /* requests waiting for feedback */
#define GATE_TRAIN_PENDING 3072u    /* ... of which may be training-only */
#define GATE_MAX_PENDING_BYTES (16u << 20)

#define GATE_CANDIDATE_BUDGET_FACTOR 20 /* a classifier snapshot is dropped
                                         * after this many times the proof a
                                         * flawless threshold would need */
#define GATE_DRIFT_ALARM 1000.0     /* drift detector alarm level */

/* ----------------------------- Sparse vector -----------------------------
 * Coalesced sparse feature vector: idx strictly increasing and < GATE_DIM,
 * no zero values. Values are stored as float32 (every waiting request keeps
 * one); arithmetic is carried out in double. */

typedef struct gateSparse {
    uint32_t n;      /* number of non-zero entries */
    uint32_t *idx;   /* n ascending bucket indices */
    float *val;      /* n values */
} gateSparse;

/* Bytes of heap memory attributed to a sparse vector with n entries. */
#define GATE_SPARSE_BYTES(n) ((size_t)(n) * (sizeof(uint32_t) + sizeof(float)))

/* Free the arrays of 'x' (not 'x' itself) and reset it to the empty vector. */
void gateSparseFree(gateSparse *x);
/* Deep copy; returns GATE_OK or GATE_ERR on allocation failure. */
int gateSparseCopy(gateSparse *dst, const gateSparse *src);

/* ------------------------------ Name policy ------------------------------
 * Labels and feature names: returns 1 iff 's' is non-empty, at most
 * 'maxlen' bytes, well-formed UTF-8 (no overlong forms, no surrogates,
 * nothing above U+10FFFF) and contains no C0/C1 control character or DEL.
 * TEXT is not subject to this: it is read as bytes. */
int gateValidName(const char *s, size_t len, size_t maxlen);

/* ---------------------------- Allocator hooks ----------------------------
 * The library allocates through these wrappers. They default to libc; the
 * Redis module installs the Redis allocator at load time (the pattern
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
