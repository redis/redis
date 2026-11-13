#ifdef CRC16_TEST_MAIN
#define _GNU_SOURCE
#include <stdint.h>
#else
#include "server.h"
#endif

#if defined(REDIS_CRC16_PMULL) && defined(__linux__) && defined(__aarch64__) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <sys/auxv.h>
#include <asm/hwcap.h>
extern uint16_t crc16_xmodem(uint16_t, const uint8_t *, uint64_t);
static int crc16_has_optimized;
#ifdef REDIS_CRC16_SVE
int crc16_use_sve_wide;
#endif
__attribute__((constructor)) static void crc16InitOptimized(void) {
    unsigned long hw = getauxval(AT_HWCAP);
    unsigned long required = HWCAP_PMULL;
#ifdef REDIS_CRC16_SHA3
    required |= HWCAP_SHA3;
#endif
    int available = (hw & required) == required;
#ifdef REDIS_CRC16_SVE
    unsigned long hw2 = getauxval(AT_HWCAP2);
    crc16_use_sve_wide = available && (hw & HWCAP_SVE) &&
        (hw2 & (HWCAP2_SVE2 | HWCAP2_SVEPMULL)) == (HWCAP2_SVE2 | HWCAP2_SVEPMULL);
#endif
    crc16_has_optimized = available;
}
#define CRC16_USE_OPTIMIZED
#endif

/*
 * Copyright 2001-2010 Georges Menie (www.menie.org)
 * Copyright 2010-current Redis Ltd.
 * Copyright 2010-2012 Salvatore Sanfilippo (adapted to Redis coding style)
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

#ifdef CRC16_USE_OPTIMIZED
uint16_t crc16_xmodem_scalar(uint16_t crc, const uint8_t *buf, uint64_t len) {
#else
static uint16_t crc16_xmodem_scalar(uint16_t crc, const uint8_t *buf, uint64_t len) {
#endif
    for (uint64_t i = 0; i < len; i++)
        crc = (uint16_t)((crc << 8) ^ crc16tab[((crc >> 8) ^ buf[i]) & 0xff]);
    return crc;
}

uint16_t crc16(const char *buf, int len) {
    if (len <= 0) return 0;
#ifdef CRC16_USE_OPTIMIZED
    if (crc16_has_optimized)
        return crc16_xmodem(0, (const uint8_t *)buf, (uint64_t)len);
#endif
    return crc16_xmodem_scalar(0, (const uint8_t *)buf, (uint64_t)len);
}

#ifdef CRC16_TEST_MAIN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

/* 独立的逐位参考算法，避免测试和生产路径共享查表错误。 */
static uint16_t crc16Reference(uint16_t crc, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int bit = 0; bit < 8; bit++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static void crc16Check(const char *path, uint16_t actual, uint16_t expected,
                       size_t len, unsigned int seed, unsigned int offset) {
    if (actual != expected) {
        fprintf(stderr, "%s: len=%zu seed=%04x offset=%u actual=%04x expected=%04x\n",
                path, len, seed, offset, actual, expected);
        exit(1);
    }
}

#ifdef CRC16_USE_OPTIMIZED
extern uint16_t crc16_xmodem_short(uint16_t, const uint8_t *, uint64_t);
extern uint16_t crc16_xmodem_pmull(uint16_t, const uint8_t *, uint64_t);
extern uint16_t crc16_xmodem_pmull_8way_neon(uint16_t, const uint8_t *, uint64_t);
#ifdef REDIS_CRC16_SVE
extern uint16_t crc16_xmodem_pmull_8way(uint16_t, const uint8_t *, uint64_t);
#endif
#endif

static void crc16CheckPaths(const uint8_t *buf, size_t len, uint16_t seed,
                            uint16_t expected, unsigned int offset) {
    crc16Check("scalar", crc16_xmodem_scalar(seed, buf, len), expected, len, seed, offset);
    if (seed == 0)
        crc16Check("public", crc16((const char *)buf, (int)len), expected, len, seed, offset);
#ifdef CRC16_USE_OPTIMIZED
    /* 硬件能力不足时不直接调用包含可选指令的入口。 */
    if (crc16_has_optimized) {
        crc16Check("dispatch", crc16_xmodem(seed, buf, len), expected, len, seed, offset);
        if (len >= 16)
            crc16Check("short", crc16_xmodem_short(seed, buf, len), expected, len, seed, offset);
        crc16Check("pmull", crc16_xmodem_pmull(seed, buf, len), expected, len, seed, offset);
        crc16Check("neon", crc16_xmodem_pmull_8way_neon(seed, buf, len), expected, len, seed, offset);
#ifdef REDIS_CRC16_SVE
        if (crc16_use_sve_wide)
            crc16Check("sve", crc16_xmodem_pmull_8way(seed, buf, len), expected, len, seed, offset);
#endif
    }
#endif
}

static void crc16CheckBuffer(const uint8_t *buf, size_t len, unsigned int offset) {
    static const uint16_t seeds[] = {0, 1, 0x1021, 0x8000, 0xffff};
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        uint16_t expected = crc16Reference(seeds[i], buf, len);
        crc16CheckPaths(buf, len, seeds[i], expected, offset);
    }
}

