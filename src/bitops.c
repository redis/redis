/* Bit operations.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "bitroar.h"
#include "ctype.h"
#include <limits.h>

#ifdef HAVE_AVX2
/* Define __MM_MALLOC_H to prevent importing the memory aligned
 * allocation functions, which we don't use. */
#define __MM_MALLOC_H
#include <immintrin.h>
#endif

#ifdef HAVE_AVX512
/* Define __MM_MALLOC_H to prevent importing the memory aligned
 * allocation functions, which we don't use. */
#define __MM_MALLOC_H
#include <immintrin.h>
#endif

#ifdef HAVE_AARCH64_NEON
#include <arm_neon.h>
#endif

#ifdef HAVE_AVX2
#define BITOP_USE_AVX2 (__builtin_cpu_supports("avx2"))
#else
#define BITOP_USE_AVX2 0
#endif

/* AArch64 NEON support is determined at compile time via HAVE_AARCH64_NEON */
#ifdef HAVE_AVX512
#define BITOP_USE_AVX512 (__builtin_cpu_supports("avx512f"))
#define BITOPS_USE_AVX512_POPCOUNT  (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vpopcntdq"))
#else
#define BITOP_USE_AVX512 0
#define BITOPS_USE_AVX512_POPCOUNT  0
#endif


/* -----------------------------------------------------------------------------
 * Helpers and low level bit functions.
 * -------------------------------------------------------------------------- */

 /* Shared lookup table for bit counting - maps each byte value to its popcount */
static const uint8_t bitsinbyte[256] = {
    #define B2(n) n, n+1, n+1, n+2
    #define B4(n) B2(n), B2(n+1), B2(n+1), B2(n+2)
    #define B6(n) B4(n), B4(n+1), B4(n+1), B4(n+2)
    B6(0), B6(1), B6(1), B6(2)
    #undef B6
    #undef B4
    #undef B2
};

/* Count number of bits set in the binary array pointed by 's' and long
 * 'count' bytes. The implementation of this function is required to
 * work with an input string length up to 512 MB or more (server.proto_max_bulk_len) */
ATTRIBUTE_TARGET_POPCNT
long long redisPopcount(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;
    uint32_t *p4;
#if defined(HAVE_POPCNT)
    int use_popcnt = __builtin_cpu_supports("popcnt"); /* Check if CPU supports POPCNT instruction. */
#else
    int use_popcnt = 0; /* Assume CPU does not support POPCNT if
                         * __builtin_cpu_supports() is not available. */
#endif
    /* Count initial bytes not aligned to 64-bit when using the POPCNT instruction,
     * otherwise align to 32-bit. */
    int align = use_popcnt ? 7 : 3;
    while ((unsigned long)p & align && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    if (likely(use_popcnt)) {
        /* Use separate counters to make the CPU think there are no
         * dependencies between these popcnt operations. */
        uint64_t cnt[4];
        memset(cnt, 0, sizeof(cnt));

        /* Count bits 32 bytes at a time by using popcnt.
         * Unroll the loop to avoid the overhead of a single popcnt per iteration,
         * allowing the CPU to extract more instruction-level parallelism.
         * Reference: https://danluu.com/assembly-intrinsics/ */
        while (count >= 32) {
            cnt[0] += __builtin_popcountll(*(uint64_t*)(p));
            cnt[1] += __builtin_popcountll(*(uint64_t*)(p + 8));
            cnt[2] += __builtin_popcountll(*(uint64_t*)(p + 16));
            cnt[3] += __builtin_popcountll(*(uint64_t*)(p + 24));
            count -= 32;
            p += 32;
            /* Prefetch with 2K stride is just enough to overlap L3 miss latency effectively
             * without causing pressure on lower memory hierarchy or polluting L1/L2 */
            redis_prefetch_read(p + 2048);
        }
        bits += cnt[0] + cnt[1] + cnt[2] + cnt[3];
        goto remain;
    }

    /* Count bits 28 bytes at a time */
    p4 = (uint32_t*)p;
    while(count>=28) {
        uint32_t aux1, aux2, aux3, aux4, aux5, aux6, aux7;

        aux1 = *p4++;
        aux2 = *p4++;
        aux3 = *p4++;
        aux4 = *p4++;
        aux5 = *p4++;
        aux6 = *p4++;
        aux7 = *p4++;
        count -= 28;

        aux1 = aux1 - ((aux1 >> 1) & 0x55555555);
        aux1 = (aux1 & 0x33333333) + ((aux1 >> 2) & 0x33333333);
        aux2 = aux2 - ((aux2 >> 1) & 0x55555555);
        aux2 = (aux2 & 0x33333333) + ((aux2 >> 2) & 0x33333333);
        aux3 = aux3 - ((aux3 >> 1) & 0x55555555);
        aux3 = (aux3 & 0x33333333) + ((aux3 >> 2) & 0x33333333);
        aux4 = aux4 - ((aux4 >> 1) & 0x55555555);
        aux4 = (aux4 & 0x33333333) + ((aux4 >> 2) & 0x33333333);
        aux5 = aux5 - ((aux5 >> 1) & 0x55555555);
        aux5 = (aux5 & 0x33333333) + ((aux5 >> 2) & 0x33333333);
        aux6 = aux6 - ((aux6 >> 1) & 0x55555555);
        aux6 = (aux6 & 0x33333333) + ((aux6 >> 2) & 0x33333333);
        aux7 = aux7 - ((aux7 >> 1) & 0x55555555);
        aux7 = (aux7 & 0x33333333) + ((aux7 >> 2) & 0x33333333);
        bits += ((((aux1 + (aux1 >> 4)) & 0x0F0F0F0F) +
                    ((aux2 + (aux2 >> 4)) & 0x0F0F0F0F) +
                    ((aux3 + (aux3 >> 4)) & 0x0F0F0F0F) +
                    ((aux4 + (aux4 >> 4)) & 0x0F0F0F0F) +
                    ((aux5 + (aux5 >> 4)) & 0x0F0F0F0F) +
                    ((aux6 + (aux6 >> 4)) & 0x0F0F0F0F) +
                    ((aux7 + (aux7 >> 4)) & 0x0F0F0F0F))* 0x01010101) >> 24;
    }
    p = (unsigned char*)p4;

remain:
    /* Count the remaining bytes. */
    while(count--) bits += bitsinbyte[*p++];
    return bits;
}

#ifdef HAVE_AARCH64_NEON
/* AArch64 optimized popcount implementation.
 * Processes the input bitmap using four NEON vector accumulators in parallel
 * to improve instruction-level parallelism and reduce the frequency of
 * scalar reductions. Each accumulator holds 16-bit partial sums that are
 * combined only once per large block (128 bytes), minimizing data movement.
 *
 * Benchmark results show this approach outperforms 2-lane implementations
 * and matches or exceeds 8-lane versions in throughput, while avoiding
 * register pressure and keeping the backend pipeline fully utilized.
 *
 * This function is now memory bound on large bitmaps, as confirmed by perf
 * profiling, with backend stalls dominated by L1/L2 data cache refills.
 */
long long redisPopCountAarch64(void *s, long count) {
    long long bits = 0;
    const uint8_t *p = (const uint8_t*)s;

    /* Align */
    while (((uintptr_t)p & 15) && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    /* Four vector accumulators of u16 (pairwise-accumulated byte counts). */
    uint16x8_t acc0 = vdupq_n_u16(0);
    uint16x8_t acc1 = vdupq_n_u16(0);
    uint16x8_t acc2 = vdupq_n_u16(0);
    uint16x8_t acc3 = vdupq_n_u16(0);

    /* Process 128B per loop to amortize reductions. */
    while (count >= 128) {
        uint8x16_t d0 = vld1q_u8(p +  0);
        uint8x16_t d1 = vld1q_u8(p + 16);
        uint8x16_t d2 = vld1q_u8(p + 32);
        uint8x16_t d3 = vld1q_u8(p + 48);
        uint8x16_t d4 = vld1q_u8(p + 64);
        uint8x16_t d5 = vld1q_u8(p + 80);
        uint8x16_t d6 = vld1q_u8(p + 96);
        uint8x16_t d7 = vld1q_u8(p +112);

        /* Per-byte popcount */
        uint8x16_t c0 = vcntq_u8(d0);
        uint8x16_t c1 = vcntq_u8(d1);
        uint8x16_t c2 = vcntq_u8(d2);
        uint8x16_t c3 = vcntq_u8(d3);
        uint8x16_t c4 = vcntq_u8(d4);
        uint8x16_t c5 = vcntq_u8(d5);
        uint8x16_t c6 = vcntq_u8(d6);
        uint8x16_t c7 = vcntq_u8(d7);

        /* Pairwise widen-add with accumulation: u8 -> u16, stay in vectors */
        acc0 = vpadalq_u8(acc0, c0);
        acc1 = vpadalq_u8(acc1, c1);
        acc2 = vpadalq_u8(acc2, c2);
        acc3 = vpadalq_u8(acc3, c3);

        acc0 = vpadalq_u8(acc0, c4);
        acc1 = vpadalq_u8(acc1, c5);
        acc2 = vpadalq_u8(acc2, c6);
        acc3 = vpadalq_u8(acc3, c7);

        p += 128;
        count -= 128;
    }

    /* Reduce vector accumulators to scalar once. */
    uint32x4_t s0 = vpaddlq_u16(acc0);
    uint32x4_t s1 = vpaddlq_u16(acc1);
    uint32x4_t s2 = vpaddlq_u16(acc2);
    uint32x4_t s3 = vpaddlq_u16(acc3);
    uint32x4_t s01 = vaddq_u32(s0, s1);
    uint32x4_t s23 = vaddq_u32(s2, s3);
    uint32x4_t st = vaddq_u32(s01, s23);
    uint64x2_t s64 = vpaddlq_u32(st);
    bits += (long long)(vgetq_lane_u64(s64, 0) + vgetq_lane_u64(s64, 1));

    /* Remaining 64B blocks (keep vector domain) */
    while (count >= 64) {
        uint8x16_t d0 = vld1q_u8(p +  0);
        uint8x16_t d1 = vld1q_u8(p + 16);
        uint8x16_t d2 = vld1q_u8(p + 32);
        uint8x16_t d3 = vld1q_u8(p + 48);

        uint8x16_t c0 = vcntq_u8(d0);
        uint8x16_t c1 = vcntq_u8(d1);
        uint8x16_t c2 = vcntq_u8(d2);
        uint8x16_t c3 = vcntq_u8(d3);

        uint64x2_t t0 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c0)));
        uint64x2_t t1 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c1)));
        uint64x2_t t2 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c2)));
        uint64x2_t t3 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c3)));

        uint64x2_t s = vaddq_u64(vaddq_u64(t0, t1), vaddq_u64(t2, t3));
        bits += (long long)(vgetq_lane_u64(s, 0) + vgetq_lane_u64(s, 1));

        p += 64;
        count -= 64;
    }

    /* 16B chunks */
    while (count >= 16) {
        uint8x16_t d = vld1q_u8(p);
        uint64x2_t s = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(vcntq_u8(d))));
        bits += (long long)(vgetq_lane_u64(s, 0) + vgetq_lane_u64(s, 1));
        p += 16;
        count -= 16;
    }

    /* Tail */
    while (count--) bits += bitsinbyte[*p++];

    return bits;
}
#endif

#ifdef HAVE_AVX512
/* AVX512 optimized version of redisPopcount using VPOPCNTDQ instruction.
 * This function requires AVX512F and AVX512VPOPCNTDQ support. */
