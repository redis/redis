#include "test/jemalloc_test.h"

#include "jemalloc/internal/sec.h"

typedef struct pai_test_allocator_s pai_test_allocator_t;
struct pai_test_allocator_s {
	pai_t pai;
	bool alloc_fail;
	size_t alloc_count;
	size_t alloc_batch_count;
	size_t dalloc_count;
	size_t dalloc_batch_count;
	/*
	 * We use a simple bump allocator as the implementation.  This isn't
	 * *really* correct, since we may allow expansion into a subsequent
	 * allocation, but it's not like the SEC is really examining the
	 * pointers it gets back; this is mostly just helpful for debugging.
	 */
	uintptr_t next_ptr;
	size_t expand_count;
	bool expand_return_value;
	size_t shrink_count;
	bool shrink_return_value;
};

static void
test_sec_init(sec_t *sec, pai_t *fallback, size_t nshards, size_t max_alloc,
    size_t max_bytes) {
	sec_opts_t opts;
	opts.nshards = 1;
	opts.max_alloc = max_alloc;
	opts.max_bytes = max_bytes;
	/*
	 * Just choose reasonable defaults for these; most tests don't care so
	 * long as they're something reasonable.
	 */
	opts.bytes_after_flush = max_bytes / 2;
	opts.batch_fill_extra = 4;

	/*
	 * We end up leaking this base, but that's fine; this test is
	 * short-running, and SECs are arena-scoped in reality.
	 */
	base_t *base = base_new(TSDN_NULL, /* ind */ 123,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);

	bool err = sec_init(TSDN_NULL, sec, base, fallback, &opts);
	assert_false(err, "Unexpected initialization failure");
	assert_u_ge(sec->npsizes, 0, "Zero size classes allowed for caching");
}

static inline edata_t *
pai_test_allocator_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero, bool guarded, bool frequent_reuse,
    bool *deferred_work_generated) {
	assert(!guarded);
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	if (ta->alloc_fail) {
		return NULL;
	}
	edata_t *edata = malloc(sizeof(edata_t));
	assert_ptr_not_null(edata, "");
	ta->next_ptr += alignment - 1;
	edata_init(edata, /* arena_ind */ 0,
	    (void *)(ta->next_ptr & ~(alignment - 1)), size,
	    /* slab */ false,
	    /* szind */ 0, /* sn */ 1, extent_state_active, /* zero */ zero,
	    /* comitted */ true, /* ranged */ false, EXTENT_NOT_HEAD);
	ta->next_ptr += size;
	ta->alloc_count++;
	return edata;
}

static inline size_t
pai_test_allocator_alloc_batch(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t nallocs, edata_list_active_t *results,
    bool *deferred_work_generated) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	if (ta->alloc_fail) {
		return 0;
	}
	for (size_t i = 0; i < nallocs; i++) {
		edata_t *edata = malloc(sizeof(edata_t));
		assert_ptr_not_null(edata, "");
		edata_init(edata, /* arena_ind */ 0,
		    (void *)ta->next_ptr, size,
		    /* slab */ false, /* szind */ 0, /* sn */ 1,
		    extent_state_active, /* zero */ false, /* comitted */ true,
		    /* ranged */ false, EXTENT_NOT_HEAD);
		ta->next_ptr += size;
		ta->alloc_batch_count++;
		edata_list_active_append(results, edata);
	}
	return nallocs;
}

static bool
pai_test_allocator_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero,
    bool *deferred_work_generated) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	ta->expand_count++;
	return ta->expand_return_value;
}

static bool
pai_test_allocator_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool *deferred_work_generated) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	ta->shrink_count++;
	return ta->shrink_return_value;
}

static void
pai_test_allocator_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    bool *deferred_work_generated) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	ta->dalloc_count++;
	free(edata);
}

static void
pai_test_allocator_dalloc_batch(tsdn_t *tsdn, pai_t *self,
    edata_list_active_t *list, bool *deferred_work_generated) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;

	edata_t *edata;
	while ((edata = edata_list_active_first(list)) != NULL) {
		edata_list_active_remove(list, edata);
		ta->dalloc_batch_count++;
		free(edata);
	}
}

