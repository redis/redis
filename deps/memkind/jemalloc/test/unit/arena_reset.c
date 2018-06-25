#include "test/jemalloc_test.h"

static unsigned
get_nsizes_impl(const char *cmd)
{
	unsigned ret;
	size_t z;

	z = sizeof(unsigned);
	assert_d_eq(mallctl(cmd, (void *)&ret, &z, NULL, 0), 0,
	    "Unexpected mallctl(\"%s\", ...) failure", cmd);

	return (ret);
}

static unsigned
get_nsmall(void)
{

	return (get_nsizes_impl("arenas.nbins"));
}

static unsigned
get_nlarge(void)
{

	return (get_nsizes_impl("arenas.nlruns"));
}

static unsigned
get_nhuge(void)
{

	return (get_nsizes_impl("arenas.nhchunks"));
}

static size_t
get_size_impl(const char *cmd, size_t ind)
{
	size_t ret;
	size_t z;
	size_t mib[4];
	size_t miblen = 4;

	z = sizeof(size_t);
	assert_d_eq(mallctlnametomib(cmd, mib, &miblen),
	    0, "Unexpected mallctlnametomib(\"%s\", ...) failure", cmd);
	mib[2] = ind;
	z = sizeof(size_t);
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&ret, &z, NULL, 0),
	    0, "Unexpected mallctlbymib([\"%s\", %zu], ...) failure", cmd, ind);

	return (ret);
}

static size_t
get_small_size(size_t ind)
{

	return (get_size_impl("arenas.bin.0.size", ind));
}

static size_t
get_large_size(size_t ind)
{

	return (get_size_impl("arenas.lrun.0.size", ind));
}

static size_t
get_huge_size(size_t ind)
{

	return (get_size_impl("arenas.hchunk.0.size", ind));
}

TEST_BEGIN(test_arena_reset)
{
#define	NHUGE	4
	unsigned arena_ind, nsmall, nlarge, nhuge, nptrs, i;
	size_t sz, miblen;
	void **ptrs;
	int flags;
	size_t mib[3];
	tsdn_t *tsdn;

	test_skip_if((config_valgrind && unlikely(in_valgrind)) || (config_fill
	    && unlikely(opt_quarantine)));

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.extend", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	nsmall = get_nsmall();
	nlarge = get_nlarge();
	nhuge = get_nhuge() > NHUGE ? NHUGE : get_nhuge();
	nptrs = nsmall + nlarge + nhuge;
	ptrs = (void **)malloc(nptrs * sizeof(void *));
	assert_ptr_not_null(ptrs, "Unexpected malloc() failure");

	/* Allocate objects with a wide range of sizes. */
	for (i = 0; i < nsmall; i++) {
		sz = get_small_size(i);
		ptrs[i] = mallocx(sz, flags);
		assert_ptr_not_null(ptrs[i],
		    "Unexpected mallocx(%zu, %#x) failure", sz, flags);
	}
	for (i = 0; i < nlarge; i++) {
		sz = get_large_size(i);
		ptrs[nsmall + i] = mallocx(sz, flags);
		assert_ptr_not_null(ptrs[i],
		    "Unexpected mallocx(%zu, %#x) failure", sz, flags);
	}
	for (i = 0; i < nhuge; i++) {
		sz = get_huge_size(i);
		ptrs[nsmall + nlarge + i] = mallocx(sz, flags);
		assert_ptr_not_null(ptrs[i],
		    "Unexpected mallocx(%zu, %#x) failure", sz, flags);
	}

	tsdn = tsdn_fetch();

	/* Verify allocations. */
	for (i = 0; i < nptrs; i++) {
		assert_zu_gt(ivsalloc(tsdn, ptrs[i], false), 0,
		    "Allocation should have queryable size");
	}

	/* Reset. */
	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.reset", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");

	/* Verify allocations no longer exist. */
	for (i = 0; i < nptrs; i++) {
		assert_zu_eq(ivsalloc(tsdn, ptrs[i], false), 0,
		    "Allocation should no longer exist");
	}

	free(ptrs);
}
TEST_END

int
main(void)
{

	return (test(
	    test_arena_reset));
}
