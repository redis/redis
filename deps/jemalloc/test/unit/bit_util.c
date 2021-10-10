#include "test/jemalloc_test.h"

#include "jemalloc/internal/bit_util.h"

#define TEST_POW2_CEIL(t, suf, pri) do {				\
	unsigned i, pow2;						\
	t x;								\
									\
	assert_##suf##_eq(pow2_ceil_##suf(0), 0, "Unexpected result");	\
									\
	for (i = 0; i < sizeof(t) * 8; i++) {				\
		assert_##suf##_eq(pow2_ceil_##suf(((t)1) << i), ((t)1)	\
		    << i, "Unexpected result");				\
	}								\
									\
	for (i = 2; i < sizeof(t) * 8; i++) {				\
		assert_##suf##_eq(pow2_ceil_##suf((((t)1) << i) - 1),	\
		    ((t)1) << i, "Unexpected result");			\
	}								\
									\
	for (i = 0; i < sizeof(t) * 8 - 1; i++) {			\
		assert_##suf##_eq(pow2_ceil_##suf((((t)1) << i) + 1),	\
		    ((t)1) << (i+1), "Unexpected result");		\
	}								\
									\
	for (pow2 = 1; pow2 < 25; pow2++) {				\
		for (x = (((t)1) << (pow2-1)) + 1; x <= ((t)1) << pow2;	\
		    x++) {						\
			assert_##suf##_eq(pow2_ceil_##suf(x),		\
			    ((t)1) << pow2,				\
			    "Unexpected result, x=%"pri, x);		\
		}							\
	}								\
} while (0)

TEST_BEGIN(test_pow2_ceil_u64) {
	TEST_POW2_CEIL(uint64_t, u64, FMTu64);
}
TEST_END

TEST_BEGIN(test_pow2_ceil_u32) {
	TEST_POW2_CEIL(uint32_t, u32, FMTu32);
}
TEST_END

TEST_BEGIN(test_pow2_ceil_zu) {
	TEST_POW2_CEIL(size_t, zu, "zu");
}
TEST_END

void
assert_lg_ceil_range(size_t input, unsigned answer) {
	if (input == 1) {
		assert_u_eq(0, answer, "Got %u as lg_ceil of 1", answer);
		return;
	}
	assert_zu_le(input, (ZU(1) << answer),
	    "Got %u as lg_ceil of %zu", answer, input);
	assert_zu_gt(input, (ZU(1) << (answer - 1)),
	    "Got %u as lg_ceil of %zu", answer, input);
}

void
assert_lg_floor_range(size_t input, unsigned answer) {
	if (input == 1) {
		assert_u_eq(0, answer, "Got %u as lg_floor of 1", answer);
		return;
	}
	assert_zu_ge(input, (ZU(1) << answer),
	    "Got %u as lg_floor of %zu", answer, input);
	assert_zu_lt(input, (ZU(1) << (answer + 1)),
	    "Got %u as lg_floor of %zu", answer, input);
}

TEST_BEGIN(test_lg_ceil_floor) {
	for (size_t i = 1; i < 10 * 1000 * 1000; i++) {
		assert_lg_ceil_range(i, lg_ceil(i));
		assert_lg_ceil_range(i, LG_CEIL(i));
		assert_lg_floor_range(i, lg_floor(i));
		assert_lg_floor_range(i, LG_FLOOR(i));
	}
	for (int i = 10; i < 8 * (1 << LG_SIZEOF_PTR) - 5; i++) {
		for (size_t j = 0; j < (1 << 4); j++) {
			size_t num1 = ((size_t)1 << i)
			    - j * ((size_t)1 << (i - 4));
			size_t num2 = ((size_t)1 << i)
			    + j * ((size_t)1 << (i - 4));
			assert_zu_ne(num1, 0, "Invalid lg argument");
			assert_zu_ne(num2, 0, "Invalid lg argument");
			assert_lg_ceil_range(num1, lg_ceil(num1));
			assert_lg_ceil_range(num1, LG_CEIL(num1));
			assert_lg_ceil_range(num2, lg_ceil(num2));
			assert_lg_ceil_range(num2, LG_CEIL(num2));

			assert_lg_floor_range(num1, lg_floor(num1));
			assert_lg_floor_range(num1, LG_FLOOR(num1));
			assert_lg_floor_range(num2, lg_floor(num2));
			assert_lg_floor_range(num2, LG_FLOOR(num2));
		}
	}
}
TEST_END

int
main(void) {
	return test(
	    test_pow2_ceil_u64,
	    test_pow2_ceil_u32,
	    test_pow2_ceil_zu,
	    test_lg_ceil_floor);
}