static inline void
pai_test_allocator_init(pai_test_allocator_t *ta) {
	ta->alloc_fail = false;
	ta->alloc_count = 0;
	ta->alloc_batch_count = 0;
	ta->dalloc_count = 0;
	ta->dalloc_batch_count = 0;
	/* Just don't start the edata at 0. */
	ta->next_ptr = 10 * PAGE;
	ta->expand_count = 0;
	ta->expand_return_value = false;
	ta->shrink_count = 0;
	ta->shrink_return_value = false;
	ta->pai.alloc = &pai_test_allocator_alloc;
	ta->pai.alloc_batch = &pai_test_allocator_alloc_batch;
	ta->pai.expand = &pai_test_allocator_expand;
	ta->pai.shrink = &pai_test_allocator_shrink;
	ta->pai.dalloc = &pai_test_allocator_dalloc;
	ta->pai.dalloc_batch = &pai_test_allocator_dalloc_batch;
}

TEST_BEGIN(test_reuse) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/*
	 * We can't use the "real" tsd, since we malloc within the test
	 * allocator hooks; we'd get lock inversion crashes.  Eventually, we
	 * should have a way to mock tsds, but for now just don't do any
	 * lock-order checking.
	 */
	tsdn_t *tsdn = TSDN_NULL;
	/*
	 * 11 allocs apiece of 1-PAGE and 2-PAGE objects means that we should be
	 * able to get to 33 pages in the cache before triggering a flush.  We
	 * set the flush liimt to twice this amount, to avoid accidentally
	 * triggering a flush caused by the batch-allocation down the cache fill
	 * pathway disrupting ordering.
	 */
	enum { NALLOCS = 11 };
	edata_t *one_page[NALLOCS];
	edata_t *two_page[NALLOCS];
	bool deferred_work_generated = false;
	test_sec_init(&sec, &ta.pai, /* nshards */ 1, /* max_alloc */ 2 * PAGE,
	    /* max_bytes */ 2 * (NALLOCS * PAGE + NALLOCS * 2 * PAGE));
	for (int i = 0; i < NALLOCS; i++) {
		one_page[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_ptr_not_null(one_page[i], "Unexpected alloc failure");
		two_page[i] = pai_alloc(tsdn, &sec.pai, 2 * PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_ptr_not_null(one_page[i], "Unexpected alloc failure");
	}
	expect_zu_eq(0, ta.alloc_count, "Should be using batch allocs");
	size_t max_allocs = ta.alloc_count + ta.alloc_batch_count;
	expect_zu_le(2 * NALLOCS, max_allocs,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/*
	 * Free in a different order than we allocated, to make sure free-list
	 * separation works correctly.
	 */
	for (int i = NALLOCS - 1; i >= 0; i--) {
		pai_dalloc(tsdn, &sec.pai, one_page[i],
		    &deferred_work_generated);
	}
	for (int i = NALLOCS - 1; i >= 0; i--) {
		pai_dalloc(tsdn, &sec.pai, two_page[i],
		    &deferred_work_generated);
	}
	expect_zu_eq(max_allocs, ta.alloc_count + ta.alloc_batch_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/*
	 * Check that the n'th most recent deallocated extent is returned for
	 * the n'th alloc request of a given size.
	 */
	for (int i = 0; i < NALLOCS; i++) {
		edata_t *alloc1 = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		edata_t *alloc2 = pai_alloc(tsdn, &sec.pai, 2 * PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_ptr_eq(one_page[i], alloc1,
		    "Got unexpected allocation");
		expect_ptr_eq(two_page[i], alloc2,
		    "Got unexpected allocation");
	}
	expect_zu_eq(max_allocs, ta.alloc_count + ta.alloc_batch_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
}
TEST_END


TEST_BEGIN(test_auto_flush) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;
	/*
	 * 10-allocs apiece of 1-PAGE and 2-PAGE objects means that we should be
	 * able to get to 30 pages in the cache before triggering a flush.  The
	 * choice of NALLOCS here is chosen to match the batch allocation
	 * default (4 extra + 1 == 5; so 10 allocations leaves the cache exactly
	 * empty, even in the presence of batch allocation on fill).
	 * Eventually, once our allocation batching strategies become smarter,
	 * this should change.
	 */
	enum { NALLOCS = 10 };
	edata_t *extra_alloc;
	edata_t *allocs[NALLOCS];
	bool deferred_work_generated = false;
	test_sec_init(&sec, &ta.pai, /* nshards */ 1, /* max_alloc */ PAGE,
	    /* max_bytes */ NALLOCS * PAGE);
	for (int i = 0; i < NALLOCS; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_ptr_not_null(allocs[i], "Unexpected alloc failure");
	}
	extra_alloc = pai_alloc(tsdn, &sec.pai, PAGE, PAGE, /* zero */ false,
	    /* guarded */ false, /* frequent_reuse */ false,
	    &deferred_work_generated);
	expect_ptr_not_null(extra_alloc, "Unexpected alloc failure");
	size_t max_allocs = ta.alloc_count + ta.alloc_batch_count;
	expect_zu_le(NALLOCS + 1, max_allocs,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/* Free until the SEC is full, but should not have flushed yet. */
	for (int i = 0; i < NALLOCS; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i], &deferred_work_generated);
	}
	expect_zu_le(NALLOCS + 1, max_allocs,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/*
	 * Free the extra allocation; this should trigger a flush.  The internal
	 * flushing logic is allowed to get complicated; for now, we rely on our
	 * whitebox knowledge of the fact that the SEC flushes bins in their
	 * entirety when it decides to do so, and it has only one bin active
	 * right now.
	 */
	pai_dalloc(tsdn, &sec.pai, extra_alloc, &deferred_work_generated);
	expect_zu_eq(max_allocs, ta.alloc_count + ta.alloc_batch_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of (non-batch) deallocations");
	expect_zu_eq(NALLOCS + 1, ta.dalloc_batch_count,
	    "Incorrect number of batch deallocations");
}
TEST_END

/*
 * A disable and a flush are *almost* equivalent; the only difference is what
 * happens afterwards; disabling disallows all future caching as well.
 */
static void
do_disable_flush_test(bool is_disable) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum { NALLOCS = 11 };
	edata_t *allocs[NALLOCS];
	bool deferred_work_generated = false;
	test_sec_init(&sec, &ta.pai, /* nshards */ 1, /* max_alloc */ PAGE,
	    /* max_bytes */ NALLOCS * PAGE);
	for (int i = 0; i < NALLOCS; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_ptr_not_null(allocs[i], "Unexpected alloc failure");
	}
	/* Free all but the last aloc. */
	for (int i = 0; i < NALLOCS - 1; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i], &deferred_work_generated);
	}
	size_t max_allocs = ta.alloc_count + ta.alloc_batch_count;

	expect_zu_le(NALLOCS, max_allocs, "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");

	if (is_disable) {
		sec_disable(tsdn, &sec);
	} else {
		sec_flush(tsdn, &sec);
	}

	expect_zu_eq(max_allocs, ta.alloc_count + ta.alloc_batch_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of (non-batch) deallocations");
	expect_zu_le(NALLOCS - 1, ta.dalloc_batch_count,
	    "Incorrect number of batch deallocations");
	size_t old_dalloc_batch_count = ta.dalloc_batch_count;

	/*
	 * If we free into a disabled SEC, it should forward to the fallback.
	 * Otherwise, the SEC should accept the allocation.
	 */
	pai_dalloc(tsdn, &sec.pai, allocs[NALLOCS - 1],
	    &deferred_work_generated);

	expect_zu_eq(max_allocs, ta.alloc_count + ta.alloc_batch_count,
	    "Incorrect number of allocations");
	expect_zu_eq(is_disable ? 1 : 0, ta.dalloc_count,
	    "Incorrect number of (non-batch) deallocations");
	expect_zu_eq(old_dalloc_batch_count, ta.dalloc_batch_count,
	    "Incorrect number of batch deallocations");
}

TEST_BEGIN(test_disable) {
	do_disable_flush_test(/* is_disable */ true);
}
TEST_END

TEST_BEGIN(test_flush) {
	do_disable_flush_test(/* is_disable */ false);
}
TEST_END

TEST_BEGIN(test_max_alloc_respected) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	size_t max_alloc = 2 * PAGE;
	size_t attempted_alloc = 3 * PAGE;

	bool deferred_work_generated = false;

	test_sec_init(&sec, &ta.pai, /* nshards */ 1, max_alloc,
	    /* max_bytes */ 1000 * PAGE);

	for (size_t i = 0; i < 100; i++) {
		expect_zu_eq(i, ta.alloc_count,
		    "Incorrect number of allocations");
		expect_zu_eq(i, ta.dalloc_count,
		    "Incorrect number of deallocations");
		edata_t *edata = pai_alloc(tsdn, &sec.pai, attempted_alloc,
		    PAGE, /* zero */ false, /* guarded */ false,
		    /* frequent_reuse */ false, &deferred_work_generated);
		expect_ptr_not_null(edata, "Unexpected alloc failure");
		expect_zu_eq(i + 1, ta.alloc_count,
		    "Incorrect number of allocations");
		expect_zu_eq(i, ta.dalloc_count,
		    "Incorrect number of deallocations");
		pai_dalloc(tsdn, &sec.pai, edata, &deferred_work_generated);
	}
}
TEST_END

TEST_BEGIN(test_expand_shrink_delegate) {
	/*
	 * Expand and shrink shouldn't affect sec state; they should just
	 * delegate to the fallback PAI.
	 */
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	bool deferred_work_generated = false;

	test_sec_init(&sec, &ta.pai, /* nshards */ 1, /* max_alloc */ 10 * PAGE,
	    /* max_bytes */ 1000 * PAGE);
	edata_t *edata = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
	    /* zero */ false, /* guarded */ false, /* frequent_reuse */ false,
	    &deferred_work_generated);
	expect_ptr_not_null(edata, "Unexpected alloc failure");

	bool err = pai_expand(tsdn, &sec.pai, edata, PAGE, 4 * PAGE,
	    /* zero */ false, &deferred_work_generated);
	expect_false(err, "Unexpected expand failure");
	expect_zu_eq(1, ta.expand_count, "");
	ta.expand_return_value = true;
	err = pai_expand(tsdn, &sec.pai, edata, 4 * PAGE, 3 * PAGE,
	    /* zero */ false, &deferred_work_generated);
	expect_true(err, "Unexpected expand success");
	expect_zu_eq(2, ta.expand_count, "");

	err = pai_shrink(tsdn, &sec.pai, edata, 4 * PAGE, 2 * PAGE,
	    &deferred_work_generated);
	expect_false(err, "Unexpected shrink failure");
	expect_zu_eq(1, ta.shrink_count, "");
	ta.shrink_return_value = true;
	err = pai_shrink(tsdn, &sec.pai, edata, 2 * PAGE, PAGE,
	    &deferred_work_generated);
	expect_true(err, "Unexpected shrink success");
	expect_zu_eq(2, ta.shrink_count, "");
}
TEST_END

TEST_BEGIN(test_nshards_0) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;
	base_t *base = base_new(TSDN_NULL, /* ind */ 123,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);

	sec_opts_t opts = SEC_OPTS_DEFAULT;
	opts.nshards = 0;
	sec_init(TSDN_NULL, &sec, base, &ta.pai, &opts);

	bool deferred_work_generated = false;
	edata_t *edata = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
	    /* zero */ false, /* guarded */ false, /* frequent_reuse */ false,
	    &deferred_work_generated);
	pai_dalloc(tsdn, &sec.pai, edata, &deferred_work_generated);

	/* Both operations should have gone directly to the fallback. */
	expect_zu_eq(1, ta.alloc_count, "");
	expect_zu_eq(1, ta.dalloc_count, "");
}
TEST_END

