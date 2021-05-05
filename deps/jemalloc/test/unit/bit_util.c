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

int
main(void) {
	return test(
	    test_pow2_ceil_u64,
	    test_pow2_ceil_u32,
	    test_pow2_ceil_zu);
}
