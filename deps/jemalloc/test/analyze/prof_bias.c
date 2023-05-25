#include "test/jemalloc_test.h"

/*
 * This is a helper utility, only meant to be run manually (and, for example,
 * doesn't check for failures, try to skip execution in non-prof modes, etc.).
 * It runs, allocates objects of two different sizes from the same stack trace,
 * and exits.
 *
 * The idea is that some human operator will run it like:
 *     MALLOC_CONF="prof:true,prof_final:true" test/analyze/prof_bias
 * and manually inspect the results.
 *
 * The results should be:
 * jeprof --text test/analyze/prof_bias --inuse_space jeprof.<pid>.0.f.heap:
 * 	around 1024 MB
 * jeprof --text test/analyze/prof_bias --inuse_objects jeprof.<pid>.0.f.heap:
 * 	around 33554448 = 16 + 32 * 1024 * 1024
 *
 * And, if prof_accum is on:
 * jeprof --text test/analyze/prof_bias --alloc_space jeprof.<pid>.0.f.heap:
 *     around 2048 MB
 * jeprof --text test/analyze/prof_bias --alloc_objects jeprof.<pid>.0.f.heap:
 * 	around 67108896 = 2 * (16 + 32 * 1024 * 1024)
 */

static void
mock_backtrace(void **vec, unsigned *len, unsigned max_len) {
	*len = 4;
	vec[0] = (void *)0x111;
	vec[1] = (void *)0x222;
	vec[2] = (void *)0x333;
	vec[3] = (void *)0x444;
}

static void
do_allocs(size_t sz, size_t cnt, bool do_frees) {
	for (size_t i = 0; i < cnt; i++) {
		void *ptr = mallocx(sz, 0);
		assert_ptr_not_null(ptr, "Unexpected mallocx failure");
		if (do_frees) {
			dallocx(ptr, 0);
		}
	}
}

int
main(void) {
	size_t lg_prof_sample_local = 19;
	int err = mallctl("prof.reset", NULL, NULL,
	    (void *)&lg_prof_sample_local, sizeof(lg_prof_sample_local));
	assert(err == 0);

	prof_backtrace_hook_set(mock_backtrace);
	do_allocs(16, 32 * 1024 * 1024, /* do_frees */ true);
	do_allocs(32 * 1024* 1024, 16, /* do_frees */ true);
	do_allocs(16, 32 * 1024 * 1024, /* do_frees */ false);
	do_allocs(32 * 1024* 1024, 16, /* do_frees */ false);

	return 0;
}
