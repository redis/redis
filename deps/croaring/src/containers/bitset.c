/*
 * bitset.c
 *
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <roaring/bitset_util.h>
#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/memory.h>
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

extern inline int bitset_container_cardinality(
    const bitset_container_t *bitset);
extern inline void bitset_container_set(bitset_container_t *bitset,
                                        uint16_t pos);
// unused at this time:
// extern inline void bitset_container_unset(bitset_container_t *bitset,
// uint16_t pos);
extern inline bool bitset_container_get(const bitset_container_t *bitset,
                                        uint16_t pos);
extern inline int32_t bitset_container_serialized_size_in_bytes(void);
extern inline bool bitset_container_add(bitset_container_t *bitset,
                                        uint16_t pos);
extern inline bool bitset_container_remove(bitset_container_t *bitset,
                                           uint16_t pos);
extern inline bool bitset_container_contains(const bitset_container_t *bitset,
                                             uint16_t pos);

void bitset_container_clear(bitset_container_t *bitset) {
    memset(bitset->words, 0, sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    bitset->cardinality = 0;
}

void bitset_container_set_all(bitset_container_t *bitset) {
    memset(bitset->words, INT64_C(-1),
           sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    bitset->cardinality = (1 << 16);
}

/* Create a new bitset. Return NULL in case of failure. */
bitset_container_t *bitset_container_create(void) {
    bitset_container_t *bitset =
        (bitset_container_t *)roaring_malloc(sizeof(bitset_container_t));

    if (!bitset) {
        return NULL;
    }

    size_t align_size = 32;
#if CROARING_IS_X64
    int support = croaring_hardware_support();
    if (support & ROARING_SUPPORTS_AVX512) {
        // sizeof(__m512i) == 64
        align_size = 64;
    } else {
        // sizeof(__m256i) == 32
        align_size = 32;
    }
#endif
    bitset->words = (uint64_t *)roaring_aligned_malloc(
        align_size, sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    if (!bitset->words) {
        roaring_free(bitset);
        return NULL;
    }
    bitset_container_clear(bitset);
    return bitset;
}

/* Copy one container into another. We assume that they are distinct. */
void bitset_container_copy(const bitset_container_t *source,
                           bitset_container_t *dest) {
    dest->cardinality = source->cardinality;
    memcpy(dest->words, source->words,
           sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
}

void bitset_container_add_from_range(bitset_container_t *bitset, uint32_t min,
                                     uint32_t max, uint16_t step) {
    if (step == 0) return;   // refuse to crash
    if ((64 % step) == 0) {  // step divides 64
        uint64_t mask = 0;   // construct the repeated mask
        for (uint32_t value = (min % step); value < 64; value += step) {
            mask |= ((uint64_t)1 << value);
        }
        uint32_t firstword = min / 64;
        uint32_t endword = (max - 1) / 64;
        bitset->cardinality = (max - min + step - 1) / step;
        if (firstword == endword) {
            bitset->words[firstword] |=
                mask & (((~UINT64_C(0)) << (min % 64)) &
                        ((~UINT64_C(0)) >> ((~max + 1) % 64)));
            return;
        }
        bitset->words[firstword] = mask & ((~UINT64_C(0)) << (min % 64));
        for (uint32_t i = firstword + 1; i < endword; i++)
            bitset->words[i] = mask;
        bitset->words[endword] = mask & ((~UINT64_C(0)) >> ((~max + 1) % 64));
    } else {
        for (uint32_t value = min; value < max; value += step) {
            bitset_container_add(bitset, value);
        }
    }
}

/* Free memory. */
void bitset_container_free(bitset_container_t *bitset) {
    if (bitset == NULL) return;
    roaring_aligned_free(bitset->words);
    roaring_free(bitset);
}

/* duplicate container. */
CROARING_ALLOW_UNALIGNED
bitset_container_t *bitset_container_clone(const bitset_container_t *src) {
    bitset_container_t *bitset =
        (bitset_container_t *)roaring_malloc(sizeof(bitset_container_t));

    if (!bitset) {
        return NULL;
    }

    size_t align_size = 32;
#if CROARING_IS_X64
    if (croaring_hardware_support() & ROARING_SUPPORTS_AVX512) {
        // sizeof(__m512i) == 64
        align_size = 64;
    } else {
        // sizeof(__m256i) == 32
        align_size = 32;
    }
#endif
    bitset->words = (uint64_t *)roaring_aligned_malloc(
        align_size, sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    if (!bitset->words) {
        roaring_free(bitset);
        return NULL;
    }
    bitset->cardinality = src->cardinality;
    memcpy(bitset->words, src->words,
           sizeof(uint64_t) * BITSET_CONTAINER_SIZE_IN_WORDS);
    return bitset;
}

void bitset_container_offset(const bitset_container_t *c, container_t **loc,
                             container_t **hic, uint16_t offset) {
    bitset_container_t *bc = NULL;
    uint64_t val;
    uint16_t b, i, end;

    b = offset >> 6;
    i = offset % 64;
    end = 1024 - b;

    if (loc != NULL) {
        bc = bitset_container_create();
        if (i == 0) {
            memcpy(bc->words + b, c->words, 8 * end);
        } else {
            bc->words[b] = c->words[0] << i;
            for (uint32_t k = 1; k < end; ++k) {
                val = c->words[k] << i;
                val |= c->words[k - 1] >> (64 - i);
                bc->words[b + k] = val;
            }
        }

        bc->cardinality = bitset_container_compute_cardinality(bc);
        if (bc->cardinality != 0) {
            *loc = bc;
        }
        if (bc->cardinality == c->cardinality) {
            return;
        }
    }

    if (hic == NULL) {
        // Both hic and loc can't be NULL, so bc is never NULL here
        if (bc->cardinality == 0) {
            bitset_container_free(bc);
        }
        return;
    }

    if (bc == NULL || bc->cardinality != 0) {
        bc = bitset_container_create();
    }

    if (i == 0) {
        memcpy(bc->words, c->words + end, 8 * b);
    } else {
        for (uint32_t k = end; k < 1024; ++k) {
            val = c->words[k] << i;
            val |= c->words[k - 1] >> (64 - i);
            bc->words[k - end] = val;
        }
        bc->words[b] = c->words[1023] >> (64 - i);
    }

    bc->cardinality = bitset_container_compute_cardinality(bc);
    if (bc->cardinality == 0) {
        bitset_container_free(bc);
        return;
    }
    *hic = bc;
}

void bitset_container_set_range(bitset_container_t *bitset, uint32_t begin,
                                uint32_t end) {
    bitset_set_range(bitset->words, begin, end);
    bitset->cardinality =
        bitset_container_compute_cardinality(bitset);  // could be smarter
}

bool bitset_container_intersect(const bitset_container_t *src_1,
                                const bitset_container_t *src_2) {
    // could vectorize, but this is probably already quite fast in practice
    const uint64_t *__restrict__ words_1 = src_1->words;
    const uint64_t *__restrict__ words_2 = src_2->words;
    for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i++) {
        if ((words_1[i] & words_2[i]) != 0) return true;
    }
    return false;
}

#if CROARING_IS_X64
#ifndef CROARING_WORDS_IN_AVX2_REG
#define CROARING_WORDS_IN_AVX2_REG sizeof(__m256i) / sizeof(uint64_t)
#endif
#ifndef WORDS_IN_AVX512_REG
#define WORDS_IN_AVX512_REG sizeof(__m512i) / sizeof(uint64_t)
#endif
/* Get the number of bits set (force computation) */
static inline int _scalar_bitset_container_compute_cardinality(
    const bitset_container_t *bitset) {
    const uint64_t *words = bitset->words;
    int32_t sum = 0;
    for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 4) {
        sum += roaring_hamming(words[i]);
        sum += roaring_hamming(words[i + 1]);
        sum += roaring_hamming(words[i + 2]);
        sum += roaring_hamming(words[i + 3]);
    }
    return sum;
}
/* Get the number of bits set (force computation) */
int bitset_container_compute_cardinality(const bitset_container_t *bitset) {
    int support = croaring_hardware_support();
#if CROARING_COMPILER_SUPPORTS_AVX512
    if (support & ROARING_SUPPORTS_AVX512) {
        return (int)avx512_vpopcount(
            (const __m512i *)bitset->words,
            BITSET_CONTAINER_SIZE_IN_WORDS / (WORDS_IN_AVX512_REG));
    } else
#endif  // CROARING_COMPILER_SUPPORTS_AVX512
        if (support & ROARING_SUPPORTS_AVX2) {
            return (int)avx2_harley_seal_popcount256(
                (const __m256i *)bitset->words,
                BITSET_CONTAINER_SIZE_IN_WORDS / (CROARING_WORDS_IN_AVX2_REG));
        } else {
            return _scalar_bitset_container_compute_cardinality(bitset);
        }
}

#elif defined(CROARING_USENEON)
int bitset_container_compute_cardinality(const bitset_container_t *bitset) {
    uint16x8_t n0 = vdupq_n_u16(0);
    uint16x8_t n1 = vdupq_n_u16(0);
    uint16x8_t n2 = vdupq_n_u16(0);
    uint16x8_t n3 = vdupq_n_u16(0);
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 8) {
        uint64x2_t c0 = vld1q_u64(&bitset->words[i + 0]);
        n0 = vaddq_u16(n0, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c0))));
        uint64x2_t c1 = vld1q_u64(&bitset->words[i + 2]);
        n1 = vaddq_u16(n1, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c1))));
        uint64x2_t c2 = vld1q_u64(&bitset->words[i + 4]);
        n2 = vaddq_u16(n2, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c2))));
        uint64x2_t c3 = vld1q_u64(&bitset->words[i + 6]);
        n3 = vaddq_u16(n3, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c3))));
    }
    uint64x2_t n = vdupq_n_u64(0);
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n0)));
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n1)));
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n2)));
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n3)));
    return vgetq_lane_u64(n, 0) + vgetq_lane_u64(n, 1);
}

