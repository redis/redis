#include "test/jemalloc_test.h"

#define HPDATA_ADDR ((void *)(10 * HUGEPAGE))
#define HPDATA_AGE 123

TEST_BEGIN(test_reserve_alloc) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE);

	/* Allocating a page at a time, we should do first fit. */
	for (size_t i = 0; i < HUGEPAGE_PAGES; i++) {
		expect_true(hpdata_consistent(&hpdata), "");
		expect_zu_eq(HUGEPAGE_PAGES - i,
		    hpdata_longest_free_range_get(&hpdata), "");
		void *alloc = hpdata_reserve_alloc(&hpdata, PAGE);
		expect_ptr_eq((char *)HPDATA_ADDR + i * PAGE, alloc, "");
		expect_true(hpdata_consistent(&hpdata), "");
	}
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(0, hpdata_longest_free_range_get(&hpdata), "");

	/*
	 * Build up a bigger free-range, 2 pages at a time, until we've got 6
	 * adjacent free pages total.  Pages 8-13 should be unreserved after
	 * this.
	 */
	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 10 * PAGE, 2 * PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(2, hpdata_longest_free_range_get(&hpdata), "");

	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 12 * PAGE, 2 * PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(4, hpdata_longest_free_range_get(&hpdata), "");

	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 8 * PAGE, 2 * PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(6, hpdata_longest_free_range_get(&hpdata), "");

	/*
	 * Leave page 14 reserved, but free page 15 (this test the case where
	 * unreserving combines two ranges).
	 */
	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 15 * PAGE, PAGE);
	/*
	 * Longest free range shouldn't change; we've got a free range of size
	 * 6, then a reserved page, then another free range.
	 */
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(6, hpdata_longest_free_range_get(&hpdata), "");

	/* After freeing page 14, the two ranges get combined. */
	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 14 * PAGE, PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(8, hpdata_longest_free_range_get(&hpdata), "");
}
TEST_END

TEST_BEGIN(test_purge_simple) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE);

	void *alloc = hpdata_reserve_alloc(&hpdata, HUGEPAGE_PAGES / 2 * PAGE);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	/* Create HUGEPAGE_PAGES / 4 dirty inactive pages at the beginning. */
	hpdata_unreserve(&hpdata, alloc, HUGEPAGE_PAGES / 4 * PAGE);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 2, "");

	hpdata_alloc_allowed_set(&hpdata, false);
	hpdata_purge_state_t purge_state;
	size_t to_purge = hpdata_purge_begin(&hpdata, &purge_state);
	expect_zu_eq(HUGEPAGE_PAGES / 4, to_purge, "");

	void *purge_addr;
	size_t purge_size;
	bool got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(HPDATA_ADDR, purge_addr, "");
	expect_zu_eq(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_false(got_result, "Unexpected additional purge range: "
	    "extent at %p of size %zu", purge_addr, purge_size);

	hpdata_purge_end(&hpdata, &purge_state);
	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 4, "");
}
TEST_END

/*
 * We only test intervening dalloc's not intervening allocs; the latter are
 * disallowed as a purging precondition (because they interfere with purging
 * across a retained extent, saving a purge call).
 */
