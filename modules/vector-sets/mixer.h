/* Redis implementation for vector sets. The data structure itself
 * is implemented in hnsw.c.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 * Originally authored by: Salvatore Sanfilippo.
 *
 * =============================================================================
 *
 * Mixing function for HNSW link integrity verification
 * Designed to resist collision attacks when salts are unknown.
 */

#include <stdint.h>
#include <string.h>

static inline uint64_t ROTL64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

// Use more rounds and stronger constants
#define MIX_PRIME_1 0xFF51AFD7ED558CCDULL
#define MIX_PRIME_2 0xC4CEB9FE1A85EC53ULL
#define MIX_PRIME_3 0x9E3779B97F4A7C15ULL
#define MIX_PRIME_4 0xBF58476D1CE4E5B9ULL
#define MIX_PRIME_5 0x94D049BB133111EBULL
#define MIX_PRIME_6 0x2B7E151628AED2A7ULL

/* Mixer design goals:
 * 1. Thorough mixing of the level parameter.
 * 2. Enough rounds of mixing.
 * 3. Cross-influence between h1 and h2.
 * 4. Domain separation to prevent related-key attacks.
 */
void secure_pair_mixer_128(uint64_t salt0, uint64_t salt1,
                          uint64_t id1_in, uint64_t id2_in, uint64_t level,
                          uint64_t* out_h1, uint64_t* out_h2) {
    // Order independence (A -> B links should hash as B -> A links).
    uint64_t id_a = (id1_in < id2_in) ? id1_in : id2_in;
    uint64_t id_b = (id1_in < id2_in) ? id2_in : id1_in;

    // Domain separation: mix salts with a constant to prevent
    // related-key attacks.
    uint64_t h1 = salt0 ^ 0xDEADBEEFDEADBEEFULL;
    uint64_t h2 = salt1 ^ 0xCAFEBABECAFEBABEULL;

    // First, thoroughly mix the level into both accumulators
    // This prevents predictable level values from being a weakness
    uint64_t level_mix = level;
    level_mix *= MIX_PRIME_5;
    level_mix ^= level_mix >> 32;
    level_mix *= MIX_PRIME_6;

    h1 ^= level_mix;
    h2 ^= ROTL64(level_mix, 31);

    // Mix in id_a with strong diffusion.
    h1 ^= id_a;
    h1 *= MIX_PRIME_1;
    h1 = ROTL64(h1, 23);
    h1 *= MIX_PRIME_2;

    // Mix in id_b.
    h2 ^= id_b;
    h2 *= MIX_PRIME_3;
    h2 = ROTL64(h2, 29);
    h2 *= MIX_PRIME_4;

    // Three rounds of cross-mixing for better security.
    for (int i = 0; i < 3; i++) {
        // Cross-influence.
        uint64_t tmp = h1;
        h1 += h2;
        h2 += tmp;

        // Mix h1.
        h1 ^= ROTL64(h1, 31);
        h1 *= MIX_PRIME_1;
        h1 ^= salt0;

        // Mix h2.
        h2 ^= ROTL64(h2, 37);
        h2 *= MIX_PRIME_2;
        h2 ^= salt1;
    }

    // Finalization with avalanche rounds.
    h1 ^= h1 >> 33;
    h1 *= MIX_PRIME_3;
    h1 ^= h1 >> 29;
    h1 *= MIX_PRIME_4;
    h1 ^= h1 >> 32;

    h2 ^= h2 >> 33;
    h2 *= MIX_PRIME_5;
    h2 ^= h2 >> 29;
    h2 *= MIX_PRIME_6;
    h2 ^= h2 >> 32;

    *out_h1 = h1;
    *out_h2 = h2;
}
