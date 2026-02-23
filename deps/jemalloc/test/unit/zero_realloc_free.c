#include "test/jemalloc_test.h"

static uint64_t
deallocated() {
	if (!config_stats) {
		return 0;
	}
	uint64_t deallocated;
	size_t sz = sizeof(deallocated);
	expect_d_eq(mallctl("thread.deallocated", (void *)&deallocated, &sz,
	    NULL, 0), 0, "Unexpected mallctl failure");
	return deallocated;
}

TEST_BEGIN(test_realloc_free) {
	void *ptr = mallocx(42, 0);
	expect_ptr_not_null(ptr, "Unexpected mallocx error");
	uint64_t deallocated_before = deallocated();
	ptr = realloc(ptr, 0);
	uint64_t deallocated_after = deallocated();
	expect_ptr_null(ptr, "Realloc didn't free");
	if (config_stats) {
		expect_u64_gt(deallocated_after, deallocated_before,
		    "Realloc didn't free");
	}
}
TEST_END

int
main(void) {
	return test(
	    test_realloc_free);
}
