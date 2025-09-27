/*
 * Copyright (C) 2013 Mark Adler
 * Copyright (C) 2019-2024 Josiah Carlson
 * Originally by: crc64.c Version 1.4  16 Dec 2013  Mark Adler
 * Modifications by Matt Stancliff <matt@genges.com>:
 *   - removed CRC64-specific behavior
 *   - added generation of lookup tables by parameters
 *   - removed inversion of CRC input/result
 *   - removed automatic initialization in favor of explicit initialization
 * Modifications by Josiah Carlson <josiah.carlson@gmail.com>
 *   - Added case/vector/AVX/+ versions of crc combine function; see crccombine.c
 *     - added optional static cache
 *   - Modified to use 1 thread to:
 *     - Partition large crc blobs into 2-3 segments
 *     - Process the 2-3 segments in parallel
 *     - Merge the resulting crcs
 *     -> Resulting in 10-90% performance boost for data > 1 meg
 *     - macro-ized to reduce copy/pasta

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler
  madler@alumni.caltech.edu
 */

#include "crcspeed.h"
#include "crccombine.h"

#define CRC64_LEN_MASK UINT64_C(0x7ffffffffffffff8)
#define CRC64_REVERSED_POLY UINT64_C(0x95ac9329ac4bc9b5)

/* Fill in a CRC constants table. */
void crcspeed64little_init(crcfn64 crcfn, uint64_t table[8][256]) {
    uint64_t crc;

    /* generate CRCs for all single byte sequences */
    for (int n = 0; n < 256; n++) {
        unsigned char v = n;
        table[0][n] = crcfn(0, &v, 1);
    }

    /* generate nested CRC table for future slice-by-8/16/24+ lookup */
    for (int n = 0; n < 256; n++) {
        crc = table[0][n];
        for (int k = 1; k < 8; k++) {
            crc = table[0][crc & 0xff] ^ (crc >> 8);
            table[k][n] = crc;
        }
    }
#if USE_STATIC_COMBINE_CACHE
    /* initialize combine cache for CRC stapling for slice-by 16/24+ */
    init_combine_cache(CRC64_REVERSED_POLY, 64);
#endif
}

void crcspeed16little_init(crcfn16 crcfn, uint16_t table[8][256]) {
    uint16_t crc;

    /* generate CRCs for all single byte sequences */
    for (int n = 0; n < 256; n++) {
        table[0][n] = crcfn(0, &n, 1);
    }

    /* generate nested CRC table for future slice-by-8 lookup */
    for (int n = 0; n < 256; n++) {
        crc = table[0][n];
        for (int k = 1; k < 8; k++) {
            crc = table[0][(crc >> 8) & 0xff] ^ (crc << 8);
            table[k][n] = crc;
        }
    }
}

/* Reverse the bytes in a 64-bit word. */
static inline uint64_t rev8(uint64_t a) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(a);
#else
    uint64_t m;

    m = UINT64_C(0xff00ff00ff00ff);
    a = ((a >> 8) & m) | (a & m) << 8;
    m = UINT64_C(0xffff0000ffff);
    a = ((a >> 16) & m) | (a & m) << 16;
    return a >> 32 | a << 32;
#endif
}

/* This function is called once to initialize the CRC table for use on a
   big-endian architecture. */
void crcspeed64big_init(crcfn64 fn, uint64_t big_table[8][256]) {
    /* Create the little endian table then reverse all the entries. */
    crcspeed64little_init(fn, big_table);
    for (int k = 0; k < 8; k++) {
        for (int n = 0; n < 256; n++) {
            big_table[k][n] = rev8(big_table[k][n]);
        }
    }
}

void crcspeed16big_init(crcfn16 fn, uint16_t big_table[8][256]) {
    /* Create the little endian table then reverse all the entries. */
    crcspeed16little_init(fn, big_table);
    for (int k = 0; k < 8; k++) {
        for (int n = 0; n < 256; n++) {
            big_table[k][n] = rev8(big_table[k][n]);
        }
    }
}

/* Note: doing all of our crc/next modifications *before* the crc table
 * references is an absolute speedup on all CPUs tested. So... keep these
 * macros separate.
 */

#define DO_8_1(crc, next)                            \
    crc ^= *(uint64_t *)next;                        \
    next += 8

#define DO_8_2(crc)                                  \
    crc = little_table[7][(uint8_t)crc] ^            \
             little_table[6][(uint8_t)(crc >> 8)] ^  \
             little_table[5][(uint8_t)(crc >> 16)] ^ \
             little_table[4][(uint8_t)(crc >> 24)] ^ \
             little_table[3][(uint8_t)(crc >> 32)] ^ \
             little_table[2][(uint8_t)(crc >> 40)] ^ \
             little_table[1][(uint8_t)(crc >> 48)] ^ \
             little_table[0][crc >> 56]

#define CRC64_SPLIT(div) \
    olen = len; \
    next2 = next1 + ((len / div) & CRC64_LEN_MASK); \
    len = (next2 - next1)