static void
expect_stats_pages(tsdn_t *tsdn, sec_t *sec, size_t npages) {
	sec_stats_t stats;
	/*
	 * Check that the stats merging accumulates rather than overwrites by
	 * putting some (made up) data there to begin with.
	 */
	stats.bytes = 123;
	sec_stats_merge(tsdn, sec, &stats);
	assert_zu_le(npages * PAGE + 123, stats.bytes, "");
}

TEST_BEGIN(test_stats_simple) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;

	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum {
		NITERS = 100,
		FLUSH_PAGES = 20,
	};

	bool deferred_work_generated = false;

	test_sec_init(&sec, &ta.pai, /* nshards */ 1, /* max_alloc */ PAGE,
	    /* max_bytes */ FLUSH_PAGES * PAGE);

	edata_t *allocs[FLUSH_PAGES];
	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_stats_pages(tsdn, &sec, 0);
	}

	/* Increase and decrease, without flushing. */
	for (size_t i = 0; i < NITERS; i++) {
		for (size_t j = 0; j < FLUSH_PAGES / 2; j++) {
			pai_dalloc(tsdn, &sec.pai, allocs[j],
			    &deferred_work_generated);
			expect_stats_pages(tsdn, &sec, j + 1);
		}
		for (size_t j = 0; j < FLUSH_PAGES / 2; j++) {
			allocs[j] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
			    /* zero */ false, /* guarded */ false,
			    /* frequent_reuse */ false,
			    &deferred_work_generated);
			expect_stats_pages(tsdn, &sec, FLUSH_PAGES / 2 - j - 1);
		}
	}
}
TEST_END

