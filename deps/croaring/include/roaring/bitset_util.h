/*
 * bitset_util.h
 *
 * This header collects low-level utility functions for operating on raw
 * bitsets represented as arrays of 64-bit words. It includes helpers for
 * setting, clearing, flipping, counting, extracting, and combining bit ranges,
 * along with architecture-specific SIMD implementations for performance-
 * critical routines.
 *
 * In particular, this file contains accelerated extraction and population-
 * count code paths for x64 targets using AVX2 and, when supported by the
 * compiler and hardware, AVX-512.
 */
#ifndef CROARING_BITSET_UTIL_H
#define CROARING_BITSET_UTIL_H

#include <stdint.h>

#include <roaring/portability.h>
#include <roaring/utilasm.h>

#if CROARING_IS_X64
#ifndef CROARING_COMPILER_SUPPORTS_AVX512
#error "CROARING_COMPILER_SUPPORTS_AVX512 needs to be defined."
#endif  // CROARING_COMPILER_SUPPORTS_AVX512
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

/*
 * Set all bits in indexes [begin,end) to true.
 */
static inline void bitset_set_range(uint64_t *words, uint32_t start,
                                    uint32_t end) {
    if (start == end) return;
    uint32_t firstword = start / 64;
    uint32_t endword = (end - 1) / 64;
    if (firstword == endword) {
        words[firstword] |= ((~UINT64_C(0)) << (start % 64)) &
                            ((~UINT64_C(0)) >> ((~end + 1) % 64));
        return;
    }
    words[firstword] |= (~UINT64_C(0)) << (start % 64);
    for (uint32_t i = firstword + 1; i < endword; i++) {
        words[i] = ~UINT64_C(0);
    }
    words[endword] |= (~UINT64_C(0)) >> ((~end + 1) % 64);
}

/*
 * Find the cardinality of the bitset in [begin,begin+lenminusone]
 */
static inline int bitset_lenrange_cardinality(const uint64_t *words,
                                              uint32_t start,
                                              uint32_t lenminusone) {
    uint32_t firstword = start / 64;
    uint32_t endword = (start + lenminusone) / 64;
    if (firstword == endword) {
        return roaring_hamming(words[firstword] &
                               ((~UINT64_C(0)) >> ((63 - lenminusone) % 64))
                                   << (start % 64));
    }
    int answer =
        roaring_hamming(words[firstword] & ((~UINT64_C(0)) << (start % 64)));
    for (uint32_t i = firstword + 1; i < endword; i++) {
        answer += roaring_hamming(words[i]);
    }
    answer += roaring_hamming(words[endword] &
                              (~UINT64_C(0)) >>
                                  (((~start + 1) - lenminusone - 1) % 64));
    return answer;
}

/*
 * Check whether the cardinality of the bitset in [begin,begin+lenminusone] is 0
 */
static inline bool bitset_lenrange_empty(const uint64_t *words, uint32_t start,
                                         uint32_t lenminusone) {
    uint32_t firstword = start / 64;
    uint32_t endword = (start + lenminusone) / 64;
    if (firstword == endword) {
        return (words[firstword] & ((~UINT64_C(0)) >> ((63 - lenminusone) % 64))
                                       << (start % 64)) == 0;
    }
    if (((words[firstword] & ((~UINT64_C(0)) << (start % 64)))) != 0) {
        return false;
    }
    for (uint32_t i = firstword + 1; i < endword; i++) {
        if (words[i] != 0) {
            return false;
        }
    }
    if ((words[endword] &
         (~UINT64_C(0)) >> (((~start + 1) - lenminusone - 1) % 64)) != 0) {
        return false;
    }
    return true;
}

/*
 * Set all bits in indexes [begin,begin+lenminusone] to true.
 */
