#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <immintrin.h>
#include "crccombine.h"

/* Copyright (C) 2013 Mark Adler
 * Copyright (C) 2019-2023 Josiah Carlson
 * Portions originally from: crc64.c Version 1.4  16 Dec 2013  Mark Adler
 * Modifications by Josiah Carlson <josiah.carlson@gmail.com>
 *   - Added implementation variations with sample timings for gf_matrix_times*()
 *   - Most folks would be best using gf2_matrix_times_vec or
 *	   gf2_matrix_times_vec2, unless some processor does AVX2 fast.

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

#define STATIC_ASSERT(VVV) do {int test = 1 / (VVV);test++;} while (0)

uint64_t gf2_matrix_times_original(uint64_t *mat, uint64_t vec) {
	/*
	 * This version from the original source works, though is much slower
	 * than what is currently available from (we have static crcs, no need for
	 * models):
	 *     https://github.com/madler/crcany/blob/master/crc.c
	 * E5-2670 ~270us to extend ~1 meg 64 bit hash
	 * i3-8130U ~235us to extend ~1 meg 64 bit hash
	 */
	uint64_t sum;

	sum = 0;
	while (vec)
	{
		if (vec & 1)
			sum ^= *mat;
		vec >>= 1;
		mat++;
	}
	return sum;
}

uint64_t gf2_matrix_times_switch(uint64_t *mat, uint64_t vec) {
	/*
	 * Without using any vector math, this handles 4 bits at a time,
	 * and saves 40+% of the time compared to the original version. Use if you
	 * have no vector compile option available to you.
	 * E5-2670 ~150us to extend ~1 meg 64 bit hash
	 * i3-8130U ~140us to extend ~1 meg 64 bit hash
	 */
	uint64_t sum;

	sum = 0;
	while (vec) {
		/* reversing the case order is ~10% slower on Xeon E5-2670 */
		switch (vec & 15) {
		case 15:
			sum ^= *mat ^ *(mat+1) ^ *(mat+2) ^ *(mat+3);
			break;
		case 14:
			sum ^= *(mat+1) ^ *(mat+2) ^ *(mat+3);
			break;
		case 13:
			sum ^= *mat ^ *(mat+2) ^ *(mat+3);
			break;
		case 12:
			sum ^= *(mat+2) ^ *(mat+3);
			break;
		case 11:
			sum ^= *mat ^ *(mat+1) ^ *(mat+3);
			break;
		case 10:
			sum ^= *(mat+1) ^ *(mat+3);
			break;
		case 9:
			sum ^= *mat ^ *(mat+3);
			break;
		case 8:
			sum ^= *(mat+3);
			break;
		case 7:
			sum ^= *mat ^ *(mat+1) ^ *(mat+2);
			break;
		case 6:
			sum ^= *(mat+1) ^ *(mat+2);
			break;
		case 5:
			sum ^= *mat ^ *(mat+2);
			break;
		case 4:
			sum ^= *(mat+2);
			break;
		case 3:
			sum ^= *mat ^ *(mat+1);
			break;
		case 2:
			sum ^= *(mat+1);
			break;
		case 1:
			sum ^= *mat;
			break;
		default:
			break;
		}
		vec >>= 4;
		mat += 4;
	}
	return sum;
}

/*
	Warning: here there be dragons involving vector math, and macros to save us
	from repeating the same information over and over.
*/

/* need a mapping of bits -> indexes in the matrix */
#define MASKS_4() \
	static v4uq masks[16] = { \
		{0,0,0,0}, \
		{-1,0,0,0}, \
		{0,-1,0,0}, \
		{-1,-1,0,0}, \
		{0,0,-1,0}, \
		{-1,0,-1,0}, \
		{0,-1,-1,0}, \
		{-1,-1,-1,0}, \
		{0,0,0,-1}, \
		{-1,0,0,-1}, \
		{0,-1,0,-1}, \
		{-1,-1,0,-1}, \
		{0,0,-1,-1}, \
		{-1,0,-1,-1}, \
		{0,-1,-1,-1}, \
		{-1,-1,-1,-1}, \
	}

#if HAVE_AVX2_AND_WANT_USE_AVX2

uint64_t gf2_matrix_times_vec_avx2(uint64_t *mat, uint64_t vec) {
	/*
	 * The compiler-automated vectorization provided later, tends to be
	 * a bit faster than this function in my runs on the hardware I tested.
	 * I suspect non-mobile CPUs might do a bit better here.
	 * i3-8130U +AVX2 ~48us to extend ~1 meg 64 bit hash
	 */
	v4uq sum = {0, 0, 0, 0},
		*mv4 = (v4uq*)mat;
	MASKS_4();

	/* called CHUNK4 because we're handling 4 bits at a time */
	v4uq  m0 = {0, 0, 0, 0};
	int tv;
#define DO_CHUNK4() \
		tv = (vec & 15); \
		vec >>= 4; \
		sum ^= (v4uq)_mm256_mask_i32gather_epi64( \
			(__m256i)m0, \
			(const long long int *)(mv4), \
			(__m128i)indexes[tv], \
			(__m256i)masks[tv], \
			8); \
		mv4++

	/* obviously let's compose; no loops! */
#define DO_CHUNK16() \
		DO_CHUNK4(); \
		DO_CHUNK4(); \
		DO_CHUNK4(); \
		DO_CHUNK4()

	DO_CHUNK16();
	DO_CHUNK16();
	DO_CHUNK16();
	DO_CHUNK16();

	STATIC_ASSERT(sizeof(uint64_t) == 8);
	STATIC_ASSERT(sizeof(long long unsigned int) == 8);
	return sum[0] ^ sum[1] ^ sum[2] ^ sum[3];
}

