/*
   SipHash reference C implementation

   Copyright (c) 2012-2016 Jean-Philippe Aumasson
   <jeanphilippe.aumasson@gmail.com>
   Copyright (c) 2012-2014 Daniel J. Bernstein <djb@cr.yp.to>
   Copyright (c) 2017-current Redis Ltd.

   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along
   with this software. If not, see
   <http://creativecommons.org/publicdomain/zero/1.0/>.

   ----------------------------------------------------------------------------

   This version was modified by Salvatore Sanfilippo <antirez@gmail.com>
   in the following ways:

   1. We use SipHash 1-2. This is not believed to be as strong as the
      suggested 2-4 variant, but AFAIK there are not trivial attacks
      against this reduced-rounds version, and it runs at the same speed
      as Murmurhash2 that we used previously, while the 2-4 variant slowed
      down Redis by a 4% figure more or less.
   2. Hard-code rounds in the hope the compiler can optimize it more
      in this raw form. Anyway we always want the standard 2-4 variant.
   3. Modify the prototype and implementation so that the function directly
      returns an uint64_t value, the hash itself, instead of receiving an
      output buffer. This also means that the output size is set to 8 bytes
      and the 16 bytes output code handling was removed.
   4. Provide a case insensitive variant to be used when hashing strings that
      must be considered identical by the hash table regardless of the case.
      If we don't have directly a case insensitive hash function, we need to
      perform a text transformation in some temporary buffer, which is costly.
   5. Remove debugging code.
   6. Modified the original test.c file to be a stand-alone function testing
      the function in the new form (returning an uint64_t) using just the
      relevant test vector.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Fast tolower() alike function that does not care about locale
 * but just returns a-z instead of A-Z. */
int siptlw(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c+('a'-'A');
    } else {
        return c;
    }
}

#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#define NO_SANITIZE(sanitizer) __attribute__((no_sanitize(sanitizer)))
#endif
#endif

#if !defined(NO_SANITIZE)
#define NO_SANITIZE(sanitizer)
#endif

/* Test of the CPU is Little Endian and supports not aligned accesses.
 * Two interesting conditions to speedup the function that happen to be
 * in most of x86 servers. */
#if defined(__X86_64__) || defined(__x86_64__) || defined (__i386__) \
	|| defined (__aarch64__) || defined (__arm64__) \
    || (defined(__riscv) && defined(__riscv_zicclsm))
#define UNALIGNED_LE_CPU
#endif

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define U32TO8_LE(p, v)                                                        \
    (p)[0] = (uint8_t)((v));                                                   \
    (p)[1] = (uint8_t)((v) >> 8);                                              \
    (p)[2] = (uint8_t)((v) >> 16);                                             \
    (p)[3] = (uint8_t)((v) >> 24);

#define U64TO8_LE(p, v)                                                        \
    U32TO8_LE((p), (uint32_t)((v)));                                           \
    U32TO8_LE((p) + 4, (uint32_t)((v) >> 32));

#ifdef UNALIGNED_LE_CPU
#define U8TO64_LE(p) (*((uint64_t*)(p)))
#else
#define U8TO64_LE(p)                                                           \
    (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) |                        \
     ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) |                 \
     ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) |                 \
     ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56))
#endif

#define U8TO64_LE_NOCASE(p)                                                    \
    (((uint64_t)(siptlw((p)[0]))) |                                           \
     ((uint64_t)(siptlw((p)[1])) << 8) |                                      \
     ((uint64_t)(siptlw((p)[2])) << 16) |                                     \
     ((uint64_t)(siptlw((p)[3])) << 24) |                                     \
     ((uint64_t)(siptlw((p)[4])) << 32) |                                              \
     ((uint64_t)(siptlw((p)[5])) << 40) |                                              \
     ((uint64_t)(siptlw((p)[6])) << 48) |                                              \
     ((uint64_t)(siptlw((p)[7])) << 56))

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = ROTL(v1, 13);                                                     \
        v1 ^= v0;                                                              \
        v0 = ROTL(v0, 32);                                                     \
        v2 += v3;                                                              \
        v3 = ROTL(v3, 16);                                                     \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = ROTL(v3, 21);                                                     \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = ROTL(v1, 17);                                                     \
        v1 ^= v2;                                                              \
        v2 = ROTL(v2, 32);                                                     \
    } while (0)

