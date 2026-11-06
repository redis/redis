#include "test/jemalloc_test.h"

#include "test/extent_hooks.h"

static extent_hooks_t hooks_null = {
	extent_alloc_hook,
	NULL, /* dalloc */
	NULL, /* destroy */
	NULL, /* commit */
	NULL, /* decommit */
	NULL, /* purge_lazy */
	NULL, /* purge_forced */
	NULL, /* split */
	NULL /* merge */
};

static extent_hooks_t hooks_not_null = {
	extent_alloc_hook,
	extent_dalloc_hook,
	extent_destroy_hook,
	NULL, /* commit */
	extent_decommit_hook,
	extent_purge_lazy_hook,
	extent_purge_forced_hook,
	NULL, /* split */
	NULL /* merge */
};

TEST_BEGIN(test_base_hooks_default) {
	base_t *base;
	size_t allocated0, allocated1, resident, mapped, n_thp;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	base = base_new(tsdn, 0,
	    (extent_hooks_t *)&ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);

	if (config_stats) {
		base_stats_get(tsdn, base, &allocated0, &resident, &mapped,
		    &n_thp);
		expect_zu_ge(allocated0, sizeof(base_t),
		    "Base header should count as allocated");
		if (opt_metadata_thp == metadata_thp_always) {
			expect_zu_gt(n_thp, 0,
			    "Base should have 1 THP at least.");
		}
	}

	expect_ptr_not_null(base_alloc(tsdn, base, 42, 1),
	    "Unexpected base_alloc() failure");

	if (config_stats) {
		base_stats_get(tsdn, base, &allocated1, &resident, &mapped,
		    &n_thp);
		expect_zu_ge(allocated1 - allocated0, 42,
		    "At least 42 bytes were allocated by base_alloc()");
	}

	base_delete(tsdn, base);
}
TEST_END

TEST_BEGIN(test_base_hooks_null) {
	extent_hooks_t hooks_orig;
	base_t *base;
	size_t allocated0, allocated1, resident, mapped, n_thp;

	extent_hooks_prep();
	try_dalloc = false;
	try_destroy = true;
	try_decommit = false;
	try_purge_lazy = false;
	try_purge_forced = false;
	memcpy(&hooks_orig, &hooks, sizeof(extent_hooks_t));
	memcpy(&hooks, &hooks_null, sizeof(extent_hooks_t));

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	base = base_new(tsdn, 0, &hooks, /* metadata_use_hooks */ true);
	expect_ptr_not_null(base, "Unexpected base_new() failure");

	if (config_stats) {
		base_stats_get(tsdn, base, &allocated0, &resident, &mapped,
		    &n_thp);
		expect_zu_ge(allocated0, sizeof(base_t),
		    "Base header should count as allocated");
		if (opt_metadata_thp == metadata_thp_always) {
			expect_zu_gt(n_thp, 0,
			    "Base should have 1 THP at least.");
		}
	}

	expect_ptr_not_null(base_alloc(tsdn, base, 42, 1),
	    "Unexpected base_alloc() failure");

	if (config_stats) {
		base_stats_get(tsdn, base, &allocated1, &resident, &mapped,
		    &n_thp);
		expect_zu_ge(allocated1 - allocated0, 42,
		    "At least 42 bytes were allocated by base_alloc()");
	}

	base_delete(tsdn, base);

	memcpy(&hooks, &hooks_orig, sizeof(extent_hooks_t));
}
TEST_END

