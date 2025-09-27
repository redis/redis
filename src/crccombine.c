#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#if defined(__i386__) || defined(__X86_64__)
#include <immintrin.h>
#endif
#include "crccombine.h"

/* Copyright (C) 2013 Mark Adler
 * Copyright (C) 2019-2024 Josiah Carlson
 * Portions originally from: crc64.c Version 1.4  16 Dec 2013  Mark Adler
 * Modifications by Josiah Carlson <josiah.carlson@gmail.com>
 *   - Added implementation variations with sample timings for gf_matrix_times*()
 *   - Most folks would be best using gf2_matrix_times_vec or
 *	   gf2_matrix_times_vec2, unless some processor does AVX2 fast.
 *   - This is the implementation of the MERGE_CRC macro defined in
 *     crcspeed.c (which calls crc_combine()), and is a specialization of the
 *     generic crc_combine() (and related from the 2013 edition of Mark Adler's
 *     crc64.c)) for the sake of clarity and performance.

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

#if !((defined(__i386__) || defined(__X86_64__)))

/* This cuts 40% of the time vs bit-by-bit. */

uint64_t gf2_matrix_times_switch(uint64_t *mat, uint64_t vec) {
	/*
	 * Without using any vector math, this handles 4 bits at a time,
	 * and saves 40+% of the time compared to the bit-by-bit version. Use if you
	 * have no vector compile option available to you. With cache, we see:
	 * E5-2670 ~1-2us to extend ~1 meg 64 bit hash
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

#define CRC_MULTIPLY gf2_matrix_times_switch

#else

/*
	Warning: here there be dragons involving vector math, and macros to save us
	from repeating the same information over and over.
*/

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
	/* this table allows us to eliminate conditions during gf2_matrix_times_vec2() */
	static v2uq masks2[4] = {
		{0,0},
		{-1,0},
		{0,-1},
		{-1,-1},
	};

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

#define CRC_MULTIPLY gf2_matrix_times_vec2
#endif

static void gf2_matrix_square(uint64_t *square, uint64_t *mat, uint8_t dim) {
	unsigned n;

	for (n = 0; n < dim; n++)
		square[n] = CRC_MULTIPLY(mat, mat[n]);
}

/* Turns out our Redis / Jones CRC cycles at this point, so we can support
 * more than 64 bits of extension if we want. Trivially. */
static uint64_t combine_cache[64][64];

/* Mark Adler has some amazing updates to crc.c in his crcany repository. I
 * like static caches, and not worrying about finding cycles generally. We are
 * okay to spend the 32k of memory here, leaving the algorithm unchanged from
 * as it was a decade ago, and be happy that it costs <200 microseconds to
 * init, and that subsequent calls to the combine function take under 100
 * nanoseconds. We also note that the crcany/crc.c code applies to any CRC, and
 * we are currently targeting one: Jones CRC64.
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
	 * re-generated in the last two layers for the Redis polynomial */
	do {
		gf2_matrix_square(combine_cache[cache_num], combine_cache[cache_num + prev], dim);
		prev = -1;
	} while (++cache_num < 64);
}

/* Return the CRC-64 of two sequential blocks, where crc1 is the CRC-64 of the
 * first block, crc2 is the CRC-64 of the second block, and len2 is the length
 * of the second block.
 *
 * If you want reflections on your CRCs; do them outside before / after.
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
	if (combine_cache[0][0] == 0) {
		init_combine_cache(poly, dim);
	}

	/* apply len2 zeros to crc1 (first square will put the operator for one
	   zero byte, eight zero bits, in even) */
	do
	{
		/* apply zeros operator for this bit of len2 */
		if (len2 & 1)
			crc1 = CRC_MULTIPLY(combine_cache[cache_num], crc1);
		len2 >>= 1;
		cache_num = (cache_num + 1) & 63;
		/* if no more bits set, then done */
	} while (len2 != 0);

	/* return combined crc */
	crc1 ^= crc2;
	return crc1;
}

#undef CRC_MULTIPLY