#else  // CROARING_IS_X64

/* Get the number of bits set (force computation) */
int bitset_container_compute_cardinality(const bitset_container_t *bitset) {
    const uint64_t *words = bitset->words;
    int32_t sum = 0;
    for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 4) {
        sum += roaring_hamming(words[i]);
        sum += roaring_hamming(words[i + 1]);
        sum += roaring_hamming(words[i + 2]);
        sum += roaring_hamming(words[i + 3]);
    }
    return sum;
}

#endif  // CROARING_IS_X64

#if CROARING_IS_X64

#define CROARING_BITSET_CONTAINER_FN_REPEAT 8
#ifndef WORDS_IN_AVX512_REG
#define WORDS_IN_AVX512_REG sizeof(__m512i) / sizeof(uint64_t)
#endif  // WORDS_IN_AVX512_REG

/* Computes a binary operation (eg union) on bitset1 and bitset2 and write the
   result to bitsetout */
// clang-format off
#define CROARING_AVX512_BITSET_CONTAINER_FN1(before, opname, opsymbol, avx_intrinsic,   \
                                neon_intrinsic, after)                         \
  static inline int _avx512_bitset_container_##opname##_nocard(                \
      const bitset_container_t *src_1, const bitset_container_t *src_2,        \
      bitset_container_t *dst) {                                               \
    const uint8_t * __restrict__ words_1 = (const uint8_t *)src_1->words;      \
    const uint8_t * __restrict__ words_2 = (const uint8_t *)src_2->words;      \
    /* not using the blocking optimization for some reason*/                   \
    uint8_t *out = (uint8_t*)dst->words;                                       \
    const int innerloop = 8;                                                   \
    for (size_t i = 0;                                                         \
        i < BITSET_CONTAINER_SIZE_IN_WORDS / (WORDS_IN_AVX512_REG);            \
                                                         i+=innerloop) {       \
        __m512i A1, A2, AO;                                                    \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1));                   \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2));                   \
        AO = avx_intrinsic(A2, A1);                                            \
        _mm512_storeu_si512((__m512i *)out, AO);                               \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1 + 64));              \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2 + 64));              \
        AO = avx_intrinsic(A2, A1);                                            \
        _mm512_storeu_si512((__m512i *)(out+64), AO);                          \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1 + 128));             \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2 + 128));             \
        AO = avx_intrinsic(A2, A1);                                            \
        _mm512_storeu_si512((__m512i *)(out+128), AO);                         \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1 + 192));             \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2 + 192));             \
        AO = avx_intrinsic(A2, A1);                                            \
        _mm512_storeu_si512((__m512i *)(out+192), AO);                         \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1 + 256));             \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2 + 256));             \
        AO = avx_intrinsic(A2, A1);                                            \
        _mm512_storeu_si512((__m512i *)(out+256), AO);                         \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1 + 320));             \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2 + 320));             \
        AO = avx_intrinsic(A2, A1);                                            \
        _mm512_storeu_si512((__m512i *)(out+320), AO);                         \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1 + 384));             \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2 + 384));             \
        AO = avx_intrinsic(A2, A1);                                            \
        _mm512_storeu_si512((__m512i *)(out+384), AO);                         \
        A1 = _mm512_loadu_si512((const __m512i *)(words_1 + 448));             \
        A2 = _mm512_loadu_si512((const __m512i *)(words_2 + 448));             \
        AO = avx_intrinsic(A2, A1);                                     \
        _mm512_storeu_si512((__m512i *)(out+448), AO);                  \
        out+=512;                                                       \
        words_1 += 512;                                                 \
        words_2 += 512;                                                 \
    }                                                                   \
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;                      \
    return dst->cardinality;                                            \
  }