ATTRIBUTE_TARGET_AVX512_POPCOUNT
long long redisPopCountAvx512(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;

    /* Align to 64-byte boundary for optimal AVX512 performance */
    while ((unsigned long)p & 63 && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    /* Process 64 bytes at a time using AVX512 */
    while (count >= 64) {
        __m512i data = _mm512_loadu_si512((__m512i*)p);
        __m512i popcnt = _mm512_popcnt_epi64(data);

        /* Sum all 8 64-bit popcount results */
        bits += _mm512_reduce_add_epi64(popcnt);

        p += 64;
        count -= 64;

        /* Prefetch next cache line */
        redis_prefetch_read(p + 2048);
    }

    /* Handle remaining bytes with scalar popcount */
    while (count >= 8) {
        bits += __builtin_popcountll(*(uint64_t*)p);
        p += 8;
        count -= 8;
    }

    /* Handle final bytes */
    while (count--) {
        bits += bitsinbyte[*p++];
    }

    return bits;
}
#endif

#ifdef HAVE_AVX2
/* AVX2 optimized version of redisPopcount.
 * This function requires AVX2 and POPCNT support. */
ATTRIBUTE_TARGET_AVX2_POPCOUNT
long long redisPopCountAvx2(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;

    /* Align to 8-byte boundary for 64-bit operations */
    while ((unsigned long)p & 7 && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    /* Use separate counters to avoid dependencies, similar to regular redisPopcount */
    uint64_t cnt[4];
    memset(cnt, 0, sizeof(cnt));

    /* Process 32 bytes at a time using POPCNT on 64-bit chunks */
    while (count >= 32) {
        cnt[0] += __builtin_popcountll(*(uint64_t*)(p));
        cnt[1] += __builtin_popcountll(*(uint64_t*)(p + 8));
        cnt[2] += __builtin_popcountll(*(uint64_t*)(p + 16));
        cnt[3] += __builtin_popcountll(*(uint64_t*)(p + 24));

        p += 32;
        count -= 32;

        /* Prefetch next cache line */
        redis_prefetch_read(p + 2048);
    }

    bits += cnt[0] + cnt[1] + cnt[2] + cnt[3];

    /* Handle remaining bytes with scalar popcount */
    while (count >= 8) {
        bits += __builtin_popcountll(*(uint64_t*)p);
        p += 8;
        count -= 8;
    }

    /* Handle final bytes */
    while (count--) {
        bits += bitsinbyte[*p++];
    }

    return bits;
}
#endif

/* Automatically select the best available popcount implementation */
static inline long long redisPopcountAuto(const unsigned char *p, long count) {
#ifdef HAVE_AVX512
    if (BITOPS_USE_AVX512_POPCOUNT) {
        return redisPopCountAvx512((void*)p, count);
    }
#endif
#ifdef HAVE_AVX2
    if (BITOP_USE_AVX2) {
        return redisPopCountAvx2((void*)p, count);
    }
#endif
#ifdef HAVE_AARCH64_NEON
    return redisPopCountAarch64((void*)p, count);
#else
    return redisPopcount((void*)p, count);
#endif
}

/* Return the position of the first bit set to one (if 'bit' is 1) or
 * zero (if 'bit' is 0) in the bitmap starting at 's' and long 'count' bytes.
 *
 * The function is guaranteed to return a value >= 0 if 'bit' is 0 since if
 * no zero bit is found, it returns count*8 assuming the string is zero
 * padded on the right. However if 'bit' is 1 it is possible that there is
 * not a single set bit in the bitmap. In this special case -1 is returned. */
long long redisBitpos(void *s, unsigned long count, int bit) {
    unsigned long *l;
    unsigned char *c;
    unsigned long skipval, word = 0, one;
    long long pos = 0; /* Position of bit, to return to the caller. */
    unsigned long j;
    int found;

    /* Process whole words first, seeking for first word that is not
     * all ones or all zeros respectively if we are looking for zeros
     * or ones. This is much faster with large strings having contiguous
     * blocks of 1 or 0 bits compared to the vanilla bit per bit processing.
     *
     * Note that if we start from an address that is not aligned
     * to sizeof(unsigned long) we consume it byte by byte until it is
     * aligned. */

    /* Skip initial bits not aligned to sizeof(unsigned long) byte by byte. */
    skipval = bit ? 0 : UCHAR_MAX;
    c = (unsigned char*) s;
    found = 0;
    while((unsigned long)c & (sizeof(*l)-1) && count) {
        if (*c != skipval) {
            found = 1;
            break;
        }
        c++;
        count--;
        pos += 8;
    }

    /* Skip bits with full word step. */
    l = (unsigned long*) c;
    if (!found) {
        skipval = bit ? 0 : ULONG_MAX;
        while (count >= sizeof(*l)) {
            if (*l != skipval) break;
            l++;
            count -= sizeof(*l);
            pos += sizeof(*l)*8;
        }
    }

    /* Load bytes into "word" considering the first byte as the most significant
     * (we basically consider it as written in big endian, since we consider the
     * string as a set of bits from left to right, with the first bit at position
     * zero.
     *
     * Note that the loading is designed to work even when the bytes left
     * (count) are less than a full word. We pad it with zero on the right. */
    c = (unsigned char*)l;
    for (j = 0; j < sizeof(*l); j++) {
        word <<= 8;
        if (count) {
            word |= *c;
            c++;
            count--;
        }
    }

    /* Special case:
     * If bits in the string are all zero and we are looking for one,
     * return -1 to signal that there is not a single "1" in the whole
     * string. This can't happen when we are looking for "0" as we assume
     * that the right of the string is zero padded. */
    if (bit == 1 && word == 0) return -1;

    /* Last word left, scan bit by bit. The first thing we need is to
     * have a single "1" set in the most significant position in an
     * unsigned long. We don't know the size of the long so we use a
     * simple trick. */
    one = ULONG_MAX; /* All bits set to 1.*/
    one >>= 1;       /* All bits set to 1 but the MSB. */
    one = ~one;      /* All bits set to 0 but the MSB. */

    while(one) {
        if (((one & word) != 0) == bit) return pos;
        pos++;
        one >>= 1;
    }

    /* If we reached this point, there is a bug in the algorithm, since
     * the case of no match is handled as a special case before. */
    serverPanic("End of redisBitpos() reached.");
    return 0; /* Just to avoid warnings. */
}

/* The following set.*Bitfield and get.*Bitfield functions implement setting
 * and getting arbitrary size (up to 64 bits) signed and unsigned integers
 * at arbitrary positions into a bitmap.
 *
 * The representation considers the bitmap as having the bit number 0 to be
 * the most significant bit of the first byte, and so forth, so for example
 * setting a 5 bits unsigned integer to value 23 at offset 7 into a bitmap
 * previously set to all zeroes, will produce the following representation:
 *
 * +--------+--------+
 * |00000001|01110000|
 * +--------+--------+
 *
 * When offsets and integer sizes are aligned to bytes boundaries, this is the
 * same as big endian, however when such alignment does not exist, its important
 * to also understand how the bits inside a byte are ordered.
 *
 * Note that this format follows the same convention as SETBIT and related
 * commands.
 */

void setUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, uint64_t value) {
    uint64_t byte, bit, byteval, bitval, j;

    for (j = 0; j < bits; j++) {
        bitval = (value & ((uint64_t)1<<(bits-1-j))) != 0;
        byte = offset >> 3;
        bit = 7 - (offset & 0x7);
        byteval = p[byte];
        byteval &= ~(1 << bit);
        byteval |= bitval << bit;
        p[byte] = byteval & 0xff;
        offset++;
    }
}

void setSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, int64_t value) {
    uint64_t uv = value; /* Casting will add UINT64_MAX + 1 if v is negative. */
    setUnsignedBitfield(p,offset,bits,uv);
}

uint64_t getUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits) {
    uint64_t byte, bit, byteval, bitval, j, value = 0;

    for (j = 0; j < bits; j++) {
        byte = offset >> 3;
        bit = 7 - (offset & 0x7);
        byteval = p[byte];
        bitval = (byteval >> bit) & 1;
        value = (value<<1) | bitval;
        offset++;
    }
    return value;
}

/* Sign extend the 'bits' wide two's complement integer stored in the low bits
 * of 'value'. Shared by the string and the Roaring signed readers below.
 *
 * Converting from unsigned to signed is undefined when the value does
 * not fit, however here we assume two's complement and the original value
 * was obtained from signed -> unsigned conversion, so we'll find the
 * most significant bit set if the original value was negative.
 *
 * Note that two's complement is mandatory for exact-width types
 * according to the C99 standard. */
static int64_t signExtendBitfield(uint64_t value, uint64_t bits) {
    union {uint64_t u; int64_t i;} conv;
    conv.u = value;

    /* If the top significant bit is 1, propagate it to all the
     * higher bits for two's complement representation of signed
     * integers. */
    if (bits < 64 && (conv.i & ((uint64_t)1 << (bits-1))))
        conv.i |= ((uint64_t)-1) << bits;
    return conv.i;
}

int64_t getSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits) {
    return signExtendBitfield(getUnsignedBitfield(p,offset,bits),bits);
}

/* Roaring counterparts of get/setSignedBitfield(). The unsigned ones are
 * bitroarGetUnsignedBitfield() and bitroarSetUnsignedBitfield(), the latter
 * returning C_ERR when the value cannot represent the write. */
static int64_t bitroarGetSignedBitfield(const robj *o, uint64_t offset, uint64_t bits) {
    return signExtendBitfield(bitroarGetUnsignedBitfield(o,offset,bits),bits);
}

static int bitroarSetSignedBitfield(robj *o, uint64_t offset, uint64_t bits, int64_t value) {
    uint64_t uv = value; /* Casting adds UINT64_MAX + 1 when negative. */
    return bitroarSetUnsignedBitfield(o,offset,bits,uv);
}

/* The following two functions detect overflow of a value in the context
 * of storing it as an unsigned or signed integer with the specified
 * number of bits. The functions both take the value and a possible increment.
 * If no overflow could happen and the value+increment fit inside the limits,
 * then zero is returned, otherwise in case of overflow, 1 is returned,
 * otherwise in case of underflow, -1 is returned.
 *
 * When non-zero is returned (overflow or underflow), if not NULL, *limit is
 * set to the value the operation should result when an overflow happens,
 * depending on the specified overflow semantics:
 *
 * For BFOVERFLOW_SAT if 1 is returned, *limit it is set maximum value that
 * you can store in that integer. when -1 is returned, *limit is set to the
 * minimum value that an integer of that size can represent.
 *
 * For BFOVERFLOW_WRAP *limit is set by performing the operation in order to
 * "wrap" around towards zero for unsigned integers, or towards the most
 * negative number that is possible to represent for signed integers. */

#define BFOVERFLOW_WRAP 0
#define BFOVERFLOW_SAT 1
#define BFOVERFLOW_FAIL 2 /* Used by the BITFIELD command implementation. */

int checkUnsignedBitfieldOverflow(uint64_t value, int64_t incr, uint64_t bits, int owtype, uint64_t *limit) {
    uint64_t max = (bits == 64) ? UINT64_MAX : (((uint64_t)1<<bits)-1);
    int64_t maxincr = max-value;
    int64_t minincr = -value;

    if (value > max || (incr > 0 && incr > maxincr)) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = max;
            }
        }
        return 1;
    } else if (incr < 0 && incr < minincr) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = 0;
            }
        }
        return -1;
    }
    return 0;

handle_wrap:
    {
        uint64_t mask = ((uint64_t)-1) << bits;
        uint64_t res = value+incr;

        res &= ~mask;
        *limit = res;
    }
    return 1;
}

int checkSignedBitfieldOverflow(int64_t value, int64_t incr, uint64_t bits, int owtype, int64_t *limit) {
    int64_t max = (bits == 64) ? INT64_MAX : (((int64_t)1<<(bits-1))-1);
    int64_t min = (-max)-1;

    /* Note that maxincr and minincr could overflow, but we use the values
     * only after checking 'value' range, so when we use it no overflow
     * happens. 'uint64_t' casts are there just to prevent undefined behavior on
     * overflow */
    int64_t maxincr = (uint64_t)max-value;
    int64_t minincr = (uint64_t)min-value;

    if (value > max || (bits != 64 && incr > maxincr) || (value >= 0 && incr > 0 && incr > maxincr))
    {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = max;
            }
        }
        return 1;
    } else if (value < min || (bits != 64 && incr < minincr) || (value < 0 && incr < 0 && incr < minincr)) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = min;
            }
        }
        return -1;
    }
    return 0;

handle_wrap:
    {
        uint64_t msb = (uint64_t)1 << (bits-1);
        uint64_t a = value, b = incr, c;
        c = a+b; /* Perform addition as unsigned so that's defined. */

        /* If the sign bit is set, propagate to all the higher order
         * bits, to cap the negative value. If it's clear, mask to
         * the positive integer limit. */
        if (bits < 64) {
            uint64_t mask = ((uint64_t)-1) << bits;
            if (c & msb) {
                c |= mask;
            } else {
                c &= ~mask;
            }
        }
        *limit = c;
    }
    return 1;
}

/* Debugging function. Just show bits in the specified bitmap. Not used
 * but here for not having to rewrite it when debugging is needed. */
void printBits(unsigned char *p, unsigned long count) {
    unsigned long j, i, byte;

    for (j = 0; j < count; j++) {
        byte = p[j];
        for (i = 0x80; i > 0; i /= 2)
            printf("%c", (byte & i) ? '1' : '0');
        printf("|");
    }
    printf("\n");
}