int main(void) {
    static const size_t large_lengths[] = {4097, 8191, 8192, 8193, 16383,
                                           16384, 16385, 32767, 32768, 32769};
    uint8_t buf[32769 + 16];
    uint32_t random = 0x12345678;
    for (size_t i = 0; i < sizeof(buf); i++) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        buf[i] = (uint8_t)random;
    }
    crc16Check("XMODEM vector", crc16("123456789", 9), 0x31c3, 9, 0, 0);
    crc16Check("negative length", crc16(NULL, -1), 0, 0, 0, 0);
    crc16Check("zero length", crc16(NULL, 0), 0, 0, 0, 0);
    for (unsigned int offset = 0; offset < 16; offset++) {
        /* 逐步扩展参考值，覆盖每个长度而不重复执行整段逐位运算。 */
        static const uint16_t seeds[] = {0, 1, 0x1021, 0x8000, 0xffff};
        for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
            uint16_t expected = seeds[s];
            for (size_t len = 0; len <= 4096; len++) {
                crc16CheckPaths(buf + offset, len, seeds[s], expected, offset);
                expected = crc16Reference(expected, buf + offset + len, 1);
            }
        }
        for (size_t i = 0; i < sizeof(large_lengths) / sizeof(large_lengths[0]); i++)
            crc16CheckBuffer(buf + offset, large_lengths[i], offset);
    }
    memset(buf, 0, sizeof(buf));
    crc16CheckBuffer(buf, 32769, 0);
    memset(buf, 0xff, sizeof(buf));
    crc16CheckBuffer(buf, 32769, 0);
#ifdef __linux__
    /* 两端保护页分别捕获越界前读和越界后读，包括零长度输入。 */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return 1;
    size_t span = ((32769 + (size_t)page - 1) / (size_t)page) * (size_t)page;
    uint8_t *mapping = mmap(NULL, span + 2 * (size_t)page, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) { perror("mmap"); return 1; }
    uint8_t *data = mapping + page;
    if (mprotect(data, span, PROT_READ | PROT_WRITE)) { perror("mprotect"); return 1; }
    memset(data, 0xa5, span);
    for (size_t len = 0; len <= 4096; len++) {
        crc16CheckBuffer(data, len, 0);
        crc16CheckBuffer(data + span - len, len, 0);
    }
    for (size_t i = 0; i < sizeof(large_lengths) / sizeof(large_lengths[0]); i++) {
        size_t len = large_lengths[i];
        crc16CheckBuffer(data, len, 0);
        crc16CheckBuffer(data + span - len, len, 0);
    }
    munmap(mapping, span + 2 * (size_t)page);
#endif
#ifdef CRC16_USE_OPTIMIZED
    if (!crc16_has_optimized)
        puts("SKIP: assembly paths (required CPU features unavailable)");
#ifdef REDIS_CRC16_SVE
    if (!crc16_use_sve_wide)
        puts("SKIP: SVE path (required CPU features unavailable)");
#endif
#else
    puts("SKIP: assembly paths (not built on this target)");
#endif
    puts("CRC16 correctness tests passed");
    return 0;
}
#endif