#undef DO_CHUNK16
#undef DO_CHUNK4

#endif


uint64_t gf2_matrix_times_vec(uint64_t *mat, uint64_t vec) {
	/*
	 * Some pre-calculation up front, then mostly pand/pxor/movqda
	 * Is 8-11x faster than original.
	 * E5-2670 ~35us to extend ~1 meg 64 bit hash
	 * i3-8130U ~22us to extend ~1 meg 64 bit hash
	 */
	v4uq sum = {0, 0, 0, 0},
		*mv4 = (v4uq*)mat;
	MASKS_4();

	/* called CHUNK4 because we're handling 4 bits at a time.
	 * Look at that macro. My god. It's beautiful. */
#define DO_CHUNK4() \
		sum ^= (*mv4++) & masks[vec & 15]; \
		vec >>= 4

	/* obviously let's compose; no loops! */
#define DO_CHUNK16() \
		DO_CHUNK4(); \
		DO_CHUNK4(); \
		DO_CHUNK4(); \
		DO_CHUNK4()

	DO_CHUNK16();
	DO_CHUNK16();
	DO_CHUNK16();
	DO_CHUNK16();

	STATIC_ASSERT(sizeof(uint64_t) == 8);
	STATIC_ASSERT(sizeof(long long unsigned int) == 8);
	return sum[0] ^ sum[1] ^ sum[2] ^ sum[3];
}

#undef DO_CHUNK16
#undef DO_CHUNK4


uint64_t gf2_matrix_times_vec8(uint64_t *mat, uint64_t vec) {
	/*
	 * Don't want to use a 256*8*8 table for masks, a lot of ymm shuffling
	 * E5-2670 ~75us to extend ~1 meg 64 bit hash
	 * i3-8130U ~65us to extend ~1 meg 64 bit hash
	 */
	union {
		v8uq mask;
		v4uq mask4[2];
	} m;
	v8uq sum = {0, 0, 0, 0, 0, 0, 0, 0},
		*mv8 = (v8uq*)mat;
	MASKS_4();

#define DO_CHUNK8() \
		m.mask4[0] = masks[vec & 15]; \
		m.mask4[1] = masks[(vec>>4) & 15]; \
		sum ^= (*mv8) & m.mask; \
		vec >>= 8; \
		mv8++

	DO_CHUNK8();
	DO_CHUNK8();
	DO_CHUNK8();
	DO_CHUNK8();
	DO_CHUNK8();
	DO_CHUNK8();
	DO_CHUNK8();
	DO_CHUNK8();

	STATIC_ASSERT(sizeof(uint64_t) == 8);
	STATIC_ASSERT(sizeof(long long unsigned int) == 8);
	return sum[0] ^ sum[1] ^ sum[2] ^ sum[3] ^ sum[4] ^ sum[5] ^ sum[6] ^ sum[7];
}

#undef DO_CHUNK8
#undef MASKS_4

/* this table allows us to eliminate conditions during gf2_matrix_times_vec2() */
#define MASKS_2() \
	static v2uq masks2[4] = { \
		{0,0}, \
		{-1,0}, \
		{0,-1}, \
		{-1,-1}, \
	}

uint64_t gf2_matrix_times_vec2(uint64_t *mat, uint64_t vec) {
	/*
	 * Uses xmm registers on x86, works basically everywhere fast, doing
	 * cycles of movqda, mov, shr, pand, and, pxor, at least on gcc 8.
	 * Is 9-11x faster than original.
	 * E5-2670 ~29us to extend ~1 meg 64 bit hash
	 * i3-8130U ~22us to extend ~1 meg 64 bit hash
	 */
	v2uq sum = {0, 0},
		*mv2 = (v2uq*)mat;
	MASKS_2();

	/* Almost as beautiful as gf2_matrix_times_vec, but only half as many
	 * bits per step, so we need 2 per chunk4 operation. Faster in my tests. */

#define DO_CHUNK4() \
		sum ^= (*mv2++) & masks2[vec & 3]; \
		vec >>= 2; \
		sum ^= (*mv2++) & masks2[vec & 3]; \
		vec >>= 2

#define DO_CHUNK16() \
		DO_CHUNK4(); \
		DO_CHUNK4(); \
		DO_CHUNK4(); \
		DO_CHUNK4()

	DO_CHUNK16();
	DO_CHUNK16();
	DO_CHUNK16();
	DO_CHUNK16();

	STATIC_ASSERT(sizeof(uint64_t) == 8);
	STATIC_ASSERT(sizeof(long long unsigned int) == 8);
	return sum[0] ^ sum[1];
}

