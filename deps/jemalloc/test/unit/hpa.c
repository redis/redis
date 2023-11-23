#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/nstime.h"

#define SHARD_IND 111

#define ALLOC_MAX (HUGEPAGE / 4)

typedef struct test_data_s test_data_t;
struct test_data_s {
	/*
	 * Must be the first member -- we convert back and forth between the
	 * test_data_t and the hpa_shard_t;
	 */
	hpa_shard_t shard;
	hpa_central_t central;
	base_t *base;
	edata_cache_t shard_edata_cache;

	emap_t emap;
};

static hpa_shard_opts_t test_hpa_shard_opts_default = {
	/* slab_max_alloc */
	ALLOC_MAX,
	/* hugification threshold */
	HUGEPAGE,
	/* dirty_mult */
	FXP_INIT_PERCENT(25),
	/* deferral_allowed */
	false,
	/* hugify_delay_ms */
	10 * 1000,
};

static hpa_shard_t *
create_test_data(hpa_hooks_t *hooks, hpa_shard_opts_t *opts) {
	bool err;
	base_t *base = base_new(TSDN_NULL, /* ind */ SHARD_IND,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(base, "");

	test_data_t *test_data = malloc(sizeof(test_data_t));
	assert_ptr_not_null(test_data, "");

	test_data->base = base;

	err = edata_cache_init(&test_data->shard_edata_cache, base);
	assert_false(err, "");

	err = emap_init(&test_data->emap, test_data->base, /* zeroed */ false);
	assert_false(err, "");

	err = hpa_central_init(&test_data->central, test_data->base, hooks);
	assert_false(err, "");

	err = hpa_shard_init(&test_data->shard, &test_data->central,
	    &test_data->emap, test_data->base, &test_data->shard_edata_cache,
	    SHARD_IND, opts);
	assert_false(err, "");

	return (hpa_shard_t *)test_data;
}

static void
destroy_test_data(hpa_shard_t *shard) {
	test_data_t *test_data = (test_data_t *)shard;
	base_delete(TSDN_NULL, test_data->base);
	free(test_data);
}

TEST_BEGIN(test_alloc_max) {
	test_skip_if(!hpa_supported());

	hpa_shard_t *shard = create_test_data(&hpa_hooks_default,
	    &test_hpa_shard_opts_default);
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());

	edata_t *edata;

	/* Small max */
	bool deferred_work_generated = false;
	edata = pai_alloc(tsdn, &shard->pai, ALLOC_MAX, PAGE, false, false,
	    false, &deferred_work_generated);
	expect_ptr_not_null(edata, "Allocation of small max failed");
	edata = pai_alloc(tsdn, &shard->pai, ALLOC_MAX + PAGE, PAGE, false,
	    false, false, &deferred_work_generated);
	expect_ptr_null(edata, "Allocation of larger than small max succeeded");

	destroy_test_data(shard);
}
TEST_END

typedef struct mem_contents_s mem_contents_t;
struct mem_contents_s {
	uintptr_t my_addr;
	size_t size;
	edata_t *my_edata;
	rb_node(mem_contents_t) link;
};

static int
mem_contents_cmp(const mem_contents_t *a, const mem_contents_t *b) {
	return (a->my_addr > b->my_addr) - (a->my_addr < b->my_addr);
}

typedef rb_tree(mem_contents_t) mem_tree_t;
rb_gen(static, mem_tree_, mem_tree_t, mem_contents_t, link,
    mem_contents_cmp);

static void
node_assert_ordered(mem_contents_t *a, mem_contents_t *b) {
	assert_zu_lt(a->my_addr, a->my_addr + a->size, "Overflow");
	assert_zu_le(a->my_addr + a->size, b->my_addr, "");
}