static inline void bitset_set_lenrange(uint64_t *words, uint32_t start,
                                       uint32_t lenminusone) {
    uint32_t firstword = start / 64;
    uint32_t endword = (start + lenminusone) / 64;
    if (firstword == endword) {
        words[firstword] |= ((~UINT64_C(0)) >> ((63 - lenminusone) % 64))
                            << (start % 64);
        return;
    }
    uint64_t temp = words[endword];
    words[firstword] |= (~UINT64_C(0)) << (start % 64);
    for (uint32_t i = firstword + 1; i < endword; i += 2)
        words[i] = words[i + 1] = ~UINT64_C(0);
    words[endword] =
        temp | (~UINT64_C(0)) >> (((~start + 1) - lenminusone - 1) % 64);
}

/*
 * Flip all the bits in indexes [begin,end).
 */
static inline void bitset_flip_range(uint64_t *words, uint32_t start,
                                     uint32_t end) {
    if (start == end) return;
    uint32_t firstword = start / 64;
    uint32_t endword = (end - 1) / 64;
    words[firstword] ^= ~((~UINT64_C(0)) << (start % 64));
    for (uint32_t i = firstword; i < endword; i++) {
        words[i] = ~words[i];
    }
    words[endword] ^= ((~UINT64_C(0)) >> ((~end + 1) % 64));
}

/*
 * Set all bits in indexes [begin,end) to false.
 */
static inline void bitset_reset_range(uint64_t *words, uint32_t start,
                                      uint32_t end) {
    if (start == end) return;
    uint32_t firstword = start / 64;
    uint32_t endword = (end - 1) / 64;
    if (firstword == endword) {
        words[firstword] &= ~(((~UINT64_C(0)) << (start % 64)) &
                              ((~UINT64_C(0)) >> ((~end + 1) % 64)));
        return;
    }
    words[firstword] &= ~((~UINT64_C(0)) << (start % 64));
    for (uint32_t i = firstword + 1; i < endword; i++) {
        words[i] = UINT64_C(0);
    }
    words[endword] &= ~((~UINT64_C(0)) >> ((~end + 1) % 64));
}

/*
 * Given a bitset containing "length" 64-bit words, write out the position
 * of all the set bits to "out", values start at "base".
 *
 * The "out" pointer should be sufficient to store the actual number of bits
 * set.
 *
 * Returns how many values were actually decoded.
 *
 * This function should only be expected to be faster than
 * bitset_extract_setbits
 * when the density of the bitset is high.
 *
 * This function uses AVX2 decoding.
 */
size_t bitset_extract_setbits_avx2(const uint64_t *words, size_t length,
                                   uint32_t *out, size_t outcapacity,
                                   uint32_t base);

size_t bitset_extract_setbits_avx512(const uint64_t *words, size_t length,
                                     uint32_t *out, size_t outcapacity,
                                     uint32_t base);
/*
 * Given a bitset containing "length" 64-bit words, write out the position
 * of all the set bits to "out", values start at "base".
 *
 * The "out" pointer should be sufficient to store the actual number of bits
 *set.
 *
 * Returns how many values were actually decoded.
 */
size_t bitset_extract_setbits(const uint64_t *words, size_t length,
                              uint32_t *out, uint32_t base);

/*
 * Given a bitset containing "length" 64-bit words, write out the position
 * of all the set bits to "out" as 16-bit integers, values start at "base" (can
 *be set to zero)
 *
 * The "out" pointer should be sufficient to store the actual number of bits
 *set.
 *
 * Returns how many values were actually decoded.
 *
 * This function should only be expected to be faster than
 *bitset_extract_setbits_uint16
 * when the density of the bitset is high.
 *
 * This function uses SSE decoding.
 */
size_t bitset_extract_setbits_sse_uint16(const uint64_t *words, size_t length,
                                         uint16_t *out, size_t outcapacity,
                                         uint16_t base);

size_t bitset_extract_setbits_avx512_uint16(const uint64_t *words,
                                            size_t length, uint16_t *out,
                                            size_t outcapacity, uint16_t base);

/*
 * Given a bitset containing "length" 64-bit words, write out the position
 * of all the set bits to "out",  values start at "base"
 * (can be set to zero)
 *
 * The "out" pointer should be sufficient to store the actual number of bits
 *set.
 *
 * Returns how many values were actually decoded.
 */
