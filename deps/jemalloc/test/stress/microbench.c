#include "test/jemalloc_test.h"

JEMALLOC_INLINE_C void
time_func(timedelta_t *timer, uint64_t nwarmup, uint64_t niter,
    void (*func)(void))
{
	uint64_t i;

	for (i = 0; i < nwarmup; i++)
		func();
	timer_start(timer);
	for (i = 0; i < niter; i++)
		func();
	timer_stop(timer);
}

void
compare_funcs(uint64_t nwarmup, uint64_t niter, const char *name_a,
    void (*func_a), const char *name_b, void (*func_b))
{
	timedelta_t timer_a, timer_b;
	char ratio_buf[6];
	void *p;

	p = mallocx(1, 0);
	if (p == NULL) {
		test_fail("Unexpected mallocx() failure");
		return;
	}

	time_func(&timer_a, nwarmup, niter, func_a);
	time_func(&timer_b, nwarmup, niter, func_b);

	timer_ratio(&timer_a, &timer_b, ratio_buf, sizeof(ratio_buf));
	malloc_printf("%"FMTu64" iterations, %s=%"FMTu64"us, "
	    "%s=%"FMTu64"us, ratio=1:%s\n",
	    niter, name_a, timer_usec(&timer_a), name_b, timer_usec(&timer_b),
	    ratio_buf);

	dallocx(p, 0);
}

static void
malloc_free(void)
{
	/* The compiler can optimize away free(malloc(1))! */
	void *p = malloc(1);
	if (p == NULL) {
		test_fail("Unexpected malloc() failure");
		return;
	}
	free(p);
}

static void
mallocx_free(void)
{
	void *p = mallocx(1, 0);
	if (p == NULL) {
		test_fail("Unexpected mallocx() failure");
		return;
	}
	free(p);
}

TEST_BEGIN(test_malloc_vs_mallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "malloc",
	    malloc_free, "mallocx", mallocx_free);
}
TEST_END

static void
malloc_dallocx(void)
{
	void *p = malloc(1);
	if (p == NULL) {
		test_fail("Unexpected malloc() failure");
		return;
	}
	dallocx(p, 0);
}

static void
malloc_sdallocx(void)
{
	void *p = malloc(1);
	if (p == NULL) {
		test_fail("Unexpected malloc() failure");
		return;
	}
	sdallocx(p, 1, 0);
}

TEST_BEGIN(test_free_vs_dallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "free", malloc_free,
	    "dallocx", malloc_dallocx);
}
TEST_END

TEST_BEGIN(test_dallocx_vs_sdallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "dallocx", malloc_dallocx,
	    "sdallocx", malloc_sdallocx);
}
TEST_END

static void
malloc_mus_free(void)
{
	void *p;

	p = malloc(1);
	if (p == NULL) {
		test_fail("Unexpected malloc() failure");
		return;
	}
	malloc_usable_size(p);
	free(p);
}

static void
malloc_sallocx_free(void)
{
	void *p;

	p = malloc(1);
	if (p == NULL) {
		test_fail("Unexpected malloc() failure");
		return;
	}
	if (sallocx(p, 0) < 1)
		test_fail("Unexpected sallocx() failure");
	free(p);
}

TEST_BEGIN(test_mus_vs_sallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "malloc_usable_size",
	    malloc_mus_free, "sallocx", malloc_sallocx_free);
}
TEST_END

static void
malloc_nallocx_free(void)
{
	void *p;

	p = malloc(1);
	if (p == NULL) {
		test_fail("Unexpected malloc() failure");
		return;
	}
	if (nallocx(1, 0) < 1)
		test_fail("Unexpected nallocx() failure");
	free(p);
}

TEST_BEGIN(test_sallocx_vs_nallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "sallocx",
	    malloc_sallocx_free, "nallocx", malloc_nallocx_free);
}
TEST_END

int
main(void)
{

	return (test(
	    test_malloc_vs_mallocx,
	    test_free_vs_dallocx,
	    test_dallocx_vs_sdallocx,
	    test_mus_vs_sallocx,
	    test_sallocx_vs_nallocx));
}
