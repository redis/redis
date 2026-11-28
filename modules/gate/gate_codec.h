/*
 * Gate data type: versioned binary encoding of a whole gate.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * One self-describing blob holds the complete durable state of a gate:
 * configuration, threshold grid, ledger, trainer, candidate and serving
 * generations (weights, per-region evidence, policy, drift windows),
 * pending observations in order (flags, frozen predictions, results,
 * stored features), id bookkeeping and counters. It is used for RDB
 * save/load, DUMP/RESTORE, AOF rewrite and DEBUG DIGEST.
 *
 * FORMAT RULES
 *   - Magic "GATE" followed by a little-endian uint16 format version
 *     (GATE_CODEC_VERSION). Unknown versions are rejected.
 *   - Fixed width little-endian integers; doubles and floats as the
 *     little-endian bytes of their IEEE-754 bit patterns; strings and arrays
 *     carry explicit lengths. Nothing depends on host endianness, struct
 *     layout or padding.
 *   - The encoding is canonical: equal states produce equal bytes, and
 *     gateEncode(gateDecode(b)) == b for every blob b that decodes.
 *   - Not stored: gate.runtime_tag (always 0 after decode) and values that
 *     are pure functions of stored ones (the per-row bet increments, the
 *     cached fixed footprint, the observation index and list links).
 *     The threshold grid IS stored: thresholds are policy, and must not
 *     move if a libm rounds differently on another host.
 *
 * DECODING IS UNTRUSTED-INPUT SAFE. RESTORE hands attacker-controlled bytes
 * to the decoder, so it must never read out of bounds, overflow a size
 * computation, or allocate more than the limits in gate_defs.h allow, for
 * ANY input. Every length is checked against the remaining input and
 * against the hard limits before allocating; every number is checked for
 * finiteness and range; class indices, feature indices (ascending, below
 * the model dimension), generation numbers, observation order, the grid
 * (strictly increasing) and policy indices are all validated; trailing
 * bytes are an error. A blob that passes must also satisfy
 * gateConfigValidate() and gateCheckInvariants(). On any failure the
 * decoder releases everything it built and returns NULL.
 */

#ifndef GATE_CODEC_H
#define GATE_CODEC_H

#include <stddef.h>
#include "gate_core.h"

#define GATE_CODEC_VERSION 1

/* Serialize 'g'. On success '*buf' is a gate_malloc()ed buffer of '*len'
 * bytes that the caller releases with gate_free(). GATE_OK / GATE_ERR
 * (allocation failure; '*buf' is then NULL). */
int gateEncode(const gate *g, unsigned char **buf, size_t *len);

/* Parse a blob produced by gateEncode(). Returns a new gate, or NULL after
 * writing a short reason into 'err' (no trailing newline). */
gate *gateDecode(const unsigned char *buf, size_t len, char *err, size_t errlen);

#endif /* GATE_CODEC_H */
