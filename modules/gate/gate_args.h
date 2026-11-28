/*
 * Gate data type: command argument grammar.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Turns the argument vectors of the G* commands into validated structures.
 * Pure C over (pointer, length) pairs: no Redis dependency, so the grammar
 * is unit tested without a server, and the public commands and the internal
 * replication command share one parser.
 *
 * Conventions for every parser:
 *   - option tokens are matched case-insensitively (ASCII);
 *   - an option may appear at most once;
 *   - nothing is partially applied: on error the output is released/left
 *     empty and 'err' holds a message suitable for an "-ERR <msg>" reply
 *     (no "ERR " prefix, no trailing newline);
 *   - numbers are strict: the whole argument must be consumed, no leading or
 *     trailing space, no "nan"/"inf", no hex floats; integers are decimal
 *     digits only (no sign for unsigned values).
 */

#ifndef GATE_ARGS_H
#define GATE_ARGS_H

#include <stddef.h>
#include <stdint.h>
#include "gate_core.h"

typedef struct gateArg {
    const char *ptr;
    size_t len;
} gateArg;

/* --------------------------- Scalar helpers ------------------------------ */

/* Strict finite double / unsigned 64-bit integer. Return 1 on success. */
int gateParseDouble(const char *s, size_t len, double *out);
int gateParseU64(const char *s, size_t len, uint64_t *out);

/* Case-insensitive comparison of an argument with an upper-case token. */
int gateArgIs(const gateArg *a, const char *token);

/* Observation ids: "<epoch as 16 lower-case hex digits>-<seq in decimal>",
 * e.g. "5d0c3f9a7b21e684-1042". Parsing is strict (exactly 16 hex digits of
 * either case, one '-', 1..20 decimal digits without leading zeros, seq
 * >= 1 and fitting in 64 bits). */
#define GATE_OBSERVATION_ID_MAX 40
size_t gateFormatObservationId(char *buf, size_t buflen, uint64_t epoch, uint64_t seq);
int gateParseObservationId(const char *s, size_t len, uint64_t *epoch, uint64_t *seq);

/* Doubles that must survive replication bit-exactly travel as the 16
 * lower-case hex digits of their IEEE-754 bit pattern. */
#define GATE_DOUBLE_HEX_LEN 16
void gateFormatDoubleBits(char *buf /* 17 bytes */, double v);
int gateParseDoubleBits(const char *s, size_t len, double *out);
/* 64-bit ids (lineage, epoch) as 16 lower-case hex digits. */
void gateFormatHex64(char *buf /* 17 bytes */, uint64_t v);
int gateParseHex64(const char *s, size_t len, uint64_t *out);

/* -------------------------------- GCREATE -------------------------------- */

/* Parse the arguments that follow the key of
 *
 *   GCREATE key MODE <SCORE|CLASSIFIER> ALPHA alpha [DELTA delta] [AUDIT q]
 *     [COLDAUDIT q] [THRESHOLDS n] [CLASSES count label ...]
 *     [CLASSALPHA count label alpha ...] [ALWAYSDEFER count label ...]
 *     [INPUTS count <TEXT|VECTOR|FEATURES> ...] [TEXTDIM n] [VECTORDIM n]
 *     [FEATUREDIM n] [LR rate] [L2 lambda] [TEACHER version]
 *     [SCHEMAVERSION version] [MAXMEMORY bytes] [MAXPENDING n]
 *     [OBSERVATIONTTL ms] [CERTTTL ms] [DRIFTMARGIN m]
 *     [ONMISSING <REVOKE|ERROR>] [EXPAND <NO|YES>]
 *
 * into '*cfg' (overwritten; the caller releases it with gateConfigFree() on
 * success). Options may come in any order; MODE and ALPHA are mandatory.
 * Defaults: COLDAUDIT = AUDIT; TEXTDIM / FEATUREDIM get their defaults only
 * when that input is configured; a dimension given for an unconfigured input
 * is an error, as is VECTOR without VECTORDIM. Class options are resolved
 * after the whole command is read, so CLASSES may follow them. Labels must
 * be distinct; a label may not appear twice in CLASSALPHA or ALWAYSDEFER.
 * The result has passed gateConfigValidate(). GATE_OK / GATE_ERR. */
int gateParseCreate(const gateArg *argv, int argc, gateConfig *cfg,
                    char *err, size_t errlen);

/* ----------------------- Request sections (CLASSIFIER) -------------------- */

typedef struct gateRequest {
    gateInput in;               /* borrows from the argument vector */
    gateNamedFeature *features; /* owned array backing in.features */
} gateRequest;

/* Parse zero or more of
 *
 *   TEXT text | VECTOR blob | FEATURES count name value [name value ...]
 *
 * starting at argv[*pos], in any order, each at most once, stopping at the
 * first argument that is not one of those three tokens (or at argc). On
 * return *pos is the index of the first unconsumed argument. This only
 * checks the grammar (counts, numeric values, per-request limits); schema
 * conformance is checked by gateFeaturize(). 'req' must be zeroed by the
 * caller and released with gateRequestFree() in every case. */
int gateParseSections(const gateArg *argv, int argc, int *pos, gateRequest *req,
                      char *err, size_t errlen);
void gateRequestFree(gateRequest *req);

/* ------------------------------- Targets --------------------------------- */

typedef struct gateTarget {
    gateFeedback fb; /* kind, label (argmax for PROBS), probs, correct */
    double *q;       /* owned: K-element target distribution for training
                      * (one-hot for LABEL, renormalized values for PROBS);
                      * NULL for CORRECT. fb.probs aliases it for PROBS. */
} gateTarget;

/* Parse one of
 *
 *   LABEL label            (gates with classes)
 *   PROBS p1 ... pK        (CLASSIFIER; exactly K = cfg->nclasses values,
 *                           finite, >= 0, summing to 1 within 1e-3;
 *                           renormalized to sum exactly to 1)
 *   CORRECT <0|1>          (only if 'allow_correct')
 *
 * at argv[*pos], advancing *pos past it. 'allow_classes' = 0 rejects LABEL
 * and PROBS (GLEARN on a SCORE gate). 't' must be zeroed by the caller and
 * released with gateTargetFree() in every case. */
int gateParseTarget(const gateConfig *cfg, const gateArg *argv, int argc, int *pos,
                    int allow_classes, int allow_correct, gateTarget *t,
                    char *err, size_t errlen);
void gateTargetFree(gateTarget *t);

/* ------------------------------ Small enums ------------------------------ */

/* CANDIDATE | CALIBRATION | MODEL | ALL. Returns 1 on success. */
int gateParseResetScope(const gateArg *a, gateResetScope *scope);

#endif /* GATE_ARGS_H */
