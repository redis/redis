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

#define N	16
#define MASK	((1 << N) - 1)
#define LOW(x)	((unsigned)(x) & MASK)
#define HIGH(x)	LOW((x) >> N)

/* Initial X: 0x1234ABCD330E */
#define X0	0x330E
#define X1	0xABCD
#define X2	0x1234

/* A: 0x5DEECE66D */
#define A0	0xE66D
#define A1	0xDEEC
#define A2	0x5

/* C: 0xB */
#define C	0xB

static uint32_t x[3] = { X0, X1, X2 }, a[3] = { A0, A1, A2 }, c = C;
static void next(void);

int32_t redisLrand48() {
    next();
    return (((int32_t)x[2] << (N - 1)) + (x[1] >> 1));
}

void redisSrand48(int32_t seedval) {
    x[0] = X0;
    x[1] = LOW(seedval);
    x[2] = HIGH(seedval);
}

static void next(void) {
    /* Z = (X * A) mod 2^48
     *
     * Every block is 16 bits
     *     Z: | z2 | z1 | z0 |
     *     X: |x[2]|x[1]|x[0]|
     *     A: |a[2]|a[1]|a[0]|
     */
    uint32_t y0, y1, y2;
    y0 = x[0] * a[0];
    y1 = x[0] * a[1] + x[1] * a[0];
    y2 = x[1] * a[1] + x[0] * a[2] + x[2] * a[0];
    uint32_t z0, z1, z2;
    z0 = LOW(y0);
    z1 = LOW(HIGH(y0) + LOW(y1));
    z2 = LOW(HIGH(HIGH(y0) + LOW(y1)) + HIGH(y1) + y2);

    /* X = (Z + c) mod 2^48
     *   = (X * A + c) mod 2^48
     */
    x[0] = LOW(z0 + c);
    x[1] = LOW(z1 + HIGH(z0 + c));
    x[2] = LOW(z2 + HIGH(z1 + HIGH(z0 + c)));
}