#undef DO_CHUNK16
#undef DO_CHUNK4
#undef MASKS_2


#if USE_VECTOR_COMBINE_INNER
// gf2_matrix_times_vec is also a pretty good option to avoid the mask_4 table.
# if HAVE_AVX2_AND_WANT_USE_AVX2
#define GMT gf2_matrix_times_vec_avx2
# else
#define GMT gf2_matrix_times_vec2
# endif
#else
# if USE_SWITCH_IF_NO_VECTOR
// use the switch-based version if we can't do vectors on the platform
#define GMT gf2_matrix_times_switch
# else
#define GMT gf2_matrix_times_original
# endif
#endif

static void gf2_matrix_square(uint64_t *square, uint64_t *mat, uint8_t dim) {
	unsigned n;

	for (n = 0; n < dim; n++)
		square[n] = GMT(mat, mat[n]);
}

#if USE_STATIC_COMBINE_CACHE
/* Turns out our Redis / Jones CRC cycles at this point, so we can support
 * more than 64 bits of extension if we want. Trivially. */
static uint64_t combine_cache[64][64];

/* Mark Adler has some amazing updates to crc.c in his crcany repository. I
 * like static caches, and not worrying about finding cycles generally. We are
 * okay to spend the 32k of memory here, leaving the algorithm unchanged from
 * as it was a decade ago, and be happy that it costs <200 microseconds to
 * init, and that subsequent calls to the combine function take under 100
 * nanoseconds.
 */

void init_combine_cache(uint64_t poly, uint8_t dim) {
	unsigned n, cache_num = 0;
	combine_cache[1][0] = poly;
	int prev = 1;
	uint64_t row = 1;
	for (n = 1; n < dim; n++)
	{
		combine_cache[1][n] = row;
		row <<= 1;
	}

	gf2_matrix_square(combine_cache[0], combine_cache[1], dim);
	gf2_matrix_square(combine_cache[1], combine_cache[0], dim);

	/* do/while to overwrite the first two layers, they are not used, but are
	 * re-generated in the last two layer for the Redis polynomial */
	do {
		gf2_matrix_square(combine_cache[cache_num], combine_cache[cache_num + prev], dim);
		prev = -1;
	} while (++cache_num < 64);
}

#endif

/* Return the CRC-64 of two sequential blocks, where crc1 is the CRC-64 of the
 * first block, crc2 is the CRC-64 of the second block, and len2 is the length
 * of the second block.
 *
 * If you want relfections on your CRCs; do them outside before / after.
 * WARNING: if you enable USE_STATIC_COMBINE_CACHE to make this fast, you MUST
 * ALWAYS USE THE SAME POLYNOMIAL, otherwise you will get the wrong results.
 * You MAY bzero() the even/odd static arrays, which will induce a re-cache on
 * next call as a work-around, but ... maybe just parameterize the cached
 * models at that point like Mark Adler does in modern crcany/crc.c .
 */

uint64_t crc64_combine(uint64_t crc1, uint64_t crc2, uintmax_t len2, uint64_t poly, uint8_t dim) {
	/* degenerate case */
	if (len2 == 0)
		return crc1;

	unsigned cache_num = 0;
#if USE_STATIC_COMBINE_CACHE
	if (combine_cache[0][0] == 0) {
		init_combine_cache(poly, dim);
	}
#define MASK 63

#else
#define MASK 1
	unsigned n;
	uint64_t row;
	/* not really a cache, but it is if you USE_STATIC_COMBINE_CACHE */
	uint64_t combine_cache[2][64];

	/* put operator for one zero bit in odd */
	combine_cache[1][0] = poly;
	row = 1;
	for (n = 1; n < dim; n++)
	{
		combine_cache[1][n] = row;
		row <<= 1;
	}
	/* put operator for two zero bits in even */
	gf2_matrix_square(combine_cache[0], combine_cache[1], dim);

	/* put operator for four zero bits in odd */
	gf2_matrix_square(combine_cache[1], combine_cache[0], dim);
#endif

	/* apply len2 zeros to crc1 (first square will put the operator for one
	   zero byte, eight zero bits, in even) */
	do
	{
#if !USE_STATIC_COMBINE_CACHE
		gf2_matrix_square(combine_cache[cache_num], combine_cache[!cache_num], dim);
#endif
		/* apply zeros operator for this bit of len2 */
		if (len2 & 1)
			crc1 = GMT(combine_cache[cache_num], crc1);
		len2 >>= 1;
		cache_num = (cache_num + 1) & MASK;
		/* if no more bits set, then done */
	} while (len2 != 0);

	/* return combined crc */
	crc1 ^= crc2;
	return crc1;
}

#undef GMT