NO_SANITIZE("alignment")
uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k) {
#ifndef UNALIGNED_LE_CPU
    uint64_t hash;
    uint8_t *out = (uint8_t*) &hash;
#endif
    uint64_t v0 = 0x736f6d6570736575ULL;
    uint64_t v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL;
    uint64_t v3 = 0x7465646279746573ULL;
    uint64_t k0 = U8TO64_LE(k);
    uint64_t k1 = U8TO64_LE(k + 8);
    uint64_t m;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint64_t));
    const int left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    for (; in != end; in += 8) {
        m = U8TO64_LE(in);
        v3 ^= m;

        SIPROUND;

        v0 ^= m;
    }

    switch (left) {
    case 7: b |= ((uint64_t)in[6]) << 48; /* fall-thru */
    case 6: b |= ((uint64_t)in[5]) << 40; /* fall-thru */
    case 5: b |= ((uint64_t)in[4]) << 32; /* fall-thru */
    case 4: b |= ((uint64_t)in[3]) << 24; /* fall-thru */
    case 3: b |= ((uint64_t)in[2]) << 16; /* fall-thru */
    case 2: b |= ((uint64_t)in[1]) << 8; /* fall-thru */
    case 1: b |= ((uint64_t)in[0]); break;
    case 0: break;
    }

    v3 ^= b;

    SIPROUND;

    v0 ^= b;
    v2 ^= 0xff;

    SIPROUND;
    SIPROUND;

    b = v0 ^ v1 ^ v2 ^ v3;
#ifndef UNALIGNED_LE_CPU
    U64TO8_LE(out, b);
    return hash;
#else
    return b;
#endif
}

NO_SANITIZE("alignment")
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k)
{
#ifndef UNALIGNED_LE_CPU
    uint64_t hash;
    uint8_t *out = (uint8_t*) &hash;
#endif
    uint64_t v0 = 0x736f6d6570736575ULL;
    uint64_t v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL;
    uint64_t v3 = 0x7465646279746573ULL;
    uint64_t k0 = U8TO64_LE(k);
    uint64_t k1 = U8TO64_LE(k + 8);
    uint64_t m;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint64_t));
    const int left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    for (; in != end; in += 8) {
        m = U8TO64_LE_NOCASE(in);
        v3 ^= m;

        SIPROUND;

        v0 ^= m;
    }

    switch (left) {
    case 7: b |= ((uint64_t)siptlw(in[6])) << 48; /* fall-thru */
    case 6: b |= ((uint64_t)siptlw(in[5])) << 40; /* fall-thru */
    case 5: b |= ((uint64_t)siptlw(in[4])) << 32; /* fall-thru */
    case 4: b |= ((uint64_t)siptlw(in[3])) << 24; /* fall-thru */
    case 3: b |= ((uint64_t)siptlw(in[2])) << 16; /* fall-thru */
    case 2: b |= ((uint64_t)siptlw(in[1])) << 8; /* fall-thru */
    case 1: b |= ((uint64_t)siptlw(in[0])); break;
    case 0: break;
    }

    v3 ^= b;

    SIPROUND;

    v0 ^= b;
    v2 ^= 0xff;

    SIPROUND;
    SIPROUND;

    b = v0 ^ v1 ^ v2 ^ v3;
#ifndef UNALIGNED_LE_CPU
    U64TO8_LE(out, b);
    return hash;
#else
    return b;
#endif
}

