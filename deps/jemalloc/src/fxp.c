#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/fxp.h"

static bool
fxp_isdigit(char c) {
	return '0' <= c && c <= '9';
}

bool
fxp_parse(fxp_t *result, const char *str, char **end) {
	/*
	 * Using malloc_strtoumax in this method isn't as handy as you might
	 * expect (I tried). In the fractional part, significant leading zeros
	 * mean that you still need to do your own parsing, now with trickier
	 * math.  In the integer part, the casting (uintmax_t to uint32_t)
	 * forces more reasoning about bounds than just checking for overflow as
	 * we parse.
	 */
	uint32_t integer_part = 0;

	const char *cur = str;

	/* The string must start with a digit or a decimal point. */
	if (*cur != '.' && !fxp_isdigit(*cur)) {
		return true;
	}

	while ('0' <= *cur && *cur <= '9') {
		integer_part *= 10;
		integer_part += *cur - '0';
		if (integer_part >= (1U << 16)) {
			return true;
		}
		cur++;
	}

	/*
	 * We've parsed all digits at the beginning of the string, without
	 * overflow.  Either we're done, or there's a fractional part.
	 */
	if (*cur != '.') {
		*result = (integer_part << 16);
		if (end != NULL) {
			*end = (char *)cur;
		}
		return false;
	}

	/* There's a fractional part. */
	cur++;
	if (!fxp_isdigit(*cur)) {
		/* Shouldn't end on the decimal point. */
		return true;
	}

	/*
	 * We use a lot of precision for the fractional part, even though we'll
	 * discard most of it; this lets us get exact values for the important
	 * special case where the denominator is a small power of 2 (for
	 * instance, 1/512 == 0.001953125 is exactly representable even with
	 * only 16 bits of fractional precision).  We need to left-shift by 16
	 * before dividing so we pick the number of digits to be
	 * floor(log(2**48)) = 14.
	 */
	uint64_t fractional_part = 0;
	uint64_t frac_div = 1;
	for (int i = 0; i < FXP_FRACTIONAL_PART_DIGITS; i++) {
		fractional_part *= 10;
		frac_div *= 10;
		if (fxp_isdigit(*cur)) {
			fractional_part += *cur - '0';
			cur++;
		}
	}
	/*
	 * We only parse the first maxdigits characters, but we can still ignore
	 * any digits after that.
	 */
	while (fxp_isdigit(*cur)) {
		cur++;
	}

	assert(fractional_part < frac_div);
	uint32_t fractional_repr = (uint32_t)(
	    (fractional_part << 16) / frac_div);

	/* Success! */
	*result = (integer_part << 16) + fractional_repr;
	if (end != NULL) {
		*end = (char *)cur;
	}
	return false;
}

void
fxp_print(fxp_t a, char buf[FXP_BUF_SIZE]) {
	uint32_t integer_part = fxp_round_down(a);
	uint32_t fractional_part = (a & ((1U << 16) - 1));

	int leading_fraction_zeros = 0;
	uint64_t fraction_digits = fractional_part;
	for (int i = 0; i < FXP_FRACTIONAL_PART_DIGITS; i++) {
		if (fraction_digits < (1U << 16)
		    && fraction_digits * 10 >= (1U << 16)) {
			leading_fraction_zeros = i;
		}
		fraction_digits *= 10;
	}
	fraction_digits >>= 16;
	while (fraction_digits > 0 && fraction_digits % 10 == 0) {
		fraction_digits /= 10;
	}

	size_t printed = malloc_snprintf(buf, FXP_BUF_SIZE, "%"FMTu32".",
	    integer_part);
	for (int i = 0; i < leading_fraction_zeros; i++) {
		buf[printed] = '0';
		printed++;
	}
	malloc_snprintf(&buf[printed], FXP_BUF_SIZE - printed, "%"FMTu64,
	    fraction_digits);
}
