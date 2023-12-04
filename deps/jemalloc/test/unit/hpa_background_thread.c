#include "test/jemalloc_test.h"
#include "test/sleep.h"

static void
sleep_for_background_thread_interval() {
	/*
	 * The sleep interval set in our .sh file is 50ms.  So it likely will
	 * run if we sleep for four times that.
	 */
	sleep_ns(200 * 1000 * 1000);
}

static unsigned
create_arena() {
	unsigned arena_ind;
	size_t sz;

	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 2),
	    0, "Unexpected mallctl() failure");
	return arena_ind;
}

static size_t
get_empty_ndirty(unsigned arena_ind) {
	int err;
	size_t ndirty_huge;
	size_t ndirty_nonhuge;
	uint64_t epoch = 1;
	size_t sz = sizeof(epoch);
	err = je_mallctl("epoch", (void *)&epoch, &sz, (void *)&epoch,
	    sizeof(epoch));
	expect_d_eq(0, err, "Unexpected mallctl() failure");

	size_t mib[6];
	size_t miblen = sizeof(mib)/sizeof(mib[0]);
	err = mallctlnametomib(
	    "stats.arenas.0.hpa_shard.empty_slabs.ndirty_nonhuge", mib,
	    &miblen);
	expect_d_eq(0, err, "Unexpected mallctlnametomib() failure");

	sz = sizeof(ndirty_nonhuge);
	mib[2] = arena_ind;
	err = mallctlbymib(mib, miblen, &ndirty_nonhuge, &sz, NULL, 0);
	expect_d_eq(0, err, "Unexpected mallctlbymib() failure");

	err = mallctlnametomib(
	    "stats.arenas.0.hpa_shard.empty_slabs.ndirty_huge", mib,
	    &miblen);
	expect_d_eq(0, err, "Unexpected mallctlnametomib() failure");

	sz = sizeof(ndirty_huge);
	mib[2] = arena_ind;
	err = mallctlbymib(mib, miblen, &ndirty_huge, &sz, NULL, 0);
	expect_d_eq(0, err, "Unexpected mallctlbymib() failure");

	return ndirty_huge + ndirty_nonhuge;
}

static void
set_background_thread_enabled(bool enabled) {
	int err;
	err = je_mallctl("background_thread", NULL, NULL, &enabled,
	    sizeof(enabled));
	expect_d_eq(0, err, "Unexpected mallctl failure");
}

static void
wait_until_thread_is_enabled(unsigned arena_id) {
	tsd_t* tsd = tsd_fetch();

	bool sleeping = false;
	int iterations = 0;
	do {
		background_thread_info_t *info =
		    background_thread_info_get(arena_id);
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		sleeping = background_thread_indefinite_sleep(info);
		assert_d_lt(iterations, UINT64_C(1000000),
		    "Waiting for a thread to start for too long");
	} while (!sleeping);
}

static void
expect_purging(unsigned arena_ind, bool expect_deferred) {
	size_t empty_ndirty;

	empty_ndirty = get_empty_ndirty(arena_ind);
	expect_zu_eq(0, empty_ndirty, "Expected arena to start unused.");

	/*
	 * It's possible that we get unlucky with our stats collection timing,
	 * and the background thread runs in between the deallocation and the
	 * stats collection.  So we retry 10 times, and see if we *ever* see
	 * deferred reclamation.
	 */
	bool observed_dirty_page = false;
	for (int i = 0; i < 10; i++) {
		void *ptr = mallocx(PAGE,
		    MALLOCX_TCACHE_NONE | MALLOCX_ARENA(arena_ind));
		empty_ndirty = get_empty_ndirty(arena_ind);
		expect_zu_eq(0, empty_ndirty, "All pages should be active");
		dallocx(ptr, MALLOCX_TCACHE_NONE);
		empty_ndirty = get_empty_ndirty(arena_ind);
		if (expect_deferred) {
			expect_true(empty_ndirty == 0 || empty_ndirty == 1 ||
			    opt_prof, "Unexpected extra dirty page count: %zu",
			    empty_ndirty);
		} else {
			assert_zu_eq(0, empty_ndirty,
			    "Saw dirty pages without deferred purging");
		}
		if (empty_ndirty > 0) {
			observed_dirty_page = true;
			break;
		}
	}
	expect_b_eq(expect_deferred, observed_dirty_page, "");

	/*
	 * Under high concurrency / heavy test load (e.g. using run_test.sh),
	 * the background thread may not get scheduled for a longer period of
	 * time.  Retry 100 times max before bailing out.
	 */
	unsigned retry = 0;
	while ((empty_ndirty = get_empty_ndirty(arena_ind)) > 0 &&
	    expect_deferred && (retry++ < 100)) {
		sleep_for_background_thread_interval();
	}

	expect_zu_eq(0, empty_ndirty, "Should have seen a background purge");
}

TEST_BEGIN(test_hpa_background_thread_purges) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!have_background_thread);
	/* Skip since guarded pages cannot be allocated from hpa. */
	test_skip_if(san_guard_enabled());

	unsigned arena_ind = create_arena();
	/*
	 * Our .sh sets dirty mult to 0, so all dirty pages should get purged
	 * any time any thread frees.
	 */
	expect_purging(arena_ind, /* expect_deferred */ true);
}
TEST_END

TEST_BEGIN(test_hpa_background_thread_enable_disable) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!have_background_thread);
	/* Skip since guarded pages cannot be allocated from hpa. */
	test_skip_if(san_guard_enabled());

	unsigned arena_ind = create_arena();

	set_background_thread_enabled(false);
	expect_purging(arena_ind, false);

	set_background_thread_enabled(true);
	wait_until_thread_is_enabled(arena_ind);
	expect_purging(arena_ind, true);
}
TEST_END

int
main(void) {
	/*
	 * OK, this is a sort of nasty hack.  We don't want to add *another*
	 * config option for HPA (the intent is that it becomes available on
	 * more platforms over time, and we're trying to prune back config
	 * options generally.  But we'll get initialization errors on other
	 * platforms if we set hpa:true in the MALLOC_CONF (even if we set
	 * abort_conf:false as well).  So we reach into the internals and set
	 * them directly, but only if we know that we're actually going to do
	 * something nontrivial in the tests.
	 */
	if (config_stats && hpa_supported() && have_background_thread) {
		opt_hpa = true;
		opt_background_thread = true;
	}
	return test_no_reentrancy(
	    test_hpa_background_thread_purges,
	    test_hpa_background_thread_enable_disable);
}
