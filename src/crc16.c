#ifdef CRC16_TEST_MAIN
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#else
#include "server.h"
#endif

#if defined(__linux__) && defined(__aarch64__) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
#define HAVE_CRC16_PMULL
#ifdef __clang__
#define CRC16_TARGET __attribute__((target("aes")))
#else
#define CRC16_TARGET __attribute__((target("+crypto")))
#endif

static int crc16_pmull_supported;

__attribute__((constructor)) static void crc16PmullInit(void) {
    crc16_pmull_supported = !!(getauxval(AT_HWCAP) & HWCAP_PMULL);
}

/* Reverse the entire block: XMODEM processes the most significant bit first. */
CRC16_TARGET
static inline uint64x2_t crc16Load(const unsigned char *p) {
    uint8x16_t v = vrev64q_u8(vld1q_u8(p));
    return vreinterpretq_u64_u8(vextq_u8(v, v, 8));
}

CRC16_TARGET
static uint16_t crc16Pmull(const unsigned char *p, int len) {
    uint64x2_t a;
    if (len < 16) {
        uint64_t first, last;
        memcpy(&first, p, 8);
        memcpy(&last, p + len - 8, 8);
        a = vcombine_u64(vcreate_u64(__builtin_bswap64(last)),
                        vcreate_u64(len == 8 ? 0 : __builtin_bswap64(first) >> ((16 - len) * 8)));
    } else {
        const unsigned char *end = p + len;
        a = crc16Load(p);
        p += 16;
        len -= 16;
        while (len >= 16) {
            poly128_t lo = vmull_p64(vgetq_lane_u64(a, 0), UINT64_C(0x45630000));
            poly128_t hi = vmull_p64(vgetq_lane_u64(a, 1), UINT64_C(0xd5f60000));
            a = veorq_u64(veorq_u64(vreinterpretq_u64_p128(lo),
                                    vreinterpretq_u64_p128(hi)), crc16Load(p));
            p += 16;
            len -= 16;
        }
        if (len) {
            /* x^(8r) and x^(8r+64) modulo x^16 * (x^16+x^12+x^5+1).
             * Fold by the actual tail length, not by a zero-padded block. */
            static const uint64_t k[16][2] = {
                {0,0}, {0x100,0x45a00000}, {0x10000,0xb8610000},
                {0x1000000,0x47d30000}, {0x10210000,0xeb230000},
                {0x33310000,0x6f450000}, {0x37300000,0xd8490000},
                {0x76b40000,0x03750000}, {0xaa510000,0x45630000},
                {0x45a00000,0x7b610000}, {0xb8610000,0xaefc0000},
                {0x47d30000,0xa8240000}, {0xeb230000,0x10e20000},
                {0x6f450000,0xf0310000}, {0xd8490000,0xde1f0000},
                {0x03750000,0x35b30000}
            };
            static const unsigned char indices[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
            uint8x16_t mask = vcltq_u8(vld1q_u8(indices), vdupq_n_u8(len));
            uint64x2_t tail = vandq_u64(crc16Load(end - 16), vreinterpretq_u64_u8(mask));
            poly128_t lo = vmull_p64(vgetq_lane_u64(a, 0), k[len][0]);
            poly128_t hi = vmull_p64(vgetq_lane_u64(a, 1), k[len][1]);
            a = veorq_u64(veorq_u64(vreinterpretq_u64_p128(lo),
                                    vreinterpretq_u64_p128(hi)), tail);
        }
    }
    /* For P=x^16+x^12+x^5+1, floor(x^144/P)=x^128+KH*x^64+KL.
     * If A=H*x^64+L, the low 16 quotient bits of A*x^16/P are
     * L + H*KH + high64(L*KH + H*KL), with carry-less products. */
    const uint64_t kh = UINT64_C(0x11303471a041b343);
    const uint64_t kl = UINT64_C(0xe569d503417ca4a2);
    uint64_t l = vgetq_lane_u64(a, 0), h = vgetq_lane_u64(a, 1);
    uint64x2_t t = veorq_u64(vreinterpretq_u64_p128(vmull_p64(l, kh)),
                             vreinterpretq_u64_p128(vmull_p64(h, kl)));
    uint64x2_t u = vreinterpretq_u64_p128(vmull_p64(h, kh));
    uint16_t q = l ^ vgetq_lane_u64(u, 0) ^ vgetq_lane_u64(t, 1);
    return (uint16_t)(q ^ (q << 5) ^ (q << 12));
}
#endif

/*
 * Copyright 2001-2010 Georges Menie (www.menie.org)
 * Copyright 2010-current Redis Ltd.
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

uint16_t crc16(const char *buf, int len) {
#ifdef HAVE_CRC16_PMULL
    if (len >= 8 && crc16_pmull_supported)
        return crc16Pmull((const unsigned char *)buf, len);
#endif
    int counter;
    uint16_t crc = 0;
    for (counter = 0; counter < len; counter++)
            crc = (crc<<8) ^ crc16tab[((crc>>8) ^ *buf++)&0x00FF];
    return crc;
}

#ifdef CRC16_TEST_MAIN
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

static uint16_t crc16Reference(const unsigned char *p, int len) {
    uint16_t crc = 0;
    while (len-- > 0) {
        crc ^= (uint16_t)*p++ << 8;
        for (int bit = 0; bit < 8; bit++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static void crc16Check(const unsigned char *p, int len) {
    uint16_t expected = crc16Reference(p, len);
    if (crc16((const char *)p, len) != expected) {
        fprintf(stderr, "CRC16 mismatch: len=%d\n", len);
        exit(1);
    }
#ifdef HAVE_CRC16_PMULL
    int supported = crc16_pmull_supported;
    crc16_pmull_supported = 0;
    if (crc16((const char *)p, len) != expected) abort();
    crc16_pmull_supported = supported;
#endif
}

int main(void) {
    unsigned char buf[256 + 16];
    uint32_t random = 0x12345678;
    if (crc16("123456789", 9) != 0x31c3 || crc16(NULL, 0) ||
        crc16(NULL, -1) || crc16(NULL, INT_MIN)) abort();
    for (int pattern = 0; pattern < 64; pattern++) {
        for (unsigned int i = 0; i < sizeof(buf); i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            buf[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : (unsigned char)random;
        }
        for (int off = 0; off < 16; off++)
            for (int len = 0; len <= 256; len++)
                crc16Check(buf + off, len);
    }
    static const int lengths[] = {257, 511, 512, 513, 1023, 1024, 1025,
                                  4095, 4096, 4097, 65535, 65536, 65537};
    unsigned char *large = malloc(65537 + 16);
    if (!large) abort();
    for (int pattern = 0; pattern < 4; pattern++) {
        for (int i = 0; i < 65537 + 16; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            large[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : (unsigned char)random;
        }
        for (int off = 0; off < 16; off++)
            for (unsigned int i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
                crc16Check(large + off, lengths[i]);
    }
    free(large);
#ifdef __linux__
    long page = sysconf(_SC_PAGESIZE);
    if (page < 256) abort();
    unsigned char *mem = mmap(NULL, page * 3, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED || mprotect(mem + page, page, PROT_READ | PROT_WRITE)) abort();
    memset(mem + page, 0xa5, page);
    for (int len = 0; len <= 128; len++) {
        crc16Check(mem + page, len);
        crc16Check(mem + page * 2 - len, len);
    }
    if (munmap(mem, page * 3)) abort();
#endif
#ifdef HAVE_CRC16_PMULL
    printf("CRC16 tests passed (PMULL %s; forced scalar checked)\n",
           crc16_pmull_supported ? "enabled" : "unavailable");
#else
    puts("CRC16 tests passed (scalar-only platform)");
#endif
    return 0;
}
#endif
