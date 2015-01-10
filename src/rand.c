/* Pseudo random number generation functions derived from the drand48()
 * function obtained from pysam source code.
 *
 * This functions are used in order to replace the default math.random()
 * Lua implementation with something having exactly the same behavior
 * across different systems (by default Lua uses libc's rand() that is not
 * required to implement a specific PRNG generating the same sequence
 * in different systems if seeded with the same integer).
 *
 * The original code appears to be under the public domain.
 * I modified it removing the non needed functions and all the
 * 1960-style C coding stuff...
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>

/* LOW16 & HIGH16 macros of a uint32_t type */
#define MASK16 0xffff
#define LOW16(x) ((unsigned)(x) & MASK16)
#define HIGH16(x) LOW16((x) >> 16)

/* MASK48 is used for "mod 2^48" operation */
#define MASK48 0xffffffffffffULL

/* A: 0x5DEECE66D */
#define A 0x5deece66dULL
/* C: 0xB */
#define C 0xbULL
/* Initial X: 0x1234ABCD330E */
static uint64_t X = 0x1234abcd330eULL;

static void next(void);

int32_t redisLrand48() {
    next();
    return (int32_t)(X >> 17);
}

void redisSrand48(int32_t seedval) {
    X = ((uint64_t)HIGH16(seedval)<<32) | (LOW16(seedval)<<16) | 0x330e;
}

static void next(void) {
    X = (X * A + C) & MASK48;
}