/* -----------------------------------------------------------------------------
 * Bits related string commands: GETBIT, SETBIT, BITCOUNT, BITOP.
 * -------------------------------------------------------------------------- */

#define BITFIELDOP_GET 0
#define BITFIELDOP_SET 1
#define BITFIELDOP_INCRBY 2

/* This helper function used by GETBIT / SETBIT parses the bit offset argument
 * making sure an error is returned if it is negative or if it overflows
 * Redis 512 MB limit for the string value or more (server.proto_max_bulk_len).
 *
 * If the 'hash' argument is true, and 'bits is positive, then the command
 * will also parse bit offsets prefixed by "#". In such a case the offset
 * is multiplied by 'bits'. This is useful for the BITFIELD command. */
int getBitOffsetFromArgument(client *c, robj *o, uint64_t *offset, int hash, int bits) {
    long long loffset;
    char *err = "bit offset is not an integer or out of range";
    char *p = o->ptr;
    size_t plen = sdslen(p);
    int usehash = 0;

    /* Handle #<offset> form. */
    if (p[0] == '#' && hash && bits > 0) usehash = 1;

    if (string2ll(p+usehash,plen-usehash,&loffset) == 0 || loffset < 0) {
        addReplyError(c,err);
        return C_ERR;
    }

    /* Adjust the offset by 'bits' for #<offset> form. */
    if (usehash) {
        if (loffset > LLONG_MAX / bits) {
            addReplyError(c,err);
            return C_ERR;
        }
        loffset *= bits;
    }

    /* Limit offset to server.proto_max_bulk_len (512MB in bytes by default) */
    if (!mustObeyClient(c) && (loffset >> 3) >= server.proto_max_bulk_len)
    {
        addReplyError(c,err);
        return C_ERR;
    }

    *offset = loffset;
    return C_OK;
}

/* This helper function for BITFIELD parses a bitfield type in the form
 * <sign><bits> where sign is 'u' or 'i' for unsigned and signed, and
 * the bits is a value between 1 and 64. However 64 bits unsigned integers
 * are reported as an error because of current limitations of Redis protocol
 * to return unsigned integer values greater than INT64_MAX.
 *
 * On error C_ERR is returned and an error is sent to the client. */
int getBitfieldTypeFromArgument(client *c, robj *o, int *sign, int *bits) {
    char *p = o->ptr;
    char *err = "Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.";
    long long llbits;

    if (p[0] == 'i') {
        *sign = 1;
    } else if (p[0] == 'u') {
        *sign = 0;
    } else {
        addReplyError(c,err);
        return C_ERR;
    }

    if ((string2ll(p+1,strlen(p+1),&llbits)) == 0 ||
        llbits < 1 ||
        (*sign == 1 && llbits > 64) ||
        (*sign == 0 && llbits > 63))
    {
        addReplyError(c,err);
        return C_ERR;
    }
    *bits = llbits;
    return C_OK;
}

/* This is a helper function for commands implementations that need to write
 * bits to a string object. The command creates or pad with zeroes the string
 * so that the 'maxbit' bit can be addressed. The object is finally
 * returned. Otherwise if the key holds a wrong type NULL is returned and
 * an error is sent to the client.
 *
 * The caller provides 'o' and 'link' from a single lookupKeyWriteWithLink()
 * call, so command paths that first probe for a Roaring bitmap don't pay a
 * second keyspace lookup.
 *
 * (Must provide all the arguments to the function)
 */
static kvobj *lookupStringForBitCommand(client *c, uint64_t maxbit,
                                       kvobj *o, dictEntryLink link,
                                       size_t *strOldSize, size_t *strGrowSize)
{
    size_t byte = maxbit >> 3;
    size_t oldAllocSize = 0;
    if (checkType(c,o,OBJ_STRING)) return NULL;

    if (o == NULL) {
        o = createObject(OBJ_STRING,sdsnewlen(NULL, byte+1));
        dbAddByLink(c->db,c->argv[1],&o,&link);
        *strGrowSize = byte + 1;
        *strOldSize = 0;
    } else {
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        *strOldSize  = sdslen(o->ptr);
        if (server.memory_tracking_enabled)
            oldAllocSize = kvobjAllocSize(o);
        o->ptr = sdsgrowzero(o->ptr,byte+1);
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldAllocSize, kvobjAllocSize(o));
        *strGrowSize = sdslen(o->ptr) - *strOldSize;
    }
    return o;
}

/* Return a pointer to the string object content, and stores its length
 * in 'len'. The user is required to pass (likely stack allocated) buffer
 * 'llbuf' of at least LONG_STR_SIZE bytes. Such a buffer is used in the case
 * the object is integer encoded in order to provide the representation
 * without using heap allocation.
 *
 * The function returns the pointer to the object array of bytes representing
 * the string it contains, that may be a pointer to 'llbuf' or to the
 * internal object representation. As a side effect 'len' is filled with
 * the length of such buffer.
 *
 * If the source object is NULL the function is guaranteed to return NULL
 * and set 'len' to 0. */
unsigned char *getObjectReadOnlyString(robj *o, long *len, char *llbuf) {
    serverAssert(!o || o->type == OBJ_STRING);
    unsigned char *p = NULL;

    /* Set the 'p' pointer to the string, that can be just a stack allocated
     * array if our string was integer encoded. */
    if (o && o->encoding == OBJ_ENCODING_INT) {
        p = (unsigned char*) llbuf;
        if (len) *len = ll2string(llbuf,LONG_STR_SIZE,(long)o->ptr);
    } else if (o) {
        p = (unsigned char*) o->ptr;
        if (len) *len = sdslen(o->ptr);
    } else {
        if (len) *len = 0;
    }
    return p;
}

/* Public commands create Roaring bitmaps, or convert string values they write
 * to, only when bitmap-default-roaring is enabled and never while obeying a
 * master or the AOF: the representation decision must stay a pure function
 * of replicated logical state instead of being re-derived from node-local
 * configuration. BITCONVERT carries write transitions and BITOP's optional
 * ROARING mode carries config-selected BITOP results in the replicated command
 * stream. */
static int bitroarDefaultEnabled(client *c) {
    return server.bitmap_default_roaring && !mustObeyClient(c);
}

/* Convert a string value of any encoding into a Roaring bitmap object holding
 * the same logical bits. Returns NULL beyond the internal representability
 * bound. */
static robj *bitroarFromStringObject(robj *o) {
    robj *decoded = getDecodedObject(o);
    robj *bitmap = bitroarCreateFromString((unsigned char *)decoded->ptr, sdslen(decoded->ptr));
    decrRefCount(decoded);
    return bitmap;
}

/* A 32-bit size_t cannot exceed the signed Roaring length bound. Keep the
 * comparison out of those builds so -Wtype-limits does not flag an
 * intentionally impossible condition. */
static int bitroarStringTooLarge(robj *o) {
#if SIZE_MAX > BITROAR_MAX_BYTES_RAW
    return (uint64_t)stringObjectLen(o) > BITROAR_MAX_BYTES;
#else
    UNUSED(o);
    return 0;
#endif
}

/* Return the propagation targets enabled for this invocation of call(). This
 * matters for Lua redis.set_repl() and selective RM_Call propagation: an
 * explicitly queued transition must never escape to a target suppressed for
 * its triggering command. */
static int bitroarPropagationTarget(client *c) {
    int target = PROPAGATE_NONE;

    if (c->command_call_flags & CMD_CALL_PROPAGATE_AOF)
        target |= PROPAGATE_AOF;
    if (c->command_call_flags & CMD_CALL_PROPAGATE_REPL)
        target |= PROPAGATE_REPL;
    if (c->flags & (CLIENT_PREVENT_AOF_PROP | CLIENT_MODULE_PREVENT_AOF_PROP))
        target &= ~PROPAGATE_AOF;
    if (c->flags & (CLIENT_PREVENT_REPL_PROP | CLIENT_MODULE_PREVENT_REPL_PROP))
        target &= ~PROPAGATE_REPL;
    return target;
}

/* Queue the current command before synchronous notification callbacks, while
 * retaining the exact AOF/replica target selected for this call(). */
static void bitroarPropagateCurrentCommand(client *c) {
    alsoPropagate(c->db->id, c->argv, c->argc, bitroarPropagationTarget(c));
    preventCommandPropagation(c);
}

/* Queue the narrow representation transition used by commands whose result
 * must be converted explicitly on replicas and during AOF replay. */
static void bitroarPropagateConvert(client *c, robj *key) {
    robj *argv[3];

    argv[0] = createStringObject("BITCONVERT", 10);
    argv[1] = key;
    argv[2] = createStringObject("ROARING", 7);
    alsoPropagate(c->db->id, argv, 3, bitroarPropagationTarget(c));
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
}

/* Apply BITCONVERT's local state transition without producing a command reply.
 * SETBIT and BITFIELD call this before their logical write so primary event and
 * module-callback order is identical to replaying BITCONVERT followed by the
 * original command. o and link are the caller's fresh lookupKeyWriteWithLink()
 * result for key; the caller must already have rejected other value types and
 * strings beyond the Roaring bound, so the transition itself cannot fail. This
 * matters because callers queue the conversion for propagation first: nothing
 * may error between queueing and applying it. A callback that mutates the key
 * must either run during replay too or propagate that mutation itself; keeping
 * BITCONVERT first preserves both cases and places callback-propagated writes
 * before the triggering bitmap command. Callers must re-lookup the key
 * afterwards: NOTIFY_NEW and NOTIFY_TYPE_CHANGED callbacks may replace or
 * delete it synchronously. */
static void bitroarConvertKey(client *c, robj *key, kvobj *o, dictEntryLink link) {
    serverAssert(o == NULL || o->type == OBJ_STRING);

    robj *bitmap = (o == NULL) ? bitroarCreate() : bitroarFromStringObject(o);
    serverAssert(bitmap != NULL);

    if (o == NULL) {
        dbAddByLink(c->db, key, &bitmap, &link);
        /* dbAddByLink() initializes LRU/LFU before emitting NOTIFY_NEW. Pass
         * NULL here rather than retaining a value pointer across that callback. */
        keyModified(c, c->db, key, NULL, 1);
    } else {
        dbReplaceValueWithLink(c->db, key, &bitmap, link);
        keyModified(c, c->db, key, bitmap, 1);
        notifyKeyspaceEvent(NOTIFY_TYPE_CHANGED, "type_changed", key, c->db->id);
    }
}

/* BITCONVERT is an internal propagation primitive. It changes only the value
 * representation, so replacing a string retains the key's TTL and module
 * metadata in its existing kvobj. Missing keys become empty native bitmaps;
 * native bitmaps are already in the requested representation. */