size_t bitset_extract_setbits_uint16(const uint64_t *words, size_t length,
                                     uint16_t *out, uint16_t base);

/*
 * Given two bitsets containing "length" 64-bit words, write out the position
 * of all the common set bits to "out", values start at "base"
 * (can be set to zero)
 *
 * The "out" pointer should be sufficient to store the actual number of bits
 * set.
 *
 * Returns how many values were actually decoded.
 */
size_t bitset_extract_intersection_setbits_uint16(
    const uint64_t *__restrict__ words1, const uint64_t *__restrict__ words2,
    size_t length, uint16_t *out, uint16_t base);

/*
 * Given a bitset having cardinality card, set all bit values in the list (there
 * are length of them)
 * and return the updated cardinality. This evidently assumes that the bitset
 * already contained data.
 */
uint64_t bitset_set_list_withcard(uint64_t *words, uint64_t card,
                                  const uint16_t *list, uint64_t length);
/*
 * Given a bitset, set all bit values in the list (there
 * are length of them).
 */
void bitset_set_list(uint64_t *words, const uint16_t *list, uint64_t length);

/*
 * Given a bitset having cardinality card, unset all bit values in the list
 * (there are length of them)
 * and return the updated cardinality. This evidently assumes that the bitset
 * already contained data.
 */
uint64_t bitset_clear_list(uint64_t *words, uint64_t card, const uint16_t *list,
                           uint64_t length);

/*
 * Given a bitset having cardinality card, toggle all bit values in the list
 * (there are length of them)
 * and return the updated cardinality. This evidently assumes that the bitset
 * already contained data.
 */

uint64_t bitset_flip_list_withcard(uint64_t *words, uint64_t card,
                                   const uint16_t *list, uint64_t length);

void bitset_flip_list(uint64_t *words, const uint16_t *list, uint64_t length);

#if CROARING_IS_X64
/***
 * BEGIN Harley-Seal popcount functions.
 */
CROARING_TARGET_AVX2
/**
 * Compute the population count of a 256-bit word
 * This is not especially fast, but it is convenient as part of other functions.
 */
static inline __m256i popcount256(__m256i v) {
    const __m256i lookuppos = _mm256_setr_epi8(
        /* 0 */ 4 + 0, /* 1 */ 4 + 1, /* 2 */ 4 + 1, /* 3 */ 4 + 2,
        /* 4 */ 4 + 1, /* 5 */ 4 + 2, /* 6 */ 4 + 2, /* 7 */ 4 + 3,
        /* 8 */ 4 + 1, /* 9 */ 4 + 2, /* a */ 4 + 2, /* b */ 4 + 3,
        /* c */ 4 + 2, /* d */ 4 + 3, /* e */ 4 + 3, /* f */ 4 + 4,

        /* 0 */ 4 + 0, /* 1 */ 4 + 1, /* 2 */ 4 + 1, /* 3 */ 4 + 2,
        /* 4 */ 4 + 1, /* 5 */ 4 + 2, /* 6 */ 4 + 2, /* 7 */ 4 + 3,
        /* 8 */ 4 + 1, /* 9 */ 4 + 2, /* a */ 4 + 2, /* b */ 4 + 3,
        /* c */ 4 + 2, /* d */ 4 + 3, /* e */ 4 + 3, /* f */ 4 + 4);
    const __m256i lookupneg = _mm256_setr_epi8(
        /* 0 */ 4 - 0, /* 1 */ 4 - 1, /* 2 */ 4 - 1, /* 3 */ 4 - 2,
        /* 4 */ 4 - 1, /* 5 */ 4 - 2, /* 6 */ 4 - 2, /* 7 */ 4 - 3,
        /* 8 */ 4 - 1, /* 9 */ 4 - 2, /* a */ 4 - 2, /* b */ 4 - 3,
        /* c */ 4 - 2, /* d */ 4 - 3, /* e */ 4 - 3, /* f */ 4 - 4,

        /* 0 */ 4 - 0, /* 1 */ 4 - 1, /* 2 */ 4 - 1, /* 3 */ 4 - 2,
        /* 4 */ 4 - 1, /* 5 */ 4 - 2, /* 6 */ 4 - 2, /* 7 */ 4 - 3,
        /* 8 */ 4 - 1, /* 9 */ 4 - 2, /* a */ 4 - 2, /* b */ 4 - 3,
        /* c */ 4 - 2, /* d */ 4 - 3, /* e */ 4 - 3, /* f */ 4 - 4);
    const __m256i low_mask = _mm256_set1_epi8(0x0f);

    const __m256i lo = _mm256_and_si256(v, low_mask);
    const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), low_mask);
    const __m256i popcnt1 = _mm256_shuffle_epi8(lookuppos, lo);
    const __m256i popcnt2 = _mm256_shuffle_epi8(lookupneg, hi);
    return _mm256_sad_epu8(popcnt1, popcnt2);
}
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
/**
 * Simple CSA over 256 bits
 */
