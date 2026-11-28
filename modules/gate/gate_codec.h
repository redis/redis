/*
 * Gate data type: binary encoding of a whole gate.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * One self-describing blob holds the complete state of a gate:
 * configuration, ledger, trainer, candidate and serving generations
 * (weights, per-region proof, policy, drift detectors), waiting requests in
 * order (flags, frozen guesses, answers, stored clues), ids and counters.
 * It is used for RDB save/load, DUMP/RESTORE, AOF rewrite and DEBUG DIGEST.
 *
 * FORMAT RULES
 *   - Magic "GATE", then a little-endian uint16 format version
 *     (GATE_CODEC_VERSION), then a uint16 GATE_FEATURES_VERSION. Unknown
 *     versions are rejected.
 *   - Fixed-width little-endian integers; doubles and floats as the
 *     little-endian bytes of their IEEE-754 patterns; strings and arrays
 *     carry explicit lengths. Nothing depends on host endianness, struct
 *     layout or padding.
 *   - Canonical: equal states give equal bytes, and
 *     gateEncode(gateDecode(b)) == b for every blob b that decodes.
 *   - Not stored: runtime_tag (0 after decoding), and values that are pure
 *     functions of stored ones (the threshold grid, the betting and drift
 *     increments, the request index and list links).
 *   - With 'for_digest' set, the fields only the primary maintains are
 *     written as zero: now_ms and the request counters (requests, accepted,
 *     audited, abstained, full, window, coverage). Replicas agree with their
 *     primary on everything else, so digests compare equal.
 *
 * DECODING IS SAFE ON UNTRUSTED INPUT. RESTORE hands attacker-controlled
 * bytes to the decoder. For ANY input it must not read out of bounds,
 * overflow a size computation, or allocate more than the limits in
 * gate_defs.h allow. Every length is checked against the remaining input
 * and the limits before allocating; every number is checked for finiteness
 * and range; label indices, bucket indices (ascending, below GATE_DIM),
 * generation numbers, request order, flags and policy indices are
 * validated; trailing bytes are an error. A decoded gate must pass
 * gateConfigValidate() and gateCheckInvariants(), and its policies are
 * recomputed from the crossed regions rather than trusted. On any failure
 * everything built so far is released and NULL is returned.
 */

#ifndef GATE_CODEC_H
#define GATE_CODEC_H

#include <stddef.h>
#include "gate_core.h"

#define GATE_CODEC_VERSION 1

/* Serialize 'g'. On success '*buf' is a gate_malloc()ed buffer of '*len'
 * bytes that the caller releases with gate_free(). GATE_OK / GATE_ERR
 * (allocation failure; '*buf' is then NULL). */
int gateEncode(const gate *g, int for_digest, unsigned char **buf, size_t *len);

/* Parse a blob produced by gateEncode(g, 0, ...). Returns a new gate, or
 * NULL after writing a short reason into 'err' (no trailing newline). */
gate *gateDecode(const unsigned char *buf, size_t len, char *err, size_t errlen);

#endif /* GATE_CODEC_H */
