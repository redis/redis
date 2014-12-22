/* Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE. */

#include "crc16speed.h"

/* If CRCSPEED16_DUAL is defined, we allow calls to
 * both _little and _big CRC.
 * By default, we only allow one endianness to be used
 * and the first call to either _init function will set the
 * lookup table endianness for the life of this module.
 * We don't enable dual lookups by default because
 * each 8x256 lookup table is 4k. */
#ifndef CRC16SPEED_DUAL
static uint16_t crc16_table[8][256] = { { 0 } };
static void *crc16_table_little = NULL, *crc16_table_big = NULL;
static const bool dual = false;
#else
static uint16_t crc16_table_little[8][256] = { { 0 } };
static uint16_t crc16_table_big[8][256] = { { 0 } };
static void *crc16_table = NULL;
static const bool dual = true;
#endif

/* value of crc16_table[0][1], architecture dependent. */
#define LITTLE1 UINT16_C(0x1021)
#define BIG1 UINT16_C(0x2110)

/* Define CRC16SPEED_SAFE if you want runtime checks to stop
 * CRCs from being calculated by uninitialized tables (and also stop tables
 * from being initialized more than once). */
#ifdef CRC16SPEED_SAFE
#define should_init(table, val)                                                \
    do {                                                                       \
        if ((table)[0][1] == (val))                                            \
            return false;                                                      \
    } while (0)
#define check_init(table, val)                                                 \
    do {                                                                       \
        if ((table)[0][1] != (val))                                            \
            return false;                                                      \
    } while (0)
#else
#define should_init(a, b)
#define check_init(a, b)
#endif

/* This is CRC-16-CCITT (non-reflected poly, non-inverted input/output).
 * crc16() is only used to bootstrap an initial 256-entry lookup table. */
#define POLY 0x1021
uint16_t crc16(uint16_t crc, const void *in_data, uint64_t len) {
    const uint8_t *data = in_data;
    for (uint64_t i = 0; i < len; i++) {
        crc = crc ^ (data[i] << 8);
        for (int j = 0; j < 8; j++)
            if (crc & 0x8000)
                crc = (crc << 1) ^ POLY;
            else
                crc = (crc << 1);
    }
    return crc;
}

/* Only for testing; doesn't support DUAL */
uint16_t crc16_lookup(uint16_t crc, const void *in_data, uint64_t len) {
    const uint8_t *data = in_data;
    for (uint64_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc16_table[0][((crc >> 8) ^ data[i]) & 0x00ff];
    return crc;
}

/* Returns false if CRC16SPEED_SAFE and table already initialized. */
bool crc16speed_init(void) {
#ifndef CRC16SPEED_DUAL
    should_init(crc16_table, LITTLE1);
#else
    should_init(crc16_table_little, LITTLE1);
#endif
    crcspeed16little_init(crc16, dual ? crc16_table_little : crc16_table);
    return true;
}

/* Returns false if CRC16SPEED_SAFE and table already initialized. */
bool crc16speed_init_big(void) {
#ifndef CRC16SPEED_DUAL
    should_init(crc16_table, BIG1);
#else
    should_init(crc16_table_big, BIG1);
#endif
    crcspeed16big_init(crc16, dual ? crc16_table_big : crc16_table);
    return true;
}

uint16_t crc16speed(uint16_t crc, const void *s, const uint64_t l) {
/* Quickly check if CRC table is initialized to little endian correctly. */
#ifndef CRC16SPEED_DUAL
    check_init(crc16_table, LITTLE1);
#else
    check_init(crc16_table_little, LITTLE1);
#endif
    return crcspeed16little(dual ? crc16_table_little : crc16_table, crc,
                            (void *)s, l);
}

uint16_t crc16speed_big(uint16_t crc, const void *s, const uint64_t l) {
/* Quickly check if CRC table is initialized to big endian correctly. */
#ifndef CRC16SPEED_DUAL
    check_init(crc16_table, BIG1);
#else
    check_init(crc16_table_big, BIG1);
#endif
    return crcspeed16big(dual ? crc16_table_big : crc16_table, crc, (void *)s,
                         l);
}

bool crc16speed_init_native(void) {
    const uint64_t n = 1;
    return *(char *)&n ? crc16speed_init() : crc16speed_init_big();
}

/* If you are on a platform where endianness can change at runtime, this
 * will break unless you compile with CRC16SPEED_DUAL and manually run
 * _init() and _init_big() instead of using _init_native() */
uint16_t crc16speed_native(uint16_t crc, const void *s, const uint64_t l) {
    const uint64_t n = 1;
    return *(char *)&n ? crc16speed(crc, s, l) : crc16speed_big(crc, s, l);
}

/* Iterate over table to fully load it into a cache near the CPU. */
void crc16speed_cache_table(void) {
    uint16_t m;
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 256; ++j) {
#ifndef CRC16SPEED_DUAL
            m = crc16_table[i][j];
#else
            m = crc16_table_little[i][j];
            m += crc16_table_big[i][j];
#endif
            ++m;
        }
    }
}

/* Test main */
#if defined(REDIS_TEST) || defined(REDIS_TEST_MAIN)
#include <stdio.h>
#include <stdlib.h>

#define UNUSED(x) (void)(x)
int crc16Test(int argc, char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);
    crc16speed_init();
    printf("[calcula]: 31c3 == %04" PRIx64 "\n",
           (uint64_t)crc16(0, "123456789", 9));
    printf("[lookupt]: 31c3 == %04" PRIx64 "\n",
           (uint64_t)crc16_lookup(0, "123456789", 9));
    printf("[16speed]: 31c3 == %04" PRIx64 "\n",
           (uint64_t)crc16speed(0, "123456789", 9));
    char li[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed "
                "do eiusmod tempor incididunt ut labore et dolore magna "
                "aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
                "ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis "
                "aute irure dolor in reprehenderit in voluptate velit esse "
                "cillum dolore eu fugiat nulla pariatur. Excepteur sint "
                "occaecat cupidatat non proident, sunt in culpa qui officia "
                "deserunt mollit anim id est laborum.";
    printf("[calcula]: 4b20 == %04" PRIx64 "\n",
           (uint64_t)crc16(0, li, sizeof li));
    printf("[lookupt]: 4b20 == %04" PRIx64 "\n",
           (uint64_t)crc16_lookup(0, li, sizeof li));
    printf("[16speed]: 4b20 == %04" PRIx64 "\n",
           (uint64_t)crc16speed(0, li, sizeof li));
    return 0;
}
#endif

#ifdef REDIS_TEST_MAIN
int main(int argc, char *argv[]) { return crc16Test(argc, argv); }
#endif