static inline void CSA(__m256i *h, __m256i *l, __m256i a, __m256i b,
                       __m256i c) {
    const __m256i u = _mm256_xor_si256(a, b);
    *h = _mm256_or_si256(_mm256_and_si256(a, b), _mm256_and_si256(u, c));
    *l = _mm256_xor_si256(u, c);
}
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
/**
 * Fast Harley-Seal AVX population count function
 */
inline static uint64_t avx2_harley_seal_popcount256(const __m256i *data,
                                                    const uint64_t size) {
    __m256i total = _mm256_setzero_si256();
    __m256i ones = _mm256_setzero_si256();
    __m256i twos = _mm256_setzero_si256();
    __m256i fours = _mm256_setzero_si256();
    __m256i eights = _mm256_setzero_si256();
    __m256i sixteens = _mm256_setzero_si256();
    __m256i twosA, twosB, foursA, foursB, eightsA, eightsB;

    const uint64_t limit = size - size % 16;
    uint64_t i = 0;

    for (; i < limit; i += 16) {
        CSA(&twosA, &ones, ones, _mm256_lddqu_si256(data + i),
            _mm256_lddqu_si256(data + i + 1));
        CSA(&twosB, &ones, ones, _mm256_lddqu_si256(data + i + 2),
            _mm256_lddqu_si256(data + i + 3));
        CSA(&foursA, &twos, twos, twosA, twosB);
        CSA(&twosA, &ones, ones, _mm256_lddqu_si256(data + i + 4),
            _mm256_lddqu_si256(data + i + 5));
        CSA(&twosB, &ones, ones, _mm256_lddqu_si256(data + i + 6),
            _mm256_lddqu_si256(data + i + 7));
        CSA(&foursB, &twos, twos, twosA, twosB);
        CSA(&eightsA, &fours, fours, foursA, foursB);
        CSA(&twosA, &ones, ones, _mm256_lddqu_si256(data + i + 8),
            _mm256_lddqu_si256(data + i + 9));
        CSA(&twosB, &ones, ones, _mm256_lddqu_si256(data + i + 10),
            _mm256_lddqu_si256(data + i + 11));
        CSA(&foursA, &twos, twos, twosA, twosB);
        CSA(&twosA, &ones, ones, _mm256_lddqu_si256(data + i + 12),
            _mm256_lddqu_si256(data + i + 13));
        CSA(&twosB, &ones, ones, _mm256_lddqu_si256(data + i + 14),
            _mm256_lddqu_si256(data + i + 15));
        CSA(&foursB, &twos, twos, twosA, twosB);
        CSA(&eightsB, &fours, fours, foursA, foursB);
        CSA(&sixteens, &eights, eights, eightsA, eightsB);

        total = _mm256_add_epi64(total, popcount256(sixteens));
    }

    total = _mm256_slli_epi64(total, 4);  // * 16
    total = _mm256_add_epi64(
        total, _mm256_slli_epi64(popcount256(eights), 3));  // += 8 * ...
    total = _mm256_add_epi64(
        total, _mm256_slli_epi64(popcount256(fours), 2));  // += 4 * ...
    total = _mm256_add_epi64(
        total, _mm256_slli_epi64(popcount256(twos), 1));  // += 2 * ...
    total = _mm256_add_epi64(total, popcount256(ones));
    for (; i < size; i++)
        total =
            _mm256_add_epi64(total, popcount256(_mm256_lddqu_si256(data + i)));

    return (uint64_t)(_mm256_extract_epi64(total, 0)) +
           (uint64_t)(_mm256_extract_epi64(total, 1)) +
           (uint64_t)(_mm256_extract_epi64(total, 2)) +
           (uint64_t)(_mm256_extract_epi64(total, 3));
}
CROARING_UNTARGET_AVX2