void bitconvertCommand(client *c) {
    if (strcasecmp(c->argv[2]->ptr, "ROARING")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    dictEntryLink link = NULL;
    kvobj *o = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    if (o != NULL && o->type != OBJ_STRING && o->type != OBJ_BITMAP) {
        addReplyErrorObject(c, shared.wrongtypeerr);
        return;
    }
    if (o != NULL && o->type == OBJ_BITMAP) {
        addReply(c, shared.ok);
        return;
    }
    if (o != NULL && bitroarStringTooLarge(o)) {
        addReplyError(c, "bitmap length exceeds Roaring bitmap limit");
        return;
    }

    /* A replica may have AOF enabled. Queue BITCONVERT before its local
     * callbacks there as well; automatic propagation would append it after
     * callback-generated writes and invert the replay order. */
    bitroarPropagateCurrentCommand(c);
    bitroarConvertKey(c, c->argv[1], o, link);
    server.dirty++;
    addReply(c, shared.ok);
}

/* SETBIT and BITFIELD share the decision and bookkeeping for the representation
 * a bitmap write should target. Returns C_ERR after replying to the client when
 * the Roaring representation cannot hold the write; the keyspace is left
 * untouched, which keeps multi-op BITFIELD syntax and range failures atomic.
 * On C_OK, missing and string values are marked for a transition that callers
 * apply before re-looking up the key and executing the logical write. */
static int bitroarResolveTarget(client *c, kvobj *o, uint64_t maxbit,
                                int *transition) {
    *transition = 0;

    int is_roaring = (o != NULL && o->type == OBJ_BITMAP);
    if (!is_roaring) {
        /* Only missing keys or existing strings can transition to Roaring,
         * and only when bitmap-default-roaring is on (never on a replica/AOF).
         * Any other case (other types, feature off) stays on the string path,
         * which also handles the WRONGTYPE reply. */
        int can_default = bitroarDefaultEnabled(c) && (o == NULL || o->type == OBJ_STRING);
        if (!can_default) return C_OK;
    }

    /* Parsing already enforced proto-max-bulk-len. Keep writes inside the
     * internal range used by bitmap length arithmetic as well. */
    if (!bitroarCanRepresentBit(maxbit)) {
        addReplyError(c, "bit offset is out of range");
        return C_ERR;
    }

    if (!is_roaring) {
        if (o != NULL && bitroarStringTooLarge(o)) {
            addReplyError(c, "bitmap length exceeds Roaring bitmap limit");
            return C_ERR;
        }
        /* bitmap-default-roaring yes: missing keys and string values first
         * undergo the explicit representation transition. */
        *transition = 1;
    }
    return C_OK;
}

/* SETBIT against an already installed Roaring bitmap target. */
static void setbitCommandBitmap(client *c, robj *roaring,
                                uint64_t bitoffset, long on,
                                int transitioned)
{
    uint64_t oldlen = bitroarLen(roaring);
    uint64_t byte = bitoffset >> 3;
    int changed = 0;
    int bitval;
    size_t oldAllocSize = 0;

    bitval = bitroarGetBit(roaring, bitoffset);
    if (byte >= oldlen || (!!bitval != on)) {
        if (server.memory_tracking_enabled)
            oldAllocSize = kvobjAllocSize(roaring);
        /* bitroarResolveTarget() already checked the internal bound. */
        serverAssert(bitroarSetBit(roaring, bitoffset, on) == C_OK);
        changed = 1;
    }

    if (changed) {
        /* Native SETBIT can run keyspace callbacks too. Queue it before the
         * notification unless a transition path already placed it after
         * BITCONVERT and its callbacks. This also preserves order in an
         * AOF-enabled replica applying the transition pair. */
        if ((c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP)
            bitroarPropagateCurrentCommand(c);
        if (oldlen != bitroarLen(roaring))
            updateKeysizesHist(c->db, OBJ_BITMAP, oldlen, bitroarLen(roaring));
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), roaring, oldAllocSize, kvobjAllocSize(roaring));
        keyModified(c,c->db,c->argv[1],roaring,1);
        notifyKeyspaceEvent(NOTIFY_BITMAP,"setbit",c->argv[1],c->db->id);
        server.dirty++;
    } else if (transitioned) {
        /* The representation transition itself changed the key even though
         * SETBIT left its logical value and length unchanged. */
        server.dirty++;
    }

    addReply(c, bitval ? shared.cone : shared.czero);
}

/* SETBIT key offset bitvalue */
void setbitCommand(client *c) {
    char *err = "bit is not an integer or out of range";
    uint64_t bitoffset;
    uint64_t byte;
    int bit, byteval, bitval;
    long on;

    if (getBitOffsetFromArgument(c,c->argv[2],&bitoffset,0,0) != C_OK)
        return;

    if (getLongFromObjectOrReply(c,c->argv[3],&on,err) != C_OK)
        return;

    /* Bits can only be set or cleared... */
    if (on & ~1) {
        addReplyError(c,err);
        return;
    }

    dictEntryLink link = NULL;
    kvobj *o = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    int transition;

    if (bitroarResolveTarget(c, o, bitoffset, &transition) != C_OK)
        return;

    if (transition) {
        /* Queue the conversion first, then perform that same transition
         * locally. The triggering SETBIT is queued after conversion callbacks
         * but before its own notifications below. */
        bitroarPropagateConvert(c, c->argv[1]);
        bitroarConvertKey(c, c->argv[1], o, link);

        /* Conversion callbacks may replace or delete the key. Replay observes
         * those effects before SETBIT too, so continue from a fresh lookup and
         * deliberately do not apply bitmap-default-roaring a second time. */
        link = NULL;
        o = lookupKeyWriteWithLink(c->db, c->argv[1], &link);

        if (o != NULL && checkStringOrBitmapType(c, o)) {
            /* The conversion callback replaced the key with an incompatible
             * type. Replay stops at the same error, so suppress the automatic
             * SETBIT that dirty accounting for BITCONVERT would otherwise add. */
            preventCommandPropagation(c);
            server.dirty++;
            return;
        }
        bitroarPropagateCurrentCommand(c);
    }

    if (o != NULL && o->type == OBJ_BITMAP) {
        setbitCommandBitmap(c, o, bitoffset, on, transition);
        return;
    }

    size_t strOldSize, strGrowSize;
    int created = (o == NULL);
    o = lookupStringForBitCommand(c, bitoffset, o, link,
                                  &strOldSize, &strGrowSize);
    if (o == NULL) return;

    /* Get current values */
    byte = bitoffset >> 3;
    byteval = ((uint8_t*)o->ptr)[byte];
    bit = 7 - (bitoffset & 0x7);
    bitval = byteval & (1 << bit);

    /* Either it is newly created, changed length, or the bit changes before and after.
     * Note that the bitval here is actually a decimal number.
     * So we need to use `!!` to convert it to 0 or 1 for comparison. */
    int changed = strGrowSize || (!!bitval != on);
    if (changed) {
        /* Update byte with new bit value. */
        byteval &= ~(1 << bit);
        byteval |= ((on & 0x1) << bit);
        ((uint8_t*)o->ptr)[byte] = byteval;

        /* If this is not a new key and size changed, then update the keysizes
         * histogram. Otherwise, the histogram already updated in
         * lookupStringForBitCommand() by calling dbAdd(). Use "not created"
         * rather than "old size not 0", and update before notification; see
         * #15598. */
        if (!created && strGrowSize != 0)
            updateKeysizesHist(c->db, OBJ_STRING, strOldSize, strOldSize + strGrowSize);

        keyModified(c,c->db,c->argv[1],o,1);
        notifyKeyspaceEvent(NOTIFY_STRING,"setbit",c->argv[1],c->db->id);
        server.dirty++;
    } else if (transition) {
        server.dirty++;
    }

    /* Return original value. */
    addReply(c, bitval ? shared.cone : shared.czero);
}

/* GETBIT key offset */
void getbitCommand(client *c) {
    char llbuf[32];
    uint64_t bitoffset;
    size_t byte, bit;
    size_t bitval = 0;

    if (getBitOffsetFromArgument(c,c->argv[2],&bitoffset,0,0) != C_OK)
        return;

    kvobj *kv = lookupKeyReadOrReply(c, c->argv[1], shared.czero);
    if (kv == NULL || checkStringOrBitmapType(c,kv)) return;

    /* Offsets past the end of the value read as 0, like any other unset bit. */
    byte = bitoffset >> 3;
    bit = 7 - (bitoffset & 0x7);
    if (kv->type == OBJ_BITMAP) {
        bitval = bitroarGetBit(kv, bitoffset);
    } else if (sdsEncodedObject(kv)) {
        if (byte < sdslen(kv->ptr))
            bitval = ((uint8_t*)kv->ptr)[byte] & (1 << bit);
    } else {
        if (byte < (uint64_t)ll2string(llbuf,sizeof(llbuf),(long)kv->ptr))
            bitval = llbuf[byte] & (1 << bit);
    }

    addReply(c, bitval ? shared.cone : shared.czero);
}

#ifdef HAVE_AVX2
/* Compute the given bitop operation using AVX2 intrinsics.
 * Return how many bytes were successfully processed, as AVX2 operates on
 * 256-bit registers so if `minlen` is not a multiple of 32 some of the bytes
 * will be skipped. They will be taken care for in the unoptimized loop in the
 * main bitopCommand function. */
ATTRIBUTE_TARGET_AVX2
unsigned long bitopCommandAVX(unsigned char **keys, unsigned char *res,
                              bitroarOp op, unsigned long numkeys,
                              unsigned long minlen)
{
    const unsigned long step = sizeof(__m256i);

    unsigned long i;
    unsigned long processed = 0;
    unsigned char *res_start = res;
    unsigned char *fst_key = keys[0];

    if (minlen < step) {
        return 0;
    }

    const __m256i max256 = _mm256_set1_epi64x(-1);
    const __m256i zero256 = _mm256_set1_epi64x(0);

    switch (op) {
    case BITOP_AND:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_and_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    /* Unlike other operations that do the same with all source keys
     * DIFF, DIFF1 and ANDOR all compute the disjunction of all the source keys
     * but the first one. We first store that disjunction in `lres` and later
     * compute the final operation using the first source key. */
    case BITOP_DIFF:
    case BITOP_DIFF1:
    case BITOP_ANDOR:
    case BITOP_OR:
        while (minlen >= step) {
            __m256i lres = (op == BITOP_OR) ?
                _mm256_lddqu_si256((__m256i*)(keys[0]+processed)) :
                zero256;

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_or_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_XOR:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_xor_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_NOT:
        while (minlen >= step) {
             __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));
            lres = _mm256_xor_si256(lres, max256);
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_ONE:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));
            __m256i common_bits = zero256;

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                __m256i common = _mm256_and_si256(lres, lkey);
                common_bits = _mm256_or_si256(common_bits, common);

                lres = _mm256_xor_si256(lres, lkey);
            }
            lres = _mm256_andnot_si256(common_bits, lres);
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    default:
        break;
    }

    res = res_start;
    switch (op) {
    case BITOP_DIFF:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_andnot_si256(lres, fkey);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_DIFF1:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_andnot_si256(fkey, lres);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_ANDOR:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_and_si256(fkey, lres);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    default:
        break;
    }

    return processed;
}
#endif /* HAVE_AVX2 */

#ifdef HAVE_AVX512
/* Compute the given bitop operation using AVX512 intrinsics.
 * Return how many bytes were successfully processed, as AVX512 operates on
 * 512-bit registers so if `minlen` is not a multiple of 64 some of the bytes
 * will be skipped. They will be taken care for in the unoptimized loop in the
 * main bitopCommand function. */