#define CROARING_AVX512_BITSET_CONTAINER_FN2(before, opname, opsymbol, avx_intrinsic,           \
                                neon_intrinsic, after)                                 \
  /* next, a version that updates cardinality*/                                        \
  static inline int _avx512_bitset_container_##opname(const bitset_container_t *src_1, \
                                      const bitset_container_t *src_2,                 \
                                      bitset_container_t *dst) {                       \
    const __m512i * __restrict__ words_1 = (const __m512i *) src_1->words;             \
    const __m512i * __restrict__ words_2 = (const __m512i *) src_2->words;             \
    __m512i *out = (__m512i *) dst->words;                                             \
    dst->cardinality = (int32_t)avx512_harley_seal_popcount512andstore_##opname(words_2,\
				words_1, out,BITSET_CONTAINER_SIZE_IN_WORDS / (WORDS_IN_AVX512_REG));           \
    return dst->cardinality;                                                            \
  }

#define CROARING_AVX512_BITSET_CONTAINER_FN3(before, opname, opsymbol, avx_intrinsic,            \
                                neon_intrinsic, after)                                  \
  /* next, a version that just computes the cardinality*/                               \
  static inline int _avx512_bitset_container_##opname##_justcard(                       \
      const bitset_container_t *src_1, const bitset_container_t *src_2) {               \
    const __m512i * __restrict__ data1 = (const __m512i *) src_1->words;                \
    const __m512i * __restrict__ data2 = (const __m512i *) src_2->words;                \
    return (int)avx512_harley_seal_popcount512_##opname(data2,                          \
				data1, BITSET_CONTAINER_SIZE_IN_WORDS / (WORDS_IN_AVX512_REG));                 \
  }


// we duplicate the function because other containers use the "or" term, makes API more consistent
#if CROARING_COMPILER_SUPPORTS_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX512, or,    |, _mm512_or_si512, vorrq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX512, union, |, _mm512_or_si512, vorrq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX512, and,          &, _mm512_and_si512, vandq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX512, intersection, &, _mm512_and_si512, vandq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX512, xor,    ^,  _mm512_xor_si512,    veorq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX512, andnot, &~, _mm512_andnot_si512, vbicq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

// we duplicate the function because other containers use the "or" term, makes API more consistent
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX512, or,    |, _mm512_or_si512, vorrq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX512, union, |, _mm512_or_si512, vorrq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX512, and,          &, _mm512_and_si512, vandq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX512, intersection, &, _mm512_and_si512, vandq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX512, xor,    ^,  _mm512_xor_si512,    veorq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX512, andnot, &~, _mm512_andnot_si512, vbicq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

// we duplicate the function because other containers use the "or" term, makes API more consistent
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX512, or,    |, _mm512_or_si512, vorrq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX512, union, |, _mm512_or_si512, vorrq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX512, and,          &, _mm512_and_si512, vandq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX512, intersection, &, _mm512_and_si512, vandq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512

CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX512, xor,    ^,  _mm512_xor_si512,    veorq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
CROARING_TARGET_AVX512
CROARING_AVX512_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX512, andnot, &~, _mm512_andnot_si512, vbicq_u64, CROARING_UNTARGET_AVX512)
CROARING_UNTARGET_AVX512
#endif // CROARING_COMPILER_SUPPORTS_AVX512

#ifndef CROARING_WORDS_IN_AVX2_REG
#define CROARING_WORDS_IN_AVX2_REG sizeof(__m256i) / sizeof(uint64_t)
#endif // CROARING_WORDS_IN_AVX2_REG
#define CROARING_LOOP_SIZE                    \
    BITSET_CONTAINER_SIZE_IN_WORDS / \
        ((CROARING_WORDS_IN_AVX2_REG)*CROARING_BITSET_CONTAINER_FN_REPEAT)

/* Computes a binary operation (eg union) on bitset1 and bitset2 and write the
   result to bitsetout */
// clang-format off
#define CROARING_AVX_BITSET_CONTAINER_FN1(before, opname, opsymbol, avx_intrinsic,               \
                                neon_intrinsic, after)                                \
  static inline int _avx2_bitset_container_##opname##_nocard(                                \
      const bitset_container_t *src_1, const bitset_container_t *src_2,        \
      bitset_container_t *dst) {                                               \
    const uint8_t *__restrict__ words_1 = (const uint8_t *)src_1->words;       \
    const uint8_t *__restrict__ words_2 = (const uint8_t *)src_2->words;       \
    /* not using the blocking optimization for some reason*/                   \
    uint8_t *out = (uint8_t *)dst->words;                                      \
    const int innerloop = 8;                                                   \
    for (size_t i = 0;                                                         \
         i < BITSET_CONTAINER_SIZE_IN_WORDS / (CROARING_WORDS_IN_AVX2_REG);             \
         i += innerloop) {                                                     \
      __m256i A1, A2, AO;                                                      \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1));                     \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2));                     \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)out, AO);                                 \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1 + 32));                \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2 + 32));                \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)(out + 32), AO);                          \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1 + 64));                \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2 + 64));                \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)(out + 64), AO);                          \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1 + 96));                \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2 + 96));                \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)(out + 96), AO);                          \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1 + 128));               \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2 + 128));               \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)(out + 128), AO);                         \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1 + 160));               \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2 + 160));               \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)(out + 160), AO);                         \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1 + 192));               \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2 + 192));               \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)(out + 192), AO);                         \
      A1 = _mm256_lddqu_si256((const __m256i *)(words_1 + 224));               \
      A2 = _mm256_lddqu_si256((const __m256i *)(words_2 + 224));               \
      AO = avx_intrinsic(A2, A1);                                              \
      _mm256_storeu_si256((__m256i *)(out + 224), AO);                         \
      out += 256;                                                              \
      words_1 += 256;                                                          \
      words_2 += 256;                                                          \
    }                                                                          \
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;                             \
    return dst->cardinality;                                                   \
  }

