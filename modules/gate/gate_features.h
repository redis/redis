/*
 * Gate data type: deterministic feature extraction.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Maps one request (TEXT, VECTOR and/or named FEATURES sections) to a
 * coalesced sparse vector of dimension D = text_dim + vector_dim +
 * feature_dim. Each configured modality owns a disjoint index range, in
 * that order, so one modality can never collide with another:
 *
 *     [0, text_dim)                          hashed text features
 *     [text_dim, text_dim + vector_dim)      client embedding
 *     [text_dim + vector_dim, D)             hashed named features
 *
 * The mapping is a pure function of the schema and the request bytes: no
 * vocabulary, no state, no locale, no floating point environment. It is
 * versioned as GATE_FEATURES_VERSION; any change to the rules below must
 * bump it, because frozen weights are only meaningful for the mapping they
 * were trained with.
 *
 * HASH. H(domain, bytes) = fmix64(FNV-1a-64(domain byte || bytes)) where
 * FNV-1a-64 uses offset basis 0xcbf29ce484222325 and prime 0x100000001b3,
 * and fmix64 is the MurmurHash3 64-bit finalizer:
 *
 *     h ^= h >> 33;  h *= 0xff51afd7ed558ccd;
 *     h ^= h >> 33;  h *= 0xc4ceb9fe1a85ec53;
 *     h ^= h >> 33;
 *
 * The finalizer is there because raw FNV-1a has weak low bits, which is
 * exactly what "hash % dim" consumes when dim is a power of two.
 *
 * TEXT (domain bytes 0x01 unigram, 0x02 bigram).
 *   1. The text is processed as raw bytes; it does not have to be valid
 *      UTF-8. ASCII 'A'-'Z' are lowercased. Nothing else is normalized.
 *   2. Token bytes are ASCII letters, ASCII digits and every byte >= 0x80
 *      (so a UTF-8 multi-byte sequence stays inside one token). Every other
 *      byte separates tokens. Tokens longer than GATE_MAX_TOKEN_BYTES are
 *      truncated to their first GATE_MAX_TOKEN_BYTES bytes.
 *   3. Features are the distinct tokens (unigrams: H(0x01, token)) and the
 *      distinct ordered pairs of adjacent tokens (bigrams:
 *      H(0x02, token1 || 0x00 || token2)). Presence is binary: repeating a
 *      token or a pair adds nothing. Distinctness is decided on the 64-bit
 *      hash.
 *   4. Feature h contributes +1 (bit 63 of h clear) or -1 (bit 63 set) to
 *      bucket h % text_dim. Contributions landing in one bucket are summed;
 *      buckets that sum to zero are dropped.
 *   5. The block is scaled to unit L2 norm, so the logit scale does not grow
 *      with message length. A text without tokens yields an empty block.
 *
 * VECTOR. A blob of exactly vector_dim little-endian IEEE-754 float32
 *   values, each finite with |v| <= GATE_MAX_VECTOR_VALUE. The block is
 *   scaled to unit L2 norm (an all-zero vector stays empty). No embedding is
 *   ever computed inside Redis.
 *
 * FEATURES (domain byte 0x03). Each (name, value) pair contributes 'value'
 *   to bucket H(0x03, name) % feature_dim. Names are 1..GATE_MAX_NAME_BYTES
 *   bytes of valid UTF-8 without control characters; values are finite with
 *   |v| <= GATE_MAX_FEATURE_VALUE and are used as given (clients should
 *   scale them to roughly [-1, 1]). A name repeated in one request is an
 *   error. Two different names of one request that hash to the same bucket
 *   are an error too ("increase FEATUREDIM") instead of silently sharing a
 *   weight. Zero values are dropped.
 *
 * A request must contain exactly the sections the schema configures: a
 * missing section and an unconfigured section are both errors. An empty
 * TEXT string or a FEATURES section with zero pairs is a present, empty
 * section.
 */

#ifndef GATE_FEATURES_H
#define GATE_FEATURES_H

#include <stddef.h>
#include <stdint.h>
#include "gate_defs.h"

#define GATE_FEATURES_VERSION 1

#define GATE_INPUT_TEXT (1u << 0)
#define GATE_INPUT_VECTOR (1u << 1)
#define GATE_INPUT_FEATURES (1u << 2)

/* Input schema of a CLASSIFIER gate. A dimension is 0 exactly when its
 * modality is not in 'inputs'. */
typedef struct gateSchema {
    uint32_t inputs;      /* mask of GATE_INPUT_* */
    uint32_t text_dim;
    uint32_t vector_dim;
    uint32_t feature_dim;
} gateSchema;

/* Total model dimension D. */
static inline uint32_t gateSchemaDim(const gateSchema *s) {
    return s->text_dim + s->vector_dim + s->feature_dim;
}

typedef struct gateNamedFeature {
    const char *name;
    size_t name_len;
    double value;
} gateNamedFeature;

/* One request. All pointers are borrowed for the duration of the call. */
typedef struct gateInput {
    uint32_t present;              /* mask of GATE_INPUT_* sections given */
    const char *text;
    size_t text_len;
    const unsigned char *vector;   /* float32 little-endian blob */
    size_t vector_len;             /* in bytes */
    const gateNamedFeature *features;
    uint32_t nfeatures;
} gateInput;

/* Build the coalesced feature vector of 'in' under schema 's' into 'out'
 * (which must be empty; on success the caller owns its arrays and releases
 * them with gateSparseFree()). On error returns GATE_ERR, leaves 'out'
 * empty and writes a human readable reason into 'err' (no trailing
 * newline), e.g. "missing TEXT section", "VECTOR must be 1536 bytes",
 * "feature names 'a' and 'b' collide; increase FEATUREDIM". */
int gateFeaturize(const gateSchema *s, const gateInput *in, gateSparse *out,
                  char *err, size_t errlen);

/* H(domain, bytes) as defined above. Exposed for test vectors. */
uint64_t gateHash(uint8_t domain, const void *data, size_t len);
/* Raw FNV-1a 64 of 'data' (no domain byte, no finalizer), for test vectors. */
uint64_t gateFnv1a64(const void *data, size_t len);

#endif /* GATE_FEATURES_H */