#define CROARING_AVXPOPCNTFNC(opname, avx_intrinsic)                           \
    static inline uint64_t avx2_harley_seal_popcount256_##opname(              \
        const __m256i *data1, const __m256i *data2, const uint64_t size) {     \
        __m256i total = _mm256_setzero_si256();                                \
        __m256i ones = _mm256_setzero_si256();                                 \
        __m256i twos = _mm256_setzero_si256();                                 \
        __m256i fours = _mm256_setzero_si256();                                \
        __m256i eights = _mm256_setzero_si256();                               \
        __m256i sixteens = _mm256_setzero_si256();                             \
        __m256i twosA, twosB, foursA, foursB, eightsA, eightsB;                \
        __m256i A1, A2;                                                        \
        const uint64_t limit = size - size % 16;                               \
        uint64_t i = 0;                                                        \
        for (; i < limit; i += 16) {                                           \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i),                  \
                               _mm256_lddqu_si256(data2 + i));                 \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 1),              \
                               _mm256_lddqu_si256(data2 + i + 1));             \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 2),              \
                               _mm256_lddqu_si256(data2 + i + 2));             \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 3),              \
                               _mm256_lddqu_si256(data2 + i + 3));             \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursA, &twos, twos, twosA, twosB);                           \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 4),              \
                               _mm256_lddqu_si256(data2 + i + 4));             \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 5),              \
                               _mm256_lddqu_si256(data2 + i + 5));             \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 6),              \
                               _mm256_lddqu_si256(data2 + i + 6));             \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 7),              \
                               _mm256_lddqu_si256(data2 + i + 7));             \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursB, &twos, twos, twosA, twosB);                           \
            CSA(&eightsA, &fours, fours, foursA, foursB);                      \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 8),              \
                               _mm256_lddqu_si256(data2 + i + 8));             \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 9),              \
                               _mm256_lddqu_si256(data2 + i + 9));             \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 10),             \
                               _mm256_lddqu_si256(data2 + i + 10));            \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 11),             \
                               _mm256_lddqu_si256(data2 + i + 11));            \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursA, &twos, twos, twosA, twosB);                           \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 12),             \
                               _mm256_lddqu_si256(data2 + i + 12));            \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 13),             \
                               _mm256_lddqu_si256(data2 + i + 13));            \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 14),             \
                               _mm256_lddqu_si256(data2 + i + 14));            \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 15),             \
                               _mm256_lddqu_si256(data2 + i + 15));            \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursB, &twos, twos, twosA, twosB);                           \
            CSA(&eightsB, &fours, fours, foursA, foursB);                      \
            CSA(&sixteens, &eights, eights, eightsA, eightsB);                 \
            total = _mm256_add_epi64(total, popcount256(sixteens));            \
        }                                                                      \
        total = _mm256_slli_epi64(total, 4);                                   \
        total = _mm256_add_epi64(total,                                        \
                                 _mm256_slli_epi64(popcount256(eights), 3));   \
        total =                                                                \
            _mm256_add_epi64(total, _mm256_slli_epi64(popcount256(fours), 2)); \
        total =                                                                \
            _mm256_add_epi64(total, _mm256_slli_epi64(popcount256(twos), 1));  \
        total = _mm256_add_epi64(total, popcount256(ones));                    \
        for (; i < size; i++) {                                                \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i),                  \
                               _mm256_lddqu_si256(data2 + i));                 \
            total = _mm256_add_epi64(total, popcount256(A1));                  \
        }                                                                      \
        return (uint64_t)(_mm256_extract_epi64(total, 0)) +                    \
               (uint64_t)(_mm256_extract_epi64(total, 1)) +                    \
               (uint64_t)(_mm256_extract_epi64(total, 2)) +                    \
               (uint64_t)(_mm256_extract_epi64(total, 3));                     \
    }                                                                          \
    static inline uint64_t avx2_harley_seal_popcount256andstore_##opname(      \
        const __m256i *__restrict__ data1, const __m256i *__restrict__ data2,  \
        __m256i *__restrict__ out, const uint64_t size) {                      \
        __m256i total = _mm256_setzero_si256();                                \
        __m256i ones = _mm256_setzero_si256();                                 \
        __m256i twos = _mm256_setzero_si256();                                 \
        __m256i fours = _mm256_setzero_si256();                                \
        __m256i eights = _mm256_setzero_si256();                               \
        __m256i sixteens = _mm256_setzero_si256();                             \
        __m256i twosA, twosB, foursA, foursB, eightsA, eightsB;                \
        __m256i A1, A2;                                                        \
        const uint64_t limit = size - size % 16;                               \
        uint64_t i = 0;                                                        \
        for (; i < limit; i += 16) {                                           \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i),                  \
                               _mm256_lddqu_si256(data2 + i));                 \
            _mm256_storeu_si256(out + i, A1);                                  \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 1),              \
                               _mm256_lddqu_si256(data2 + i + 1));             \
            _mm256_storeu_si256(out + i + 1, A2);                              \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 2),              \
                               _mm256_lddqu_si256(data2 + i + 2));             \
            _mm256_storeu_si256(out + i + 2, A1);                              \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 3),              \
                               _mm256_lddqu_si256(data2 + i + 3));             \
            _mm256_storeu_si256(out + i + 3, A2);                              \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursA, &twos, twos, twosA, twosB);                           \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 4),              \
                               _mm256_lddqu_si256(data2 + i + 4));             \
            _mm256_storeu_si256(out + i + 4, A1);                              \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 5),              \
                               _mm256_lddqu_si256(data2 + i + 5));             \
            _mm256_storeu_si256(out + i + 5, A2);                              \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 6),              \
                               _mm256_lddqu_si256(data2 + i + 6));             \
            _mm256_storeu_si256(out + i + 6, A1);                              \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 7),              \
                               _mm256_lddqu_si256(data2 + i + 7));             \
            _mm256_storeu_si256(out + i + 7, A2);                              \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursB, &twos, twos, twosA, twosB);                           \
            CSA(&eightsA, &fours, fours, foursA, foursB);                      \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 8),              \
                               _mm256_lddqu_si256(data2 + i + 8));             \
            _mm256_storeu_si256(out + i + 8, A1);                              \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 9),              \
                               _mm256_lddqu_si256(data2 + i + 9));             \
            _mm256_storeu_si256(out + i + 9, A2);                              \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 10),             \
                               _mm256_lddqu_si256(data2 + i + 10));            \
            _mm256_storeu_si256(out + i + 10, A1);                             \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 11),             \
                               _mm256_lddqu_si256(data2 + i + 11));            \
            _mm256_storeu_si256(out + i + 11, A2);                             \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursA, &twos, twos, twosA, twosB);                           \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 12),             \
                               _mm256_lddqu_si256(data2 + i + 12));            \
            _mm256_storeu_si256(out + i + 12, A1);                             \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 13),             \
                               _mm256_lddqu_si256(data2 + i + 13));            \
            _mm256_storeu_si256(out + i + 13, A2);                             \
            CSA(&twosA, &ones, ones, A1, A2);                                  \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 14),             \
                               _mm256_lddqu_si256(data2 + i + 14));            \
            _mm256_storeu_si256(out + i + 14, A1);                             \
            A2 = avx_intrinsic(_mm256_lddqu_si256(data1 + i + 15),             \
                               _mm256_lddqu_si256(data2 + i + 15));            \
            _mm256_storeu_si256(out + i + 15, A2);                             \
            CSA(&twosB, &ones, ones, A1, A2);                                  \
            CSA(&foursB, &twos, twos, twosA, twosB);                           \
            CSA(&eightsB, &fours, fours, foursA, foursB);                      \
            CSA(&sixteens, &eights, eights, eightsA, eightsB);                 \
            total = _mm256_add_epi64(total, popcount256(sixteens));            \
        }                                                                      \
        total = _mm256_slli_epi64(total, 4);                                   \
        total = _mm256_add_epi64(total,                                        \
                                 _mm256_slli_epi64(popcount256(eights), 3));   \
        total =                                                                \
            _mm256_add_epi64(total, _mm256_slli_epi64(popcount256(fours), 2)); \
        total =                                                                \
            _mm256_add_epi64(total, _mm256_slli_epi64(popcount256(twos), 1));  \
        total = _mm256_add_epi64(total, popcount256(ones));                    \
        for (; i < size; i++) {                                                \
            A1 = avx_intrinsic(_mm256_lddqu_si256(data1 + i),                  \
                               _mm256_lddqu_si256(data2 + i));                 \
            _mm256_storeu_si256(out + i, A1);                                  \
            total = _mm256_add_epi64(total, popcount256(A1));                  \
        }                                                                      \
        return (uint64_t)(_mm256_extract_epi64(total, 0)) +                    \
               (uint64_t)(_mm256_extract_epi64(total, 1)) +                    \
               (uint64_t)(_mm256_extract_epi64(total, 2)) +                    \
               (uint64_t)(_mm256_extract_epi64(total, 3));                     \
    }

