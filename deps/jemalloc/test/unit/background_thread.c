#include "test/jemalloc_test.h"

#include "jemalloc/internal/util.h"

static void
test_switch_background_thread_ctl(bool new_val) {
	bool e0, e1;
	size_t sz = sizeof(bool);

	e1 = new_val;
	expect_d_eq(mallctl("background_thread", (void *)&e0, &sz,
	    &e1, sz), 0, "Unexpected mallctl() failure");
	expect_b_eq(e0, !e1,
	    "background_thread should be %d before.\n", !e1);
	if (e1) {
		expect_zu_gt(n_background_threads, 0,
		    "Number of background threads should be non zero.\n");
	} else {
		expect_zu_eq(n_background_threads, 0,
		    "Number of background threads should be zero.\n");
	}
}

static void
test_repeat_background_thread_ctl(bool before) {
	bool e0, e1;
	size_t sz = sizeof(bool);

	e1 = before;
	expect_d_eq(mallctl("background_thread", (void *)&e0, &sz,
	    &e1, sz), 0, "Unexpected mallctl() failure");
	expect_b_eq(e0, before,
	    "background_thread should be %d.\n", before);
	if (e1) {
		expect_zu_gt(n_background_threads, 0,
		    "Number of background threads should be non zero.\n");
	} else {
		expect_zu_eq(n_background_threads, 0,
		    "Number of background threads should be zero.\n");
	}
}

TEST_BEGIN(test_background_thread_ctl) {
	test_skip_if(!have_background_thread);

	bool e0, e1;
	size_t sz = sizeof(bool);

	expect_d_eq(mallctl("opt.background_thread", (void *)&e0, &sz,
	    NULL, 0), 0, "Unexpected mallctl() failure");
	expect_d_eq(mallctl("background_thread", (void *)&e1, &sz,
	    NULL, 0), 0, "Unexpected mallctl() failure");
	expect_b_eq(e0, e1,
	    "Default and opt.background_thread does not match.\n");
	if (e0) {
		test_switch_background_thread_ctl(false);
	}
	expect_zu_eq(n_background_threads, 0,
	    "Number of background threads should be 0.\n");

	for (unsigned i = 0; i < 4; i++) {
		test_switch_background_thread_ctl(true);
		test_repeat_background_thread_ctl(true);
		test_repeat_background_thread_ctl(true);

		test_switch_background_thread_ctl(false);
		test_repeat_background_thread_ctl(false);
		test_repeat_background_thread_ctl(false);
	}
}
TEST_END

TEST_BEGIN(test_background_thread_running) {
	test_skip_if(!have_background_thread);
	test_skip_if(!config_stats);

#if defined(JEMALLOC_BACKGROUND_THREAD)
	tsd_t *tsd = tsd_fetch();
	background_thread_info_t *info = &background_thread_info[0];

	test_repeat_background_thread_ctl(false);
	test_switch_background_thread_ctl(true);
	expect_b_eq(info->state, background_thread_started,
	    "Background_thread did not start.\n");

	nstime_t start;
	nstime_init_update(&start);

	bool ran = false;
	while (true) {
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
		if (info->tot_n_runs > 0) {
			ran = true;
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		if (ran) {
			break;
		}

		nstime_t now;
		nstime_init_update(&now);
		nstime_subtract(&now, &start);
		expect_u64_lt(nstime_sec(&now), 1000,
		    "Background threads did not run for 1000 seconds.");
		sleep(1);
	}
	test_switch_background_thread_ctl(false);
#endif
}
TEST_END

int
main(void) {
	/* Background_thread creation tests reentrancy naturally. */
	return test_no_reentrancy(
	    test_background_thread_ctl,
	    test_background_thread_running);
}
