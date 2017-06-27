#include "server.h"

/*
 * Copyright 2001-2010 Georges Menie (www.menie.org)
 * Copyright 2010-2012 Salvatore Sanfilippo (adapted to Redis coding style)
 * Copyright 2017 ARM Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of California, Berkeley nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS AND CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* CRC16 implementation according to CCITT standards.
 *
 * Note by @antirez: this is actually the XMODEM CRC 16 algorithm, using the
 * following parameters:
 *
 * Name                       : "XMODEM", also known as "ZMODEM", "CRC-16/ACORN"
 * Width                      : 16 bit
 * Poly                       : 1021 (That is actually x^16 + x^12 + x^5 + 1)
 * Initialization             : 0000
 * Reflect Input byte         : False
 * Reflect Output CRC         : False
 * Xor constant to output CRC : 0000
 * Output for "123456789"     : 31C3
 */

static const uint16_t crc16tab[256]= {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50a5,0x60c6,0x70e7,
    0x8108,0x9129,0xa14a,0xb16b,0xc18c,0xd1ad,0xe1ce,0xf1ef,
    0x1231,0x0210,0x3273,0x2252,0x52b5,0x4294,0x72f7,0x62d6,
    0x9339,0x8318,0xb37b,0xa35a,0xd3bd,0xc39c,0xf3ff,0xe3de,
    0x2462,0x3443,0x0420,0x1401,0x64e6,0x74c7,0x44a4,0x5485,
    0xa56a,0xb54b,0x8528,0x9509,0xe5ee,0xf5cf,0xc5ac,0xd58d,
    0x3653,0x2672,0x1611,0x0630,0x76d7,0x66f6,0x5695,0x46b4,
    0xb75b,0xa77a,0x9719,0x8738,0xf7df,0xe7fe,0xd79d,0xc7bc,
    0x48c4,0x58e5,0x6886,0x78a7,0x0840,0x1861,0x2802,0x3823,
    0xc9cc,0xd9ed,0xe98e,0xf9af,0x8948,0x9969,0xa90a,0xb92b,
    0x5af5,0x4ad4,0x7ab7,0x6a96,0x1a71,0x0a50,0x3a33,0x2a12,
    0xdbfd,0xcbdc,0xfbbf,0xeb9e,0x9b79,0x8b58,0xbb3b,0xab1a,
    0x6ca6,0x7c87,0x4ce4,0x5cc5,0x2c22,0x3c03,0x0c60,0x1c41,
    0xedae,0xfd8f,0xcdec,0xddcd,0xad2a,0xbd0b,0x8d68,0x9d49,
    0x7e97,0x6eb6,0x5ed5,0x4ef4,0x3e13,0x2e32,0x1e51,0x0e70,
    0xff9f,0xefbe,0xdfdd,0xcffc,0xbf1b,0xaf3a,0x9f59,0x8f78,
    0x9188,0x81a9,0xb1ca,0xa1eb,0xd10c,0xc12d,0xf14e,0xe16f,
    0x1080,0x00a1,0x30c2,0x20e3,0x5004,0x4025,0x7046,0x6067,
    0x83b9,0x9398,0xa3fb,0xb3da,0xc33d,0xd31c,0xe37f,0xf35e,
    0x02b1,0x1290,0x22f3,0x32d2,0x4235,0x5214,0x6277,0x7256,
    0xb5ea,0xa5cb,0x95a8,0x8589,0xf56e,0xe54f,0xd52c,0xc50d,
    0x34e2,0x24c3,0x14a0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xa7db,0xb7fa,0x8799,0x97b8,0xe75f,0xf77e,0xc71d,0xd73c,
    0x26d3,0x36f2,0x0691,0x16b0,0x6657,0x7676,0x4615,0x5634,
    0xd94c,0xc96d,0xf90e,0xe92f,0x99c8,0x89e9,0xb98a,0xa9ab,
    0x5844,0x4865,0x7806,0x6827,0x18c0,0x08e1,0x3882,0x28a3,
    0xcb7d,0xdb5c,0xeb3f,0xfb1e,0x8bf9,0x9bd8,0xabbb,0xbb9a,
    0x4a75,0x5a54,0x6a37,0x7a16,0x0af1,0x1ad0,0x2ab3,0x3a92,
    0xfd2e,0xed0f,0xdd6c,0xcd4d,0xbdaa,0xad8b,0x9de8,0x8dc9,
    0x7c26,0x6c07,0x5c64,0x4c45,0x3ca2,0x2c83,0x1ce0,0x0cc1,
    0xef1f,0xff3e,0xcf5d,0xdf7c,0xaf9b,0xbfba,0x8fd9,0x9ff8,
    0x6e17,0x7e36,0x4e55,0x5e74,0x2e93,0x3eb2,0x0ed1,0x1ef0
};

#if defined(__ARM_NEON) && defined(__ARM_FEATURE_CRYPTO)
#define HAVE_CLMUL   1

#include <arm_neon.h>

#define __shift_p128_left(data, imm)    vreinterpretq_u64_u8(vextq_u8(vdupq_n_u8(0), vreinterpretq_u8_u64((data)), (imm)))
#define __shift_p128_right(data, imm)   vreinterpretq_u64_u8(vextq_u8(vreinterpretq_u8_u64((data)), vdupq_n_u8(0), (imm)))