ATTRIBUTE_TARGET_AVX512
unsigned long bitopCommandAVX512(unsigned char **keys, unsigned char *res,
                                 bitroarOp op, unsigned long numkeys,
                                 unsigned long minlen)
{
    const unsigned long step = sizeof(__m512i);  /* 64 bytes */

    unsigned long i;
    unsigned long processed = 0;
    unsigned char *res_start = res;
    unsigned char *fst_key = keys[0];

    if (minlen < step) {
        return 0;
    }

    const __m512i max512 = _mm512_set1_epi64(-1);
    const __m512i zero512 = _mm512_set1_epi64(0);
    switch (op) {
    case BITOP_AND:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                lres = _mm512_and_si512(lres, lkey);
            }
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    /* Unlike other operations that do the same with all source keys
     * DIFF, DIFF1 and ANDOR all compute the disjunction of all the source keys
     * but the first one. We first store that disjunction in `lres` and later
     * compute the final operation using the first source key. */
    case BITOP_DIFF:
    case BITOP_DIFF1:
    case BITOP_ANDOR:
    case BITOP_OR:
        while (minlen >= step) {
            __m512i lres = (op == BITOP_OR) ?
                _mm512_loadu_si512((__m512i*)(keys[0]+processed)) :
                zero512;

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                lres = _mm512_or_si512(lres, lkey);
            }
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_XOR:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                lres = _mm512_xor_si512(lres, lkey);
            }
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_NOT:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));
            lres = _mm512_xor_si512(lres, max512);
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_ONE:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));
            __m512i common_bits = zero512;

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                /* common_bits |= (lres & lkey): ternary-logic with imm8 0xEA == c|(a&b)
                 * (a=lres, b=lkey, c=common_bits), replacing a separate AND+OR. */
                common_bits = _mm512_ternarylogic_epi32(lres, lkey, common_bits, 0xEA);

                lres = _mm512_xor_si512(lres, lkey);
            }
            lres = _mm512_andnot_si512(common_bits, lres);
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    default:
        break;
    }

    res = res_start;
    switch (op) {
    case BITOP_DIFF:
        for (i = 0; i < processed; i += step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)res);
            __m512i fkey = _mm512_loadu_si512((__m512i*)fst_key);

            lres = _mm512_andnot_si512(lres, fkey);
            _mm512_storeu_si512((__m512i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_DIFF1:
        for (i = 0; i < processed; i += step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)res);
            __m512i fkey = _mm512_loadu_si512((__m512i*)fst_key);

            lres = _mm512_andnot_si512(fkey, lres);
            _mm512_storeu_si512((__m512i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_ANDOR:
        for (i = 0; i < processed; i += step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)res);
            __m512i fkey = _mm512_loadu_si512((__m512i*)fst_key);

            lres = _mm512_and_si512(fkey, lres);
            _mm512_storeu_si512((__m512i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    default:
        break;
    }

    return processed;
}
#endif /* HAVE_AVX512 */

/* Queue BITOP with an explicit ROARING result mode. The primary injects this
 * mode when bitmap-default-roaring selects a native result from string-only
 * sources, so replicas and AOF replay do not consult their local config. */
static void bitroarPropagateBitopWithMode(client *c) {
    robj **argv = zmalloc(sizeof(*argv) * (c->argc + 1));
    int target = bitroarPropagationTarget(c);

    argv[0] = c->argv[0];
    argv[1] = c->argv[1];
    argv[2] = createStringObject("ROARING", 7);
    for (int i = 2; i < c->argc; i++)
        argv[i + 1] = c->argv[i];

    alsoPropagate(c->db->id, argv, c->argc + 1, target);
    preventCommandPropagation(c);
    decrRefCount(argv[2]);
    zfree(argv);
}

/* BITOP whose result is a Roaring bitmap. Sources come from
 * bitopCommand()'s lookup loop through objects[]. */
static void bitopCommandBitmap(client *c, bitroarOp op, robj *targetkey,
                               robj **objects, unsigned long numkeys,
                               uint64_t maxlen, int propagate_mode)
{
    robj *res_bitmap = NULL;

    /* Only borrowed Roaring sources can encode many missing logical chunks in
     * little resident memory. Bound that allocation amplification without
     * rejecting dense sources solely because their byte length is large. */
    if (op == BITOP_NOT && objects[0] != NULL &&
        objects[0]->type == OBJ_BITMAP &&
        !bitroarBitopNotWithinMissingChunkLimit(objects[0]))
    {
        addReplyErrorFormat(c,
            "BITOP NOT would materialize more than %llu missing Roaring chunks",
            (unsigned long long)BITROAR_BITOP_NOT_MAX_MISSING_CHUNKS);
        return;
    }

    if (maxlen)
        res_bitmap = bitroarApplyOp(op, objects, numkeys, maxlen);
    if (maxlen && res_bitmap == NULL) {
        addReplyError(c, "BITOP failed allocating the result, out of memory");
        return;
    }

    /* Store the computed value into the target key. Queue the command before
     * notifications on every native path, including mode-forced replay on a
     * replica with AOF enabled. */
    if (maxlen) {
        if (propagate_mode)
            bitroarPropagateBitopWithMode(c);
        else
            bitroarPropagateCurrentCommand(c);
        setKey(c, c->db, targetkey, &res_bitmap, 0);
        server.dirty++;
        notifyKeyspaceEvent(NOTIFY_BITMAP,"set",targetkey,c->db->id);
    } else if (lookupKeyWrite(c->db, targetkey) != NULL) {
        if (propagate_mode)
            bitroarPropagateBitopWithMode(c);
        else
            bitroarPropagateCurrentCommand(c);
        serverAssert(dbDelete(c->db,targetkey));
        keyModified(c,c->db,targetkey,NULL,1);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",targetkey,c->db->id);
        server.dirty++;
    }
    serverAssert(maxlen <= (uint64_t)LLONG_MAX);
    addReplyLongLong(c,(long long)maxlen); /* Return the destination length in bytes. */
}

/* BITOP op_name [ROARING] target_key src_key1 src_key2 src_key3 ... src_keyN */
REDIS_NO_SANITIZE("alignment")
void bitopCommand(client *c) {
    char *opname = c->argv[1]->ptr;
    int force_roaring = c->argc > 2 && !strcasecmp(c->argv[2]->ptr, "ROARING");
    int targetkey_index = force_roaring ? 3 : 2;
    int first_source_index = targetkey_index + 1;
    if (c->argc <= first_source_index) {
        addReplyErrorArity(c);
        return;
    }
    robj *targetkey = c->argv[targetkey_index];
    bitroarOp op;
    unsigned long j, numkeys;
    robj **objects;      /* Array of source objects. */
    unsigned char **src; /* Array of source strings pointers. */
    size_t *len;         /* Array of length of src strings. */
    uint64_t maxlen = 0; /* Max len among the input keys. */
    size_t minlen = 0;   /* Min len among the input keys. */
    unsigned char *res = NULL; /* Resulting string. */
    int has_roaring_bitmap = 0;

    /* Parse the operation name. */
    if ((opname[0] == 'a' || opname[0] == 'A') && !strcasecmp(opname,"and"))
        op = BITOP_AND;
    else if((opname[0] == 'o' || opname[0] == 'O') && !strcasecmp(opname,"or"))
        op = BITOP_OR;
    else if((opname[0] == 'x' || opname[0] == 'X') && !strcasecmp(opname,"xor"))
        op = BITOP_XOR;
    else if((opname[0] == 'n' || opname[0] == 'N') && !strcasecmp(opname,"not"))
        op = BITOP_NOT;
    else if ((opname[0] == 'd' || opname[0] == 'D') && !strcasecmp(opname,"diff"))
        op = BITOP_DIFF;
    else if ((opname[0] == 'd' || opname[0] == 'D') && !strcasecmp(opname,"diff1"))
        op = BITOP_DIFF1;
    else if ((opname[0] == 'a' || opname[0] == 'A') && !strcasecmp(opname,"andor"))
        op = BITOP_ANDOR;
    else if ((opname[0] == 'o' || opname[0] == 'O') && !strcasecmp(opname,"one"))
        op = BITOP_ONE;
    else {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Sanity check: NOT accepts only a single key argument. */
    if (op == BITOP_NOT && c->argc != first_source_index + 1) {
        addReplyError(c,"BITOP NOT must be called with a single source key.");
        return;
    }

    if ((op == BITOP_DIFF || op == BITOP_DIFF1 || op == BITOP_ANDOR) &&
        c->argc < first_source_index + 2)
    {
        sds opname_upper = sdsnew(opname);
        sdstoupper(opname_upper);
        addReplyErrorFormat(c,"BITOP %s must be called with at least two source keys.", opname_upper);
        sdsfree(opname_upper);
        return;
    }

    /* Lookup keys, and store pointers to the string objects into an array. */
    numkeys = c->argc - first_source_index;
    src = zmalloc(sizeof(unsigned char*) * numkeys);
    len = zmalloc(sizeof(size_t) * numkeys);
    objects = zmalloc(sizeof(robj*) * numkeys);
    for (j = 0; j < numkeys; j++) {
        kvobj *kv = lookupKeyRead(c->db, c->argv[j + first_source_index]);
        /* Handle non-existing keys as empty strings. */
        if (kv == NULL) {
            objects[j] = NULL;
            src[j] = NULL;
            len[j] = 0;
            minlen = 0;
            continue;
        }
        /* Return an error if one of the keys is not a string or a Roaring bitmap. */
        if (checkStringOrBitmapType(c, kv)) {
            unsigned long i;
            for (i = 0; i < j; i++) {
                if (objects[i] && objects[i]->type == OBJ_STRING)
                    decrRefCount(objects[i]);
            }
            zfree(src);
            zfree(len);
            zfree(objects);
            return;
        }
        if (kv->type == OBJ_BITMAP) {
            /* Roaring operands are borrowed from the keyspace and consumed by
             * the Roaring path below through objects[]; len[] holds their
             * logical byte length. */
            objects[j] = kv;
            src[j] = NULL;
            len[j] = bitroarLen(kv);
            has_roaring_bitmap = 1;
        } else {
            objects[j] = getDecodedObject(kv);
            src[j] = objects[j]->ptr;
            len[j] = sdslen(objects[j]->ptr);
        }
        if (len[j] > maxlen) maxlen = len[j];
        if (j == 0 || len[j] < minlen) minlen = len[j];
    }

    /* Roaring bitmap sources, an explicit result mode, or a destination selected
     * by bitmap-default-roaring take the dedicated path. A config-only decision
     * is propagated by injecting the ROARING mode into the ordinary BITOP. */
    int mode_forced = maxlen && !has_roaring_bitmap &&
                      (force_roaring || bitroarDefaultEnabled(c));
    if (has_roaring_bitmap || mode_forced) {
        bitopCommandBitmap(c, op, targetkey, objects, numkeys,
                           maxlen, mode_forced && !force_roaring);
        for (j = 0; j < numkeys; j++) {
            /* Borrowed Roaring operands may be freed by now: storing the
             * result overwrites (or deletes) the target key, which can be
             * one of the sources, and the store notifications can delete the
             * others. Owned decoded strings hold a reference and are told
             * apart by their src[] pointer, without dereferencing objects[]. */
            if (src[j]) decrRefCount(objects[j]);
        }
        zfree(src);
        zfree(len);
        zfree(objects);
        return;
    }

    /* Compute the bit operation, if at least one string is not empty. */
    if (maxlen) {
        /* The dense result is one byte per source byte and can exceed
         * proto-max-bulk-len when a source was created under a larger limit
         * (or loaded from RDB/replication). Allocate it with a try-variant so
         * an oversized BITOP fails with an OOM error instead of aborting,
         * rather than being rejected outright. */
        res = (unsigned char*) sdstrynewlen(NULL,maxlen);
        if (res == NULL) {
            addReplyError(c, "BITOP failed allocating the result, out of memory");
            for (j = 0; j < numkeys; j++) {
                if (objects[j])
                    decrRefCount(objects[j]);
            }
            zfree(src);
            zfree(len);
            zfree(objects);
            return;
        }
        unsigned char output, byte, disjunction, common_bits;
        unsigned long i;
        int useAVX = 0;

        /* Number of bytes processed from each source key */
        j = 0;

#if defined(HAVE_AVX512)
        if (BITOP_USE_AVX512 && (minlen >= 10000) && (numkeys >= 8)) {
            j = bitopCommandAVX512(src, res, op, numkeys, minlen);

            serverAssert(minlen >= j);
            minlen -= j;

            useAVX = 1;
        }
#endif

#if defined(HAVE_AVX2)
        if (!useAVX && BITOP_USE_AVX2) {
            j = bitopCommandAVX(src, res, op, numkeys, minlen);

            serverAssert(minlen >= j);
            minlen -= j;

            useAVX = 1;
        }
#endif

#if !defined(USE_ALIGNED_ACCESS)
        /* If no SIMD path was used (no AVX2/AVX512), fall back 
         * to a word-at-a-time fast path that is still much better 
         * than the byte-by-byte loop below. On ARM we skip this since 
         * it would cause GCC to emit multiple-word load/store ops
         * not supported even on ARM >= v6. */
        if (!useAVX && minlen >= sizeof(unsigned long)*4) {

            unsigned long **lp = (unsigned long**)src;
            unsigned long *lres = (unsigned long*) res;

            /* Index over the unsigned long version of the source keys */
            size_t k = 0;

            /* Unlike other operations that do the same with all source keys
             * DIFF, DIFF1 and ANDOR all compute the disjunction of all the
             * source keys but the first one. We first store that disjunction
             * in `lres` and later compute the final operation using the first
             * source key. */
            if (op != BITOP_DIFF && op != BITOP_DIFF1 && op != BITOP_ANDOR)
                memcpy(lres,src[0],minlen);

            /* Different branches per different operations for speed (sorry). */
            if (op == BITOP_AND) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] &= lp[i][k+0];
                        lres[1] &= lp[i][k+1];
                        lres[2] &= lp[i][k+2];
                        lres[3] &= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_OR) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] |= lp[i][k+0];
                        lres[1] |= lp[i][k+1];
                        lres[2] |= lp[i][k+2];
                        lres[3] |= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_XOR) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] ^= lp[i][k+0];
                        lres[1] ^= lp[i][k+1];
                        lres[2] ^= lp[i][k+2];
                        lres[3] ^= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_NOT) {
                while(minlen >= sizeof(unsigned long)*4) {
                    lres[0] = ~lres[0];
                    lres[1] = ~lres[1];
                    lres[2] = ~lres[2];
                    lres[3] = ~lres[3];
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_DIFF || op == BITOP_DIFF1 || op == BITOP_ANDOR) {
                size_t processed = 0;
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] |= lp[i][k+0];
                        lres[1] |= lp[i][k+1];
                        lres[2] |= lp[i][k+2];
                        lres[3] |= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                    processed += sizeof(unsigned long)*4;
                }

                lres = (unsigned long*) res;
                unsigned long *first_key = (unsigned long*)src[0];
                switch (op) {
                case BITOP_DIFF:
                    for (i = 0; i < processed; i += sizeof(unsigned long)*4) {
                        lres[0] = (first_key[0] & ~lres[0]);
                        lres[1] = (first_key[1] & ~lres[1]);
                        lres[2] = (first_key[2] & ~lres[2]);
                        lres[3] = (first_key[3] & ~lres[3]);
                        lres+=4;
                        first_key += 4;
                    }
                    break;
                case BITOP_DIFF1:
                    for (i = 0; i < processed; i += sizeof(unsigned long)*4) {
                        lres[0] = (~first_key[0] & lres[0]);
                        lres[1] = (~first_key[1] & lres[1]);
                        lres[2] = (~first_key[2] & lres[2]);
                        lres[3] = (~first_key[3] & lres[3]);
                        lres+=4;
                        first_key += 4;
                    }
                    break;
                case BITOP_ANDOR:
                    for (i = 0; i < processed; i += sizeof(unsigned long)*4) {
                        lres[0] = (first_key[0] & lres[0]);
                        lres[1] = (first_key[1] & lres[1]);
                        lres[2] = (first_key[2] & lres[2]);
                        lres[3] = (first_key[3] & lres[3]);
                        lres+=4;
                        first_key += 4;
                    }
                    break;
                default:
                    break;
                }
            } else if (op == BITOP_ONE) {
                unsigned long lcommon_bits[4];

                while(minlen >= sizeof(unsigned long)*4) {
                    memset(lcommon_bits, 0, sizeof(lcommon_bits));

                    for (i = 1; i < numkeys; i++) {
                        lcommon_bits[0] |= (lres[0] & lp[i][k+0]);
                        lcommon_bits[1] |= (lres[1] & lp[i][k+1]);
                        lcommon_bits[2] |= (lres[2] & lp[i][k+2]);
                        lcommon_bits[3] |= (lres[3] & lp[i][k+3]);

                        lres[0] ^= lp[i][k+0];
                        lres[1] ^= lp[i][k+1];
                        lres[2] ^= lp[i][k+2];
                        lres[3] ^= lp[i][k+3];
                    }

                    lres[0] &= ~lcommon_bits[0];
                    lres[1] &= ~lcommon_bits[1];
                    lres[2] &= ~lcommon_bits[2];
                    lres[3] &= ~lcommon_bits[3];

                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            }
        }
