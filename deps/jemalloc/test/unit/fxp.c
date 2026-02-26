#include "test/jemalloc_test.h"

#include "jemalloc/internal/fxp.h"

static double
fxp2double(fxp_t a) {
	double intpart = (double)(a >> 16);
	double fracpart = (double)(a & ((1U << 16) - 1)) / (1U << 16);
	return intpart + fracpart;
}

/* Is a close to b? */
static bool
double_close(double a, double b) {
	/*
	 * Our implementation doesn't try for precision.  Correspondingly, don't
	 * enforce it too strenuously here; accept values that are close in
	 * either relative or absolute terms.
	 */
	return fabs(a - b) < 0.01 || fabs(a - b) / a < 0.01;
}

static bool
fxp_close(fxp_t a, fxp_t b) {
	return double_close(fxp2double(a), fxp2double(b));
}

static fxp_t
xparse_fxp(const char *str) {
	fxp_t result;
	bool err = fxp_parse(&result, str, NULL);
	assert_false(err, "Invalid fxp string: %s", str);
	return result;
}

static void
expect_parse_accurate(const char *str, const char *parse_str) {
	double true_val = strtod(str, NULL);
	fxp_t fxp_val;
	char *end;
	bool err = fxp_parse(&fxp_val, parse_str, &end);
	expect_false(err, "Unexpected parse failure");
	expect_ptr_eq(parse_str + strlen(str), end,
	    "Didn't parse whole string");
	expect_true(double_close(fxp2double(fxp_val), true_val),
	    "Misparsed %s", str);
}

static void
parse_valid_trial(const char *str) {
	/* The value it parses should be correct. */
	expect_parse_accurate(str, str);
	char buf[100];
	snprintf(buf, sizeof(buf), "%swith_some_trailing_text", str);
	expect_parse_accurate(str, buf);
	snprintf(buf, sizeof(buf), "%s with a space", str);
	expect_parse_accurate(str, buf);
	snprintf(buf, sizeof(buf), "%s,in_a_malloc_conf_string:1", str);
	expect_parse_accurate(str, buf);
}

TEST_BEGIN(test_parse_valid) {
	parse_valid_trial("0");
	parse_valid_trial("1");
	parse_valid_trial("2");
	parse_valid_trial("100");
	parse_valid_trial("345");
	parse_valid_trial("00000000123");
	parse_valid_trial("00000000987");

	parse_valid_trial("0.0");
	parse_valid_trial("0.00000000000456456456");
	parse_valid_trial("100.00000000000456456456");

	parse_valid_trial("123.1");
	parse_valid_trial("123.01");
	parse_valid_trial("123.001");
	parse_valid_trial("123.0001");
	parse_valid_trial("123.00001");
	parse_valid_trial("123.000001");
	parse_valid_trial("123.0000001");

	parse_valid_trial(".0");
	parse_valid_trial(".1");
	parse_valid_trial(".01");
	parse_valid_trial(".001");
	parse_valid_trial(".0001");
	parse_valid_trial(".00001");
	parse_valid_trial(".000001");

	parse_valid_trial(".1");
	parse_valid_trial(".10");
	parse_valid_trial(".100");
	parse_valid_trial(".1000");
	parse_valid_trial(".100000");
}
TEST_END

static void
expect_parse_failure(const char *str) {
	fxp_t result = FXP_INIT_INT(333);
	char *end = (void *)0x123;
	bool err = fxp_parse(&result, str, &end);
	expect_true(err, "Expected a parse error on: %s", str);
	expect_ptr_eq((void *)0x123, end,
	    "Parse error shouldn't change results");
	expect_u32_eq(result, FXP_INIT_INT(333),
	    "Parse error shouldn't change results");
}

TEST_BEGIN(test_parse_invalid) {
	expect_parse_failure("123.");
	expect_parse_failure("3.a");
	expect_parse_failure(".a");
	expect_parse_failure("a.1");
	expect_parse_failure("a");
	/* A valid string, but one that overflows. */
	expect_parse_failure("123456789");
	expect_parse_failure("0000000123456789");
	expect_parse_failure("1000000");
}
TEST_END

static void
expect_init_percent(unsigned percent, const char *str) {
	fxp_t result_init = FXP_INIT_PERCENT(percent);
	fxp_t result_parse = xparse_fxp(str);
	expect_u32_eq(result_init, result_parse,
	    "Expect representations of FXP_INIT_PERCENT(%u) and "
	    "fxp_parse(\"%s\") to be equal; got %x and %x",
	    percent, str, result_init, result_parse);

}

/*
 * Every other test uses either parsing or FXP_INIT_INT; it gets tested in those
 * ways.  We need a one-off for the percent-based initialization, though.
 */
TEST_BEGIN(test_init_percent) {
	expect_init_percent(100, "1");
	expect_init_percent(75, ".75");
	expect_init_percent(1, ".01");
	expect_init_percent(50, ".5");
}
TEST_END