static inline uint64x2_t endian_swap(uint64x2_t val)
{
    return (uint64x2_t)__builtin_shuffle((uint8x16_t)val, (uint8x16_t){ 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
}

static inline uint64x2_t fold_128b(uint64x2_t to, const uint64x2_t from, const uint64x2_t constant)
{
    uint64x2_t tmp_h = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(from, 1), (poly64_t)vgetq_lane_u64(constant, 1));
    uint64x2_t tmp_l = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(from, 0), (poly64_t)vgetq_lane_u64(constant, 0));
    return veorq_u64(tmp_l, veorq_u64(to, tmp_h));
}

static inline uint64x2_t crc16_fold(uint64x2_t from, const uint64x2_t constant)
{
    uint64x2_t tmp = from;

    from = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(from, 1), (poly64_t)vgetq_lane_u64(constant, 1));

    /* get from:low_64b + 32b '0' appended (96b total) */
    tmp = __shift_p128_right(__shift_p128_left(tmp, 8), 4);
    from = veorq_u64(from, tmp);

    /* 96bit --> 64bit */
    tmp = from;
    from = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(from, 1), (poly64_t)vgetq_lane_u64(constant, 0));
    return veorq_u64(from, tmp);
}

static inline uint64_t crc16_barrett_reduction(uint64x2_t data, const uint64x2_t p_q)
{
    uint64x2_t tmp = vcombine_u64((uint64x1_t)vgetq_lane_u64(data, 0), (uint64x1_t)0ULL);

    /* T1 = floor(R(x)/x^32) * [1/P(x)]; */
    tmp = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(__shift_p128_right(tmp, 4), 0), (poly64_t)vgetq_lane_u64(p_q, 0));
    /* T2 = floor(T1/x^32) * P(x) */
    tmp = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(__shift_p128_right(tmp, 4), 0), (poly64_t)vgetq_lane_u64(p_q, 1));
    /* R-int(R/P)*P */
    data = veorq_u64(tmp, data);

    return vgetq_lane_u64(data, 0);
}

/*
 * crc16_clmul assumes:
 * 1. input buffer s is 16-byte aligned
 * 2. buffer length is 64*N bytes
 *
 */
static uint64_t crc16_clmul(uint64_t crc, const unsigned char *s, uint64_t l)
{
    /* pre-computed constants */
    const uint64x2_t foldConstants_p4 = {0x0000000059b00000ULL, 0x0000000060190000ULL};
    const uint64x2_t foldConstants_p1 = {0x0000000045630000ULL, 0x00000000d5f60000ULL};
    const uint64x2_t foldConstants_p0 = {0x00000000aa510000ULL, 0x00000000eb230000ULL};
    const uint64x2_t foldConstants_br = {0x0000000111303471ULL, 0x0000000110210000ULL};

    crc <<= 48;
    uint64x2_t *p_data = (uint64x2_t *)s;
    uint64_t remain_len = l;

    uint64x2_t x0, x1, x2, x3;
    uint64x2_t y0, y1, y2, y3;

    /* expand crc to 128bit */
    y0 = vcombine_u64((uint64x1_t)0ULL, (uint64x1_t)crc);
    /* load first 64B */
    x0 = *p_data++; x0 = endian_swap(x0);
    x1 = *p_data++; x1 = endian_swap(x1);
    x2 = *p_data++; x2 = endian_swap(x2);
    x3 = *p_data++; x3 = endian_swap(x3);

    remain_len -= 64;

    x0 = x0 ^ y0; /* x0 ^ crc */

    /* 1024bit --> 512bit loop */
    while(remain_len >= 64) {
        y0 = *p_data++; y0 = endian_swap(y0);
        y1 = *p_data++; y1 = endian_swap(y1);
        y2 = *p_data++; y2 = endian_swap(y2);
        y3 = *p_data++; y3 = endian_swap(y3);

        x0 = fold_128b(y0, x0, foldConstants_p4);
        x1 = fold_128b(y1, x1, foldConstants_p4);
        x2 = fold_128b(y2, x2, foldConstants_p4);
        x3 = fold_128b(y3, x3, foldConstants_p4);

        remain_len -= 64;
    }

    /* folding 512bit --> 128bit */
    x1 = fold_128b(x1, x0, foldConstants_p1);
    x2 = fold_128b(x2, x1, foldConstants_p1);
    x0 = fold_128b(x3, x2, foldConstants_p1);

    x0 = crc16_fold(x0, foldConstants_p0);

    /* calc crc using barrett reduction method */
    crc = crc16_barrett_reduction(x0, foldConstants_br);
    crc >>= 16;

    return crc;
}
#endif

uint16_t crc16(const char *buf, int len) {
    int counter = 0;
    uint16_t crc = 0;

#if HAVE_CLMUL
    uint64_t remain = len;
    uint64_t l = len;
    /* make sure 16-byte aligned for CLMUL routine */
    if ((uintptr_t)buf & 15) {
        int64_t n = (l > (-(uintptr_t)buf & 15))? -(uintptr_t)buf & 15 : l;
        for (; counter<n; counter++) {
            crc = (crc<<8) ^ crc16tab[((crc>>8) ^ *buf++)&0x00FF];
        }
        remain -= n;
    }
    if (remain >= 1*1024) {
        /*
         * Use CLMUL to compute CRC for a "large" block
         * this block is 64*N bytes
         */
        crc = crc16_clmul(crc, (unsigned char *)(buf+counter), remain&(~63ULL));
        counter += remain & (~63ULL);
        buf += counter;
    }
#endif

    for (; counter < len; counter++)
            crc = (crc<<8) ^ crc16tab[((crc>>8) ^ *buf++)&0x00FF];
    return crc;
}
