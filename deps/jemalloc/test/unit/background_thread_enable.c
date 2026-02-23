#include "test/jemalloc_test.h"

const char *malloc_conf = "background_thread:false,narenas:1,max_background_threads:20";

static unsigned
max_test_narenas(void) {
	/*
	 * 10 here is somewhat arbitrary, except insofar as we want to ensure
	 * that the number of background threads is smaller than the number of
	 * arenas.  I'll ragequit long before we have to spin up 10 threads per
	 * cpu to handle background purging, so this is a conservative
	 * approximation.
	 */
	unsigned ret = 10 * ncpus;
	/* Limit the max to avoid VM exhaustion on 32-bit . */
	if (ret > 512) {
		ret = 512;
	}

	return ret;
}

TEST_BEGIN(test_deferred) {
	test_skip_if(!have_background_thread);

	unsigned id;
	size_t sz_u = sizeof(unsigned);

	for (unsigned i = 0; i < max_test_narenas(); i++) {
		expect_d_eq(mallctl("arenas.create", &id, &sz_u, NULL, 0), 0,
		    "Failed to create arena");
	}

	bool enable = true;
	size_t sz_b = sizeof(bool);
	expect_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to enable background threads");
	enable = false;
	expect_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to disable background threads");
}
TEST_END

TEST_BEGIN(test_max_background_threads) {
	test_skip_if(!have_background_thread);

	size_t max_n_thds;
	size_t opt_max_n_thds;
	size_t sz_m = sizeof(max_n_thds);
	expect_d_eq(mallctl("opt.max_background_threads",
	    &opt_max_n_thds, &sz_m, NULL, 0), 0,
	    "Failed to get opt.max_background_threads");
	expect_d_eq(mallctl("max_background_threads", &max_n_thds, &sz_m, NULL,
	    0), 0, "Failed to get max background threads");
	expect_zu_eq(opt_max_n_thds, max_n_thds,
	    "max_background_threads and "
	    "opt.max_background_threads should match");
	expect_d_eq(mallctl("max_background_threads", NULL, NULL, &max_n_thds,
	    sz_m), 0, "Failed to set max background threads");

	unsigned id;
	size_t sz_u = sizeof(unsigned);

	for (unsigned i = 0; i < max_test_narenas(); i++) {
		expect_d_eq(mallctl("arenas.create", &id, &sz_u, NULL, 0), 0,
		    "Failed to create arena");
	}

	bool enable = true;
	size_t sz_b = sizeof(bool);
	expect_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to enable background threads");
	expect_zu_eq(n_background_threads, max_n_thds,
	    "Number of background threads should not change.\n");
	size_t new_max_thds = max_n_thds - 1;
	if (new_max_thds > 0) {
		expect_d_eq(mallctl("max_background_threads", NULL, NULL,
		    &new_max_thds, sz_m), 0,
		    "Failed to set max background threads");
		expect_zu_eq(n_background_threads, new_max_thds,
		    "Number of background threads should decrease by 1.\n");
	}
	new_max_thds = 1;
	expect_d_eq(mallctl("max_background_threads", NULL, NULL, &new_max_thds,
	    sz_m), 0, "Failed to set max background threads");
	expect_zu_eq(n_background_threads, new_max_thds,
	    "Number of background threads should be 1.\n");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
		test_deferred,
		test_max_background_threads);
}
