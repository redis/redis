#ifndef JEMALLOC_INTERNAL_PRNG_H
#define JEMALLOC_INTERNAL_PRNG_H

#include "jemalloc/internal/bit_util.h"

/*
 * Simple linear congruential pseudo-random number generator:
 *
 *   prng(y) = (a*x + c) % m
 *
 * where the following constants ensure maximal period:
 *
 *   a == Odd number (relatively prime to 2^n), and (a-1) is a multiple of 4.
 *   c == Odd number (relatively prime to 2^n).
 *   m == 2^32
 *
 * See Knuth's TAOCP 3rd Ed., Vol. 2, pg. 17 for details on these constraints.
 *
 * This choice of m has the disadvantage that the quality of the bits is
 * proportional to bit position.  For example, the lowest bit has a cycle of 2,
 * the next has a cycle of 4, etc.  For this reason, we prefer to use the upper
 * bits.
 */

/******************************************************************************/
/* INTERNAL DEFINITIONS -- IGNORE */
/******************************************************************************/
#define PRNG_A_32	UINT32_C(1103515241)
#define PRNG_C_32	UINT32_C(12347)

#define PRNG_A_64	UINT64_C(6364136223846793005)
#define PRNG_C_64	UINT64_C(1442695040888963407)

JEMALLOC_ALWAYS_INLINE uint32_t
prng_state_next_u32(uint32_t state) {
	return (state * PRNG_A_32) + PRNG_C_32;
}

JEMALLOC_ALWAYS_INLINE uint64_t
prng_state_next_u64(uint64_t state) {
	return (state * PRNG_A_64) + PRNG_C_64;
}

JEMALLOC_ALWAYS_INLINE size_t
prng_state_next_zu(size_t state) {
#if LG_SIZEOF_PTR == 2
	return (state * PRNG_A_32) + PRNG_C_32;
#elif LG_SIZEOF_PTR == 3
	return (state * PRNG_A_64) + PRNG_C_64;
#else
#error Unsupported pointer size
#endif
}

/******************************************************************************/
/* BEGIN PUBLIC API */
/******************************************************************************/

/*
 * The prng_lg_range functions give a uniform int in the half-open range [0,
 * 2**lg_range).
 */

JEMALLOC_ALWAYS_INLINE uint32_t
prng_lg_range_u32(uint32_t *state, unsigned lg_range) {
	assert(lg_range > 0);
	assert(lg_range <= 32);

	*state = prng_state_next_u32(*state);
	uint32_t ret = *state >> (32 - lg_range);

	return ret;
}

JEMALLOC_ALWAYS_INLINE uint64_t
prng_lg_range_u64(uint64_t *state, unsigned lg_range) {
	assert(lg_range > 0);
	assert(lg_range <= 64);

	*state = prng_state_next_u64(*state);
	uint64_t ret = *state >> (64 - lg_range);

	return ret;
}

JEMALLOC_ALWAYS_INLINE size_t
prng_lg_range_zu(size_t *state, unsigned lg_range) {
	assert(lg_range > 0);
	assert(lg_range <= ZU(1) << (3 + LG_SIZEOF_PTR));

	*state = prng_state_next_zu(*state);
	size_t ret = *state >> ((ZU(1) << (3 + LG_SIZEOF_PTR)) - lg_range);

	return ret;
}

/*
 * The prng_range functions behave like the prng_lg_range, but return a result
 * in [0, range) instead of [0, 2**lg_range).
 */

JEMALLOC_ALWAYS_INLINE uint32_t
prng_range_u32(uint32_t *state, uint32_t range) {
	assert(range != 0);
	/*
	 * If range were 1, lg_range would be 0, so the shift in
	 * prng_lg_range_u32 would be a shift of a 32-bit variable by 32 bits,
	 * which is UB.  Just handle this case as a one-off.
	 */
	if (range == 1) {
		return 0;
	}

	/* Compute the ceiling of lg(range). */
	unsigned lg_range = ffs_u32(pow2_ceil_u32(range));

	/* Generate a result in [0..range) via repeated trial. */
	uint32_t ret;
	do {
		ret = prng_lg_range_u32(state, lg_range);
	} while (ret >= range);

	return ret;
}

JEMALLOC_ALWAYS_INLINE uint64_t
prng_range_u64(uint64_t *state, uint64_t range) {
	assert(range != 0);

	/* See the note in prng_range_u32. */
	if (range == 1) {
		return 0;
	}

	/* Compute the ceiling of lg(range). */
	unsigned lg_range = ffs_u64(pow2_ceil_u64(range));

	/* Generate a result in [0..range) via repeated trial. */
	uint64_t ret;
	do {
		ret = prng_lg_range_u64(state, lg_range);
	} while (ret >= range);

	return ret;
}

JEMALLOC_ALWAYS_INLINE size_t
prng_range_zu(size_t *state, size_t range) {
	assert(range != 0);

	/* See the note in prng_range_u32. */
	if (range == 1) {
		return 0;
	}

	/* Compute the ceiling of lg(range). */
	unsigned lg_range = ffs_u64(pow2_ceil_u64(range));

	/* Generate a result in [0..range) via repeated trial. */
	size_t ret;
	do {
		ret = prng_lg_range_zu(state, lg_range);
	} while (ret >= range);

	return ret;
}

#endif /* JEMALLOC_INTERNAL_PRNG_H */
