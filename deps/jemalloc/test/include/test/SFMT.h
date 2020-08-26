/*
 * This file derives from SFMT 1.3.3
 * (http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/SFMT/index.html), which was
 * released under the terms of the following license:
 *
 *   Copyright (c) 2006,2007 Mutsuo Saito, Makoto Matsumoto and Hiroshima
 *   University. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met:
 *
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer in the documentation and/or other materials provided
 *         with the distribution.
 *       * Neither the name of the Hiroshima University nor the names of
 *         its contributors may be used to endorse or promote products
 *         derived from this software without specific prior written
 *         permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/** 
 * @file SFMT.h 
 *
 * @brief SIMD oriented Fast Mersenne Twister(SFMT) pseudorandom
 * number generator
 *
 * @author Mutsuo Saito (Hiroshima University)
 * @author Makoto Matsumoto (Hiroshima University)
 *
 * Copyright (C) 2006, 2007 Mutsuo Saito, Makoto Matsumoto and Hiroshima
 * University. All rights reserved.
 *
 * The new BSD License is applied to this software.
 * see LICENSE.txt
 *
 * @note We assume that your system has inttypes.h.  If your system
 * doesn't have inttypes.h, you have to typedef uint32_t and uint64_t,
 * and you have to define PRIu64 and PRIx64 in this file as follows:
 * @verbatim
 typedef unsigned int uint32_t
 typedef unsigned long long uint64_t  
 #define PRIu64 "llu"
 #define PRIx64 "llx"
@endverbatim
 * uint32_t must be exactly 32-bit unsigned integer type (no more, no
 * less), and uint64_t must be exactly 64-bit unsigned integer type.
 * PRIu64 and PRIx64 are used for printf function to print 64-bit
 * unsigned int and 64-bit unsigned int in hexadecimal format.
 */

#ifndef SFMT_H
#define SFMT_H

typedef struct sfmt_s sfmt_t;

uint32_t gen_rand32(sfmt_t *ctx);
uint32_t gen_rand32_range(sfmt_t *ctx, uint32_t limit);
uint64_t gen_rand64(sfmt_t *ctx);
uint64_t gen_rand64_range(sfmt_t *ctx, uint64_t limit);
void fill_array32(sfmt_t *ctx, uint32_t *array, int size);
void fill_array64(sfmt_t *ctx, uint64_t *array, int size);
sfmt_t *init_gen_rand(uint32_t seed);
sfmt_t *init_by_array(uint32_t *init_key, int key_length);
void fini_gen_rand(sfmt_t *ctx);
const char *get_idstring(void);
int get_min_array_size32(void);
int get_min_array_size64(void);

/* These real versions are due to Isaku Wada */
/** generates a random number on [0,1]-real-interval */
static inline double to_real1(uint32_t v) {
    return v * (1.0/4294967295.0); 
    /* divided by 2^32-1 */ 
}

/** generates a random number on [0,1]-real-interval */
static inline double genrand_real1(sfmt_t *ctx) {
    return to_real1(gen_rand32(ctx));
}

/** generates a random number on [0,1)-real-interval */
static inline double to_real2(uint32_t v) {
    return v * (1.0/4294967296.0); 
    /* divided by 2^32 */
}

/** generates a random number on [0,1)-real-interval */
static inline double genrand_real2(sfmt_t *ctx) {
    return to_real2(gen_rand32(ctx));
}

/** generates a random number on (0,1)-real-interval */
static inline double to_real3(uint32_t v) {
    return (((double)v) + 0.5)*(1.0/4294967296.0); 
    /* divided by 2^32 */
}

/** generates a random number on (0,1)-real-interval */
static inline double genrand_real3(sfmt_t *ctx) {
    return to_real3(gen_rand32(ctx));
}
/** These real versions are due to Isaku Wada */

/** generates a random number on [0,1) with 53-bit resolution*/
static inline double to_res53(uint64_t v) {
    return v * (1.0/18446744073709551616.0L);
}

/** generates a random number on [0,1) with 53-bit resolution from two
 * 32 bit integers */
static inline double to_res53_mix(uint32_t x, uint32_t y) {
    return to_res53(x | ((uint64_t)y << 32));
}

/** generates a random number on [0,1) with 53-bit resolution
 */
static inline double genrand_res53(sfmt_t *ctx) {
    return to_res53(gen_rand64(ctx));
}

/** generates a random number on [0,1) with 53-bit resolution
    using 32bit integer.
 */
static inline double genrand_res53_mix(sfmt_t *ctx) {
    uint32_t x, y;

    x = gen_rand32(ctx);
    y = gen_rand32(ctx);
    return to_res53_mix(x, y);
}
#endif
