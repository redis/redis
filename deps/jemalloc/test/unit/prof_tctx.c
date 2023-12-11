#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_data.h"

TEST_BEGIN(test_prof_realloc) {
	tsd_t *tsd;
	int flags;
	void *p, *q;
	prof_info_t prof_info_p, prof_info_q;
	prof_cnt_t cnt_0, cnt_1, cnt_2, cnt_3;

	test_skip_if(!config_prof);

	tsd = tsd_fetch();
	flags = MALLOCX_TCACHE_NONE;

	prof_cnt_all(&cnt_0);
	p = mallocx(1024, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	prof_info_get(tsd, p, NULL, &prof_info_p);
	expect_ptr_ne(prof_info_p.alloc_tctx, (prof_tctx_t *)(uintptr_t)1U,
	    "Expected valid tctx");
	prof_cnt_all(&cnt_1);
	expect_u64_eq(cnt_0.curobjs + 1, cnt_1.curobjs,
	    "Allocation should have increased sample size");

	q = rallocx(p, 2048, flags);
	expect_ptr_ne(p, q, "Expected move");
	expect_ptr_not_null(p, "Unexpected rmallocx() failure");
	prof_info_get(tsd, q, NULL, &prof_info_q);
	expect_ptr_ne(prof_info_q.alloc_tctx, (prof_tctx_t *)(uintptr_t)1U,
	    "Expected valid tctx");
	prof_cnt_all(&cnt_2);
	expect_u64_eq(cnt_1.curobjs, cnt_2.curobjs,
	    "Reallocation should not have changed sample size");

	dallocx(q, flags);
	prof_cnt_all(&cnt_3);
	expect_u64_eq(cnt_0.curobjs, cnt_3.curobjs,
	    "Sample size should have returned to base level");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_prof_realloc);
}
