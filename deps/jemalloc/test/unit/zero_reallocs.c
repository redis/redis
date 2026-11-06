#include "test/jemalloc_test.h"

static size_t
zero_reallocs() {
	if (!config_stats) {
		return 0;
	}
	size_t count = 12345;
	size_t sz = sizeof(count);

	expect_d_eq(mallctl("stats.zero_reallocs", (void *)&count, &sz,
	    NULL, 0), 0, "Unexpected mallctl failure");
	return count;
}

TEST_BEGIN(test_zero_reallocs) {
	test_skip_if(!config_stats);

	for (size_t i = 0; i < 100; ++i) {
		void *ptr = mallocx(i * i + 1, 0);
		expect_ptr_not_null(ptr, "Unexpected mallocx error");
		size_t count = zero_reallocs();
		expect_zu_eq(i, count, "Incorrect zero realloc count");
		ptr = realloc(ptr, 0);
		expect_ptr_null(ptr, "Realloc didn't free");
		count = zero_reallocs();
		expect_zu_eq(i + 1, count, "Realloc didn't adjust count");
	}
}
TEST_END

int
main(void) {
	/*
	 * We expect explicit counts; reentrant tests run multiple times, so
	 * counts leak across runs.
	 */
	return test_no_reentrancy(
	    test_zero_reallocs);
}