#define CROARING_AVX_BITSET_CONTAINER_FN2(before, opname, opsymbol, avx_intrinsic,               \
                                neon_intrinsic, after)                                \
  /* next, a version that updates cardinality*/                                \
  static inline int _avx2_bitset_container_##opname(const bitset_container_t *src_1,         \
                                      const bitset_container_t *src_2,         \
                                      bitset_container_t *dst) {               \
    const __m256i *__restrict__ words_1 = (const __m256i *)src_1->words;       \
    const __m256i *__restrict__ words_2 = (const __m256i *)src_2->words;       \
    __m256i *out = (__m256i *)dst->words;                                      \
    dst->cardinality = (int32_t)avx2_harley_seal_popcount256andstore_##opname( \
        words_2, words_1, out,                                                 \
        BITSET_CONTAINER_SIZE_IN_WORDS / (CROARING_WORDS_IN_AVX2_REG));                 \
    return dst->cardinality;                                                   \
  }                                                                            \

#define CROARING_AVX_BITSET_CONTAINER_FN3(before, opname, opsymbol, avx_intrinsic,               \
                                neon_intrinsic, after)                                \
  /* next, a version that just computes the cardinality*/                      \
  static inline int _avx2_bitset_container_##opname##_justcard(                              \
      const bitset_container_t *src_1, const bitset_container_t *src_2) {      \
    const __m256i *__restrict__ data1 = (const __m256i *)src_1->words;         \
    const __m256i *__restrict__ data2 = (const __m256i *)src_2->words;         \
    return (int)avx2_harley_seal_popcount256_##opname(                         \
        data2, data1, BITSET_CONTAINER_SIZE_IN_WORDS / (CROARING_WORDS_IN_AVX2_REG));   \
  }


// we duplicate the function because other containers use the "or" term, makes API more consistent
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX2, or,    |, _mm256_or_si256, vorrq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX2, union, |, _mm256_or_si256, vorrq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX2, and,          &, _mm256_and_si256, vandq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX2, intersection, &, _mm256_and_si256, vandq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX2, xor,    ^,  _mm256_xor_si256,    veorq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN1(CROARING_TARGET_AVX2, andnot, &~, _mm256_andnot_si256, vbicq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

// we duplicate the function because other containers use the "or" term, makes API more consistent
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX2, or,    |, _mm256_or_si256, vorrq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX2, union, |, _mm256_or_si256, vorrq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX2, and,          &, _mm256_and_si256, vandq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX2, intersection, &, _mm256_and_si256, vandq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX2, xor,    ^,  _mm256_xor_si256,    veorq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN2(CROARING_TARGET_AVX2, andnot, &~, _mm256_andnot_si256, vbicq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

// we duplicate the function because other containers use the "or" term, makes API more consistent
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX2, or,    |, _mm256_or_si256, vorrq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX2, union, |, _mm256_or_si256, vorrq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX2, and,          &, _mm256_and_si256, vandq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX2, intersection, &, _mm256_and_si256, vandq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2

CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX2, xor,    ^,  _mm256_xor_si256,    veorq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2
CROARING_TARGET_AVX2
CROARING_AVX_BITSET_CONTAINER_FN3(CROARING_TARGET_AVX2, andnot, &~, _mm256_andnot_si256, vbicq_u64, CROARING_UNTARGET_AVX2)
CROARING_UNTARGET_AVX2


#define SCALAR_BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic,            \
                                   neon_intrinsic)                             \
  static inline int _scalar_bitset_container_##opname(const bitset_container_t *src_1,       \
                                        const bitset_container_t *src_2,       \
                                        bitset_container_t *dst) {             \
    const uint64_t *__restrict__ words_1 = src_1->words;                       \
    const uint64_t *__restrict__ words_2 = src_2->words;                       \
    uint64_t *out = dst->words;                                                \
    int32_t sum = 0;                                                           \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 2) {           \
      const uint64_t word_1 = (words_1[i])opsymbol(words_2[i]),                \
                     word_2 = (words_1[i + 1]) opsymbol(words_2[i + 1]);       \
      out[i] = word_1;                                                         \
      out[i + 1] = word_2;                                                     \
      sum += roaring_hamming(word_1);                                                  \
      sum += roaring_hamming(word_2);                                                  \
    }                                                                          \
    dst->cardinality = sum;                                                    \
    return dst->cardinality;                                                   \
  }                                                                            \
  static inline int _scalar_bitset_container_##opname##_nocard(                              \
      const bitset_container_t *src_1, const bitset_container_t *src_2,        \
      bitset_container_t *dst) {                                               \
    const uint64_t *__restrict__ words_1 = src_1->words;                       \
    const uint64_t *__restrict__ words_2 = src_2->words;                       \
    uint64_t *out = dst->words;                                                \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i++) {              \
      out[i] = (words_1[i])opsymbol(words_2[i]);                               \
    }                                                                          \
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;                             \
    return dst->cardinality;                                                   \
  }                                                                            \
  static inline int _scalar_bitset_container_##opname##_justcard(                            \
      const bitset_container_t *src_1, const bitset_container_t *src_2) {      \
    const uint64_t *__restrict__ words_1 = src_1->words;                       \
    const uint64_t *__restrict__ words_2 = src_2->words;                       \
    int32_t sum = 0;                                                           \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 2) {           \
      const uint64_t word_1 = (words_1[i])opsymbol(words_2[i]),                \
                     word_2 = (words_1[i + 1]) opsymbol(words_2[i + 1]);       \
      sum += roaring_hamming(word_1);                                                  \
      sum += roaring_hamming(word_2);                                                  \
    }                                                                          \
    return sum;                                                                \
  }

// we duplicate the function because other containers use the "or" term, makes API more consistent
SCALAR_BITSET_CONTAINER_FN(or,    |, _mm256_or_si256, vorrq_u64)
SCALAR_BITSET_CONTAINER_FN(union, |, _mm256_or_si256, vorrq_u64)

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
SCALAR_BITSET_CONTAINER_FN(and,          &, _mm256_and_si256, vandq_u64)
SCALAR_BITSET_CONTAINER_FN(intersection, &, _mm256_and_si256, vandq_u64)