TEST_BEGIN(test_stats_auto_flush) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;

	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum {
		FLUSH_PAGES = 10,
	};

	test_sec_init(&sec, &ta.pai, /* nshards */ 1, /* max_alloc */ PAGE,
	    /* max_bytes */ FLUSH_PAGES * PAGE);

	edata_t *extra_alloc0;
	edata_t *extra_alloc1;
	edata_t *allocs[2 * FLUSH_PAGES];

	bool deferred_work_generated = false;

	extra_alloc0 = pai_alloc(tsdn, &sec.pai, PAGE, PAGE, /* zero */ false,
	    /* guarded */ false, /* frequent_reuse */ false,
	    &deferred_work_generated);
	extra_alloc1 = pai_alloc(tsdn, &sec.pai, PAGE, PAGE, /* zero */ false,
	    /* guarded */ false, /* frequent_reuse */ false,
	    &deferred_work_generated);

	for (size_t i = 0; i < 2 * FLUSH_PAGES; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
	}

	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i], &deferred_work_generated);
	}
	pai_dalloc(tsdn, &sec.pai, extra_alloc0, &deferred_work_generated);

	/* Flush the remaining pages; stats should still work. */
	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[FLUSH_PAGES + i],
		    &deferred_work_generated);
	}

	pai_dalloc(tsdn, &sec.pai, extra_alloc1, &deferred_work_generated);

	expect_stats_pages(tsdn, &sec, ta.alloc_count + ta.alloc_batch_count
	    - ta.dalloc_count - ta.dalloc_batch_count);
}
TEST_END