#define MERGE_CRC(crcn) \
    crc1 = crc64_combine(crc1, crcn, next2 - next1, CRC64_REVERSED_POLY, 64)

#define MERGE_END(last, DIV) \
    len = olen - ((next2 - next1) * DIV); \
    next1 = last

/* Variables so we can change for benchmarking; these seem to be fairly
 * reasonable for Intel CPUs made since 2010. Please adjust as necessary if
 * or when your CPU has more load / execute units. We've written benchmark code
 * to help you tune your platform, see crc64Test. */
#if defined(__i386__) || defined(__X86_64__)
static size_t CRC64_TRI_CUTOFF = (2*1024);
static size_t CRC64_DUAL_CUTOFF = (128);
#else
static size_t CRC64_TRI_CUTOFF = (16*1024);
static size_t CRC64_DUAL_CUTOFF = (1024);
#endif


void set_crc64_cutoffs(size_t dual_cutoff, size_t tri_cutoff) {
    CRC64_DUAL_CUTOFF = dual_cutoff;
    CRC64_TRI_CUTOFF = tri_cutoff;
}

/* Calculate a non-inverted CRC multiple bytes at a time on a little-endian
 * architecture. If you need inverted CRC, invert *before* calling and invert
 * *after* calling.
 * 64 bit crc = process 8/16/24 bytes at once;
 */
uint64_t crcspeed64little(uint64_t little_table[8][256], uint64_t crc1,
                          void *buf, size_t len) {
    unsigned char *next1 = buf;

    if (CRC64_DUAL_CUTOFF < 1) {
        goto final;
    }

    /* process individual bytes until we reach an 8-byte aligned pointer */
    while (len && ((uintptr_t)next1 & 7) != 0) {
        crc1 = little_table[0][(crc1 ^ *next1++) & 0xff] ^ (crc1 >> 8);
        len--;
    }

    if (len >  CRC64_TRI_CUTOFF) {
        /* 24 bytes per loop, doing 3 parallel 8 byte chunks at a time */
        unsigned char *next2, *next3;
        uint64_t olen, crc2=0, crc3=0;
        CRC64_SPLIT(3);
        /* len is now the length of the first segment, the 3rd segment possibly
         * having extra bytes to clean up at the end
         */
        next3 = next2 + len;
        while (len >= 8) {
            len -= 8;
            DO_8_1(crc1, next1);
            DO_8_1(crc2, next2);
            DO_8_1(crc3, next3);
            DO_8_2(crc1);
            DO_8_2(crc2);
            DO_8_2(crc3);
        }

        /* merge the 3 crcs */
        MERGE_CRC(crc2);
        MERGE_CRC(crc3);
        MERGE_END(next3, 3);
    } else if (len > CRC64_DUAL_CUTOFF) {
        /* 16 bytes per loop, doing 2 parallel 8 byte chunks at a time */
        unsigned char *next2;
        uint64_t olen, crc2=0;
        CRC64_SPLIT(2);
        /* len is now the length of the first segment, the 2nd segment possibly
         * having extra bytes to clean up at the end
         */
        while (len >= 8) {
            len -= 8;
            DO_8_1(crc1, next1);
            DO_8_1(crc2, next2);
            DO_8_2(crc1);
            DO_8_2(crc2);
        }

        /* merge the 2 crcs */
        MERGE_CRC(crc2);
        MERGE_END(next2, 2);
    }
    /* We fall through here to handle our <CRC64_DUAL_CUTOFF inputs, and for any trailing
     * bytes that wasn't evenly divisble by 16 or 24 above. */

    /* fast processing, 8 bytes (aligned!) per loop */
    while (len >= 8) {
        len -= 8;
        DO_8_1(crc1, next1);
        DO_8_2(crc1);
    }
final:
    /* process remaining bytes (can't be larger than 8) */
    while (len) {
        crc1 = little_table[0][(crc1 ^ *next1++) & 0xff] ^ (crc1 >> 8);
        len--;
    }

    return crc1;
}

/* clean up our namespace */
#undef DO_8_1
#undef DO_8_2
#undef CRC64_SPLIT
#undef MERGE_CRC
#undef MERGE_END
#undef CRC64_REVERSED_POLY
#undef CRC64_LEN_MASK


/* note: similar perf advantages can be had for long strings in crc16 using all
 * of the same optimizations as above; though this is unnecessary. crc16 is
 * normally used to shard keys; not hash / verify data, so is used on shorter
 * data that doesn't warrant such changes. */