TEST_BEGIN(test_base_hooks_not_null) {
	extent_hooks_t hooks_orig;
	base_t *base;
	void *p, *q, *r, *r_exp;

	extent_hooks_prep();
	try_dalloc = false;
	try_destroy = true;
	try_decommit = false;
	try_purge_lazy = false;
	try_purge_forced = false;
	memcpy(&hooks_orig, &hooks, sizeof(extent_hooks_t));
	memcpy(&hooks, &hooks_not_null, sizeof(extent_hooks_t));

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	did_alloc = false;
	base = base_new(tsdn, 0, &hooks, /* metadata_use_hooks */ true);
	expect_ptr_not_null(base, "Unexpected base_new() failure");
	expect_true(did_alloc, "Expected alloc");

	/*
	 * Check for tight packing at specified alignment under simple
	 * conditions.
	 */
	{
		const size_t alignments[] = {
			1,
			QUANTUM,
			QUANTUM << 1,
			CACHELINE,
			CACHELINE << 1,
		};
		unsigned i;

		for (i = 0; i < sizeof(alignments) / sizeof(size_t); i++) {
			size_t alignment = alignments[i];
			size_t align_ceil = ALIGNMENT_CEILING(alignment,
			    QUANTUM);
			p = base_alloc(tsdn, base, 1, alignment);
			expect_ptr_not_null(p,
			    "Unexpected base_alloc() failure");
			expect_ptr_eq(p,
			    (void *)(ALIGNMENT_CEILING((uintptr_t)p,
			    alignment)), "Expected quantum alignment");
			q = base_alloc(tsdn, base, alignment, alignment);
			expect_ptr_not_null(q,
			    "Unexpected base_alloc() failure");
			expect_ptr_eq((void *)((uintptr_t)p + align_ceil), q,
			    "Minimal allocation should take up %zu bytes",
			    align_ceil);
			r = base_alloc(tsdn, base, 1, alignment);
			expect_ptr_not_null(r,
			    "Unexpected base_alloc() failure");
			expect_ptr_eq((void *)((uintptr_t)q + align_ceil), r,
			    "Minimal allocation should take up %zu bytes",
			    align_ceil);
		}
	}

	/*
	 * Allocate an object that cannot fit in the first block, then verify
	 * that the first block's remaining space is considered for subsequent
	 * allocation.
	 */
	expect_zu_ge(edata_bsize_get(&base->blocks->edata), QUANTUM,
	    "Remainder insufficient for test");
	/* Use up all but one quantum of block. */
	while (edata_bsize_get(&base->blocks->edata) > QUANTUM) {
		p = base_alloc(tsdn, base, QUANTUM, QUANTUM);
		expect_ptr_not_null(p, "Unexpected base_alloc() failure");
	}
	r_exp = edata_addr_get(&base->blocks->edata);
	expect_zu_eq(base->extent_sn_next, 1, "One extant block expected");
	q = base_alloc(tsdn, base, QUANTUM + 1, QUANTUM);
	expect_ptr_not_null(q, "Unexpected base_alloc() failure");
	expect_ptr_ne(q, r_exp, "Expected allocation from new block");
	expect_zu_eq(base->extent_sn_next, 2, "Two extant blocks expected");
	r = base_alloc(tsdn, base, QUANTUM, QUANTUM);
	expect_ptr_not_null(r, "Unexpected base_alloc() failure");
	expect_ptr_eq(r, r_exp, "Expected allocation from first block");
	expect_zu_eq(base->extent_sn_next, 2, "Two extant blocks expected");

	/*
	 * Check for proper alignment support when normal blocks are too small.
	 */
	{
		const size_t alignments[] = {
			HUGEPAGE,
			HUGEPAGE << 1
		};
		unsigned i;

		for (i = 0; i < sizeof(alignments) / sizeof(size_t); i++) {
			size_t alignment = alignments[i];
			p = base_alloc(tsdn, base, QUANTUM, alignment);
			expect_ptr_not_null(p,
			    "Unexpected base_alloc() failure");
			expect_ptr_eq(p,
			    (void *)(ALIGNMENT_CEILING((uintptr_t)p,
			    alignment)), "Expected %zu-byte alignment",
			    alignment);
		}
	}

	called_dalloc = called_destroy = called_decommit = called_purge_lazy =
	    called_purge_forced = false;
	base_delete(tsdn, base);
	expect_true(called_dalloc, "Expected dalloc call");
	expect_true(!called_destroy, "Unexpected destroy call");
	expect_true(called_decommit, "Expected decommit call");
	expect_true(called_purge_lazy, "Expected purge_lazy call");
	expect_true(called_purge_forced, "Expected purge_forced call");

	try_dalloc = true;
	try_destroy = true;
	try_decommit = true;
	try_purge_lazy = true;
	try_purge_forced = true;
	memcpy(&hooks, &hooks_orig, sizeof(extent_hooks_t));
}
TEST_END

TEST_BEGIN(test_base_ehooks_get_for_metadata_default_hook) {
	extent_hooks_prep();
	memcpy(&hooks, &hooks_not_null, sizeof(extent_hooks_t));
	base_t *base;
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	base = base_new(tsdn, 0, &hooks, /* metadata_use_hooks */ false);
	ehooks_t *ehooks = base_ehooks_get_for_metadata(base);
	expect_true(ehooks_are_default(ehooks),
		"Expected default extent hook functions pointer");
	base_delete(tsdn, base);
}
TEST_END


TEST_BEGIN(test_base_ehooks_get_for_metadata_custom_hook) {
	extent_hooks_prep();
	memcpy(&hooks, &hooks_not_null, sizeof(extent_hooks_t));
	base_t *base;
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	base = base_new(tsdn, 0, &hooks, /* metadata_use_hooks */ true);
	ehooks_t *ehooks = base_ehooks_get_for_metadata(base);
	expect_ptr_eq(&hooks, ehooks_get_extent_hooks_ptr(ehooks),
		"Expected user-specified extend hook functions pointer");
	base_delete(tsdn, base);
}
TEST_END

int
main(void) {
	return test(
	    test_base_hooks_default,
	    test_base_hooks_null,
	    test_base_hooks_not_null,
            test_base_ehooks_get_for_metadata_default_hook,
            test_base_ehooks_get_for_metadata_custom_hook);
}
