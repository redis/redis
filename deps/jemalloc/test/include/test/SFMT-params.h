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
#ifndef SFMT_PARAMS_H
#define SFMT_PARAMS_H

#if !defined(MEXP)
#ifdef __GNUC__
  #warning "MEXP is not defined. I assume MEXP is 19937."
#endif
  #define MEXP 19937
#endif
/*-----------------
  BASIC DEFINITIONS
  -----------------*/
/** Mersenne Exponent. The period of the sequence 
 *  is a multiple of 2^MEXP-1.
 * #define MEXP 19937 */
/** SFMT generator has an internal state array of 128-bit integers,
 * and N is its size. */
#define N (MEXP / 128 + 1)
/** N32 is the size of internal state array when regarded as an array
 * of 32-bit integers.*/
#define N32 (N * 4)
/** N64 is the size of internal state array when regarded as an array
 * of 64-bit integers.*/
#define N64 (N * 2)

/*----------------------
  the parameters of SFMT
  following definitions are in paramsXXXX.h file.
  ----------------------*/
/** the pick up position of the array.
#define POS1 122 
*/

/** the parameter of shift left as four 32-bit registers.
#define SL1 18
 */

/** the parameter of shift left as one 128-bit register. 
 * The 128-bit integer is shifted by (SL2 * 8) bits. 
#define SL2 1 
*/

/** the parameter of shift right as four 32-bit registers.
#define SR1 11
*/

/** the parameter of shift right as one 128-bit register. 
 * The 128-bit integer is shifted by (SL2 * 8) bits. 
#define SR2 1 
*/

/** A bitmask, used in the recursion.  These parameters are introduced
 * to break symmetry of SIMD.
#define MSK1 0xdfffffefU
#define MSK2 0xddfecb7fU
#define MSK3 0xbffaffffU
#define MSK4 0xbffffff6U 
*/

/** These definitions are part of a 128-bit period certification vector.
#define PARITY1	0x00000001U
#define PARITY2	0x00000000U
#define PARITY3	0x00000000U
#define PARITY4	0xc98e126aU
*/

#if MEXP == 607
  #include "test/SFMT-params607.h"
#elif MEXP == 1279
  #include "test/SFMT-params1279.h"
#elif MEXP == 2281
  #include "test/SFMT-params2281.h"
#elif MEXP == 4253
  #include "test/SFMT-params4253.h"
#elif MEXP == 11213
  #include "test/SFMT-params11213.h"
#elif MEXP == 19937
  #include "test/SFMT-params19937.h"
#elif MEXP == 44497
  #include "test/SFMT-params44497.h"
#elif MEXP == 86243
  #include "test/SFMT-params86243.h"
#elif MEXP == 132049
  #include "test/SFMT-params132049.h"
#elif MEXP == 216091
  #include "test/SFMT-params216091.h"
#else
#ifdef __GNUC__
  #error "MEXP is not valid."
  #undef MEXP
#else
  #undef MEXP
#endif

#endif

#endif /* SFMT_PARAMS_H */