#endif /* !defined(USE_ALIGNED_ACCESS) */

        /* j is set to the next byte to process by the previous loop. */
        for (; j < maxlen; j++) {
            output = (len[0] <= j) ? 0 : src[0][j];
            if (op == BITOP_NOT) output = ~output;
            disjunction = 0;
            common_bits = 0;

            for (i = 1; i < numkeys; i++) {
                int skip = 0;
                byte = (len[i] <= j) ? 0 : src[i][j];
                switch(op) {
                case BITOP_AND:
                    output &= byte;
                    skip = (output == 0);
                    break;
                case BITOP_OR:
                    output |= byte;
                    skip = (output == 0xff);
                    break;
                case BITOP_XOR: output ^= byte; break;

                /* For DIFF, DIFF1 and ANDOR we compute the disjunction of all
                 * key arguments except the first one. After that we do their
                 * respective bit op on said first arg and that disjunction.
                 * */
                case BITOP_DIFF:
                case BITOP_DIFF1:
                case BITOP_ANDOR:
                    disjunction |= byte;
                    skip = (disjunction == 0xff);
                    break;

                /* BITOP ONE dest key_1 [key_2...]
                 * If dest[i] is the i-th bit of dest then:
                 * dest[i] == 1 if and only if there is j such that key_j[i] == 1
                 * and key_n[i] == 0 for all n != j.
                 *
                 * In order to compute that on each step we track which bits
                 * were seen in more than one key and store that in a helper
                 * variable. Then the operation is just XOR but on each step we
                 * nullify the bits that are set in the helper.
                 * Logically, this operation is the same as nullifying the
                 * helper bits only once at the end, but performance-wise it had
                 * no significant benefit and makes the code only more unclear.
                 *
                 * e.g:
                 * 0001 0111 # key1
                 * 0010 0110 # key2
                 *
                 * 0011 0001 # intermediate1
                 * 0000 0110 # helper
                 * 0011 0001 # intermediate1 & ~helper
                 *
                 * 0100 1101 # key3
                 *
                 * 0111 1100 # intermediate2
                 * 0000 0111 # helper
                 * 0111 1000 # intermediate2 & ~helper
                 * ---------
                 * 0111 1000 # result
                 * */
                case BITOP_ONE:
                    common_bits |= (output & byte);
                    output ^= byte;
                    output &= ~common_bits;
                    skip = (common_bits == 0xff);
                    break;
                default:
                    break;
                }

                if (skip) {
                    break;
                }
            }

            switch(op) {
            case BITOP_DIFF:
                res[j] = (output & ~disjunction);
                break;
            case BITOP_DIFF1:
                res[j] = (~output & disjunction);
                break;
            case BITOP_ANDOR:
                res[j] = (output & disjunction);
                break;
            default:
                res[j] = output;
                break;
            }
        }
    }
    for (j = 0; j < numkeys; j++) {
        if (objects[j])
            decrRefCount(objects[j]);
    }
    zfree(src);
    zfree(len);
    zfree(objects);

    /* Store the computed value into the target key */
    if (maxlen) {
        robj *o = createObject(OBJ_STRING, res);
        setKey(c, c->db, targetkey, &o, 0);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",targetkey,c->db->id);
        server.dirty++;
    } else if (dbDelete(c->db,targetkey)) {
        keyModified(c,c->db,targetkey,NULL,1);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",targetkey,c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c,(long long)maxlen); /* Return the output string length in bytes. */
}

typedef struct bitRange {
    /* Raw START/END arguments normalized to byte or bit coordinates. */
    long long start;
    long long end;
    /* Half-open bit interval used by Roaring bitmap range operations. */
    uint64_t bit_start;
    uint64_t bit_end;
    /* Bits to ignore in the first/last byte for byte-backed BITPOS ranges. */
    unsigned char first_byte_neg_mask;
    unsigned char last_byte_neg_mask;
} bitRange;

static void normalizeBitRange(bitRange *range, long long strlen, int isbit) {
    long long totlen = strlen;

    serverAssert(totlen <= LLONG_MAX >> 3);
    if (isbit) totlen <<= 3;

    if (range->start < 0) range->start = totlen+range->start;
    if (range->end < 0) range->end = totlen+range->end;
    if (range->start < 0) range->start = 0;
    if (range->end < 0) range->end = 0;
    if (range->end >= totlen) range->end = totlen-1;

    range->first_byte_neg_mask = 0;
    range->last_byte_neg_mask = 0;
    range->bit_start = 0;
    range->bit_end = 0;
    if (range->start > range->end) return;

    if (isbit) {
        range->bit_start = (uint64_t)range->start;
        range->bit_end = (uint64_t)range->end + 1;
        range->first_byte_neg_mask = ~((1<<(8-(range->start&7)))-1) & 0xFF;
        range->last_byte_neg_mask = (1<<(7-(range->end&7)))-1;
        range->start >>= 3;
        range->end >>= 3;
    } else {
        range->bit_start = (uint64_t)range->start << 3;
        range->bit_end = ((uint64_t)range->end + 1) << 3;
    }
}

/* BITCOUNT key [start end [BIT|BYTE]] */
void bitcountCommand(client *c) {
    kvobj *o;
    long long strlen;
    long slen;
    unsigned char *p;
    char llbuf[LONG_STR_SIZE];
    bitRange range;
    int isbit = 0;

    /* Parse start/end range if any. */
    if (c->argc == 4 || c->argc == 5) {
        if (getLongLongFromObjectOrReply(c,c->argv[2],&range.start,NULL) != C_OK)
            return;
        if (getLongLongFromObjectOrReply(c,c->argv[3],&range.end,NULL) != C_OK)
            return;
        if (c->argc == 5) {
            if (!strcasecmp(c->argv[4]->ptr,"bit")) isbit = 1;
            else if (!strcasecmp(c->argv[4]->ptr,"byte")) isbit = 0;
            else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }
        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkStringOrBitmapType(c, o)) return;
        if (o != NULL && o->type == OBJ_BITMAP) {
            strlen = (long long)bitroarLen(o);
            p = NULL;
        } else {
            p = getObjectReadOnlyString(o,&slen,llbuf);
            strlen = slen;
        }
        if (range.start < 0 && range.end < 0 && range.start > range.end) {
            addReply(c,shared.czero);
            return;
        }
        normalizeBitRange(&range, strlen, isbit);

        if (o != NULL && o->type == OBJ_BITMAP) {
            if (range.start > range.end) {
                addReply(c,shared.czero);
            } else {
                addReplyLongLong(c,bitroarRangeCardinality(o,range.bit_start,range.bit_end));
            }
            return;
        }
    } else if (c->argc == 2) {
        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkStringOrBitmapType(c, o)) return;
        if (o == NULL) {
            addReply(c, shared.czero);
            return;
        }
        if (o->type == OBJ_BITMAP) {
            addReplyLongLong(c,bitroarCardinality(o));
            return;
        }
        p = getObjectReadOnlyString(o,&slen,llbuf);
        strlen = slen;
        /* The whole string. */
        range.start = 0;
        range.end = strlen-1;
        normalizeBitRange(&range, strlen, 0);
    } else {
        /* Syntax error. */
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Return 0 for non existing keys. */
    if (o == NULL) {
        addReply(c, shared.czero);
        return;
    }

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * zero can be returned is: start > end. */
    if (range.start > range.end) {
        addReply(c,shared.czero);
    } else {
        long bytes = (long)(range.end-range.start+1);
        long long count;

        /* Use the best available popcount implementation */
        count = redisPopcountAuto(p+range.start, bytes);

        if (range.first_byte_neg_mask != 0 || range.last_byte_neg_mask != 0) {
            unsigned char firstlast[2] = {0, 0};
            /* We may count bits of first byte and last byte which are out of
            * range. So we need to subtract them. Here we use a trick. We set
            * bits in the range to zero. So these bit will not be excluded. */
            if (range.first_byte_neg_mask != 0) firstlast[0] = p[range.start] & range.first_byte_neg_mask;
            if (range.last_byte_neg_mask != 0) firstlast[1] = p[range.end] & range.last_byte_neg_mask;

            /* Use the same popcount implementation for consistency */
            count -= redisPopcountAuto(firstlast, 2);
        }
        addReplyLongLong(c,count);
    }
}