/* --------- AVX-512 lane-parallel SipHash-1-2 (AMD Zen 5 "Turin") ----------- *
 *
 * siphash_x8() computes eight independent SipHash-1-2 digests at once and is
 * bit-exact with the scalar siphash() above. It targets the case where Redis
 * already needs a batch of key hashes together ΓÇö the cross-command prefetcher
 * (dictPrefetcherReset) hashes every key of a pipelined command batch in a
 * tight scalar loop. On Zen 5 that loop is a natural fit for 512-bit vectors:
 * SipHash's state is 4x uint64, and AVX-512 gives us native VPROLQ rotates,
 * 8-lane 64-bit masked gathers to fetch each lane's message words from its own
 * key pointer, and mask-blend to "freeze" lanes that have already consumed all
 * their full 8-byte blocks (keys differ in length).
 *
 * Built with a function-level target attribute so the rest of this TU stays
 * baseline-ISA; callers must gate the call on runtime AVX-512F support (see
 * dictGenHashFunctionBatch()).
 *
 * Note on the message tail: for lanes with a partial trailing block we issue a
 * full 8-byte gather at offset (len & ~7) and mask to the low (len & 7) bytes.
 * This can read up to 7 bytes past the key end; for heap-allocated sds keys
 * (always followed by a NUL terminator and allocator slack) this is safe in
 * practice. A production-hardened variant would fall back to scalar for keys
 * whose tail read would cross a page boundary. */
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

#define SIPROUND_X8(v0, v1, v2, v3)                                            \
    do {                                                                       \
        v0 = _mm512_add_epi64(v0, v1); v1 = _mm512_rol_epi64(v1, 13);          \
        v1 = _mm512_xor_si512(v1, v0); v0 = _mm512_rol_epi64(v0, 32);          \
        v2 = _mm512_add_epi64(v2, v3); v3 = _mm512_rol_epi64(v3, 16);          \
        v3 = _mm512_xor_si512(v3, v2);                                         \
        v0 = _mm512_add_epi64(v0, v3); v3 = _mm512_rol_epi64(v3, 21);          \
        v3 = _mm512_xor_si512(v3, v0);                                         \
        v2 = _mm512_add_epi64(v2, v1); v1 = _mm512_rol_epi64(v1, 17);          \
        v1 = _mm512_xor_si512(v1, v2); v2 = _mm512_rol_epi64(v2, 32);          \
    } while (0)

__attribute__((target("avx512f,avx512dq")))
void siphash_x8(const uint8_t *const in[8], const size_t inlen[8],
                const uint8_t *k, uint64_t out[8]) {
    const __m512i vk0 = _mm512_set1_epi64((long long)U8TO64_LE(k));
    const __m512i vk1 = _mm512_set1_epi64((long long)U8TO64_LE(k + 8));
    __m512i v0 = _mm512_xor_si512(_mm512_set1_epi64((long long)0x736f6d6570736575ULL), vk0);
    __m512i v1 = _mm512_xor_si512(_mm512_set1_epi64((long long)0x646f72616e646f6dULL), vk1);
    __m512i v2 = _mm512_xor_si512(_mm512_set1_epi64((long long)0x6c7967656e657261ULL), vk0);
    __m512i v3 = _mm512_xor_si512(_mm512_set1_epi64((long long)0x7465646279746573ULL), vk1);

    /* x86-64: pointers and size_t are 8 bytes, so we can load them straight
     * into 64-bit lanes. */
    __m512i vptr = _mm512_loadu_si512((const void *)in);
    __m512i vlen = _mm512_loadu_si512((const void *)inlen);
    __m512i vfull = _mm512_srli_epi64(vlen, 3);   /* number of full 8-byte blocks */

    size_t maxfull = 0;
    for (int i = 0; i < 8; i++) {
        size_t fb = inlen[i] >> 3;
        if (fb > maxfull) maxfull = fb;
    }

    for (size_t j = 0; j < maxfull; j++) {
        __mmask8 active = _mm512_cmpgt_epi64_mask(vfull, _mm512_set1_epi64((long long)j));
        __m512i addr = _mm512_add_epi64(vptr, _mm512_set1_epi64((long long)(j * 8)));
        __m512i m = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), active, addr, (const void *)0, 1);
        __m512i s0 = v0, s1 = v1, s2 = v2, s3 = v3;
        v3 = _mm512_xor_si512(v3, m);
        SIPROUND_X8(v0, v1, v2, v3);
        v0 = _mm512_xor_si512(v0, m);
        /* Lanes with no block at index j must be left untouched (SipHash rounds
         * are not idempotent): blend back their saved state. */
        v0 = _mm512_mask_blend_epi64(active, s0, v0);
        v1 = _mm512_mask_blend_epi64(active, s1, v1);
        v2 = _mm512_mask_blend_epi64(active, s2, v2);
        v3 = _mm512_mask_blend_epi64(active, s3, v3);
    }

    /* Trailing block: b = (len << 56) | last (len & 7) bytes. */
    __m512i b = _mm512_slli_epi64(vlen, 56);
    __m512i vleft = _mm512_and_si512(vlen, _mm512_set1_epi64(7));
    __mmask8 hastail = _mm512_cmpgt_epi64_mask(vleft, _mm512_setzero_si512());
    __m512i toff = _mm512_and_si512(vlen, _mm512_set1_epi64((long long)~(uint64_t)7));
    __m512i taddr = _mm512_add_epi64(vptr, toff);
    __m512i traw = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), hastail, taddr, (const void *)0, 1);
    __m512i shift = _mm512_slli_epi64(vleft, 3);  /* keep low 8*left bits */
    __m512i lomask = _mm512_sub_epi64(_mm512_sllv_epi64(_mm512_set1_epi64(1), shift), _mm512_set1_epi64(1));
    b = _mm512_or_si512(b, _mm512_and_si512(traw, lomask));

    v3 = _mm512_xor_si512(v3, b);
    SIPROUND_X8(v0, v1, v2, v3);
    v0 = _mm512_xor_si512(v0, b);
    v2 = _mm512_xor_si512(v2, _mm512_set1_epi64((long long)0xff));
    SIPROUND_X8(v0, v1, v2, v3);
    SIPROUND_X8(v0, v1, v2, v3);
    __m512i h = _mm512_xor_si512(_mm512_xor_si512(v0, v1), _mm512_xor_si512(v2, v3));
    _mm512_storeu_si512((void *)out, h);
}
#endif /* x86 */


