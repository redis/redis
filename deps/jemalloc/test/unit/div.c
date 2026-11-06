#include "test/jemalloc_test.h"

#include "jemalloc/internal/div.h"

TEST_BEGIN(test_div_exhaustive) {
	for (size_t divisor = 2; divisor < 1000 * 1000; ++divisor) {
		div_info_t div_info;
		div_init(&div_info, divisor);
		size_t max = 1000 * divisor;
		if (max < 1000 * 1000) {
			max = 1000 * 1000;
		}
		for (size_t dividend = 0; dividend < 1000 * divisor;
		    dividend += divisor) {
			size_t quotient = div_compute(
			    &div_info, dividend);
			expect_zu_eq(dividend, quotient * divisor,
			    "With divisor = %zu, dividend = %zu, "
			    "got quotient %zu", divisor, dividend, quotient);
		}
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_div_exhaustive);
}
