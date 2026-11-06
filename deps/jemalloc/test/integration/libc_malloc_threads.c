#include "test/jemalloc_test.h"

/*
 * A prefixed jemalloc lives alongside the libc malloc, and must not take over
 * the libc malloc's private thread-exit hooks.  Otherwise, on systems where
 * the libc malloc relies on them (e.g. _malloc_thread_cleanup() on FreeBSD),
 * the libc malloc never cleans up exited threads, and later threads that use
 * it crash.
 */

/* Use the libc malloc, rather than the mangled jemalloc API. */
#undef malloc
#undef free

#define NTHREADS 8
#define NROUNDS 16

#if defined(__FreeBSD__) && defined(JEMALLOC_PREFIX)
/*
 * The FreeBSD libc malloc is jemalloc, and exports its own mallctl().  Use it
 * to check that the libc malloc cleans up exited threads, rather than relying
 * on a crash, which depends on whether the freed TLS gets unmapped.
 */
#	undef mallctl
int mallctl(
    const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#	define LIBC_NTHREADS_CHECK

static unsigned
libc_nthreads_get(void) {
	uint64_t epoch = 1;
	unsigned nthreads;
	size_t   sz = sizeof(nthreads);

	expect_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected libc mallctl() failure");
	expect_d_eq(
	    mallctl("stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL) ".nthreads",
	        &nthreads, &sz, NULL, 0),
	    0, "Unexpected libc mallctl() failure");
	return nthreads;
}
#endif

static void *
thd_start(void *arg) {
	void *p;

	/* Initialize the TSD of both allocators in this thread. */
	p = mallocx(1, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	dallocx(p, 0);

	p = malloc(96);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	free(p);

	return NULL;
}

TEST_BEGIN(test_libc_malloc_threads) {
	thd_t    thds[NTHREADS];
	unsigned i, j;
#ifdef LIBC_NTHREADS_CHECK
	unsigned nthreads;

	/* Make sure the main thread is accounted for in the baseline. */
	free(malloc(1));
	nthreads = libc_nthreads_get();
#endif

	for (i = 0; i < NROUNDS; i++) {
		for (j = 0; j < NTHREADS; j++) {
			thd_create(&thds[j], thd_start, NULL);
		}
		for (j = 0; j < NTHREADS; j++) {
			thd_join(thds[j], NULL);
		}
#ifdef LIBC_NTHREADS_CHECK
		expect_u_le(libc_nthreads_get(), nthreads,
		    "The libc malloc should clean up exited threads");
#endif
	}
}
TEST_END

int
main(void) {
	return test(test_libc_malloc_threads);
}
