#ifndef JEMALLOC_INTERNAL_FXP_H
#define JEMALLOC_INTERNAL_FXP_H

/*
 * A simple fixed-point math implementation, supporting only unsigned values
 * (with overflow being an error).
 *
 * It's not in general safe to use floating point in core code, because various
 * libc implementations we get linked against can assume that malloc won't touch
 * floating point state and call it with an unusual calling convention.
 */

/*
 * High 16 bits are the integer part, low 16 are the fractional part.  Or
 * equivalently, repr == 2**16 * val, where we use "val" to refer to the
 * (imaginary) fractional representation of the true value.
 *
 * We pick a uint32_t here since it's convenient in some places to
 * double the representation size (i.e. multiplication and division use
 * 64-bit integer types), and a uint64_t is the largest type we're
 * certain is available.
 */
typedef uint32_t fxp_t;
#define FXP_INIT_INT(x) ((x) << 16)
#define FXP_INIT_PERCENT(pct) (((pct) << 16) / 100)

/*
 * Amount of precision used in parsing and printing numbers.  The integer bound
 * is simply because the integer part of the number gets 16 bits, and so is
 * bounded by 65536.
 *
 * We use a lot of precision for the fractional part, even though most of it
 * gets rounded off; this lets us get exact values for the important special
 * case where the denominator is a small power of 2 (for instance,
 * 1/512 == 0.001953125 is exactly representable even with only 16 bits of
 * fractional precision).  We need to left-shift by 16 before dividing by
 * 10**precision, so we pick precision to be floor(log(2**48)) = 14.
 */
#define FXP_INTEGER_PART_DIGITS 5
#define FXP_FRACTIONAL_PART_DIGITS 14

/*
 * In addition to the integer and fractional parts of the number, we need to
 * include a null character and (possibly) a decimal point.
 */
#define FXP_BUF_SIZE (FXP_INTEGER_PART_DIGITS + FXP_FRACTIONAL_PART_DIGITS + 2)

static inline fxp_t
fxp_add(fxp_t a, fxp_t b) {
	return a + b;
}

static inline fxp_t
fxp_sub(fxp_t a, fxp_t b) {
	assert(a >= b);
	return a - b;
}

static inline fxp_t
fxp_mul(fxp_t a, fxp_t b) {
	uint64_t unshifted = (uint64_t)a * (uint64_t)b;
	/*
	 * Unshifted is (a.val * 2**16) * (b.val * 2**16)
	 *   == (a.val * b.val) * 2**32, but we want
	 * (a.val * b.val) * 2 ** 16.
	 */
	return (uint32_t)(unshifted >> 16);
}

static inline fxp_t
fxp_div(fxp_t a, fxp_t b) {
	assert(b != 0);
	uint64_t unshifted = ((uint64_t)a << 32) / (uint64_t)b;
	/*
	 * Unshifted is (a.val * 2**16) * (2**32) / (b.val * 2**16)
	 *   == (a.val / b.val) * (2 ** 32), which again corresponds to a right
	 *   shift of 16.
	 */
	return (uint32_t)(unshifted >> 16);
}

static inline uint32_t
fxp_round_down(fxp_t a) {
	return a >> 16;
}

static inline uint32_t
fxp_round_nearest(fxp_t a) {
	uint32_t fractional_part = (a  & ((1U << 16) - 1));
	uint32_t increment = (uint32_t)(fractional_part >= (1U << 15));
	return (a >> 16) + increment;
}

/*
 * Approximately computes x * frac, without the size limitations that would be
 * imposed by converting u to an fxp_t.
 */
static inline size_t
fxp_mul_frac(size_t x_orig, fxp_t frac) {
	assert(frac <= (1U << 16));
	/*
	 * Work around an over-enthusiastic warning about type limits below (on
	 * 32-bit platforms, a size_t is always less than 1ULL << 48).
	 */
	uint64_t x = (uint64_t)x_orig;
	/*
	 * If we can guarantee no overflow, multiply first before shifting, to
	 * preserve some precision.  Otherwise, shift first and then multiply.
	 * In the latter case, we only lose the low 16 bits of a 48-bit number,
	 * so we're still accurate to within 1/2**32.
	 */
	if (x < (1ULL << 48)) {
		return (size_t)((x * frac) >> 16);
	} else {
		return (size_t)((x >> 16) * (uint64_t)frac);
	}
}

/*
 * Returns true on error.  Otherwise, returns false and updates *ptr to point to
 * the first character not parsed (because it wasn't a digit).
 */
bool fxp_parse(fxp_t *a, const char *ptr, char **end);
void fxp_print(fxp_t a, char buf[FXP_BUF_SIZE]);

#endif /* JEMALLOC_INTERNAL_FXP_H */