/* BITPOS key bit [start [end [BIT|BYTE]]] */
void bitposCommand(client *c) {
    kvobj *o;
    long long start, end;
    long bit;
    long long strlen;
    long slen;
    unsigned char *p;
    char llbuf[LONG_STR_SIZE];
    bitRange range;
    int isbit = 0, end_given = 0;
    unsigned char first_byte_neg_mask = 0, last_byte_neg_mask = 0;

    /* Parse the bit argument to understand what we are looking for, set
     * or clear bits. */
    if (getLongFromObjectOrReply(c,c->argv[2],&bit,NULL) != C_OK)
        return;
    if (bit != 0 && bit != 1) {
        addReplyError(c, "The bit argument must be 1 or 0.");
        return;
    }

    /* Parse start/end range if any. */
    if (c->argc == 4 || c->argc == 5 || c->argc == 6) {
        if (getLongLongFromObjectOrReply(c,c->argv[3],&start,NULL) != C_OK)
            return;
        if (c->argc == 6) {
            if (!strcasecmp(c->argv[5]->ptr,"bit")) isbit = 1;
            else if (!strcasecmp(c->argv[5]->ptr,"byte")) isbit = 0;
            else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }
        if (c->argc >= 5) {
            if (getLongLongFromObjectOrReply(c,c->argv[4],&end,NULL) != C_OK)
                return;
            end_given = 1;
        }

        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkStringOrBitmapType(c, o)) return;
        if (o != NULL && o->type == OBJ_BITMAP) {
            strlen = (long long)bitroarLen(o);
            p = NULL;
        } else {
            p = getObjectReadOnlyString(o,&slen,llbuf);
            strlen = slen;
        }

        if (c->argc < 5) {
            if (isbit) end = (strlen<<3) + 7;
            else end = strlen-1;
        }
        range.start = start;
        range.end = end;
        normalizeBitRange(&range, strlen, isbit);
        start = range.start;
        end = range.end;
        first_byte_neg_mask = range.first_byte_neg_mask;
        last_byte_neg_mask = range.last_byte_neg_mask;
    } else if (c->argc == 3) {
        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkStringOrBitmapType(c,o)) return;
        if (o != NULL && o->type == OBJ_BITMAP) {
            strlen = (long long)bitroarLen(o);
            p = NULL;
        } else {
            p = getObjectReadOnlyString(o,&slen,llbuf);
            strlen = slen;
        }

        /* The whole string. Roaring BITPOS still needs the derived bit interval
         * populated in range.bit_start/range.bit_end. */
        start = 0;
        end = strlen-1;
        range.start = start;
        range.end = end;
        normalizeBitRange(&range, strlen, 0);
        start = range.start;
        end = range.end;
    } else {
        /* Syntax error. */
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* If the key does not exist, from our point of view it is an infinite
     * array of 0 bits. If the user is looking for the first clear bit return 0,
     * If the user is looking for the first set bit, return -1. */
    if (o == NULL) {
        addReplyLongLong(c, bit ? -1 : 0);
        return;
    }

    if (o->type == OBJ_BITMAP) {
        if (range.start > range.end)
            addReplyLongLong(c, -1);
        else
            addReplyLongLong(c, bitroarBitpos(o, bit, range.bit_start,
                                                   range.bit_end, end_given));
        return;
    }

    /* For empty ranges (start > end) we return -1 as an empty range does
     * not contain a 0 nor a 1. */
    if (start > end) {
        addReplyLongLong(c, -1);
    } else {
        long bytes = end-start+1;
        long long pos;
        unsigned char tmpchar;
        if (first_byte_neg_mask) {
            if (bit) tmpchar = p[start] & ~first_byte_neg_mask;
            else tmpchar = p[start] | first_byte_neg_mask;
            /* Special case, there is only one byte */
            if (last_byte_neg_mask && bytes == 1) {
                if (bit) tmpchar = tmpchar & ~last_byte_neg_mask;
                else tmpchar = tmpchar | last_byte_neg_mask;
            }
            pos = redisBitpos(&tmpchar,1,bit);
            /* If there are no more bytes or we get valid pos, we can exit early */
            if (bytes == 1 || (pos != -1 && pos != 8)) goto result;
            start++;
            bytes--;
        }
        /* If the last byte has not bits in the range, we should exclude it */
        long curbytes = bytes - (last_byte_neg_mask ? 1 : 0);
        if (curbytes > 0) {
            pos = redisBitpos(p+start,curbytes,bit);
            /* If there is no more bytes or we get valid pos, we can exit early */
            if (bytes == curbytes || (pos != -1 && pos != (long long)curbytes<<3)) goto result;
            start += curbytes;
            bytes -= curbytes;
        }
        if (bit) tmpchar = p[end] & ~last_byte_neg_mask;
        else tmpchar = p[end] | last_byte_neg_mask;
        pos = redisBitpos(&tmpchar,1,bit);

    result:
        /* If we are looking for clear bits, and the user specified an exact
         * range with start-end, we can't consider the right of the range as
         * zero padded (as we do when no explicit end is given).
         *
         * So if redisBitpos() returns the first bit outside the range,
         * we return -1 to the caller, to mean, in the specified range there
         * is not a single "0" bit. */
        if (end_given && bit == 0 && pos == (long long)bytes<<3) {
            addReplyLongLong(c,-1);
            return;
        }
        if (pos != -1) pos += (long long)start<<3; /* Adjust for the bytes we skipped. */
        addReplyLongLong(c,pos);
    }
}

/* BITFIELD key subcommand-1 arg ... subcommand-2 arg ... subcommand-N ...
 *
 * Supported subcommands:
 *
 * GET <type> <offset>
 * SET <type> <offset> <value>
 * INCRBY <type> <offset> <increment>
 * OVERFLOW [WRAP|SAT|FAIL]
 */

#define BITFIELD_FLAG_NONE      0
#define BITFIELD_FLAG_READONLY  (1<<0)

struct bitfieldOp {
    uint64_t offset;    /* Bitfield offset. */
    int64_t i64;        /* Increment amount (INCRBY) or SET value */
    int opcode;         /* Operation id. */
    int owtype;         /* Overflow type to use. */
    int bits;           /* Integer bitfield bits width. */
    int sign;           /* True if signed, otherwise unsigned op. */
};

/* Compute the value a signed SET or INCRBY op has to store and the value it
 * has to reply, starting from the current value 'oldval'. Returns C_ERR when
 * the op overflows and OVERFLOW FAIL was requested, in which case the caller
 * must reply NULL and leave the value untouched.
 *
 * This is shared by the string and the Roaring executors: the arithmetic is the
 * same, only the accessors used to read and write the bitfield differ. */
static int bitfieldSignedResult(struct bitfieldOp *op, int64_t oldval,
                                int64_t *newval, int64_t *retval)
{
    /* OVERFLOW FAIL leaves wrapped untouched, though the caller discards newval. */
    int64_t wrapped = 0;
    int overflow;

    if (op->opcode == BITFIELDOP_INCRBY) {
        overflow = checkSignedBitfieldOverflow(oldval,op->i64,op->bits,
                                              op->owtype,&wrapped);
        *newval = overflow ? wrapped : oldval + op->i64;
        *retval = *newval;
    } else {
        /* SET returns the previous value, so we need fetch & store as well. */
        *newval = op->i64;
        overflow = checkSignedBitfieldOverflow(*newval,0,op->bits,
                                              op->owtype,&wrapped);
        if (overflow) *newval = wrapped;
        *retval = oldval;
    }

    return (overflow && op->owtype == BFOVERFLOW_FAIL) ? C_ERR : C_OK;
}

/* Unsigned counterpart of bitfieldSignedResult(). The two are kept separate
 * because the overflow checks and the variable types are different. */
static int bitfieldUnsignedResult(struct bitfieldOp *op, uint64_t oldval,
                                 uint64_t *newval, uint64_t *retval)
{
    /* OVERFLOW FAIL leaves wrapped untouched, though the caller discards newval. */
    uint64_t wrapped = 0;
    int overflow;

    if (op->opcode == BITFIELDOP_INCRBY) {
        *newval = oldval + op->i64;
        overflow = checkUnsignedBitfieldOverflow(oldval,op->i64,op->bits,
                                                op->owtype,&wrapped);
        if (overflow) *newval = wrapped;
        *retval = *newval;
    } else {
        /* SET returns the previous value, so we need fetch & store as well. */
        *newval = op->i64;
        overflow = checkUnsignedBitfieldOverflow(*newval,0,op->bits,
                                                op->owtype,&wrapped);
        if (overflow) *newval = wrapped;
        *retval = oldval;
    }

    return (overflow && op->owtype == BFOVERFLOW_FAIL) ? C_ERR : C_OK;
}

/* Parse the BITFIELD subcommands into a newly allocated array of ops, which the
 * caller owns and must free, on error as well. On C_OK '*numops' is the number
 * of parsed ops, '*readonly' tells whether all of them are GET, and
 * '*highest_write_offset' is the highest bit any write reaches, which is what
 * the value must be able to hold. On C_ERR the client was already replied. */
static int bitfieldParseOps(client *c, struct bitfieldOp **opsref, int *numops,
                            int *readonly, uint64_t *highest_write_offset)
{
    struct bitfieldOp *ops = NULL;
    int owtype = BFOVERFLOW_WRAP; /* Overflow type, sticky across the ops. */
    int res = C_ERR;

    *numops = 0;
    *readonly = 1;
    *highest_write_offset = 0;

    for (int j = 2; j < c->argc; j++) {
        int remargs = c->argc-j-1; /* Remaining args other than current. */
        char *subcmd = c->argv[j]->ptr; /* Current command name. */
        int opcode; /* Current operation code. */
        long long i64 = 0;  /* Signed SET value. */
        int sign = 0; /* Signed or unsigned type? */
        int bits = 0; /* Bitfield width in bits. */
        uint64_t bitoffset;

        if (!strcasecmp(subcmd,"get") && remargs >= 2)
            opcode = BITFIELDOP_GET;
        else if (!strcasecmp(subcmd,"set") && remargs >= 3)
            opcode = BITFIELDOP_SET;
        else if (!strcasecmp(subcmd,"incrby") && remargs >= 3)
            opcode = BITFIELDOP_INCRBY;
        else if (!strcasecmp(subcmd,"overflow") && remargs >= 1) {
            char *owtypename = c->argv[j+1]->ptr;
            j++;
            if (!strcasecmp(owtypename,"wrap"))
                owtype = BFOVERFLOW_WRAP;
            else if (!strcasecmp(owtypename,"sat"))
                owtype = BFOVERFLOW_SAT;
            else if (!strcasecmp(owtypename,"fail"))
                owtype = BFOVERFLOW_FAIL;
            else {
                addReplyError(c,"Invalid OVERFLOW type specified");
                goto cleanup;
            }
            continue;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            goto cleanup;
        }

        /* Get the type and offset arguments, common to all the ops. */
        if (getBitfieldTypeFromArgument(c,c->argv[j+1],&sign,&bits) != C_OK)
            goto cleanup;

        if (getBitOffsetFromArgument(c,c->argv[j+2],&bitoffset,1,bits) != C_OK)
            goto cleanup;

        if (opcode != BITFIELDOP_GET) {
            *readonly = 0;
            if (*highest_write_offset < bitoffset + bits - 1)
                *highest_write_offset = bitoffset + bits - 1;
            /* INCRBY and SET require another argument. */
            if (getLongLongFromObjectOrReply(c,c->argv[j+3],&i64,NULL) != C_OK)
                goto cleanup;
        }

        /* Populate the array of operations we'll process. */
        ops = zrealloc(ops,sizeof(*ops)*(*numops+1));
        ops[*numops].offset = bitoffset;
        ops[*numops].i64 = i64;
        ops[*numops].opcode = opcode;
        ops[*numops].owtype = owtype;
        ops[*numops].bits = bits;
        ops[*numops].sign = sign;
        (*numops)++;

        j += 3 - (opcode == BITFIELDOP_GET);
    }
    res = C_OK;

cleanup:
    *opsref = ops;
    return res;
}

/* -------------------------- BITFIELD on a string ------------------------- */

/* Execute the parsed ops against a string value, replying once per op, and
 * return how many of them modified the value, which the caller turns into
 * server.dirty. 'o' is NULL when the key does not exist, which can only happen
 * if every op is a GET. 'growing' tells that the value was just enlarged to
 * hold the highest written bit, in which case every write counts as a change,
 * even one storing the value that was already there. */
static int bitfieldExecuteStringOps(client *c, robj *o, struct bitfieldOp *ops,
                                    int numops, int growing)
{
    int changes = 0;

    for (int j = 0; j < numops; j++) {
        struct bitfieldOp *thisop = ops+j;

        if (thisop->opcode == BITFIELDOP_GET) {
            /* For GET we use a trick: before executing the operation
             * copy up to 9 bytes to a local buffer, so that we can easily
             * execute up to 64 bit operations that are at actual string
             * object boundaries, on a buffer guaranteed to be zero-padded. */
            unsigned char buf[9] = {0};
            unsigned char *src = NULL;
            char llbuf[LONG_STR_SIZE];
            long strlen = 0;
            uint64_t byte = thisop->offset >> 3;

            if (o != NULL) src = getObjectReadOnlyString(o,&strlen,llbuf);
            for (int i = 0; i < 9; i++) {
                if (src == NULL || i+byte >= (uint64_t)strlen) break;
                buf[i] = src[i+byte];
            }

            if (thisop->sign) {
                int64_t val = getSignedBitfield(buf,thisop->offset-(byte*8),thisop->bits);
                addReplyLongLong(c,val);
            } else {
                uint64_t val = getUnsignedBitfield(buf,thisop->offset-(byte*8),thisop->bits);
                addReplyLongLong(c,val);
            }
            continue;
        }

        /* SET and INCRBY: bitfield*Result() handles both, we only fetch the old
         * value and store the new one. Signed and unsigned need two different
         * but very similar code paths, since the set of functions to get/set
         * the integers and the used variables types are different. */
        if (thisop->sign) {
            int64_t oldval, newval, retval;

            oldval = getSignedBitfield(o->ptr,thisop->offset,thisop->bits);
            if (bitfieldSignedResult(thisop,oldval,&newval,&retval) != C_OK) {
                addReplyNull(c); /* OVERFLOW FAIL: don't write. */
                continue;
            }
            addReplyLongLong(c,retval);
            setSignedBitfield(o->ptr,thisop->offset,thisop->bits,newval);
            if (growing || oldval != newval) changes++;
        } else {
            uint64_t oldval, newval, retval;

            oldval = getUnsignedBitfield(o->ptr,thisop->offset,thisop->bits);
            if (bitfieldUnsignedResult(thisop,oldval,&newval,&retval) != C_OK) {
                addReplyNull(c); /* OVERFLOW FAIL: don't write. */
                continue;
            }
            addReplyLongLong(c,retval);
            setUnsignedBitfield(o->ptr,thisop->offset,thisop->bits,newval);
            if (growing || oldval != newval) changes++;
        }
    }

    return changes;
}

