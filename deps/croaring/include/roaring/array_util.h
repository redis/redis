/*
 * array_util.h
 *
 * This header provides low-level utility routines for sorted arrays of
 * 16-bit integers, which are used heavily by CRoaring's array-based
 * containers and set-operation kernels. It includes search helpers, counting
 * helpers, and array intersection/difference primitives.
 *
 * Some of the routines also have SIMD-accelerated implementations on supported
 * platforms, allowing efficient operations on sorted integer arrays that form
 * the basis of sparse container processing.
 */
#ifndef CROARING_ARRAY_UTIL_H
#define CROARING_ARRAY_UTIL_H

#include <stddef.h>  // for size_t
#include <stdint.h>

#include <roaring/portability.h>

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
 *  Sorted-array search.
 *  Assumes that array is sorted, has logarithmic complexity.
 *  if the result is x, then:
 *     if ( x>0 )  you have array[x] = ikey
 *     if ( x<0 ) then inserting ikey at position -x-1 in array (insuring that
 * array[-x-1]=ikey) keys the array sorted.
 *
 * Adapted from array_container_contains: a SIMD-quad block-narrowing
 * search at gap=16 (Daniel Lemire,
 * https://lemire.me/blog/2026/04/27/you-can-beat-the-binary-search/)
 * followed by a scalar in-block scan that recovers the exact insertion
 * point required by the binarySearch contract.
 */
inline int32_t binarySearch(const uint16_t *array, int32_t lenarray,
                            uint16_t ikey) {
    const int32_t gap = 16;
    if (lenarray < gap) {
        for (int32_t j = 0; j < lenarray; j++) {
            if (array[j] >= ikey) {
                return (array[j] == ikey) ? j : -(j + 1);
            }
        }
        return -(lenarray + 1);
    }
    const int32_t num_blocks = lenarray / gap;
    int32_t base = 0;
    int32_t n = num_blocks;
    while (n > 3) {
        int32_t quarter = n >> 2;

        int32_t k1 = array[(base + quarter + 1) * gap - 1];
        int32_t k2 = array[(base + 2 * quarter + 1) * gap - 1];
        int32_t k3 = array[(base + 3 * quarter + 1) * gap - 1];

        int32_t c1 = (k1 < ikey);
        int32_t c2 = (k2 < ikey);
        int32_t c3 = (k3 < ikey);

        base += (c1 + c2 + c3) * quarter;
        n -= 3 * quarter;
    }
    while (n > 1) {
        int32_t half = n >> 1;
        base = (array[(base + half + 1) * gap - 1] < ikey) ? base + half : base;
        n -= half;
    }
    int32_t lo = (array[(base + 1) * gap - 1] < ikey) ? base + 1 : base;

    if (lo < num_blocks) {
        const int32_t start = lo * gap;
#if defined(CROARING_IS_X64)
        // SSE2: subs_epu16 yields zero where lane >= ikey. movemask of an
        // epi16 compare gives 2 bits per lane; ctz>>1 = lane index. Scan
        // the first 8 lanes first and exit early when they contain the
        // answer; otherwise the block-narrowing invariant guarantees the
        // second-half mask is non-zero.
        __m128i needle = _mm_set1_epi16((short)ikey);
        __m128i zero = _mm_setzero_si128();
        __m128i v0 = _mm_loadu_si128((const __m128i *)(array + start));
        __m128i ge0 = _mm_cmpeq_epi16(_mm_subs_epu16(needle, v0), zero);
        unsigned m0 = (unsigned)_mm_movemask_epi8(ge0);
        if (m0 != 0) {
            int32_t j = start + (int32_t)(roaring_trailing_zeroes(m0) >> 1);
            return (array[j] == ikey) ? j : -(j + 1);
        }
        __m128i v1 = _mm_loadu_si128((const __m128i *)(array + start + 8));
        __m128i ge1 = _mm_cmpeq_epi16(_mm_subs_epu16(needle, v1), zero);
        unsigned m1 = (unsigned)_mm_movemask_epi8(ge1);
        int32_t j = start + 8 + (int32_t)(roaring_trailing_zeroes(m1) >> 1);
        return (array[j] == ikey) ? j : -(j + 1);
#else
        const int32_t end = start + gap;
        for (int32_t j = start; j < end; j++) {
            if (array[j] >= ikey) {
                return (array[j] == ikey) ? j : -(j + 1);
            }
        }
        // Unreachable: the narrowing guarantees the last element of the
        // selected block is >= ikey.
        return -(end + 1);
#endif
    }

    for (int32_t j = num_blocks * gap; j < lenarray; j++) {
        if (array[j] >= ikey) {
            return (array[j] == ikey) ? j : -(j + 1);
        }
    }
    return -(lenarray + 1);
}

/**
 * Galloping search
 * Assumes that array is sorted, has logarithmic complexity.
 * if the result is x, then if x = length, you have that all values in array
 * between pos and length are smaller than min. otherwise returns the first
 * index x such that array[x] >= min.
 */
static inline int32_t advanceUntil(const uint16_t *array, int32_t pos,
                                   int32_t length, uint16_t min) {
    int32_t lower = pos + 1;

    if ((lower >= length) || (array[lower] >= min)) {
        return lower;
    }

    int32_t spansize = 1;

    while ((lower + spansize < length) && (array[lower + spansize] < min)) {
        spansize <<= 1;
    }
    int32_t upper = (lower + spansize < length) ? lower + spansize : length - 1;

    if (array[upper] == min) {
        return upper;
    }
    if (array[upper] < min) {
        // means
        // array
        // has no
        // item
        // >= min
        // pos = array.length;
        return length;
    }

    // we know that the next-smallest span was too small
    lower += (spansize >> 1);

    int32_t mid = 0;
    while (lower + 1 != upper) {
        mid = (lower + upper) >> 1;
        if (array[mid] == min) {
            return mid;
        } else if (array[mid] < min) {
            lower = mid;
        } else {
            upper = mid;
        }
    }
    return upper;
}

/**
 * Returns number of elements which are less than ikey.
 * Array elements must be unique and sorted.
 */
static inline int32_t count_less(const uint16_t *array, int32_t lenarray,
                                 uint16_t ikey) {
    if (lenarray == 0) return 0;
    int32_t pos = binarySearch(array, lenarray, ikey);
    return pos >= 0 ? pos : -(pos + 1);
}

/**
 * Returns number of elements which are greater than ikey.
 * Array elements must be unique and sorted.
 */
static inline int32_t count_greater(const uint16_t *array, int32_t lenarray,
                                    uint16_t ikey) {
    if (lenarray == 0) return 0;
    int32_t pos = binarySearch(array, lenarray, ikey);
    if (pos >= 0) {
        return lenarray - (pos + 1);
    } else {
        return lenarray - (-pos - 1);
    }
}

/**
 * From Schlegel et al., Fast Sorted-Set Intersection using SIMD Instructions
 * Optimized by D. Lemire on May 3rd 2013
 *
 * C should have capacity greater than the minimum of s_1 and s_b + 8
 * where 8 is sizeof(__m128i)/sizeof(uint16_t).
 */
int32_t intersect_vector16(const uint16_t *A, size_t s_a, const uint16_t *B,
                           size_t s_b, uint16_t *C);

int32_t intersect_vector16_inplace(uint16_t *A, size_t s_a, const uint16_t *B,
                                   size_t s_b);

/**
 * Take an array container and write it out to a 32-bit array, using base
 * as the offset.
 */
int array_container_to_uint32_array_vector16(void *vout, const uint16_t *array,
                                             size_t cardinality, uint32_t base);
#if CROARING_COMPILER_SUPPORTS_AVX512
int avx512_array_container_to_uint32_array(void *vout, const uint16_t *array,
                                           size_t cardinality, uint32_t base);
#endif
/**
 * Compute the cardinality of the intersection using SSE4 instructions
 */
int32_t intersect_vector16_cardinality(const uint16_t *A, size_t s_a,
                                       const uint16_t *B, size_t s_b);

/* Computes the intersection between one small and one large set of uint16_t.
 * Stores the result into buffer and return the number of elements. */
int32_t intersect_skewed_uint16(const uint16_t *smallarray, size_t size_s,
                                const uint16_t *largearray, size_t size_l,
                                uint16_t *buffer);

/* Computes the size of the intersection between one small and one large set of
 * uint16_t. */
int32_t intersect_skewed_uint16_cardinality(const uint16_t *smallarray,
                                            size_t size_s,
                                            const uint16_t *largearray,
                                            size_t size_l);

/* Check whether the size of the intersection between one small and one large
 * set of uint16_t is non-zero. */
bool intersect_skewed_uint16_nonempty(const uint16_t *smallarray, size_t size_s,
                                      const uint16_t *largearray,
                                      size_t size_l);
/**
 * Generic intersection function.
 */
int32_t intersect_uint16(const uint16_t *A, const size_t lenA,
                         const uint16_t *B, const size_t lenB, uint16_t *out);
/**
 * Compute the size of the intersection (generic).
 */
int32_t intersect_uint16_cardinality(const uint16_t *A, const size_t lenA,
                                     const uint16_t *B, const size_t lenB);

/**
 * Checking whether the size of the intersection  is non-zero.
 */
bool intersect_uint16_nonempty(const uint16_t *A, const size_t lenA,
                               const uint16_t *B, const size_t lenB);
/**
 * Generic union function.
 */
size_t union_uint16(const uint16_t *set_1, size_t size_1, const uint16_t *set_2,
                    size_t size_2, uint16_t *buffer);

/**
 * Generic XOR function.
 */
int32_t xor_uint16(const uint16_t *array_1, int32_t card_1,
                   const uint16_t *array_2, int32_t card_2, uint16_t *out);

/**
 * Generic difference function (ANDNOT).
 */
int difference_uint16(const uint16_t *a1, int length1, const uint16_t *a2,
                      int length2, uint16_t *a_out);

/**
 * Generic intersection function.
 */
size_t intersection_uint32(const uint32_t *A, const size_t lenA,
                           const uint32_t *B, const size_t lenB, uint32_t *out);

/**
 * Generic intersection function, returns just the cardinality.
 */
size_t intersection_uint32_card(const uint32_t *A, const size_t lenA,
                                const uint32_t *B, const size_t lenB);

/**
 * Generic union function.
 */
size_t union_uint32(const uint32_t *set_1, size_t size_1, const uint32_t *set_2,
                    size_t size_2, uint32_t *buffer);

/**
 * A fast SSE-based union function.
 */
uint32_t union_vector16(const uint16_t *set_1, uint32_t size_1,
                        const uint16_t *set_2, uint32_t size_2,
                        uint16_t *buffer);
/**
 * A fast SSE-based XOR function.
 */
uint32_t xor_vector16(const uint16_t *array1, uint32_t length1,
                      const uint16_t *array2, uint32_t length2,
                      uint16_t *output);

/**
 * A fast SSE-based difference function.
 */
int32_t difference_vector16(const uint16_t *A, size_t s_a, const uint16_t *B,
                            size_t s_b, uint16_t *C);

/**
 * Generic union function, returns just the cardinality.
 */
size_t union_uint32_card(const uint32_t *set_1, size_t size_1,
                         const uint32_t *set_2, size_t size_2);

/**
 * combines union_uint16 and  union_vector16 optimally
 */
size_t fast_union_uint16(const uint16_t *set_1, size_t size_1,
                         const uint16_t *set_2, size_t size_2,
                         uint16_t *buffer);

bool memequals(const void *s1, const void *s2, size_t n);

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