TEST_BEGIN(test_stats_manual_flush) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;

	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum {
		FLUSH_PAGES = 10,
	};

	test_sec_init(&sec, &ta.pai, /* nshards */ 1, /* max_alloc */ PAGE,
	    /* max_bytes */ FLUSH_PAGES * PAGE);

	bool deferred_work_generated = false;
	edata_t *allocs[FLUSH_PAGES];
	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_stats_pages(tsdn, &sec, 0);
	}

	/* Dalloc the first half of the allocations. */
	for (size_t i = 0; i < FLUSH_PAGES / 2; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i], &deferred_work_generated);
		expect_stats_pages(tsdn, &sec, i + 1);
	}

	sec_flush(tsdn, &sec);
	expect_stats_pages(tsdn, &sec, 0);

	/* Flush the remaining pages. */
	for (size_t i = 0; i < FLUSH_PAGES / 2; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[FLUSH_PAGES / 2 + i],
		    &deferred_work_generated);
		expect_stats_pages(tsdn, &sec, i + 1);
	}
	sec_disable(tsdn, &sec);
	expect_stats_pages(tsdn, &sec, 0);
}
TEST_END

int
main(void) {
	return test(
	    test_reuse,
	    test_auto_flush,
	    test_disable,
	    test_flush,
	    test_max_alloc_respected,
	    test_expand_shrink_delegate,
	    test_nshards_0,
	    test_stats_simple,
	    test_stats_auto_flush,
	    test_stats_manual_flush);
}