SCALAR_BITSET_CONTAINER_FN(xor,    ^,  _mm256_xor_si256,    veorq_u64)
SCALAR_BITSET_CONTAINER_FN(andnot, &~, _mm256_andnot_si256, vbicq_u64)

#if CROARING_COMPILER_SUPPORTS_AVX512
#define CROARING_BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic, neon_intrinsic)   \
  int bitset_container_##opname(const bitset_container_t *src_1,               \
                                const bitset_container_t *src_2,               \
                                bitset_container_t *dst) {                     \
    int support = croaring_hardware_support();                                 \
    if ( support & ROARING_SUPPORTS_AVX512 ) {                                 \
      return _avx512_bitset_container_##opname(src_1, src_2, dst);             \
    }                                                                          \
    else if ( support & ROARING_SUPPORTS_AVX2 ) {                              \
      return _avx2_bitset_container_##opname(src_1, src_2, dst);               \
    } else {                                                                   \
      return _scalar_bitset_container_##opname(src_1, src_2, dst);             \
    }                                                                          \
  }                                                                            \
  int bitset_container_##opname##_nocard(const bitset_container_t *src_1,      \
                                         const bitset_container_t *src_2,      \
                                         bitset_container_t *dst) {            \
    int support = croaring_hardware_support();                                 \
    if ( support & ROARING_SUPPORTS_AVX512 ) {                                 \
      return _avx512_bitset_container_##opname##_nocard(src_1, src_2, dst);    \
    }                                                                          \
    else if ( support & ROARING_SUPPORTS_AVX2 ) {                              \
      return _avx2_bitset_container_##opname##_nocard(src_1, src_2, dst);      \
    } else {                                                                   \
      return _scalar_bitset_container_##opname##_nocard(src_1, src_2, dst);    \
    }                                                                          \
  }                                                                            \
  int bitset_container_##opname##_justcard(const bitset_container_t *src_1,    \
                                           const bitset_container_t *src_2) {  \
     int support = croaring_hardware_support();                                \
    if ( support & ROARING_SUPPORTS_AVX512 ) {                                 \
      return _avx512_bitset_container_##opname##_justcard(src_1, src_2);       \
    }                                                                          \
    else if ( support & ROARING_SUPPORTS_AVX2 ) {                              \
      return _avx2_bitset_container_##opname##_justcard(src_1, src_2);         \
    } else {                                                                   \
      return _scalar_bitset_container_##opname##_justcard(src_1, src_2);       \
    }                                                                          \
  }

#else // CROARING_COMPILER_SUPPORTS_AVX512


#define CROARING_BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic, neon_intrinsic)   \
  int bitset_container_##opname(const bitset_container_t *src_1,               \
                                const bitset_container_t *src_2,               \
                                bitset_container_t *dst) {                     \
    if ( croaring_hardware_support() & ROARING_SUPPORTS_AVX2 ) {               \
      return _avx2_bitset_container_##opname(src_1, src_2, dst);               \
    } else {                                                                   \
      return _scalar_bitset_container_##opname(src_1, src_2, dst);             \
    }                                                                          \
  }                                                                            \
  int bitset_container_##opname##_nocard(const bitset_container_t *src_1,      \
                                         const bitset_container_t *src_2,      \
                                         bitset_container_t *dst) {            \
    if ( croaring_hardware_support() & ROARING_SUPPORTS_AVX2 ) {               \
      return _avx2_bitset_container_##opname##_nocard(src_1, src_2, dst);      \
    } else {                                                                   \
      return _scalar_bitset_container_##opname##_nocard(src_1, src_2, dst);    \
    }                                                                          \
  }                                                                            \
  int bitset_container_##opname##_justcard(const bitset_container_t *src_1,    \
                                           const bitset_container_t *src_2) {  \
    if ( croaring_hardware_support() & ROARING_SUPPORTS_AVX2 ) {               \
      return _avx2_bitset_container_##opname##_justcard(src_1, src_2);         \
    } else {                                                                   \
      return _scalar_bitset_container_##opname##_justcard(src_1, src_2);       \
    }                                                                          \
  }

#endif //  CROARING_COMPILER_SUPPORTS_AVX512

#elif defined(CROARING_USENEON)

