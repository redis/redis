#include "test/jemalloc_test.h"

TEST_BEGIN(test_prof_realloc) {
	tsdn_t *tsdn;
	int flags;
	void *p, *q;
	prof_tctx_t *tctx_p, *tctx_q;
	uint64_t curobjs_0, curobjs_1, curobjs_2, curobjs_3;

	test_skip_if(!config_prof);

	tsdn = tsdn_fetch();
	flags = MALLOCX_TCACHE_NONE;

	prof_cnt_all(&curobjs_0, NULL, NULL, NULL);
	p = mallocx(1024, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tctx_p = prof_tctx_get(tsdn, p, NULL);
	assert_ptr_ne(tctx_p, (prof_tctx_t *)(uintptr_t)1U,
	    "Expected valid tctx");
	prof_cnt_all(&curobjs_1, NULL, NULL, NULL);
	assert_u64_eq(curobjs_0 + 1, curobjs_1,
	    "Allocation should have increased sample size");

	q = rallocx(p, 2048, flags);
	assert_ptr_ne(p, q, "Expected move");
	assert_ptr_not_null(p, "Unexpected rmallocx() failure");
	tctx_q = prof_tctx_get(tsdn, q, NULL);
	assert_ptr_ne(tctx_q, (prof_tctx_t *)(uintptr_t)1U,
	    "Expected valid tctx");
	prof_cnt_all(&curobjs_2, NULL, NULL, NULL);
	assert_u64_eq(curobjs_1, curobjs_2,
	    "Reallocation should not have changed sample size");

	dallocx(q, flags);
	prof_cnt_all(&curobjs_3, NULL, NULL, NULL);
	assert_u64_eq(curobjs_0, curobjs_3,
	    "Sample size should have returned to base level");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_prof_realloc);
}