static void
node_check(mem_tree_t *tree, mem_contents_t *contents) {
	edata_t *edata = contents->my_edata;
	assert_ptr_eq(contents, (void *)contents->my_addr, "");
	assert_ptr_eq(contents, edata_base_get(edata), "");
	assert_zu_eq(contents->size, edata_size_get(edata), "");
	assert_ptr_eq(contents->my_edata, edata, "");

	mem_contents_t *next = mem_tree_next(tree, contents);
	if (next != NULL) {
		node_assert_ordered(contents, next);
	}
	mem_contents_t *prev = mem_tree_prev(tree, contents);
	if (prev != NULL) {
		node_assert_ordered(prev, contents);
	}
}

static void
node_insert(mem_tree_t *tree, edata_t *edata, size_t npages) {
	mem_contents_t *contents = (mem_contents_t *)edata_base_get(edata);
	contents->my_addr = (uintptr_t)edata_base_get(edata);
	contents->size = edata_size_get(edata);
	contents->my_edata = edata;
	mem_tree_insert(tree, contents);
	node_check(tree, contents);
}

static void
node_remove(mem_tree_t *tree, edata_t *edata) {
	mem_contents_t *contents = (mem_contents_t *)edata_base_get(edata);
	node_check(tree, contents);
	mem_tree_remove(tree, contents);
}

TEST_BEGIN(test_stress) {
	test_skip_if(!hpa_supported());

	hpa_shard_t *shard = create_test_data(&hpa_hooks_default,
	    &test_hpa_shard_opts_default);

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());

	const size_t nlive_edatas_max = 500;
	size_t nlive_edatas = 0;
	edata_t **live_edatas = calloc(nlive_edatas_max, sizeof(edata_t *));
	/*
	 * Nothing special about this constant; we're only fixing it for
	 * consistency across runs.
	 */
	size_t prng_state = (size_t)0x76999ffb014df07c;

	mem_tree_t tree;
	mem_tree_new(&tree);

	bool deferred_work_generated = false;

	for (size_t i = 0; i < 100 * 1000; i++) {
		size_t operation = prng_range_zu(&prng_state, 2);
		if (operation == 0) {
			/* Alloc */
			if (nlive_edatas == nlive_edatas_max) {
				continue;
			}

			/*
			 * We make sure to get an even balance of small and
			 * large allocations.
			 */
			size_t npages_min = 1;
			size_t npages_max = ALLOC_MAX / PAGE;
			size_t npages = npages_min + prng_range_zu(&prng_state,
			    npages_max - npages_min);
			edata_t *edata = pai_alloc(tsdn, &shard->pai,
			    npages * PAGE, PAGE, false, false, false,
			    &deferred_work_generated);
			assert_ptr_not_null(edata,
			    "Unexpected allocation failure");
			live_edatas[nlive_edatas] = edata;
			nlive_edatas++;
			node_insert(&tree, edata, npages);
		} else {
			/* Free. */
			if (nlive_edatas == 0) {
				continue;
			}
			size_t victim = prng_range_zu(&prng_state, nlive_edatas);
			edata_t *to_free = live_edatas[victim];
			live_edatas[victim] = live_edatas[nlive_edatas - 1];
			nlive_edatas--;
			node_remove(&tree, to_free);
			pai_dalloc(tsdn, &shard->pai, to_free,
			    &deferred_work_generated);
		}
	}

	size_t ntreenodes = 0;
	for (mem_contents_t *contents = mem_tree_first(&tree); contents != NULL;
	    contents = mem_tree_next(&tree, contents)) {
		ntreenodes++;
		node_check(&tree, contents);
	}
	expect_zu_eq(ntreenodes, nlive_edatas, "");

	/*
	 * Test hpa_shard_destroy, which requires as a precondition that all its
	 * extents have been deallocated.
	 */
	for (size_t i = 0; i < nlive_edatas; i++) {
		edata_t *to_free = live_edatas[i];
		node_remove(&tree, to_free);
		pai_dalloc(tsdn, &shard->pai, to_free,
		    &deferred_work_generated);
	}
	hpa_shard_destroy(tsdn, shard);

	free(live_edatas);
	destroy_test_data(shard);
}
TEST_END