#define CROARING_BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic, neon_intrinsic)  \
int bitset_container_##opname(const bitset_container_t *src_1,                \
                              const bitset_container_t *src_2,                \
                              bitset_container_t *dst) {                      \
    const uint64_t * __restrict__ words_1 = src_1->words;                     \
    const uint64_t * __restrict__ words_2 = src_2->words;                     \
    uint64_t *out = dst->words;                                               \
    uint16x8_t n0 = vdupq_n_u16(0);                                           \
    uint16x8_t n1 = vdupq_n_u16(0);                                           \
    uint16x8_t n2 = vdupq_n_u16(0);                                           \
    uint16x8_t n3 = vdupq_n_u16(0);                                           \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 8) {          \
        uint64x2_t c0 = neon_intrinsic(vld1q_u64(&words_1[i + 0]),            \
                                       vld1q_u64(&words_2[i + 0]));           \
        n0 = vaddq_u16(n0, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c0))));   \
        vst1q_u64(&out[i + 0], c0);                                           \
        uint64x2_t c1 = neon_intrinsic(vld1q_u64(&words_1[i + 2]),            \
                                       vld1q_u64(&words_2[i + 2]));           \
        n1 = vaddq_u16(n1, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c1))));   \
        vst1q_u64(&out[i + 2], c1);                                           \
        uint64x2_t c2 = neon_intrinsic(vld1q_u64(&words_1[i + 4]),            \
                                       vld1q_u64(&words_2[i + 4]));           \
        n2 = vaddq_u16(n2, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c2))));   \
        vst1q_u64(&out[i + 4], c2);                                           \
        uint64x2_t c3 = neon_intrinsic(vld1q_u64(&words_1[i + 6]),            \
                                       vld1q_u64(&words_2[i + 6]));           \
        n3 = vaddq_u16(n3, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c3))));   \
        vst1q_u64(&out[i + 6], c3);                                           \
    }                                                                         \
    uint64x2_t n = vdupq_n_u64(0);                                            \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n0)));                           \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n1)));                           \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n2)));                           \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n3)));                           \
    dst->cardinality = vgetq_lane_u64(n, 0) + vgetq_lane_u64(n, 1);           \
    return dst->cardinality;                                                  \
}                                                                             \
int bitset_container_##opname##_nocard(const bitset_container_t *src_1,       \
                                       const bitset_container_t *src_2,       \
                                             bitset_container_t *dst) {       \
    const uint64_t * __restrict__ words_1 = src_1->words;                     \
    const uint64_t * __restrict__ words_2 = src_2->words;                     \
    uint64_t *out = dst->words;                                               \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 8) {          \
        vst1q_u64(&out[i + 0], neon_intrinsic(vld1q_u64(&words_1[i + 0]),     \
                                              vld1q_u64(&words_2[i + 0])));   \
        vst1q_u64(&out[i + 2], neon_intrinsic(vld1q_u64(&words_1[i + 2]),     \
                                              vld1q_u64(&words_2[i + 2])));   \
        vst1q_u64(&out[i + 4], neon_intrinsic(vld1q_u64(&words_1[i + 4]),     \
                                              vld1q_u64(&words_2[i + 4])));   \
        vst1q_u64(&out[i + 6], neon_intrinsic(vld1q_u64(&words_1[i + 6]),     \
                                              vld1q_u64(&words_2[i + 6])));   \
    }                                                                         \
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;                            \
    return dst->cardinality;                                                  \
}                                                                             \
int bitset_container_##opname##_justcard(const bitset_container_t *src_1,     \
                                         const bitset_container_t *src_2) {   \
    const uint64_t * __restrict__ words_1 = src_1->words;                     \
    const uint64_t * __restrict__ words_2 = src_2->words;                     \
    uint16x8_t n0 = vdupq_n_u16(0);                                           \
    uint16x8_t n1 = vdupq_n_u16(0);                                           \
    uint16x8_t n2 = vdupq_n_u16(0);                                           \
    uint16x8_t n3 = vdupq_n_u16(0);                                           \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 8) {          \
        uint64x2_t c0 = neon_intrinsic(vld1q_u64(&words_1[i + 0]),            \
                                       vld1q_u64(&words_2[i + 0]));           \
        n0 = vaddq_u16(n0, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c0))));   \
        uint64x2_t c1 = neon_intrinsic(vld1q_u64(&words_1[i + 2]),            \
                                       vld1q_u64(&words_2[i + 2]));           \
        n1 = vaddq_u16(n1, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c1))));   \
        uint64x2_t c2 = neon_intrinsic(vld1q_u64(&words_1[i + 4]),            \
                                       vld1q_u64(&words_2[i + 4]));           \
        n2 = vaddq_u16(n2, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c2))));   \
        uint64x2_t c3 = neon_intrinsic(vld1q_u64(&words_1[i + 6]),            \
                                       vld1q_u64(&words_2[i + 6]));           \
        n3 = vaddq_u16(n3, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_u64(c3))));   \
    }                                                                         \
    uint64x2_t n = vdupq_n_u64(0);                                            \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n0)));                           \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n1)));                           \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n2)));                           \
    n = vaddq_u64(n, vpaddlq_u32(vpaddlq_u16(n3)));                           \
    return vgetq_lane_u64(n, 0) + vgetq_lane_u64(n, 1);                       \
}

#else

#define CROARING_BITSET_CONTAINER_FN(opname, opsymbol, avx_intrinsic, neon_intrinsic)  \
int bitset_container_##opname(const bitset_container_t *src_1,            \
                              const bitset_container_t *src_2,            \
                              bitset_container_t *dst) {                  \
    const uint64_t * __restrict__ words_1 = src_1->words;                 \
    const uint64_t * __restrict__ words_2 = src_2->words;                 \
    uint64_t *out = dst->words;                                           \
    int32_t sum = 0;                                                      \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 2) {      \
        const uint64_t word_1 = (words_1[i])opsymbol(words_2[i]),         \
                       word_2 = (words_1[i + 1])opsymbol(words_2[i + 1]); \
        out[i] = word_1;                                                  \
        out[i + 1] = word_2;                                              \
        sum += roaring_hamming(word_1);                                    \
        sum += roaring_hamming(word_2);                                    \
    }                                                                     \
    dst->cardinality = sum;                                               \
    return dst->cardinality;                                              \
}                                                                         \
int bitset_container_##opname##_nocard(const bitset_container_t *src_1,   \
                                       const bitset_container_t *src_2,   \
                                       bitset_container_t *dst) {         \
    const uint64_t * __restrict__ words_1 = src_1->words;                 \
    const uint64_t * __restrict__ words_2 = src_2->words;                 \
    uint64_t *out = dst->words;                                           \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i++) {         \
        out[i] = (words_1[i])opsymbol(words_2[i]);                        \
    }                                                                     \
    dst->cardinality = BITSET_UNKNOWN_CARDINALITY;                        \
    return dst->cardinality;                                              \
}                                                                         \
int bitset_container_##opname##_justcard(const bitset_container_t *src_1, \
                              const bitset_container_t *src_2) {          \
    const uint64_t * __restrict__ words_1 = src_1->words;                 \
    const uint64_t * __restrict__ words_2 = src_2->words;                 \
    int32_t sum = 0;                                                      \
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 2) {      \
        const uint64_t word_1 = (words_1[i])opsymbol(words_2[i]),         \
                       word_2 = (words_1[i + 1])opsymbol(words_2[i + 1]); \
        sum += roaring_hamming(word_1);                                    \
        sum += roaring_hamming(word_2);                                    \
    }                                                                     \
    return sum;                                                           \
}

#endif // CROARING_IS_X64

// we duplicate the function because other containers use the "or" term, makes API more consistent
CROARING_BITSET_CONTAINER_FN(or,    |, _mm256_or_si256, vorrq_u64)
CROARING_BITSET_CONTAINER_FN(union, |, _mm256_or_si256, vorrq_u64)

// we duplicate the function because other containers use the "intersection" term, makes API more consistent
CROARING_BITSET_CONTAINER_FN(and,          &, _mm256_and_si256, vandq_u64)
CROARING_BITSET_CONTAINER_FN(intersection, &, _mm256_and_si256, vandq_u64)

CROARING_BITSET_CONTAINER_FN(xor,    ^,  _mm256_xor_si256,    veorq_u64)
CROARING_BITSET_CONTAINER_FN(andnot, &~, _mm256_andnot_si256, vbicq_u64)
// clang-format On


