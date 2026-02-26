#include "test/jemalloc_test.h"
#include "test/bench.h"

static void
large_mallocx_free(void) {
	/*
	 * We go a bit larger than the large minclass on its own to better
	 * expose costs from things like zeroing.
	 */
	void *p = mallocx(SC_LARGE_MINCLASS, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "mallocx shouldn't fail");
	free(p);
}

static void
small_mallocx_free(void) {
	void *p = mallocx(16, 0);
	assert_ptr_not_null(p, "mallocx shouldn't fail");
	free(p);
}

TEST_BEGIN(test_large_vs_small) {
	compare_funcs(100*1000, 1*1000*1000, "large mallocx",
	    large_mallocx_free, "small mallocx", small_mallocx_free);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_large_vs_small);
}