uint16_t crcspeed16little(uint16_t little_table[8][256], uint16_t crc,
                          void *buf, size_t len) {
    unsigned char *next = buf;

    /* process individual bytes until we reach an 8-byte aligned pointer */
    while (len && ((uintptr_t)next & 7) != 0) {
        crc = little_table[0][((crc >> 8) ^ *next++) & 0xff] ^ (crc << 8);
        len--;
    }

    /* fast middle processing, 8 bytes (aligned!) per loop */
    while (len >= 8) {
        uint64_t n = *(uint64_t *)next;
        crc = little_table[7][(n & 0xff) ^ ((crc >> 8) & 0xff)] ^
              little_table[6][((n >> 8) & 0xff) ^ (crc & 0xff)] ^
              little_table[5][(n >> 16) & 0xff] ^
              little_table[4][(n >> 24) & 0xff] ^
              little_table[3][(n >> 32) & 0xff] ^
              little_table[2][(n >> 40) & 0xff] ^
              little_table[1][(n >> 48) & 0xff] ^
              little_table[0][n >> 56];
        next += 8;
        len -= 8;
    }

    /* process remaining bytes (can't be larger than 8) */
    while (len) {
        crc = little_table[0][((crc >> 8) ^ *next++) & 0xff] ^ (crc << 8);
        len--;
    }

    return crc;
}

/* Calculate a non-inverted CRC eight bytes at a time on a big-endian
 * architecture.
 */
uint64_t crcspeed64big(uint64_t big_table[8][256], uint64_t crc, void *buf,
                       size_t len) {
    unsigned char *next = buf;

    crc = rev8(crc);
    while (len && ((uintptr_t)next & 7) != 0) {
        crc = big_table[0][(crc >> 56) ^ *next++] ^ (crc << 8);
        len--;
    }

    /* note: alignment + 2/3-way processing can probably be handled here nearly
       the same as above, using our updated DO_8_2 macro. Not included in these
       changes, as other authors, I don't have big-endian to test with. */

    while (len >= 8) {
        crc ^= *(uint64_t *)next;
        crc = big_table[0][crc & 0xff] ^
              big_table[1][(crc >> 8) & 0xff] ^
              big_table[2][(crc >> 16) & 0xff] ^
              big_table[3][(crc >> 24) & 0xff] ^
              big_table[4][(crc >> 32) & 0xff] ^
              big_table[5][(crc >> 40) & 0xff] ^
              big_table[6][(crc >> 48) & 0xff] ^
              big_table[7][crc >> 56];
        next += 8;
        len -= 8;
    }

    while (len) {
        crc = big_table[0][(crc >> 56) ^ *next++] ^ (crc << 8);
        len--;
    }

    return rev8(crc);
}

/* WARNING: Completely untested on big endian architecture.  Possibly broken. */
uint16_t crcspeed16big(uint16_t big_table[8][256], uint16_t crc_in, void *buf,
                       size_t len) {
    unsigned char *next = buf;
    uint64_t crc = crc_in;

    crc = rev8(crc);
    while (len && ((uintptr_t)next & 7) != 0) {
        crc = big_table[0][((crc >> (56 - 8)) ^ *next++) & 0xff] ^ (crc >> 8);
        len--;
    }

    while (len >= 8) {
        uint64_t n = *(uint64_t *)next;
        crc = big_table[0][(n & 0xff) ^ ((crc >> (56 - 8)) & 0xff)] ^
              big_table[1][((n >> 8) & 0xff) ^ (crc & 0xff)] ^
              big_table[2][(n >> 16) & 0xff] ^
              big_table[3][(n >> 24) & 0xff] ^
              big_table[4][(n >> 32) & 0xff] ^
              big_table[5][(n >> 40) & 0xff] ^
              big_table[6][(n >> 48) & 0xff] ^
              big_table[7][n >> 56];
        next += 8;
        len -= 8;
    }

    while (len) {
        crc = big_table[0][((crc >> (56 - 8)) ^ *next++) & 0xff] ^ (crc >> 8);
        len--;
    }

    return rev8(crc);
}

/* Return the CRC of buf[0..len-1] with initial crc, processing eight bytes
   at a time using passed-in lookup table.
   This selects one of two routines depending on the endianness of
   the architecture. */
uint64_t crcspeed64native(uint64_t table[8][256], uint64_t crc, void *buf,
                          size_t len) {
    uint64_t n = 1;

    return *(char *)&n ? crcspeed64little(table, crc, buf, len)
                       : crcspeed64big(table, crc, buf, len);
}

uint16_t crcspeed16native(uint16_t table[8][256], uint16_t crc, void *buf,
                          size_t len) {
    uint64_t n = 1;

    return *(char *)&n ? crcspeed16little(table, crc, buf, len)
                       : crcspeed16big(table, crc, buf, len);
}

/* Initialize CRC lookup table in architecture-dependent manner. */
void crcspeed64native_init(crcfn64 fn, uint64_t table[8][256]) {
    uint64_t n = 1;

    *(char *)&n ? crcspeed64little_init(fn, table)
                : crcspeed64big_init(fn, table);
}

void crcspeed16native_init(crcfn16 fn, uint16_t table[8][256]) {
    uint64_t n = 1;

    *(char *)&n ? crcspeed16little_init(fn, table)
                : crcspeed16big_init(fn, table);
}