CROARING_ALLOW_UNALIGNED
int bitset_container_to_uint32_array(
    uint32_t *out,
    const bitset_container_t *bc,
    uint32_t base
){
#if CROARING_IS_X64
   int support = croaring_hardware_support();
#if CROARING_COMPILER_SUPPORTS_AVX512
   if(( support & ROARING_SUPPORTS_AVX512 ) &&  (bc->cardinality >= 8192))  // heuristic
		return (int) bitset_extract_setbits_avx512(bc->words,
                BITSET_CONTAINER_SIZE_IN_WORDS, out, bc->cardinality, base);
   else
#endif
   if(( support & ROARING_SUPPORTS_AVX2 ) &&  (bc->cardinality >= 8192))  // heuristic
		return (int) bitset_extract_setbits_avx2(bc->words,
                BITSET_CONTAINER_SIZE_IN_WORDS, out, bc->cardinality, base);
	else
		return (int) bitset_extract_setbits(bc->words,
                BITSET_CONTAINER_SIZE_IN_WORDS, out, base);
#else
	return (int) bitset_extract_setbits(bc->words,
                BITSET_CONTAINER_SIZE_IN_WORDS, out, base);
#endif
}

/*
 * Print this container using printf (useful for debugging).
 */
void bitset_container_printf(const bitset_container_t * v) {
	printf("{");
	uint32_t base = 0;
	bool iamfirst = true;// TODO: rework so that this is not necessary yet still readable
	for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i) {
		uint64_t w = v->words[i];
		while (w != 0) {
			uint64_t t = w & (~w + 1);
			int r = roaring_trailing_zeroes(w);
			if(iamfirst) {// predicted to be false
				printf("%u",base + r);
				iamfirst = false;
			} else {
				printf(",%u",base + r);
			}
			w ^= t;
		}
		base += 64;
	}
	printf("}");
}


/*
 * Print this container using printf as a comma-separated list of 32-bit integers starting at base.
 */
void bitset_container_printf_as_uint32_array(const bitset_container_t * v, uint32_t base) {
	bool iamfirst = true;// TODO: rework so that this is not necessary yet still readable
	for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i) {
		uint64_t w = v->words[i];
		while (w != 0) {
			uint64_t t = w & (~w + 1);
			int r = roaring_trailing_zeroes(w);
			if(iamfirst) {// predicted to be false
				printf("%u", r + base);
				iamfirst = false;
			} else {
				printf(",%u",r + base);
			}
			w ^= t;
		}
		base += 64;
	}
}

/*
 * Validate the container. Returns true if valid.
 */
bool bitset_container_validate(const bitset_container_t *v, const char **reason) {
    if (v->words == NULL) {
        *reason = "words is NULL";
        return false;
    }
    if (v->cardinality != bitset_container_compute_cardinality(v)) {
        *reason = "cardinality is incorrect";
        return false;
    }
    if (v->cardinality <= DEFAULT_MAX_SIZE) {
        *reason = "cardinality is too small for a bitmap container";
        return false;
    }
    // Attempt to forcibly load the first and last words, hopefully causing
    // a segfault or an address sanitizer error if words is not allocated.
    volatile uint64_t *words = v->words;
    (void) words[0];
    (void) words[BITSET_CONTAINER_SIZE_IN_WORDS - 1];
    return true;
}


// TODO: use the fast lower bound, also
int bitset_container_number_of_runs(bitset_container_t *bc) {
  int num_runs = 0;
  uint64_t next_word = bc->words[0];

  for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS-1; ++i) {
    uint64_t word = next_word;
    next_word = bc->words[i+1];
    num_runs += roaring_hamming((~word) & (word << 1)) + ( (word >> 63) & ~next_word);
  }

  uint64_t word = next_word;
  num_runs += roaring_hamming((~word) & (word << 1));
  if((word & 0x8000000000000000ULL) != 0)
    num_runs++;
  return num_runs;
}


int32_t bitset_container_write(const bitset_container_t *container,
                                  char *buf) {
	memcpy(buf, container->words, BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t));
	return bitset_container_size_in_bytes(container);
}


int32_t bitset_container_read(int32_t cardinality, bitset_container_t *container,
		const char *buf)  {
	container->cardinality = cardinality;
	memcpy(container->words, buf, BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t));
	return bitset_container_size_in_bytes(container);
}

bool bitset_container_iterate(const bitset_container_t *cont, uint32_t base, roaring_iterator iterator, void *ptr) {
  for (int32_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i ) {
    uint64_t w = cont->words[i];
    while (w != 0) {
      uint64_t t = w & (~w + 1);
      int r = roaring_trailing_zeroes(w);
      if(!iterator(r + base, ptr)) return false;
      w ^= t;
    }
    base += 64;
  }
  return true;
}

bool bitset_container_iterate64(const bitset_container_t *cont, uint32_t base, roaring_iterator64 iterator, uint64_t high_bits, void *ptr) {
  for (int32_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i ) {
    uint64_t w = cont->words[i];
    while (w != 0) {
      uint64_t t = w & (~w + 1);
      int r = roaring_trailing_zeroes(w);
      if(!iterator(high_bits | (uint64_t)(r + base), ptr)) return false;
      w ^= t;
    }
    base += 64;
  }
  return true;
}

#if CROARING_IS_X64
#if CROARING_COMPILER_SUPPORTS_AVX512
CROARING_TARGET_AVX512
CROARING_ALLOW_UNALIGNED
static inline bool _avx512_bitset_container_equals(const bitset_container_t *container1, const bitset_container_t *container2) {
  const __m512i *ptr1 = (const __m512i*)container1->words;
  const __m512i *ptr2 = (const __m512i*)container2->words;
  for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS*sizeof(uint64_t)/64; i++) {
      __m512i r1 = _mm512_loadu_si512(ptr1+i);
      __m512i r2 = _mm512_loadu_si512(ptr2+i);
      __mmask64 mask = _mm512_cmpeq_epi8_mask(r1, r2);
      if ((uint64_t)mask != UINT64_MAX) {
          return false;
      }
  }
	return true;
}
CROARING_UNTARGET_AVX512
#endif // CROARING_COMPILER_SUPPORTS_AVX512
CROARING_TARGET_AVX2
CROARING_ALLOW_UNALIGNED
static inline bool _avx2_bitset_container_equals(const bitset_container_t *container1, const bitset_container_t *container2) {
    const __m256i *ptr1 = (const __m256i*)container1->words;
    const __m256i *ptr2 = (const __m256i*)container2->words;
    for (size_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS*sizeof(uint64_t)/32; i++) {
      __m256i r1 = _mm256_loadu_si256(ptr1+i);
      __m256i r2 = _mm256_loadu_si256(ptr2+i);
      int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(r1, r2));
      if ((uint32_t)mask != UINT32_MAX) {
          return false;
      }
  }
	return true;
}
CROARING_UNTARGET_AVX2
#endif // CROARING_IS_X64