/* --------------------------------- TEST ------------------------------------ */

#ifdef SIPHASH_TEST

const uint8_t vectors_sip64[64][8] = {
    { 0x31, 0x0e, 0x0e, 0xdd, 0x47, 0xdb, 0x6f, 0x72, },
    { 0xfd, 0x67, 0xdc, 0x93, 0xc5, 0x39, 0xf8, 0x74, },
    { 0x5a, 0x4f, 0xa9, 0xd9, 0x09, 0x80, 0x6c, 0x0d, },
    { 0x2d, 0x7e, 0xfb, 0xd7, 0x96, 0x66, 0x67, 0x85, },
    { 0xb7, 0x87, 0x71, 0x27, 0xe0, 0x94, 0x27, 0xcf, },
    { 0x8d, 0xa6, 0x99, 0xcd, 0x64, 0x55, 0x76, 0x18, },
    { 0xce, 0xe3, 0xfe, 0x58, 0x6e, 0x46, 0xc9, 0xcb, },
    { 0x37, 0xd1, 0x01, 0x8b, 0xf5, 0x00, 0x02, 0xab, },
    { 0x62, 0x24, 0x93, 0x9a, 0x79, 0xf5, 0xf5, 0x93, },
    { 0xb0, 0xe4, 0xa9, 0x0b, 0xdf, 0x82, 0x00, 0x9e, },
    { 0xf3, 0xb9, 0xdd, 0x94, 0xc5, 0xbb, 0x5d, 0x7a, },
    { 0xa7, 0xad, 0x6b, 0x22, 0x46, 0x2f, 0xb3, 0xf4, },
    { 0xfb, 0xe5, 0x0e, 0x86, 0xbc, 0x8f, 0x1e, 0x75, },
    { 0x90, 0x3d, 0x84, 0xc0, 0x27, 0x56, 0xea, 0x14, },
    { 0xee, 0xf2, 0x7a, 0x8e, 0x90, 0xca, 0x23, 0xf7, },
    { 0xe5, 0x45, 0xbe, 0x49, 0x61, 0xca, 0x29, 0xa1, },
    { 0xdb, 0x9b, 0xc2, 0x57, 0x7f, 0xcc, 0x2a, 0x3f, },
    { 0x94, 0x47, 0xbe, 0x2c, 0xf5, 0xe9, 0x9a, 0x69, },
    { 0x9c, 0xd3, 0x8d, 0x96, 0xf0, 0xb3, 0xc1, 0x4b, },
    { 0xbd, 0x61, 0x79, 0xa7, 0x1d, 0xc9, 0x6d, 0xbb, },
    { 0x98, 0xee, 0xa2, 0x1a, 0xf2, 0x5c, 0xd6, 0xbe, },
    { 0xc7, 0x67, 0x3b, 0x2e, 0xb0, 0xcb, 0xf2, 0xd0, },
    { 0x88, 0x3e, 0xa3, 0xe3, 0x95, 0x67, 0x53, 0x93, },
    { 0xc8, 0xce, 0x5c, 0xcd, 0x8c, 0x03, 0x0c, 0xa8, },
    { 0x94, 0xaf, 0x49, 0xf6, 0xc6, 0x50, 0xad, 0xb8, },
    { 0xea, 0xb8, 0x85, 0x8a, 0xde, 0x92, 0xe1, 0xbc, },
    { 0xf3, 0x15, 0xbb, 0x5b, 0xb8, 0x35, 0xd8, 0x17, },
    { 0xad, 0xcf, 0x6b, 0x07, 0x63, 0x61, 0x2e, 0x2f, },
    { 0xa5, 0xc9, 0x1d, 0xa7, 0xac, 0xaa, 0x4d, 0xde, },
    { 0x71, 0x65, 0x95, 0x87, 0x66, 0x50, 0xa2, 0xa6, },
    { 0x28, 0xef, 0x49, 0x5c, 0x53, 0xa3, 0x87, 0xad, },
    { 0x42, 0xc3, 0x41, 0xd8, 0xfa, 0x92, 0xd8, 0x32, },
    { 0xce, 0x7c, 0xf2, 0x72, 0x2f, 0x51, 0x27, 0x71, },
    { 0xe3, 0x78, 0x59, 0xf9, 0x46, 0x23, 0xf3, 0xa7, },
    { 0x38, 0x12, 0x05, 0xbb, 0x1a, 0xb0, 0xe0, 0x12, },
    { 0xae, 0x97, 0xa1, 0x0f, 0xd4, 0x34, 0xe0, 0x15, },
    { 0xb4, 0xa3, 0x15, 0x08, 0xbe, 0xff, 0x4d, 0x31, },
    { 0x81, 0x39, 0x62, 0x29, 0xf0, 0x90, 0x79, 0x02, },
    { 0x4d, 0x0c, 0xf4, 0x9e, 0xe5, 0xd4, 0xdc, 0xca, },
    { 0x5c, 0x73, 0x33, 0x6a, 0x76, 0xd8, 0xbf, 0x9a, },
    { 0xd0, 0xa7, 0x04, 0x53, 0x6b, 0xa9, 0x3e, 0x0e, },
    { 0x92, 0x59, 0x58, 0xfc, 0xd6, 0x42, 0x0c, 0xad, },
    { 0xa9, 0x15, 0xc2, 0x9b, 0xc8, 0x06, 0x73, 0x18, },
    { 0x95, 0x2b, 0x79, 0xf3, 0xbc, 0x0a, 0xa6, 0xd4, },
    { 0xf2, 0x1d, 0xf2, 0xe4, 0x1d, 0x45, 0x35, 0xf9, },
    { 0x87, 0x57, 0x75, 0x19, 0x04, 0x8f, 0x53, 0xa9, },
    { 0x10, 0xa5, 0x6c, 0xf5, 0xdf, 0xcd, 0x9a, 0xdb, },
    { 0xeb, 0x75, 0x09, 0x5c, 0xcd, 0x98, 0x6c, 0xd0, },
    { 0x51, 0xa9, 0xcb, 0x9e, 0xcb, 0xa3, 0x12, 0xe6, },
    { 0x96, 0xaf, 0xad, 0xfc, 0x2c, 0xe6, 0x66, 0xc7, },
    { 0x72, 0xfe, 0x52, 0x97, 0x5a, 0x43, 0x64, 0xee, },
    { 0x5a, 0x16, 0x45, 0xb2, 0x76, 0xd5, 0x92, 0xa1, },
    { 0xb2, 0x74, 0xcb, 0x8e, 0xbf, 0x87, 0x87, 0x0a, },
    { 0x6f, 0x9b, 0xb4, 0x20, 0x3d, 0xe7, 0xb3, 0x81, },
    { 0xea, 0xec, 0xb2, 0xa3, 0x0b, 0x22, 0xa8, 0x7f, },
    { 0x99, 0x24, 0xa4, 0x3c, 0xc1, 0x31, 0x57, 0x24, },
    { 0xbd, 0x83, 0x8d, 0x3a, 0xaf, 0xbf, 0x8d, 0xb7, },
    { 0x0b, 0x1a, 0x2a, 0x32, 0x65, 0xd5, 0x1a, 0xea, },
    { 0x13, 0x50, 0x79, 0xa3, 0x23, 0x1c, 0xe6, 0x60, },
    { 0x93, 0x2b, 0x28, 0x46, 0xe4, 0xd7, 0x06, 0x66, },
    { 0xe1, 0x91, 0x5f, 0x5c, 0xb1, 0xec, 0xa4, 0x6c, },
    { 0xf3, 0x25, 0x96, 0x5c, 0xa1, 0x6d, 0x62, 0x9f, },
    { 0x57, 0x5f, 0xf2, 0x8e, 0x60, 0x38, 0x1b, 0xe5, },
    { 0x72, 0x45, 0x06, 0xeb, 0x4c, 0x32, 0x8a, 0x95, },
};


