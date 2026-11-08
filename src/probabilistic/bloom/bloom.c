/*
 *  Copyright (c) 2012-2017, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

/*
 * Refer to bloom.h for documentation on the public interfaces.
 */

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bloom.h"
#include "murmurhash.h"
#include "zmalloc.h"

#define MODE_READ 0
#define MODE_WRITE 1

#define LN2 (0.693147180559945)

inline static int test_bit_set_bit(unsigned char *buf, uint64_t x, int mode) {
    uint64_t byte = x >> 3;
    uint8_t mask = 1 << (x % 8);
    uint8_t c = buf[byte]; // expensive memory access

    if (c & mask) {
        return 1;
    } else {
        if (mode == MODE_WRITE) {
            buf[byte] = c | mask;
        }
        return 0;
    }
}

/* MurmurHash2 by Austin Appleby, public domain. Keep the native-endian
 * 32-bit variant for legacy Bloom filters. */
static uint32_t bloomMurmurHash32(const void *key, size_t len, uint32_t seed) {
    // 'm' and 'r' are mixing constants generated offline.
    // They're not really 'magic', they just happen to work well.

    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    // Initialize the hash to a 'random' value

    uint32_t h = seed ^ len;

    // Mix 4 bytes at a time into the hash

    const unsigned char *data = (const unsigned char *)key;

    while (len >= 4) {
        uint32_t k;
        memcpy(&k, data, sizeof(k));

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    // Handle the last few bytes of the input array

    switch (len) {
    case 3:
        h ^= data[2] << 16;
        /* fall through */
    case 2:
        h ^= data[1] << 8;
        /* fall through */
    case 1:
        h ^= data[0];
        h *= m;
    };

    // Do a few final mixes of the hash to ensure the last few
    // bytes are well-incorporated.

    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

bloom_hashval bloom_calc_hash(const void *buffer, size_t len) {
    bloom_hashval rv;
    rv.a = bloomMurmurHash32(buffer, len, 0x9747b28c);
    rv.b = bloomMurmurHash32(buffer, len, rv.a);
    return rv;
}

bloom_hashval bloom_calc_hash64(const void *buffer, size_t len) {
    bloom_hashval rv;
    rv.a = MurmurHash64A(buffer, len, 0xc6a4a7935bd1e995ULL, BYTE_ORDER);
    rv.b = MurmurHash64A(buffer, len, rv.a, BYTE_ORDER);
    return rv;
}

// This function is defined as a macro because newer filters use a power of two
// for bit count, which is must faster to calculate. Older bloom filters don't
// use powers of two, so they are slower. Rather than calculating this inside
// the function itself, we provide two variants for this. The calling layer
// already knows which variant to call.
//
// modExp is the expression which will evaluate to the number of bits in the
// filter.
#define CHECK_ADD_FUNC(T, modExp)                                                                  \
    T i;                                                                                           \
    int found_unset = 0;                                                                           \
    register const T mod = modExp;                                                                 \
    for (i = 0; i < bloom->hashes; i++) {                                                          \
        T x = ((hashval.a + i * hashval.b)) % mod;                                                 \
        if (!test_bit_set_bit(bloom->bf, x, mode)) {                                               \
            if (mode == MODE_READ) {                                                               \
                return 0;                                                                          \
            }                                                                                      \
            found_unset = 1;                                                                       \
        }                                                                                          \
    }                                                                                              \
    if (mode == MODE_READ) {                                                                       \
        return 1;                                                                                  \
    }                                                                                              \
    return found_unset;

static int bloom_check_add32(struct bloom *bloom, bloom_hashval hashval, int mode) {
    CHECK_ADD_FUNC(uint32_t, (1U << bloom->n2));
}

static int bloom_check_add64(struct bloom *bloom, bloom_hashval hashval, int mode) {
    CHECK_ADD_FUNC(uint64_t, (1LLU << bloom->n2));
}

// This function is used for older bloom filters whose bit count was not
// 1 << X. This function is a bit slower, and isn't exposed in the API
// directly because it's deprecated
static int bloom_check_add_compat(struct bloom *bloom, bloom_hashval hashval, int mode) {
    CHECK_ADD_FUNC(uint64_t, bloom->bits)
}

static double calc_bpe(double error) {
    static const double denom = 0.480453013918201; // ln(2)^2
    double num = log(error);

    double bpe = -(num / denom);
    if (bpe < 0) {
        bpe = -bpe;
    }
    return bpe;
}

// Returns
//   0 on success.
//   1 on invalid argument
//  -1 on insufficient memory
int bloom_init(struct bloom *bloom, uint64_t entries, double error, unsigned options) {
    if (entries < 1 || !isfinite(error) || error <= 0 || error >= 1.0) {
        return 1;
    }

    bloom->error = error;
    bloom->bits = 0;
    bloom->entries = entries;
    bloom->bpe = calc_bpe(error);

    uint64_t bits;

    if (options & BLOOM_OPT_ENTS_IS_BITS) {
        // Size is determined by the number of bits
        if (/* entries == 0 || */ entries > 63) {
            return 1;
        }

        bloom->n2 = entries;
        bits = 1LLU << bloom->n2;
        double capacity = bits / bloom->bpe;
        if (capacity >= (double)UINT64_MAX) return 1;
        bloom->entries = (uint64_t)capacity;

    } else if (options & BLOOM_OPT_NOROUND) {
        // Don't perform any rounding. Conserve memory instead
        double requested_bits = entries * bloom->bpe;
        if (!isfinite(requested_bits) || requested_bits >= (double)UINT64_MAX) return 1;
        bits = (uint64_t)requested_bits;

        // Guard against very small 'bpe'. Have at least one bit in the filter.
        if (bits == 0) {
            bits = 1;
        }
        bloom->bits = bits;
        bloom->n2 = 0;

    } else {
        double bn2 = logb(entries * bloom->bpe);
        if (!isfinite(bn2) || bn2 < -1 || bn2 >= 63) {
            return 1;
        }
        bloom->n2 = bn2 + 1;
        bits = 1LLU << bloom->n2;

        // Determine the number of extra bits available for more items. We rounded
        // up the number of bits to the next-highest power of two. This means we
        // might have up to 2x the bits available to us.
        uint64_t bitDiff = bits - (entries * bloom->bpe);
        // The number of additional items we can store is the extra number of bits
        // divided by bits-per-element
        double extra_entries = bitDiff / bloom->bpe;
        if (extra_entries >= (double)(UINT64_MAX - bloom->entries)) return 1;
        bloom->entries += (uint64_t)extra_entries;
    }

    if (bits > UINT64_MAX - 63) return 1;
    if (bits % 64) {
        bloom->bytes = ((bits / 64) + 1) * 8;
    } else {
        bloom->bytes = bits / 8;
    }
    bloom->bits = bloom->bytes * 8;

    bloom->force64 = (options & BLOOM_OPT_FORCE64);
    bloom->hashes = (int)ceil(LN2 * bloom->bpe); // ln(2)
    if (bloom->bytes > SIZE_MAX) return 1;
    bloom->bf = ztrycalloc(bloom->bytes);
    if (bloom->bf == NULL) {
        return -1;
    }

    return 0;
}

int bloom_check_h(const struct bloom *bloom, bloom_hashval hash) {
    if (bloom->n2 > 0) {
        if (bloom->force64 || bloom->n2 > 31) {
            return bloom_check_add64((void *)bloom, hash, MODE_READ);
        } else {
            return bloom_check_add32((void *)bloom, hash, MODE_READ);
        }
    } else {
        return bloom_check_add_compat((void *)bloom, hash, MODE_READ);
    }
}

int bloom_check(const struct bloom *bloom, const void *buffer, size_t len) {
    return bloom_check_h(bloom, bloom_calc_hash(buffer, len));
}

int bloom_add_h(struct bloom *bloom, bloom_hashval hash) {
    if (bloom->n2 > 0) {
        if (bloom->force64 || bloom->n2 > 31) {
            return !bloom_check_add64(bloom, hash, MODE_WRITE);
        } else {
            return !bloom_check_add32(bloom, hash, MODE_WRITE);
        }
    } else {
        return !bloom_check_add_compat(bloom, hash, MODE_WRITE);
    }
}

int bloom_add(struct bloom *bloom, const void *buffer, size_t len) {
    return bloom_add_h(bloom, bloom_calc_hash(buffer, len));
}

void bloom_free(struct bloom *bloom) { zfree(bloom->bf); }

// Returns 0 on success
int bloom_validate_integrity(struct bloom *bloom) {
    /* Legacy encoding 0 can leave unused padding bits in the last byte. */
    if (!bloom || !isfinite(bloom->error) || bloom->error <= 0 || bloom->error >= 1.0 ||
        !isfinite(bloom->bpe) || bloom->bpe <= 0 || bloom->bytes > UINT64_MAX / 8 ||
        (bloom->n2 > 63) || (bloom->n2 != 0 && bloom->bits < (1ULL << bloom->n2)) ||
        bloom->bits == 0 || bloom->bits > bloom->bytes * 8) {
        return 1;
    }

    const double expected_hashes = ceil(LN2 * bloom->bpe);
    if (!isfinite(expected_hashes) || expected_hashes < 1 || expected_hashes > BLOOM_MAX_HASHES ||
        bloom->hashes != (uint32_t)expected_hashes) {
        return 1;
    }

    return 0;
}
