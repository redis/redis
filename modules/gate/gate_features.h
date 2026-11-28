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
 * One request (a TEXT and optional named FEATURES) becomes one coalesced
 * sparse vector over GATE_DIM hash buckets. The mapping is a pure function
 * of the request bytes: no vocabulary, no state, no locale. It follows the
 * reference design (words, word pairs, four-letter chunks and structural
 * hints hashed with FNV-1a into 8,192 buckets) and is versioned as
 * GATE_FEATURES_VERSION: frozen weights only mean something for the mapping
 * they were trained with, so any change to the rules below must bump it.
 *
 * HASH. H(domain, bytes) = fmix64(FNV-1a-64(domain byte || bytes)), with
 * FNV-1a-64 offset basis 0xcbf29ce484222325 and prime 0x100000001b3, and
 * fmix64 the MurmurHash3 finalizer:
 *
 *     h ^= h >> 33;  h *= 0xff51afd7ed558ccd;
 *     h ^= h >> 33;  h *= 0xc4ceb9fe1a85ec53;
 *     h ^= h >> 33;
 *
 * (Raw FNV-1a has weak low bits, which is what "hash % 8192" consumes.)
 * A clue with hash h lands in bucket h % GATE_DIM.
 *
 * TEXT. Only the first GATE_TEXT_READ_BYTES bytes are read ("the prefix");
 * the text does not have to be valid UTF-8.
 *
 *   1. ASCII 'A'-'Z' are lowercased. Nothing else is normalized.
 *   2. Tokens. A word is a maximal run of word bytes: ASCII letters, ASCII
 *      digits and every byte >= 0x80 (so a multi-byte character stays
 *      inside one word); a word longer than GATE_MAX_TOKEN_BYTES is cut to
 *      its first GATE_MAX_TOKEN_BYTES bytes. Each '?', '!' and '@' is a
 *      token of its own. Every other byte only separates tokens.
 *   3. Clues, each counted once however often it occurs (distinctness is
 *      decided on the 64-bit hash):
 *        word   H(0x01, token)                       for every token
 *        pair   H(0x02, token1 || 0x00 || token2)    for adjacent tokens
 *        chunk  H(0x03, tag || 4 bytes)              for every window of 4
 *               consecutive bytes of a word longer than 4 bytes; tag is
 *               '^' for the word's first window, '$' for its last, '.' for
 *               the ones in between (so "shipped" and "shipping" share
 *               "^ship" and ".hipp")
 *        hint   H(0x04, name) for the structural hints:
 *               "len0".."len4"  the length bucket of the WHOLE text
 *                               (<= 16, <= 64, <= 256, <= 1024, longer)
 *               "link"          the prefix contains "http://" or "https://"
 *                               (ASCII case-insensitive)
 *               "code"          the prefix contains three backticks
 *   4. A clue adds +1 to its bucket if bit 63 of its hash is clear and -1
 *      if it is set. Contributions to one bucket are summed; buckets that
 *      sum to zero are dropped.
 *   5. If the squared L2 norm of the result exceeds GATE_TEXT_NORM2_CAP it
 *      is scaled down to exactly that norm, so a long message cannot
 *      dominate the model. Short messages keep their +-1 values.
 *
 * NAMED FEATURES. Each (name, value) adds 'value', clamped to
 *   +-GATE_FEATURE_CLAMP, to bucket H(0x05, name) % GATE_DIM. A name given
 *   twice in one request is an error. If the squared L2 norm of the named
 *   part exceeds GATE_FEATURE_NORM2_CAP it is scaled down to that norm.
 *
 * The text part and the named part are scaled separately and then merged
 * bucket by bucket (summing; zero sums dropped).
 */

#ifndef GATE_FEATURES_H
#define GATE_FEATURES_H

#include <stddef.h>
#include <stdint.h>
#include "gate_defs.h"

#define GATE_FEATURES_VERSION 1

typedef struct gateNamedFeature {
    const char *name;
    size_t name_len;
    double value;      /* finite; the caller has validated it */
} gateNamedFeature;

/* Build the vector of one request into 'out' (which must be empty; on
 * success the caller owns its arrays and frees them with gateSparseFree()).
 * 'text' may be NULL only if text_len is 0. Returns GATE_OK, or GATE_ERR
 * with 'out' left empty when a feature name is repeated, nfeatures exceeds
 * GATE_MAX_FEATURES, or memory cannot be allocated; 'err' (if not NULL)
 * then holds a short reason without a trailing newline. */
int gateFeaturize(const char *text, size_t text_len,
                  const gateNamedFeature *features, uint32_t nfeatures,
                  gateSparse *out, char *err, size_t errlen);

/* H(domain, bytes) as defined above. Exposed for test vectors. */
uint64_t gateHash(uint8_t domain, const void *data, size_t len);
/* Raw FNV-1a 64 of 'data' (no domain byte, no finalizer). */
uint64_t gateFnv1a64(const void *data, size_t len);

#endif /* GATE_FEATURES_H */
