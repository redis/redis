#ifndef HNSW_POPCOUNT_H
#define HNSW_POPCOUNT_H

/* Optimized popcount implementation for binary vector distance calculations.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 * Originally authored by: Salvatore Sanfilippo.
 */

#include <stdint.h>

/* Define likely macro if not already defined */
#ifndef likely
#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)
#else
#define likely(x) (x)
#endif
#endif

/* Define HAVE_POPCNT if the compiler supports the target("popcnt") attribute */
#if defined(__x86_64__) && ((defined(__GNUC__) && __GNUC__ > 5) || (defined(__clang__)))
    #if defined(__has_attribute) && __has_attribute(target)
        #define HAVE_POPCNT
        #define ATTRIBUTE_TARGET_POPCNT __attribute__((target("popcnt")))
    #else
        #define ATTRIBUTE_TARGET_POPCNT
    #endif
#else
    #define ATTRIBUTE_TARGET_POPCNT
#endif

/* Check if CPU supports POPCNT instruction - cached per thread */
static inline int hnsw_cpu_supports_popcnt(void) {
#if defined(HAVE_POPCNT)
    static __thread int popcnt_supported = -1;
    if (popcnt_supported == -1) {
        popcnt_supported = __builtin_cpu_supports("popcnt");
    }
    return popcnt_supported;
#else
    return 0; /* Assume CPU does not support POPCNT if __builtin_cpu_supports() is not available. */
#endif
}

/* Manual popcount implementation for platforms without POPCNT support */
static inline int hnsw_popcount64(uint64_t x) {
    x = (x & 0x5555555555555555) + ((x >> 1) & 0x5555555555555555);
    x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
    x = (x & 0x0F0F0F0F0F0F0F0F) + ((x >> 4) & 0x0F0F0F0F0F0F0F0F);
    x = (x & 0x00FF00FF00FF00FF) + ((x >> 8) & 0x00FF00FF00FF00FF);
    x = (x & 0x0000FFFF0000FFFF) + ((x >> 16) & 0x0000FFFF0000FFFF);
    x = (x & 0x00000000FFFFFFFF) + ((x >> 32) & 0x00000000FFFFFFFF);
    return x;
}

/* Optimized popcount function that uses hardware POPCNT instruction when available,
 * falling back to a software implementation when necessary. The CPU feature detection
 * result is cached per thread for better performance. */
ATTRIBUTE_TARGET_POPCNT
static inline int hnsw_popcount(uint64_t x) {
    if (likely(hnsw_cpu_supports_popcnt())) {
        return __builtin_popcountll(x);
    } else {
        return hnsw_popcount64(x);
    }
}

/* Binary vectors distance function that uses POPCNT when available */
ATTRIBUTE_TARGET_POPCNT
static inline float hnsw_vectors_distance_bin(const uint64_t *x, const uint64_t *y, uint32_t dim) {
    uint32_t len = (dim+63)/64;
    uint32_t opposite = 0;

    for (uint32_t j = 0; j < len; j++) {
        uint64_t xor = x[j]^y[j];
        opposite += hnsw_popcount(xor);
    }
    return (float)opposite*2/dim;
}

#endif /* HNSW_POPCOUNT_H */