static void
expect_add(const char *astr, const char *bstr, const char* resultstr) {
	fxp_t a = xparse_fxp(astr);
	fxp_t b = xparse_fxp(bstr);
	fxp_t result = xparse_fxp(resultstr);
	expect_true(fxp_close(fxp_add(a, b), result),
	    "Expected %s + %s == %s", astr, bstr, resultstr);
}

TEST_BEGIN(test_add_simple) {
	expect_add("0", "0", "0");
	expect_add("0", "1", "1");
	expect_add("1", "1", "2");
	expect_add("1.5", "1.5", "3");
	expect_add("0.1", "0.1", "0.2");
	expect_add("123", "456", "579");
}
TEST_END

static void
expect_sub(const char *astr, const char *bstr, const char* resultstr) {
	fxp_t a = xparse_fxp(astr);
	fxp_t b = xparse_fxp(bstr);
	fxp_t result = xparse_fxp(resultstr);
	expect_true(fxp_close(fxp_sub(a, b), result),
	    "Expected %s - %s == %s", astr, bstr, resultstr);
}

TEST_BEGIN(test_sub_simple) {
	expect_sub("0", "0", "0");
	expect_sub("1", "0", "1");
	expect_sub("1", "1", "0");
	expect_sub("3.5", "1.5", "2");
	expect_sub("0.3", "0.1", "0.2");
	expect_sub("456", "123", "333");
}
TEST_END

static void
expect_mul(const char *astr, const char *bstr, const char* resultstr) {
	fxp_t a = xparse_fxp(astr);
	fxp_t b = xparse_fxp(bstr);
	fxp_t result = xparse_fxp(resultstr);
	expect_true(fxp_close(fxp_mul(a, b), result),
	    "Expected %s * %s == %s", astr, bstr, resultstr);
}

TEST_BEGIN(test_mul_simple) {
	expect_mul("0", "0", "0");
	expect_mul("1", "0", "0");
	expect_mul("1", "1", "1");
	expect_mul("1.5", "1.5", "2.25");
	expect_mul("100.0", "10", "1000");
	expect_mul(".1", "10", "1");
}
TEST_END

static void
expect_div(const char *astr, const char *bstr, const char* resultstr) {
	fxp_t a = xparse_fxp(astr);
	fxp_t b = xparse_fxp(bstr);
	fxp_t result = xparse_fxp(resultstr);
	expect_true(fxp_close(fxp_div(a, b), result),
	    "Expected %s / %s == %s", astr, bstr, resultstr);
}

TEST_BEGIN(test_div_simple) {
	expect_div("1", "1", "1");
	expect_div("0", "1", "0");
	expect_div("2", "1", "2");
	expect_div("3", "2", "1.5");
	expect_div("3", "1.5", "2");
	expect_div("10", ".1", "100");
	expect_div("123", "456", ".2697368421");
}
TEST_END

static void
expect_round(const char *str, uint32_t rounded_down, uint32_t rounded_nearest) {
	fxp_t fxp = xparse_fxp(str);
	uint32_t fxp_rounded_down = fxp_round_down(fxp);
	uint32_t fxp_rounded_nearest = fxp_round_nearest(fxp);
	expect_u32_eq(rounded_down, fxp_rounded_down,
	    "Mistake rounding %s down", str);
	expect_u32_eq(rounded_nearest, fxp_rounded_nearest,
	    "Mistake rounding %s to nearest", str);
}

TEST_BEGIN(test_round_simple) {
	expect_round("1.5", 1, 2);
	expect_round("0", 0, 0);
	expect_round("0.1", 0, 0);
	expect_round("0.4", 0, 0);
	expect_round("0.40000", 0, 0);
	expect_round("0.5", 0, 1);
	expect_round("0.6", 0, 1);
	expect_round("123", 123, 123);
	expect_round("123.4", 123, 123);
	expect_round("123.5", 123, 124);
}
TEST_END

static void
expect_mul_frac(size_t a, const char *fracstr, size_t expected) {
	fxp_t frac = xparse_fxp(fracstr);
	size_t result = fxp_mul_frac(a, frac);
	expect_true(double_close(expected, result),
	    "Expected %zu * %s == %zu (fracmul); got %zu", a, fracstr,
	    expected, result);
}

TEST_BEGIN(test_mul_frac_simple) {
	expect_mul_frac(SIZE_MAX, "1.0", SIZE_MAX);
	expect_mul_frac(SIZE_MAX, ".75", SIZE_MAX / 4 * 3);
	expect_mul_frac(SIZE_MAX, ".5", SIZE_MAX / 2);
	expect_mul_frac(SIZE_MAX, ".25", SIZE_MAX / 4);
	expect_mul_frac(1U << 16, "1.0", 1U << 16);
	expect_mul_frac(1U << 30, "0.5", 1U << 29);
	expect_mul_frac(1U << 30, "0.25", 1U << 28);
	expect_mul_frac(1U << 30, "0.125", 1U << 27);
	expect_mul_frac((1U << 30) + 1, "0.125", 1U << 27);
	expect_mul_frac(100, "0.25", 25);
	expect_mul_frac(1000 * 1000, "0.001", 1000);
}
TEST_END

