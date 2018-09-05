#include "test/jemalloc_test.h"

const char *malloc_conf = "background_thread:false,narenas:1,max_background_threads:20";

TEST_BEGIN(test_deferred) {
	test_skip_if(!have_background_thread);

	unsigned id;
	size_t sz_u = sizeof(unsigned);

	/*
	 * 10 here is somewhat arbitrary, except insofar as we want to ensure
	 * that the number of background threads is smaller than the number of
	 * arenas.  I'll ragequit long before we have to spin up 10 threads per
	 * cpu to handle background purging, so this is a conservative
	 * approximation.
	 */
	for (unsigned i = 0; i < 10 * ncpus; i++) {
		assert_d_eq(mallctl("arenas.create", &id, &sz_u, NULL, 0), 0,
		    "Failed to create arena");
	}

	bool enable = true;
	size_t sz_b = sizeof(bool);
	assert_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to enable background threads");
	enable = false;
	assert_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to disable background threads");
}
TEST_END

TEST_BEGIN(test_max_background_threads) {
	test_skip_if(!have_background_thread);

	size_t maxt;
	size_t opt_maxt;
	size_t sz_m = sizeof(maxt);
	assert_d_eq(mallctl("opt.max_background_threads",
			    &opt_maxt, &sz_m, NULL, 0), 0,
			    "Failed to get opt.max_background_threads");
	assert_d_eq(mallctl("max_background_threads", &maxt, &sz_m, NULL, 0), 0,
		    "Failed to get max background threads");
	assert_zu_eq(20, maxt, "should be ncpus");
	assert_zu_eq(opt_maxt, maxt,
		     "max_background_threads and "
		     "opt.max_background_threads should match");
	assert_d_eq(mallctl("max_background_threads", NULL, NULL, &maxt, sz_m),
		    0, "Failed to set max background threads");

	unsigned id;
	size_t sz_u = sizeof(unsigned);

	for (unsigned i = 0; i < 10 * ncpus; i++) {
		assert_d_eq(mallctl("arenas.create", &id, &sz_u, NULL, 0), 0,
		    "Failed to create arena");
	}

	bool enable = true;
	size_t sz_b = sizeof(bool);
	assert_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to enable background threads");
	assert_zu_eq(n_background_threads, maxt,
		     "Number of background threads should be 3.\n");
	maxt = 10;
	assert_d_eq(mallctl("max_background_threads", NULL, NULL, &maxt, sz_m),
		    0, "Failed to set max background threads");
	assert_zu_eq(n_background_threads, maxt,
		     "Number of background threads should be 10.\n");
	maxt = 3;
	assert_d_eq(mallctl("max_background_threads", NULL, NULL, &maxt, sz_m),
		    0, "Failed to set max background threads");
	assert_zu_eq(n_background_threads, maxt,
		     "Number of background threads should be 3.\n");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
		test_deferred,
		test_max_background_threads);
}