CROARING_ALLOW_UNALIGNED
bool bitset_container_equals(const bitset_container_t *container1, const bitset_container_t *container2) {
  if((container1->cardinality != BITSET_UNKNOWN_CARDINALITY) && (container2->cardinality != BITSET_UNKNOWN_CARDINALITY)) {
    if(container1->cardinality != container2->cardinality) {
      return false;
    }
    if (container1->cardinality == INT32_C(0x10000)) {
      return true;
    }
  }
#if CROARING_IS_X64
  int support = croaring_hardware_support();
#if CROARING_COMPILER_SUPPORTS_AVX512
  if( support & ROARING_SUPPORTS_AVX512 ) {
    return _avx512_bitset_container_equals(container1, container2);
  }
  else
#endif
  if( support & ROARING_SUPPORTS_AVX2 ) {
    return _avx2_bitset_container_equals(container1, container2);
  }
#endif
  return memcmp(container1->words,
                container2->words,
                BITSET_CONTAINER_SIZE_IN_WORDS*sizeof(uint64_t)) == 0;
}

bool bitset_container_is_subset(const bitset_container_t *container1,
                          const bitset_container_t *container2) {
    if((container1->cardinality != BITSET_UNKNOWN_CARDINALITY) && (container2->cardinality != BITSET_UNKNOWN_CARDINALITY)) {
        if(container1->cardinality > container2->cardinality) {
            return false;
        }
    }
    for(int32_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i ) {
		if((container1->words[i] & container2->words[i]) != container1->words[i]) {
			return false;
		}
	}
	return true;
}

bool bitset_container_select(const bitset_container_t *container, uint32_t *start_rank, uint32_t rank, uint32_t *element) {
    int card = bitset_container_cardinality(container);
    if(rank >= *start_rank + card) {
        *start_rank += card;
        return false;
    }
    const uint64_t *words = container->words;
    int32_t size;
    for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; i += 1) {
        size = roaring_hamming(words[i]);
        if(rank <= *start_rank + size) {
            uint64_t w = container->words[i];
            uint16_t base = i*64;
            while (w != 0) {
                uint64_t t = w & (~w + 1);
                int r = roaring_trailing_zeroes(w);
                if(*start_rank == rank) {
                    *element = r+base;
                    return true;
                }
                w ^= t;
                *start_rank += 1;
            }
        }
        else
            *start_rank += size;
    }
    assert(false);
    roaring_unreachable;
}


/* Returns the smallest value (assumes not empty) */
uint16_t bitset_container_minimum(const bitset_container_t *container) {
  for (int32_t i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS; ++i ) {
    uint64_t w = container->words[i];
    if (w != 0) {
      int r = roaring_trailing_zeroes(w);
      return r + i * 64;
    }
  }
  return UINT16_MAX;
}

/* Returns the largest value (assumes not empty) */
uint16_t bitset_container_maximum(const bitset_container_t *container) {
  for (int32_t i = BITSET_CONTAINER_SIZE_IN_WORDS - 1; i > 0; --i ) {
    uint64_t w = container->words[i];
    if (w != 0) {
      int r = roaring_leading_zeroes(w);
      return i * 64 + 63  - r;
    }
  }
  return 0;
}

/* Returns the number of values equal or smaller than x */
int bitset_container_rank(const bitset_container_t *container, uint16_t x) {
  // credit: aqrit
  int sum = 0;
  int i = 0;
  for (int end = x / 64; i < end; i++){
    sum += roaring_hamming(container->words[i]);
  }
  uint64_t lastword = container->words[i];
  uint64_t lastpos = UINT64_C(1) << (x % 64);
  uint64_t mask = lastpos + lastpos - 1; // smear right
  sum += roaring_hamming(lastword & mask);
  return sum;
}

uint32_t bitset_container_rank_many(const bitset_container_t *container, uint64_t start_rank, const uint32_t* begin, const uint32_t* end, uint64_t* ans){
  const uint16_t high = (uint16_t)((*begin) >> 16);
  int i = 0;
  int sum = 0;
  const uint32_t* iter = begin;
  for(; iter != end; iter++) {
      uint32_t x = *iter;
      uint16_t xhigh = (uint16_t)(x >> 16);
      if(xhigh != high) return iter - begin; // stop at next container

      uint16_t xlow = (uint16_t)x;
      for(int count = xlow / 64; i < count; i++){
        sum += roaring_hamming(container->words[i]);
      }
      uint64_t lastword = container->words[i];
      uint64_t lastpos = UINT64_C(1) << (xlow % 64);
      uint64_t mask = lastpos + lastpos - 1; // smear right
      *(ans++) = start_rank + sum + roaring_hamming(lastword & mask);
  }
  return iter - begin;
}


/* Returns the index of x , if not exsist return -1 */
int bitset_container_get_index(const bitset_container_t *container, uint16_t x) {
  if (bitset_container_get(container, x)) {
    // credit: aqrit
    int sum = 0;
    int i = 0;
    for (int end = x / 64; i < end; i++){
      sum += roaring_hamming(container->words[i]);
    }
    uint64_t lastword = container->words[i];
    uint64_t lastpos = UINT64_C(1) << (x % 64);
    uint64_t mask = lastpos + lastpos - 1; // smear right
    sum += roaring_hamming(lastword & mask);
    return sum - 1;
  } else {
    return -1;
  }
}

/* Returns the index of the first value equal or larger than x, or -1 */
int bitset_container_index_equalorlarger(const bitset_container_t *container, uint16_t x) {
  uint32_t x32 = x;
  uint32_t k = x32 / 64;
  uint64_t word = container->words[k];
  const int diff = x32 - k * 64; // in [0,64)
  word = (word >> diff) << diff; // a mask is faster, but we don't care
  while(word == 0) {
    k++;
    if(k == BITSET_CONTAINER_SIZE_IN_WORDS) return -1;
    word = container->words[k];
  }
  return k * 64 + roaring_trailing_zeroes(word);
}

#ifdef __cplusplus
} } }  // extern "C" { namespace roaring { namespace internal {
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif