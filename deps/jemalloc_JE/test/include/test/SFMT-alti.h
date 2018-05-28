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
 * @file SFMT-alti.h 
 *
 * @brief SIMD oriented Fast Mersenne Twister(SFMT)
 * pseudorandom number generator
 *
 * @author Mutsuo Saito (Hiroshima University)
 * @author Makoto Matsumoto (Hiroshima University)
 *
 * Copyright (C) 2007 Mutsuo Saito, Makoto Matsumoto and Hiroshima
 * University. All rights reserved.
 *
 * The new BSD License is applied to this software.
 * see LICENSE.txt
 */

#ifndef SFMT_ALTI_H
#define SFMT_ALTI_H

/**
 * This function represents the recursion formula in AltiVec and BIG ENDIAN.
 * @param a a 128-bit part of the interal state array
 * @param b a 128-bit part of the interal state array
 * @param c a 128-bit part of the interal state array
 * @param d a 128-bit part of the interal state array
 * @return output
 */
JEMALLOC_ALWAYS_INLINE
vector unsigned int vec_recursion(vector unsigned int a,
						vector unsigned int b,
						vector unsigned int c,
						vector unsigned int d) {

    const vector unsigned int sl1 = ALTI_SL1;
    const vector unsigned int sr1 = ALTI_SR1;
#ifdef ONLY64
    const vector unsigned int mask = ALTI_MSK64;
    const vector unsigned char perm_sl = ALTI_SL2_PERM64;
    const vector unsigned char perm_sr = ALTI_SR2_PERM64;
#else
    const vector unsigned int mask = ALTI_MSK;
    const vector unsigned char perm_sl = ALTI_SL2_PERM;
    const vector unsigned char perm_sr = ALTI_SR2_PERM;
#endif
    vector unsigned int v, w, x, y, z;
    x = vec_perm(a, (vector unsigned int)perm_sl, perm_sl);
    v = a;
    y = vec_sr(b, sr1);
    z = vec_perm(c, (vector unsigned int)perm_sr, perm_sr);
    w = vec_sl(d, sl1);
    z = vec_xor(z, w);
    y = vec_and(y, mask);
    v = vec_xor(v, x);
    z = vec_xor(z, y);
    z = vec_xor(z, v);
    return z;
}

/**
 * This function fills the internal state array with pseudorandom
 * integers.
 */
JEMALLOC_INLINE void gen_rand_all(sfmt_t *ctx) {
    int i;
    vector unsigned int r, r1, r2;

    r1 = ctx->sfmt[N - 2].s;
    r2 = ctx->sfmt[N - 1].s;
    for (i = 0; i < N - POS1; i++) {
	r = vec_recursion(ctx->sfmt[i].s, ctx->sfmt[i + POS1].s, r1, r2);
	ctx->sfmt[i].s = r;
	r1 = r2;
	r2 = r;
    }
    for (; i < N; i++) {
	r = vec_recursion(ctx->sfmt[i].s, ctx->sfmt[i + POS1 - N].s, r1, r2);
	ctx->sfmt[i].s = r;
	r1 = r2;
	r2 = r;
    }
}

/**
 * This function fills the user-specified array with pseudorandom
 * integers.
 *
 * @param array an 128-bit array to be filled by pseudorandom numbers.  
 * @param size number of 128-bit pesudorandom numbers to be generated.
 */
JEMALLOC_INLINE void gen_rand_array(sfmt_t *ctx, w128_t *array, int size) {
    int i, j;
    vector unsigned int r, r1, r2;

    r1 = ctx->sfmt[N - 2].s;
    r2 = ctx->sfmt[N - 1].s;
    for (i = 0; i < N - POS1; i++) {
	r = vec_recursion(ctx->sfmt[i].s, ctx->sfmt[i + POS1].s, r1, r2);
	array[i].s = r;
	r1 = r2;
	r2 = r;
    }
    for (; i < N; i++) {
	r = vec_recursion(ctx->sfmt[i].s, array[i + POS1 - N].s, r1, r2);
	array[i].s = r;
	r1 = r2;
	r2 = r;
    }
    /* main loop */
    for (; i < size - N; i++) {
	r = vec_recursion(array[i - N].s, array[i + POS1 - N].s, r1, r2);
	array[i].s = r;
	r1 = r2;
	r2 = r;
    }
    for (j = 0; j < 2 * N - size; j++) {
	ctx->sfmt[j].s = array[j + size - N].s;
    }
    for (; i < size; i++) {
	r = vec_recursion(array[i - N].s, array[i + POS1 - N].s, r1, r2);
	array[i].s = r;
	ctx->sfmt[j++].s = r;
	r1 = r2;
	r2 = r;
    }
}

#ifndef ONLY64
#if defined(__APPLE__)
#define ALTI_SWAP (vector unsigned char) \
	(4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11)
#else
#define ALTI_SWAP {4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11}
#endif
/**
 * This function swaps high and low 32-bit of 64-bit integers in user
 * specified array.
 *
 * @param array an 128-bit array to be swaped.
 * @param size size of 128-bit array.
 */
JEMALLOC_INLINE void swap(w128_t *array, int size) {
    int i;
    const vector unsigned char perm = ALTI_SWAP;

    for (i = 0; i < size; i++) {
	array[i].s = vec_perm(array[i].s, (vector unsigned int)perm, perm);
    }
}
#endif

#endif
