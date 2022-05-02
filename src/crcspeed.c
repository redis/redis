/*
 * Copyright (C) 2013 Mark Adler
 * Originally by: crc64.c Version 1.4  16 Dec 2013  Mark Adler
 * Modifications by Matt Stancliff <matt@genges.com>:
 *   - removed CRC64-specific behavior
 *   - added generation of lookup tables by parameters
 *   - removed inversion of CRC input/result
 *   - removed automatic initialization in favor of explicit initialization

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

/* Fill in a CRC constants table. */
void crcspeed64little_init(crcfn64 crcfn, uint64_t table[8][256]) {
    uint64_t crc;

    /* generate CRCs for all single byte sequences */
    for (int n = 0; n < 256; n++) {
        unsigned char v = n;
        table[0][n] = crcfn(0, &v, 1);
    }

    /* generate nested CRC table for future slice-by-8 lookup */
    for (int n = 0; n < 256; n++) {
        crc = table[0][n];
        for (int k = 1; k < 8; k++) {
            crc = table[0][crc & 0xff] ^ (crc >> 8);
            table[k][n] = crc;
        }
    }
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

/* Calculate a non-inverted CRC multiple bytes at a time on a little-endian
 * architecture. If you need inverted CRC, invert *before* calling and invert
 * *after* calling.
 * 64 bit crc = process 8 bytes at once;
 */
uint64_t crcspeed64little(uint64_t little_table[8][256], uint64_t crc,
                          void *buf, size_t len) {
    unsigned char *next = buf;

    /* process individual bytes until we reach an 8-byte aligned pointer */
    while (len && ((uintptr_t)next & 7) != 0) {
        crc = little_table[0][(crc ^ *next++) & 0xff] ^ (crc >> 8);
        len--;
    }

    /* fast middle processing, 8 bytes (aligned!) per loop */
    while (len >= 8) {
        crc ^= *(uint64_t *)next;
        crc = little_table[7][crc & 0xff] ^
              little_table[6][(crc >> 8) & 0xff] ^
              little_table[5][(crc >> 16) & 0xff] ^
              little_table[4][(crc >> 24) & 0xff] ^
              little_table[3][(crc >> 32) & 0xff] ^
              little_table[2][(crc >> 40) & 0xff] ^
              little_table[1][(crc >> 48) & 0xff] ^
              little_table[0][crc >> 56];
        next += 8;
        len -= 8;
    }

    /* process remaining bytes (can't be larger than 8) */
    while (len) {
        crc = little_table[0][(crc ^ *next++) & 0xff] ^ (crc >> 8);
        len--;
    }

    return crc;
}

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