static void
expect_contiguous(edata_t **edatas, size_t nedatas) {
	for (size_t i = 0; i < nedatas; i++) {
		size_t expected = (size_t)edata_base_get(edatas[0])
		    + i * PAGE;
		expect_zu_eq(expected, (size_t)edata_base_get(edatas[i]),
		    "Mismatch at index %zu", i);
	}
}

TEST_BEGIN(test_alloc_dalloc_batch) {
	test_skip_if(!hpa_supported());

	hpa_shard_t *shard = create_test_data(&hpa_hooks_default,
	    &test_hpa_shard_opts_default);
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());

	bool deferred_work_generated = false;

	enum {NALLOCS = 8};

	edata_t *allocs[NALLOCS];
	/*
	 * Allocate a mix of ways; first half from regular alloc, second half
	 * from alloc_batch.
	 */
	for (size_t i = 0; i < NALLOCS / 2; i++) {
		allocs[i] = pai_alloc(tsdn, &shard->pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false,
		    /* frequent_reuse */ false, &deferred_work_generated);
		expect_ptr_not_null(allocs[i], "Unexpected alloc failure");
	}
	edata_list_active_t allocs_list;
	edata_list_active_init(&allocs_list);
	size_t nsuccess = pai_alloc_batch(tsdn, &shard->pai, PAGE, NALLOCS / 2,
	    &allocs_list, &deferred_work_generated);
	expect_zu_eq(NALLOCS / 2, nsuccess, "Unexpected oom");
	for (size_t i = NALLOCS / 2; i < NALLOCS; i++) {
		allocs[i] = edata_list_active_first(&allocs_list);
		edata_list_active_remove(&allocs_list, allocs[i]);
	}

	/*
	 * Should have allocated them contiguously, despite the differing
	 * methods used.
	 */
	void *orig_base = edata_base_get(allocs[0]);
	expect_contiguous(allocs, NALLOCS);

	/*
	 * Batch dalloc the first half, individually deallocate the second half.
	 */
	for (size_t i = 0; i < NALLOCS / 2; i++) {
		edata_list_active_append(&allocs_list, allocs[i]);
	}
	pai_dalloc_batch(tsdn, &shard->pai, &allocs_list,
	    &deferred_work_generated);
	for (size_t i = NALLOCS / 2; i < NALLOCS; i++) {
		pai_dalloc(tsdn, &shard->pai, allocs[i],
		    &deferred_work_generated);
	}

	/* Reallocate (individually), and ensure reuse and contiguity. */
	for (size_t i = 0; i < NALLOCS; i++) {
		allocs[i] = pai_alloc(tsdn, &shard->pai, PAGE, PAGE,
		    /* zero */ false, /* guarded */ false, /* frequent_reuse */
		    false, &deferred_work_generated);
		expect_ptr_not_null(allocs[i], "Unexpected alloc failure.");
	}
	void *new_base = edata_base_get(allocs[0]);
	expect_ptr_eq(orig_base, new_base,
	    "Failed to reuse the allocated memory.");
	expect_contiguous(allocs, NALLOCS);

	destroy_test_data(shard);
}
TEST_END

static uintptr_t defer_bump_ptr = HUGEPAGE * 123;
static void *
defer_test_map(size_t size) {
	void *result = (void *)defer_bump_ptr;
	defer_bump_ptr += size;
	return result;
}

static void
defer_test_unmap(void *ptr, size_t size) {
	(void)ptr;
	(void)size;
}

static bool defer_purge_called = false;
static void
defer_test_purge(void *ptr, size_t size) {
	(void)ptr;
	(void)size;
	defer_purge_called = true;
}

static bool defer_hugify_called = false;
static void
defer_test_hugify(void *ptr, size_t size) {
	defer_hugify_called = true;
}

static bool defer_dehugify_called = false;
static void
defer_test_dehugify(void *ptr, size_t size) {
	defer_dehugify_called = true;
}

static nstime_t defer_curtime;
static void
defer_test_curtime(nstime_t *r_time, bool first_reading) {
	*r_time = defer_curtime;
}

