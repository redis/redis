#include "test/jemalloc_test.h"

#include "jemalloc/internal/pa.h"

static void *
alloc_hook(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	void *ret = pages_map(new_addr, size, alignment, commit);
	return ret;
}

static bool
merge_hook(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	return !maps_coalesce;
}

static bool
split_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	return !maps_coalesce;
}

static void
init_test_extent_hooks(extent_hooks_t *hooks) {
	/*
	 * The default hooks are mostly fine for testing.  A few of them,
	 * though, access globals (alloc for dss setting in an arena, split and
	 * merge touch the global emap to find head state.  The first of these
	 * can be fixed by keeping that state with the hooks, where it logically
	 * belongs.  The second, though, we can only fix when we use the extent
	 * hook API.
	 */
	memcpy(hooks, &ehooks_default_extent_hooks, sizeof(extent_hooks_t));
	hooks->alloc = &alloc_hook;
	hooks->merge = &merge_hook;
	hooks->split = &split_hook;
}

typedef struct test_data_s test_data_t;
struct test_data_s {
	pa_shard_t shard;
	pa_central_t central;
	base_t *base;
	emap_t emap;
	pa_shard_stats_t stats;
	malloc_mutex_t stats_mtx;
	extent_hooks_t hooks;
};

test_data_t *init_test_data(ssize_t dirty_decay_ms, ssize_t muzzy_decay_ms) {
	test_data_t *test_data = calloc(1, sizeof(test_data_t));
	assert_ptr_not_null(test_data, "");
	init_test_extent_hooks(&test_data->hooks);

	base_t *base = base_new(TSDN_NULL, /* ind */ 1, &test_data->hooks,
	    /* metadata_use_hooks */ true);
	assert_ptr_not_null(base, "");

	test_data->base = base;
	bool err = emap_init(&test_data->emap, test_data->base,
	    /* zeroed */ true);
	assert_false(err, "");

	nstime_t time;
	nstime_init(&time, 0);

	err = pa_central_init(&test_data->central, base, opt_hpa,
	    &hpa_hooks_default);
	assert_false(err, "");

	const size_t pa_oversize_threshold = 8 * 1024 * 1024;
	err = pa_shard_init(TSDN_NULL, &test_data->shard, &test_data->central,
	    &test_data->emap, test_data->base, /* ind */ 1, &test_data->stats,
	    &test_data->stats_mtx, &time, pa_oversize_threshold, dirty_decay_ms,
	    muzzy_decay_ms);
	assert_false(err, "");

	return test_data;
}

void destroy_test_data(test_data_t *data) {
	base_delete(TSDN_NULL, data->base);
	free(data);
}

static void *
do_alloc_free_purge(void *arg) {
	test_data_t *test_data = (test_data_t *)arg;
	for (int i = 0; i < 10 * 1000; i++) {
		bool deferred_work_generated = false;
		edata_t *edata = pa_alloc(TSDN_NULL, &test_data->shard, PAGE,
		    PAGE, /* slab */ false, /* szind */ 0, /* zero */ false,
		    /* guarded */ false, &deferred_work_generated);
		assert_ptr_not_null(edata, "");
		pa_dalloc(TSDN_NULL, &test_data->shard, edata,
		    &deferred_work_generated);
		malloc_mutex_lock(TSDN_NULL,
		    &test_data->shard.pac.decay_dirty.mtx);
		pac_decay_all(TSDN_NULL, &test_data->shard.pac,
		    &test_data->shard.pac.decay_dirty,
		    &test_data->shard.pac.stats->decay_dirty,
		    &test_data->shard.pac.ecache_dirty, true);
		malloc_mutex_unlock(TSDN_NULL,
		    &test_data->shard.pac.decay_dirty.mtx);
	}
	return NULL;
}

TEST_BEGIN(test_alloc_free_purge_thds) {
	test_data_t *test_data = init_test_data(0, 0);
	thd_t thds[4];
	for (int i = 0; i < 4; i++) {
		thd_create(&thds[i], do_alloc_free_purge, test_data);
	}
	for (int i = 0; i < 4; i++) {
		thd_join(thds[i], NULL);
	}
}
TEST_END

int
main(void) {
	return test(
	    test_alloc_free_purge_thds);
}