/* BITFIELD write against a string value: make room up to the highest written
 * bit, execute the ops, then update the accounting and notify. */
static void bitfieldWriteString(client *c, kvobj *o, struct bitfieldOp *ops,
                                int numops, uint64_t highest_write_offset,
                                dictEntryLink link, int transitioned)
{
    size_t oldSize = 0, growSize = 0;
    int created = (o == NULL);

    o = lookupStringForBitCommand(c,highest_write_offset,o,link,
                                  &oldSize,&growSize);
    if (o == NULL) return; /* Wrong type, or the value can't be grown. */

    addReplyArrayLen(c,numops);
    int changes = bitfieldExecuteStringOps(c,o,ops,numops,growSize != 0);

    /* Growing the value is a modification on its own, even if no op wrote. */
    if (!changes && growSize == 0) {
        if (transitioned) server.dirty++;
        return;
    }

    /* If this is not a new key and size changed, then update the
     * keysizes histogram. Otherwise, the histogram already updated
     * in lookupStringForBitCommand() by calling dbAdd(). "Not created" rather
     * than "old size not 0": see the same guard in setbitCommand() and #15598. */
    if (!created && growSize != 0)
        updateKeysizesHist(c->db,OBJ_STRING,oldSize,oldSize+growSize);

    keyModified(c,c->db,c->argv[1],o,1);
    notifyKeyspaceEvent(NOTIFY_STRING,"setbit",c->argv[1],c->db->id);
    server.dirty += changes ? changes : 1;
}

/* ---------------------- BITFIELD on a Roaring bitmap --------------------- */

/* Roaring counterpart of bitfieldExecuteStringOps(): same flow and the same
 * shared arithmetic, reading and writing through the bitroar accessors. A write
 * storing the value that is already there is skipped instead of performed, as
 * rewriting a bit may cost a container update. */
static int bitfieldExecuteRoaringOps(client *c, robj *o, struct bitfieldOp *ops,
                                     int numops, int growing)
{
    int changes = 0;

    for (int j = 0; j < numops; j++) {
        struct bitfieldOp *thisop = ops+j;

        if (thisop->opcode == BITFIELDOP_GET) {
            if (thisop->sign) {
                int64_t val = bitroarGetSignedBitfield(o,thisop->offset,thisop->bits);
                addReplyLongLong(c,val);
            } else {
                uint64_t val = bitroarGetUnsignedBitfield(o,thisop->offset,thisop->bits);
                addReplyLongLong(c,val);
            }
            continue;
        }

        /* SET and INCRBY. bitroarResolveTarget() already checked that the
         * highest written bit is representable, so the writes can't fail. */
        if (thisop->sign) {
            int64_t oldval, newval, retval;

            oldval = bitroarGetSignedBitfield(o,thisop->offset,thisop->bits);
            if (bitfieldSignedResult(thisop,oldval,&newval,&retval) != C_OK) {
                addReplyNull(c); /* OVERFLOW FAIL: don't write. */
                continue;
            }
            addReplyLongLong(c,retval);
            if (growing || oldval != newval) {
                serverAssert(bitroarSetSignedBitfield(o,thisop->offset,
                                                      thisop->bits,newval) == C_OK);
                changes++;
            }
        } else {
            uint64_t oldval, newval, retval;

            oldval = bitroarGetUnsignedBitfield(o,thisop->offset,thisop->bits);
            if (bitfieldUnsignedResult(thisop,oldval,&newval,&retval) != C_OK) {
                addReplyNull(c); /* OVERFLOW FAIL: don't write. */
                continue;
            }
            addReplyLongLong(c,retval);
            if (growing || oldval != newval) {
                serverAssert(bitroarSetUnsignedBitfield(o,thisop->offset,
                                                        thisop->bits,newval) == C_OK);
                changes++;
            }
        }
    }

    return changes;
}

/* A BITFIELD write on a string always grows the value up to the highest bit it
 * touches, even when no bit is actually set. Reproduce that on a Roaring bitmap
 * by rewriting the highest written bit with its current value, which extends
 * the tracked length without changing any content. Returns 1 if the length
 * really grew, so the caller knows the key was modified. */
static int bitfieldRoaringExtendLen(robj *o, uint64_t highest_write_offset) {
    uint64_t oldlen = bitroarLen(o);
    int bit = bitroarGetBit(o, highest_write_offset);
    serverAssert(bitroarSetBit(o, highest_write_offset, bit) == C_OK);
    return oldlen != bitroarLen(o);
}

/* BITFIELD write against an already installed Roaring bitmap: execute the ops
 * in place, then update the accounting and notify. 'transitioned' records that
 * BITCONVERT installed this value immediately before the logical write. */
static void bitfieldWriteRoaring(client *c, kvobj *o, struct bitfieldOp *ops,
                                 int numops, uint64_t highest_write_offset,
                                 int transitioned)
{
    /* Unlike a string, the Roaring value is mutated in place: nothing has to be
     * allocated upfront, we only need to know whether the value has to grow to
     * cover the highest written bit. */
    uint64_t oldSize = bitroarLen(o);
    int growing = (highest_write_offset >> 3) + 1 > oldSize;
    size_t oldAllocSize = 0;

    if (server.memory_tracking_enabled)
        oldAllocSize = kvobjAllocSize(o);

    addReplyArrayLen(c,numops);
    int changes = bitfieldExecuteRoaringOps(c,o,ops,numops,growing);
    int len_extended = growing ? bitfieldRoaringExtendLen(o,highest_write_offset) : 0;

    if (!changes && !len_extended) {
        if (transitioned) server.dirty++;
        return;
    }

    /* Place BITFIELD before synchronous notification callbacks unless the
     * transition path already queued it immediately after BITCONVERT. */
    if ((c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP)
        bitroarPropagateCurrentCommand(c);
    if (oldSize != bitroarLen(o))
        updateKeysizesHist(c->db, OBJ_BITMAP, oldSize, bitroarLen(o));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db,getKeySlot(c->argv[1]->ptr),o,
                            oldAllocSize,kvobjAllocSize(o));
    keyModified(c,c->db,c->argv[1],o,1);
    notifyKeyspaceEvent(NOTIFY_BITMAP,"setbit",c->argv[1],c->db->id);
    server.dirty += changes ? changes : 1;
}

/* ------------------------------ BITFIELD --------------------------------- */

/* GET only BITFIELD, that is every BITFIELD_RO and any BITFIELD without SET or
 * INCRBY: the keyspace is left untouched, both encodings just read. */
static void bitfieldReadOnly(client *c, struct bitfieldOp *ops, int numops) {
    /* Lookup for read is ok if key doesn't exit, but errors
     * if it's not a string or bitmap. */
    kvobj *o = lookupKeyRead(c->db,c->argv[1]);
    if (o != NULL && checkStringOrBitmapType(c,o)) return;

    addReplyArrayLen(c,numops);
    if (o != NULL && o->type == OBJ_BITMAP)
        bitfieldExecuteRoaringOps(c,o,ops,numops,0);
    else
        bitfieldExecuteStringOps(c,o,ops,numops,0); /* 'o' may be NULL. */
}

/* This implements both the BITFIELD command and the BITFIELD_RO command
 * when flags is set to BITFIELD_FLAG_READONLY: in this case only the
 * GET subcommand is allowed, other subcommands will return an error.
 *
 * Parsing the ops is shared, executing them is not: a value is either a plain
 * string or a Roaring bitmap, and each encoding has its own executor and
 * bookkeeping. */
void bitfieldGeneric(client *c, int flags) {
    struct bitfieldOp *ops;
    int numops, readonly;
    uint64_t highest_write_offset;

    if (bitfieldParseOps(c,&ops,&numops,&readonly,&highest_write_offset) != C_OK)
        goto cleanup;

    if (readonly) {
        bitfieldReadOnly(c,ops,numops);
        goto cleanup;
    }

    if (flags & BITFIELD_FLAG_READONLY) {
        addReplyError(c, "BITFIELD_RO only supports the GET subcommand");
        goto cleanup;
    }

    /* Lookup keeping the link, so that the keyspace dict is probed only once:
     * the string path grows the value through it, and the Roaring path installs
     * a created or converted value with it. */
    dictEntryLink link = NULL;
    kvobj *o = lookupKeyWriteWithLink(c->db,c->argv[1],&link);

    /* When bitmap-default-roaring is enabled, missing and string keys undergo
     * the explicit transition before the BITFIELD operations. */
    int transition;
    if (bitroarResolveTarget(c,o,highest_write_offset,&transition) != C_OK)
        goto cleanup;
    if (transition) {
        bitroarPropagateConvert(c,c->argv[1]);
        bitroarConvertKey(c,c->argv[1],o,link);

        /* Resume from the state left by synchronous conversion callbacks.
         * Replay observes those callback effects before BITFIELD too. */
        link = NULL;
        o = lookupKeyWriteWithLink(c->db,c->argv[1],&link);
        if (o != NULL && checkStringOrBitmapType(c,o)) {
            preventCommandPropagation(c);
            server.dirty++;
            goto cleanup;
        }
        bitroarPropagateCurrentCommand(c);
    }

    if (o != NULL && o->type == OBJ_BITMAP)
        bitfieldWriteRoaring(c,o,ops,numops,highest_write_offset,transition);
    else
        bitfieldWriteString(c,o,ops,numops,highest_write_offset,link,transition);

cleanup:
    zfree(ops);
}

void bitfieldCommand(client *c) {
    bitfieldGeneric(c, BITFIELD_FLAG_NONE);
}

void bitfieldroCommand(client *c) {
    bitfieldGeneric(c, BITFIELD_FLAG_READONLY);
}

#ifdef REDIS_TEST
/* Test function to verify popcount implementations */
int bitopsTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Test data with known popcount values */
    unsigned char test_data[] = {0xFF, 0x00, 0xAA, 0x55, 0xF0, 0x0F, 0x33, 0xCC};
    int expected_bits = 8 + 0 + 4 + 4 + 4 + 4 + 4 + 4; /* = 32 bits */

    long long result_regular = redisPopcount(test_data, sizeof(test_data));

    printf("Regular popcount: %lld (expected: %d)\n", result_regular, expected_bits);

    if (result_regular != expected_bits) {
        printf("FAIL: Regular popcount mismatch\n");
        return 1;
    }

#ifdef HAVE_AVX2
    if (BITOP_USE_AVX2) {
        long long result_avx2 = redisPopCountAvx2(test_data, sizeof(test_data));
        printf("AVX2 popcount: %lld (expected: %d)\n", result_avx2, expected_bits);

        if (result_avx2 != expected_bits) {
            printf("FAIL: AVX2 popcount mismatch\n");
            return 1;
        }
    } else {
        printf("AVX2 not supported on this CPU\n");
    }
#else
    printf("AVX2 not compiled in\n");
#endif

#ifdef HAVE_AVX512
    if (BITOP_USE_AVX512) {
        long long result_avx512 = redisPopCountAvx512(test_data, sizeof(test_data));
        printf("AVX512 popcount: %lld (expected: %d)\n", result_avx512, expected_bits);

        if (result_avx512 != expected_bits) {
            printf("FAIL: AVX512 popcount mismatch\n");
            return 1;
        }
    } else {
        printf("AVX512 not supported on this CPU\n");
    }
#else
    printf("AVX512 not compiled in\n");
#endif

#ifdef HAVE_AARCH64_NEON
    {
        long long result_aarch64 = redisPopCountAarch64(test_data, sizeof(test_data));
        printf("AArch64 NEON popcount: %lld (expected: %d)\n", result_aarch64, expected_bits);

        if (result_aarch64 != expected_bits) {
            printf("FAIL: AArch64 NEON popcount mismatch\n");
            return 1;
        }
    }
#else
    printf("AArch64 NEON not available\n");
#endif
    printf("All popcount tests passed!\n");
    return 0;
}
#endif