static uint64_t
defer_test_ms_since(nstime_t *past_time) {
	return (nstime_ns(&defer_curtime) - nstime_ns(past_time)) / 1000 / 1000;
}

TEST_BEGIN(test_defer_time) {
	test_skip_if(!hpa_supported());

	hpa_hooks_t hooks;
	hooks.map = &defer_test_map;
	hooks.unmap = &defer_test_unmap;
	hooks.purge = &defer_test_purge;
	hooks.hugify = &defer_test_hugify;
	hooks.dehugify = &defer_test_dehugify;
	hooks.curtime = &defer_test_curtime;
	hooks.ms_since = &defer_test_ms_since;

	hpa_shard_opts_t opts = test_hpa_shard_opts_default;
	opts.deferral_allowed = true;

	hpa_shard_t *shard = create_test_data(&hooks, &opts);

	bool deferred_work_generated = false;

	nstime_init(&defer_curtime, 0);
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	edata_t *edatas[HUGEPAGE_PAGES];
	for (int i = 0; i < (int)HUGEPAGE_PAGES; i++) {
		edatas[i] = pai_alloc(tsdn, &shard->pai, PAGE, PAGE, false,
		    false, false, &deferred_work_generated);
		expect_ptr_not_null(edatas[i], "Unexpected null edata");
	}
	hpa_shard_do_deferred_work(tsdn, shard);
	expect_false(defer_hugify_called, "Hugified too early");

	/* Hugification delay is set to 10 seconds in options. */
	nstime_init2(&defer_curtime, 11, 0);
	hpa_shard_do_deferred_work(tsdn, shard);
	expect_true(defer_hugify_called, "Failed to hugify");

	defer_hugify_called = false;

	/* Purge.  Recall that dirty_mult is .25. */
	for (int i = 0; i < (int)HUGEPAGE_PAGES / 2; i++) {
		pai_dalloc(tsdn, &shard->pai, edatas[i],
		    &deferred_work_generated);
	}

	hpa_shard_do_deferred_work(tsdn, shard);

	expect_false(defer_hugify_called, "Hugified too early");
	expect_true(defer_dehugify_called, "Should have dehugified");
	expect_true(defer_purge_called, "Should have purged");
	defer_hugify_called = false;
	defer_dehugify_called = false;
	defer_purge_called = false;

	/*
	 * Refill the page.  We now meet the hugification threshold; we should
	 * be marked for pending hugify.
	 */
	for (int i = 0; i < (int)HUGEPAGE_PAGES / 2; i++) {
		edatas[i] = pai_alloc(tsdn, &shard->pai, PAGE, PAGE, false,
		    false, false, &deferred_work_generated);
		expect_ptr_not_null(edatas[i], "Unexpected null edata");
	}
	/*
	 * We would be ineligible for hugification, had we not already met the
	 * threshold before dipping below it.
	 */
	pai_dalloc(tsdn, &shard->pai, edatas[0],
	    &deferred_work_generated);
	/* Wait for the threshold again. */
	nstime_init2(&defer_curtime, 22, 0);
	hpa_shard_do_deferred_work(tsdn, shard);
	expect_true(defer_hugify_called, "Hugified too early");
	expect_false(defer_dehugify_called, "Unexpected dehugify");
	expect_false(defer_purge_called, "Unexpected purge");

	destroy_test_data(shard);
}
TEST_END

int
main(void) {
	/*
	 * These trigger unused-function warnings on CI runs, even if declared
	 * with static inline.
	 */
	(void)mem_tree_empty;
	(void)mem_tree_last;
	(void)mem_tree_search;
	(void)mem_tree_nsearch;
	(void)mem_tree_psearch;
	(void)mem_tree_iter;
	(void)mem_tree_reverse_iter;
	(void)mem_tree_destroy;
	return test_no_reentrancy(
	    test_alloc_max,
	    test_stress,
	    test_alloc_dalloc_batch,
	    test_defer_time);
}