static void
expect_print(const char *str) {
	fxp_t fxp = xparse_fxp(str);
	char buf[FXP_BUF_SIZE];
	fxp_print(fxp, buf);
	expect_d_eq(0, strcmp(str, buf), "Couldn't round-trip print %s", str);
}

TEST_BEGIN(test_print_simple) {
	expect_print("0.0");
	expect_print("1.0");
	expect_print("2.0");
	expect_print("123.0");
	/*
	 * We hit the possibility of roundoff errors whenever the fractional
	 * component isn't a round binary number; only check these here (we
	 * round-trip properly in the stress test).
	 */
	expect_print("1.5");
	expect_print("3.375");
	expect_print("0.25");
	expect_print("0.125");
	/* 1 / 2**14 */
	expect_print("0.00006103515625");
}
TEST_END

TEST_BEGIN(test_stress) {
	const char *numbers[] = {
		"0.0", "0.1", "0.2", "0.3", "0.4",
		"0.5", "0.6", "0.7", "0.8", "0.9",

		"1.0", "1.1", "1.2", "1.3", "1.4",
		"1.5", "1.6", "1.7", "1.8", "1.9",

		"2.0", "2.1", "2.2", "2.3", "2.4",
		"2.5", "2.6", "2.7", "2.8", "2.9",

		"17.0", "17.1", "17.2", "17.3", "17.4",
		"17.5", "17.6", "17.7", "17.8", "17.9",

		"18.0", "18.1", "18.2", "18.3", "18.4",
		"18.5", "18.6", "18.7", "18.8", "18.9",

		"123.0", "123.1", "123.2", "123.3", "123.4",
		"123.5", "123.6", "123.7", "123.8", "123.9",

		"124.0", "124.1", "124.2", "124.3", "124.4",
		"124.5", "124.6", "124.7", "124.8", "124.9",

		"125.0", "125.1", "125.2", "125.3", "125.4",
		"125.5", "125.6", "125.7", "125.8", "125.9"};
	size_t numbers_len = sizeof(numbers)/sizeof(numbers[0]);
	for (size_t i = 0; i < numbers_len; i++) {
		fxp_t fxp_a = xparse_fxp(numbers[i]);
		double double_a = strtod(numbers[i], NULL);

		uint32_t fxp_rounded_down = fxp_round_down(fxp_a);
		uint32_t fxp_rounded_nearest = fxp_round_nearest(fxp_a);
		uint32_t double_rounded_down = (uint32_t)double_a;
		uint32_t double_rounded_nearest = (uint32_t)round(double_a);

		expect_u32_eq(double_rounded_down, fxp_rounded_down,
		    "Incorrectly rounded down %s", numbers[i]);
		expect_u32_eq(double_rounded_nearest, fxp_rounded_nearest,
		    "Incorrectly rounded-to-nearest %s", numbers[i]);

		for (size_t j = 0; j < numbers_len; j++) {
			fxp_t fxp_b = xparse_fxp(numbers[j]);
			double double_b = strtod(numbers[j], NULL);

			fxp_t fxp_sum = fxp_add(fxp_a, fxp_b);
			double double_sum = double_a + double_b;
			expect_true(
			    double_close(fxp2double(fxp_sum), double_sum),
			    "Miscomputed %s + %s", numbers[i], numbers[j]);

			if (double_a > double_b) {
				fxp_t fxp_diff = fxp_sub(fxp_a, fxp_b);
				double double_diff = double_a - double_b;
				expect_true(
				    double_close(fxp2double(fxp_diff),
				    double_diff),
				    "Miscomputed %s - %s", numbers[i],
				    numbers[j]);
			}

			fxp_t fxp_prod = fxp_mul(fxp_a, fxp_b);
			double double_prod = double_a * double_b;
			expect_true(
			    double_close(fxp2double(fxp_prod), double_prod),
			    "Miscomputed %s * %s", numbers[i], numbers[j]);

			if (double_b != 0.0) {
				fxp_t fxp_quot = fxp_div(fxp_a, fxp_b);
				double double_quot = double_a / double_b;
				expect_true(
				    double_close(fxp2double(fxp_quot),
				    double_quot),
				    "Miscomputed %s / %s", numbers[i],
				    numbers[j]);
			}
		}
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_parse_valid,
	    test_parse_invalid,
	    test_init_percent,
	    test_add_simple,
	    test_sub_simple,
	    test_mul_simple,
	    test_div_simple,
	    test_round_simple,
	    test_mul_frac_simple,
	    test_print_simple,
	    test_stress);
}