TEST_BEGIN(test_purge_intervening_dalloc) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE);

	/* Allocate the first 3/4 of the pages. */
	void *alloc = hpdata_reserve_alloc(&hpdata, 3 * HUGEPAGE_PAGES / 4  * PAGE);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	/* Free the first 1/4 and the third 1/4 of the pages. */
	hpdata_unreserve(&hpdata, alloc, HUGEPAGE_PAGES / 4 * PAGE);
	hpdata_unreserve(&hpdata,
	    (void *)((uintptr_t)alloc + 2 * HUGEPAGE_PAGES / 4 * PAGE),
	    HUGEPAGE_PAGES / 4 * PAGE);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), 3 * HUGEPAGE_PAGES / 4, "");

	hpdata_alloc_allowed_set(&hpdata, false);
	hpdata_purge_state_t purge_state;
	size_t to_purge = hpdata_purge_begin(&hpdata, &purge_state);
	expect_zu_eq(HUGEPAGE_PAGES / 2, to_purge, "");

	void *purge_addr;
	size_t purge_size;
	/* First purge. */
	bool got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(HPDATA_ADDR, purge_addr, "");
	expect_zu_eq(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	/* Deallocate the second 1/4 before the second purge occurs. */
	hpdata_unreserve(&hpdata,
	    (void *)((uintptr_t)alloc + 1 * HUGEPAGE_PAGES / 4 * PAGE),
	    HUGEPAGE_PAGES / 4 * PAGE);

	/* Now continue purging. */
	got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(
	    (void *)((uintptr_t)alloc + 2 * HUGEPAGE_PAGES / 4 * PAGE),
	    purge_addr, "");
	expect_zu_ge(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_false(got_result, "Unexpected additional purge range: "
	    "extent at %p of size %zu", purge_addr, purge_size);

	hpdata_purge_end(&hpdata, &purge_state);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 4, "");
}
TEST_END

TEST_BEGIN(test_purge_over_retained) {
	void *purge_addr;
	size_t purge_size;

	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE);

	/* Allocate the first 3/4 of the pages. */
	void *alloc = hpdata_reserve_alloc(&hpdata, 3 * HUGEPAGE_PAGES / 4  * PAGE);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	/* Free the second quarter. */
	void *second_quarter =
	    (void *)((uintptr_t)alloc + HUGEPAGE_PAGES / 4 * PAGE);
	hpdata_unreserve(&hpdata, second_quarter, HUGEPAGE_PAGES / 4 * PAGE);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), 3 * HUGEPAGE_PAGES / 4, "");

	/* Purge the second quarter. */
	hpdata_alloc_allowed_set(&hpdata, false);
	hpdata_purge_state_t purge_state;
	size_t to_purge_dirty = hpdata_purge_begin(&hpdata, &purge_state);
	expect_zu_eq(HUGEPAGE_PAGES / 4, to_purge_dirty, "");

	bool got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(second_quarter, purge_addr, "");
	expect_zu_eq(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_false(got_result, "Unexpected additional purge range: "
	    "extent at %p of size %zu", purge_addr, purge_size);
	hpdata_purge_end(&hpdata, &purge_state);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 2, "");

	/* Free the first and third quarter. */
	hpdata_unreserve(&hpdata, HPDATA_ADDR, HUGEPAGE_PAGES / 4 * PAGE);
	hpdata_unreserve(&hpdata,
	    (void *)((uintptr_t)alloc + 2 * HUGEPAGE_PAGES / 4 * PAGE),
	    HUGEPAGE_PAGES / 4 * PAGE);

	/*
	 * Purge again.  The second quarter is retained, so we can safely
	 * re-purge it.  We expect a single purge of 3/4 of the hugepage,
	 * purging half its pages.
	 */
	to_purge_dirty = hpdata_purge_begin(&hpdata, &purge_state);
	expect_zu_eq(HUGEPAGE_PAGES / 2, to_purge_dirty, "");

	got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(HPDATA_ADDR, purge_addr, "");
	expect_zu_eq(3 * HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(&hpdata, &purge_state, &purge_addr,
	    &purge_size);
	expect_false(got_result, "Unexpected additional purge range: "
	    "extent at %p of size %zu", purge_addr, purge_size);
	hpdata_purge_end(&hpdata, &purge_state);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), 0, "");
}
TEST_END

TEST_BEGIN(test_hugify) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE);

	void *alloc = hpdata_reserve_alloc(&hpdata, HUGEPAGE / 2);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	expect_zu_eq(HUGEPAGE_PAGES / 2, hpdata_ntouched_get(&hpdata), "");

	hpdata_hugify(&hpdata);

	/* Hugeifying should have increased the dirty page count. */
	expect_zu_eq(HUGEPAGE_PAGES, hpdata_ntouched_get(&hpdata), "");
}
TEST_END

int main(void) {
	return test_no_reentrancy(
	    test_reserve_alloc,
	    test_purge_simple,
	    test_purge_intervening_dalloc,
	    test_purge_over_retained,
	    test_hugify);
}