/* Test siphash using a test vector. Returns 0 if the function passed
 * all the tests, otherwise 1 is returned.
 *
 * IMPORTANT: The test vector is for SipHash 2-4. Before running
 * the test revert back the siphash() function to 2-4 rounds since
 * now it uses 1-2 rounds. */
int siphash_test(void) {
    uint8_t in[64], k[16];
    int i;
    int fails = 0;

    for (i = 0; i < 16; ++i)
        k[i] = i;

    for (i = 0; i < 64; ++i) {
        in[i] = i;
        uint64_t hash = siphash(in, i, k);
        const uint8_t *v = NULL;
        v = (uint8_t *)vectors_sip64;
        if (memcmp(&hash, v + (i * 8), 8)) {
            /* printf("fail for %d bytes\n", i); */
            fails++;
        }
    }

    /* Run a few basic tests with the case insensitive version. */
    uint64_t h1, h2;
    h1 = siphash((uint8_t*)"hello world",11,(uint8_t*)"1234567812345678");
    h2 = siphash_nocase((uint8_t*)"hello world",11,(uint8_t*)"1234567812345678");
    if (h1 != h2) fails++;

    h1 = siphash((uint8_t*)"hello world",11,(uint8_t*)"1234567812345678");
    h2 = siphash_nocase((uint8_t*)"HELLO world",11,(uint8_t*)"1234567812345678");
    if (h1 != h2) fails++;

    h1 = siphash((uint8_t*)"HELLO world",11,(uint8_t*)"1234567812345678");
    h2 = siphash_nocase((uint8_t*)"HELLO world",11,(uint8_t*)"1234567812345678");
    if (h1 == h2) fails++;

#if defined(__x86_64__) || defined(__i386__)
    /* Verify the AVX-512 8-lane variant is bit-exact with scalar siphash()
     * across a spread of lengths (incl. 0, sub-block, block-aligned, multi-block
     * with mixed tails). Skipped on CPUs without AVX-512F. */
    if (__builtin_cpu_supports("avx512f")) {
        static uint8_t buf[8][80];
        for (int lane = 0; lane < 8; lane++)
            for (int j = 0; j < 80; j++)
                buf[lane][j] = (uint8_t)(lane * 31 + j * 7 + 3);
        for (int base = 0; base <= 64; base++) {
            const uint8_t *in8[8];
            size_t len8[8];
            uint64_t out8[8];
            for (int lane = 0; lane < 8; lane++) {
                in8[lane] = buf[lane];
                /* mix lengths across lanes so all tail cases occur together */
                len8[lane] = (size_t)(base + lane) % 73;
            }
            siphash_x8(in8, len8, k, out8);
            for (int lane = 0; lane < 8; lane++) {
                uint64_t ref = siphash(in8[lane], len8[lane], k);
                if (ref != out8[lane]) fails++;
            }
        }
    }
#endif

    if (!fails) return 0;
    return 1;
}

int main(void) {
    if (siphash_test() == 0) {
        printf("SipHash test: OK\n");
        return 0;
    } else {
        printf("SipHash test: FAILED\n");
        return 1;
    }
}

#endif