CROARING_TARGET_AVX2
CROARING_AVXPOPCNTFNC(or, _mm256_or_si256)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVXPOPCNTFNC(union, _mm256_or_si256)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVXPOPCNTFNC(and, _mm256_and_si256)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVXPOPCNTFNC(intersection, _mm256_and_si256)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVXPOPCNTFNC(xor, _mm256_xor_si256)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVXPOPCNTFNC(andnot, _mm256_andnot_si256)
CROARING_UNTARGET_AVX2

#define VPOPCNT_AND_ADD(ptr, i, accu)                                  \
    const __m512i v##i = _mm512_loadu_si512((const __m512i *)ptr + i); \
    const __m512i p##i = _mm512_popcnt_epi64(v##i);                    \
    accu = _mm512_add_epi64(accu, p##i);

#if CROARING_COMPILER_SUPPORTS_AVX512
CROARING_TARGET_AVX512
static inline uint64_t sum_epu64_256(const __m256i v) {
    return (uint64_t)(_mm256_extract_epi64(v, 0)) +
           (uint64_t)(_mm256_extract_epi64(v, 1)) +
           (uint64_t)(_mm256_extract_epi64(v, 2)) +
           (uint64_t)(_mm256_extract_epi64(v, 3));
}

static inline uint64_t simd_sum_epu64(const __m512i v) {
    __m256i lo = _mm512_extracti64x4_epi64(v, 0);
    __m256i hi = _mm512_extracti64x4_epi64(v, 1);

    return sum_epu64_256(lo) + sum_epu64_256(hi);
}

static inline uint64_t avx512_vpopcount(const __m512i *data,
                                        const uint64_t size) {
    const uint64_t limit = size - size % 4;
    __m512i total = _mm512_setzero_si512();
    uint64_t i = 0;

    for (; i < limit; i += 4) {
        VPOPCNT_AND_ADD(data + i, 0, total);
        VPOPCNT_AND_ADD(data + i, 1, total);
        VPOPCNT_AND_ADD(data + i, 2, total);
        VPOPCNT_AND_ADD(data + i, 3, total);
    }

    for (; i < size; i++) {
        total = _mm512_add_epi64(
            total, _mm512_popcnt_epi64(_mm512_loadu_si512(data + i)));
    }

    return simd_sum_epu64(total);
}
CROARING_UNTARGET_AVX512
#endif

#define CROARING_AVXPOPCNTFNC512(opname, avx_intrinsic)                       \
    static inline uint64_t avx512_harley_seal_popcount512_##opname(           \
        const __m512i *data1, const __m512i *data2, const uint64_t size) {    \
        __m512i total = _mm512_setzero_si512();                               \
        const uint64_t limit = size - size % 4;                               \
        uint64_t i = 0;                                                       \
        for (; i < limit; i += 4) {                                           \
            __m512i a1 = avx_intrinsic(_mm512_loadu_si512(data1 + i),         \
                                       _mm512_loadu_si512(data2 + i));        \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a1));         \
            __m512i a2 = avx_intrinsic(_mm512_loadu_si512(data1 + i + 1),     \
                                       _mm512_loadu_si512(data2 + i + 1));    \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a2));         \
            __m512i a3 = avx_intrinsic(_mm512_loadu_si512(data1 + i + 2),     \
                                       _mm512_loadu_si512(data2 + i + 2));    \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a3));         \
            __m512i a4 = avx_intrinsic(_mm512_loadu_si512(data1 + i + 3),     \
                                       _mm512_loadu_si512(data2 + i + 3));    \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a4));         \
        }                                                                     \
        for (; i < size; i++) {                                               \
            __m512i a = avx_intrinsic(_mm512_loadu_si512(data1 + i),          \
                                      _mm512_loadu_si512(data2 + i));         \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a));          \
        }                                                                     \
        return simd_sum_epu64(total);                                         \
    }                                                                         \
    static inline uint64_t avx512_harley_seal_popcount512andstore_##opname(   \
        const __m512i *__restrict__ data1, const __m512i *__restrict__ data2, \
        __m512i *__restrict__ out, const uint64_t size) {                     \
        __m512i total = _mm512_setzero_si512();                               \
        const uint64_t limit = size - size % 4;                               \
        uint64_t i = 0;                                                       \
        for (; i < limit; i += 4) {                                           \
            __m512i a1 = avx_intrinsic(_mm512_loadu_si512(data1 + i),         \
                                       _mm512_loadu_si512(data2 + i));        \
            _mm512_storeu_si512(out + i, a1);                                 \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a1));         \
            __m512i a2 = avx_intrinsic(_mm512_loadu_si512(data1 + i + 1),     \
                                       _mm512_loadu_si512(data2 + i + 1));    \
            _mm512_storeu_si512(out + i + 1, a2);                             \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a2));         \
            __m512i a3 = avx_intrinsic(_mm512_loadu_si512(data1 + i + 2),     \
                                       _mm512_loadu_si512(data2 + i + 2));    \
            _mm512_storeu_si512(out + i + 2, a3);                             \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a3));         \
            __m512i a4 = avx_intrinsic(_mm512_loadu_si512(data1 + i + 3),     \
                                       _mm512_loadu_si512(data2 + i + 3));    \
            _mm512_storeu_si512(out + i + 3, a4);                             \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a4));         \
        }                                                                     \
        for (; i < size; i++) {                                               \
            __m512i a = avx_intrinsic(_mm512_loadu_si512(data1 + i),          \
                                      _mm512_loadu_si512(data2 + i));         \
            _mm512_storeu_si512(out + i, a);                                  \
            total = _mm512_add_epi64(total, _mm512_popcnt_epi64(a));          \
        }                                                                     \
        return simd_sum_epu64(total);                                         \
    }

#if CROARING_COMPILER_SUPPORTS_AVX512
CROARING_TARGET_AVX512
CROARING_AVXPOPCNTFNC512(or, _mm512_or_si512)
CROARING_AVXPOPCNTFNC512(union, _mm512_or_si512)
CROARING_AVXPOPCNTFNC512(and, _mm512_and_si512)
CROARING_AVXPOPCNTFNC512(intersection, _mm512_and_si512)
CROARING_AVXPOPCNTFNC512(xor, _mm512_xor_si512)
CROARING_AVXPOPCNTFNC512(andnot, _mm512_andnot_si512)
CROARING_UNTARGET_AVX512
#endif
/***
 * END Harley-Seal popcount functions.
 */

#endif  // CROARING_IS_X64

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
