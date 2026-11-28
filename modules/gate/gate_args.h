/*
 * Gate data type: command arguments.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Turns argument vectors of the G* commands (SPEC.md section 2) into
 * validated structures. Pure C over (pointer, length) pairs, so the grammar
 * is unit tested without a server and the public commands and the internal
 * replication command share one parser.
 *
 * Conventions:
 *   - option words are matched case-insensitively (ASCII); labels and
 *     feature names are compared byte for byte;
 *   - nothing is partially applied: on error the output is released or
 *     left empty and 'err' holds a message for an "-ERR <msg>" reply (no
 *     "ERR " prefix, no trailing newline);
 *   - numbers are strict: the whole argument must be consumed, no leading
 *     or trailing space, no "nan" / "inf", no hex floats; integers are
 *     decimal digits only.
 */

#ifndef GATE_ARGS_H
#define GATE_ARGS_H

#include <stddef.h>
#include <stdint.h>
#include "gate_core.h"
#include "gate_features.h"

typedef struct gateArg {
    const char *ptr;
    size_t len;
} gateArg;

/* --------------------------- Scalar helpers ------------------------------ */

/* Strict finite double / unsigned 64-bit integer. Return 1 on success. */
int gateParseDouble(const char *s, size_t len, double *out);
int gateParseU64(const char *s, size_t len, uint64_t *out);

/* Case-insensitive comparison of an argument with an upper-case word. */
int gateArgIs(const gateArg *a, const char *word);

/* Observation ids: "<epoch as 16 lower-case hex digits>-<seq in decimal>",
 * for example "5d0c3f9a7b21e684-1042". Parsing is strict: exactly 16 hex
 * digits (either case), one '-', 1 to 20 decimal digits without a leading
 * zero, seq >= 1 and fitting in 64 bits. */
#define GATE_ID_MAX 40
size_t gateFormatId(char *buf, size_t buflen, uint64_t epoch, uint64_t seq);
int gateParseId(const char *s, size_t len, uint64_t *epoch, uint64_t *seq);

/* Doubles that must survive replication bit for bit travel as the 16
 * lower-case hex digits of their IEEE-754 pattern; 64-bit ids likewise.
 * The formatters write 16 digits and a terminating NUL. */
void gateFormatDoubleBits(char *buf, double v);
int gateParseDoubleBits(const char *s, size_t len, double *out);
void gateFormatHex64(char *buf, uint64_t v);
int gateParseHex64(const char *s, size_t len, uint64_t *out);

/* -------------------------------- GCREATE -------------------------------- */

/* Parse the arguments that follow the key of
 *
 *   GCREATE key ALPHA alpha CLASSES count label [label ...]
 *           [AUDIT rate] [DELTA delta]
 *           [ALWAYSDEFER count label [label ...]]
 *           [CLASSALPHA count label alpha [label alpha ...]]
 *   GCREATE key ALPHA alpha SCORE [AUDIT rate] [DELTA delta]
 *
 * into '*cfg' (overwritten; on success the caller frees it with
 * gateConfigFree()). Options may come in any order, each at most once.
 * ALPHA is required, and exactly one of CLASSES and SCORE. Every counted
 * list consumes exactly its count, so a label may spell an option word.
 * ALWAYSDEFER and CLASSALPHA are resolved after the whole command is read
 * (CLASSES may follow them) and require CLASSES; an unknown label, a label
 * listed twice in one of them, or a label in both is an error.
 * 'snapshot_examples' and 'feedback_timeout_ms' come from the server
 * configuration and are stored in the config. The result has passed
 * gateConfigValidate(). GATE_OK / GATE_ERR. */
int gateParseCreate(const gateArg *argv, int argc, uint32_t snapshot_examples,
                    int64_t feedback_timeout_ms, gateConfig *cfg,
                    char *err, size_t errlen);

/* ------------------------------- Requests -------------------------------- */

typedef struct gateRequest {
    const char *text; /* borrowed from the argument vector */
    size_t text_len;
    uint32_t nfeatures;
    gateNamedFeature features[GATE_MAX_FEATURES]; /* names are borrowed */
} gateRequest;

/* Parse "TEXT text [FEATURES count name value [name value ...]]" starting
 * at argv[*pos]; on return *pos is the first unconsumed argument. TEXT is
 * mandatory and at most GATE_MAX_TEXT_BYTES long (it may be empty).
 * "FEATURES count" is the number of pairs, 0 to GATE_MAX_FEATURES; names
 * must satisfy gateValidName(.., GATE_MAX_NAME_BYTES); values must be
 * finite numbers. GATE_OK / GATE_ERR. */
int gateParseRequest(const gateArg *argv, int argc, int *pos, gateRequest *req,
                     char *err, size_t errlen);

#endif /* GATE_ARGS_H */
