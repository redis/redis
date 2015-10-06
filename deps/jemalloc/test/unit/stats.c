#include "test/jemalloc_test.h"

TEST_BEGIN(test_stats_summary)
{
	size_t *cactive;
	size_t sz, allocated, active, resident, mapped;
	int expected = config_stats ? 0 : ENOENT;

	sz = sizeof(cactive);
	assert_d_eq(mallctl("stats.cactive", &cactive, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.allocated", &allocated, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.active", &active, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.resident", &resident, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.mapped", &mapped, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_le(active, *cactive,
		    "active should be no larger than cactive");
		assert_zu_le(allocated, active,
		    "allocated should be no larger than active");
		assert_zu_lt(active, resident,
		    "active should be less than resident");
		assert_zu_lt(active, mapped,
		    "active should be less than mapped");
	}
}
TEST_END

TEST_BEGIN(test_stats_huge)
{
	void *p;
	uint64_t epoch;
	size_t allocated;
	uint64_t nmalloc, ndalloc, nrequests;
	size_t sz;
	int expected = config_stats ? 0 : ENOENT;

	p = mallocx(large_maxclass+1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.huge.allocated", &allocated, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.huge.nmalloc", &nmalloc, &sz, NULL,
	    0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.huge.ndalloc", &ndalloc, &sz, NULL,
	    0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.huge.nrequests", &nrequests, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_le(nmalloc, nrequests,
		    "nmalloc should no larger than nrequests");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_summary)
{
	unsigned arena;
	void *little, *large, *huge;
	uint64_t epoch;
	size_t sz;
	int expected = config_stats ? 0 : ENOENT;
	size_t mapped;
	uint64_t npurge, nmadvise, purged;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena)),
	    0, "Unexpected mallctl() failure");

	little = mallocx(SMALL_MAXCLASS, 0);
	assert_ptr_not_null(little, "Unexpected mallocx() failure");
	large = mallocx(large_maxclass, 0);
	assert_ptr_not_null(large, "Unexpected mallocx() failure");
	huge = mallocx(chunksize, 0);
	assert_ptr_not_null(huge, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.mapped", &mapped, &sz, NULL, 0),
	    expected, "Unexepected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.npurge", &npurge, &sz, NULL, 0),
	    expected, "Unexepected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.nmadvise", &nmadvise, &sz, NULL, 0),
	    expected, "Unexepected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.purged", &purged, &sz, NULL, 0),
	    expected, "Unexepected mallctl() result");

	if (config_stats) {
		assert_u64_gt(npurge, 0,
		    "At least one purge should have occurred");
		assert_u64_le(nmadvise, purged,
		    "nmadvise should be no greater than purged");
	}

	dallocx(little, 0);
	dallocx(large, 0);
	dallocx(huge, 0);
}
TEST_END

void *
thd_start(void *arg)
{

	return (NULL);
}

static void
no_lazy_lock(void)
{
	thd_t thd;

	thd_create(&thd, thd_start, NULL);
	thd_join(thd, NULL);
}

TEST_BEGIN(test_stats_arenas_small)
{
	unsigned arena;
	void *p;
	size_t sz, allocated;
	uint64_t epoch, nmalloc, ndalloc, nrequests;
	int expected = config_stats ? 0 : ENOENT;

	no_lazy_lock(); /* Lazy locking would dodge tcache testing. */

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena)),
	    0, "Unexpected mallctl() failure");

	p = mallocx(SMALL_MAXCLASS, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    config_tcache ? 0 : ENOENT, "Unexpected mallctl() result");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.small.allocated", &allocated, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.small.nmalloc", &nmalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.small.ndalloc", &ndalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.small.nrequests", &nrequests, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be no greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_gt(nrequests, 0,
		    "nrequests should be greater than zero");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_large)
{
	unsigned arena;
	void *p;
	size_t sz, allocated;
	uint64_t epoch, nmalloc, ndalloc, nrequests;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena)),
	    0, "Unexpected mallctl() failure");

	p = mallocx(large_maxclass, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.large.allocated", &allocated, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.large.nmalloc", &nmalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.large.ndalloc", &ndalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.large.nrequests", &nrequests, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		assert_zu_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_zu_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_zu_gt(nrequests, 0,
		    "nrequests should be greater than zero");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_huge)
{
	unsigned arena;
	void *p;
	size_t sz, allocated;
	uint64_t epoch, nmalloc, ndalloc;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena)),
	    0, "Unexpected mallctl() failure");

	p = mallocx(chunksize, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.huge.allocated", &allocated, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.huge.nmalloc", &nmalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.huge.ndalloc", &ndalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		assert_zu_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_zu_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_bins)
{
	unsigned arena;
	void *p;
	size_t sz, curruns, curregs;
	uint64_t epoch, nmalloc, ndalloc, nrequests, nfills, nflushes;
	uint64_t nruns, nreruns;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena)),
	    0, "Unexpected mallctl() failure");

	p = mallocx(arena_bin_info[0].reg_size, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    config_tcache ? 0 : ENOENT, "Unexpected mallctl() result");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nmalloc", &nmalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.ndalloc", &ndalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nrequests", &nrequests, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.curregs", &curregs, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");

	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nfills", &nfills, &sz,
	    NULL, 0), config_tcache ? expected : ENOENT,
	    "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nflushes", &nflushes, &sz,
	    NULL, 0), config_tcache ? expected : ENOENT,
	    "Unexpected mallctl() result");

	assert_d_eq(mallctl("stats.arenas.0.bins.0.nruns", &nruns, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nreruns", &nreruns, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.curruns", &curruns, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_gt(nrequests, 0,
		    "nrequests should be greater than zero");
		assert_zu_gt(curregs, 0,
		    "allocated should be greater than zero");
		if (config_tcache) {
			assert_u64_gt(nfills, 0,
			    "At least one fill should have occurred");
			assert_u64_gt(nflushes, 0,
			    "At least one flush should have occurred");
		}
		assert_u64_gt(nruns, 0,
		    "At least one run should have been allocated");
		assert_zu_gt(curruns, 0,
		    "At least one run should be currently allocated");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_lruns)
{
	unsigned arena;
	void *p;
	uint64_t epoch, nmalloc, ndalloc, nrequests;
	size_t curruns, sz;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena)),
	    0, "Unexpected mallctl() failure");

	p = mallocx(LARGE_MINCLASS, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.lruns.0.nmalloc", &nmalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.lruns.0.ndalloc", &ndalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.lruns.0.nrequests", &nrequests, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.lruns.0.curruns", &curruns, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_gt(nrequests, 0,
		    "nrequests should be greater than zero");
		assert_u64_gt(curruns, 0,
		    "At least one run should be currently allocated");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_hchunks)
{
	unsigned arena;
	void *p;
	uint64_t epoch, nmalloc, ndalloc;
	size_t curhchunks, sz;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, &arena, sizeof(arena)),
	    0, "Unexpected mallctl() failure");

	p = mallocx(chunksize, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.hchunks.0.nmalloc", &nmalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.hchunks.0.ndalloc", &ndalloc, &sz,
	    NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.hchunks.0.curhchunks", &curhchunks,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_gt(curhchunks, 0,
		    "At least one chunk should be currently allocated");
	}

	dallocx(p, 0);
}
TEST_END

int
main(void)
{

	return (test(
	    test_stats_summary,
	    test_stats_huge,
	    test_stats_arenas_summary,
	    test_stats_arenas_small,
	    test_stats_arenas_large,
	    test_stats_arenas_huge,
	    test_stats_arenas_bins,
	    test_stats_arenas_lruns,
	    test_stats_arenas_hchunks));
}
