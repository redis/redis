#include "test/jemalloc_test.h"

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

TEST_BEGIN(test_stats_summary) {
	size_t sz, allocated, active, resident, mapped;
	int expected = config_stats ? 0 : ENOENT;

	sz = sizeof(size_t);
	expect_d_eq(mallctl("stats.allocated", (void *)&allocated, &sz, NULL,
	    0), expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.active", (void *)&active, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.resident", (void *)&resident, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.mapped", (void *)&mapped, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");

	if (config_stats) {
		expect_zu_le(allocated, active,
		    "allocated should be no larger than active");
		expect_zu_lt(active, resident,
		    "active should be less than resident");
		expect_zu_lt(active, mapped,
		    "active should be less than mapped");
	}
}
TEST_END

TEST_BEGIN(test_stats_large) {
	void *p;
	uint64_t epoch;
	size_t allocated;
	uint64_t nmalloc, ndalloc, nrequests;
	size_t sz;
	int expected = config_stats ? 0 : ENOENT;

	p = mallocx(SC_SMALL_MAXCLASS + 1, MALLOCX_ARENA(0));
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	expect_d_eq(mallctl("stats.arenas.0.large.allocated",
	    (void *)&allocated, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	expect_d_eq(mallctl("stats.arenas.0.large.nmalloc", (void *)&nmalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.large.ndalloc", (void *)&ndalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.large.nrequests",
	    (void *)&nrequests, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	if (config_stats) {
		expect_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		expect_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		expect_u64_le(nmalloc, nrequests,
		    "nmalloc should no larger than nrequests");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_summary) {
	void *little, *large;
	uint64_t epoch;
	size_t sz;
	int expected = config_stats ? 0 : ENOENT;
	size_t mapped;
	uint64_t dirty_npurge, dirty_nmadvise, dirty_purged;
	uint64_t muzzy_npurge, muzzy_nmadvise, muzzy_purged;

	little = mallocx(SC_SMALL_MAXCLASS, MALLOCX_ARENA(0));
	expect_ptr_not_null(little, "Unexpected mallocx() failure");
	large = mallocx((1U << SC_LG_LARGE_MINCLASS),
	    MALLOCX_ARENA(0));
	expect_ptr_not_null(large, "Unexpected mallocx() failure");

	dallocx(little, 0);
	dallocx(large, 0);

	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    opt_tcache ? 0 : EFAULT, "Unexpected mallctl() result");
	expect_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	expect_d_eq(mallctl("stats.arenas.0.mapped", (void *)&mapped, &sz, NULL,
	    0), expected, "Unexepected mallctl() result");

	sz = sizeof(uint64_t);
	expect_d_eq(mallctl("stats.arenas.0.dirty_npurge",
	    (void *)&dirty_npurge, &sz, NULL, 0), expected,
	    "Unexepected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.dirty_nmadvise",
	    (void *)&dirty_nmadvise, &sz, NULL, 0), expected,
	    "Unexepected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.dirty_purged",
	    (void *)&dirty_purged, &sz, NULL, 0), expected,
	    "Unexepected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.muzzy_npurge",
	    (void *)&muzzy_npurge, &sz, NULL, 0), expected,
	    "Unexepected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.muzzy_nmadvise",
	    (void *)&muzzy_nmadvise, &sz, NULL, 0), expected,
	    "Unexepected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.muzzy_purged",
	    (void *)&muzzy_purged, &sz, NULL, 0), expected,
	    "Unexepected mallctl() result");

	if (config_stats) {
		if (!is_background_thread_enabled() && !opt_hpa) {
			expect_u64_gt(dirty_npurge + muzzy_npurge, 0,
			    "At least one purge should have occurred");
		}
		expect_u64_le(dirty_nmadvise, dirty_purged,
		    "dirty_nmadvise should be no greater than dirty_purged");
		expect_u64_le(muzzy_nmadvise, muzzy_purged,
		    "muzzy_nmadvise should be no greater than muzzy_purged");
	}
}
TEST_END

void *
thd_start(void *arg) {
	return NULL;
}

static void
no_lazy_lock(void) {
	thd_t thd;

	thd_create(&thd, thd_start, NULL);
	thd_join(thd, NULL);
}

TEST_BEGIN(test_stats_arenas_small) {
	void *p;
	size_t sz, allocated;
	uint64_t epoch, nmalloc, ndalloc, nrequests;
	int expected = config_stats ? 0 : ENOENT;

	no_lazy_lock(); /* Lazy locking would dodge tcache testing. */

	p = mallocx(SC_SMALL_MAXCLASS, MALLOCX_ARENA(0));
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    opt_tcache ? 0 : EFAULT, "Unexpected mallctl() result");

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	expect_d_eq(mallctl("stats.arenas.0.small.allocated",
	    (void *)&allocated, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	expect_d_eq(mallctl("stats.arenas.0.small.nmalloc", (void *)&nmalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.small.ndalloc", (void *)&ndalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.small.nrequests",
	    (void *)&nrequests, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	if (config_stats) {
		expect_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		expect_u64_gt(nmalloc, 0,
		    "nmalloc should be no greater than zero");
		expect_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		expect_u64_gt(nrequests, 0,
		    "nrequests should be greater than zero");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_large) {
	void *p;
	size_t sz, allocated;
	uint64_t epoch, nmalloc, ndalloc;
	int expected = config_stats ? 0 : ENOENT;

	p = mallocx((1U << SC_LG_LARGE_MINCLASS), MALLOCX_ARENA(0));
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	expect_d_eq(mallctl("stats.arenas.0.large.allocated",
	    (void *)&allocated, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	expect_d_eq(mallctl("stats.arenas.0.large.nmalloc", (void *)&nmalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.large.ndalloc", (void *)&ndalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		expect_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		expect_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		expect_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
	}

	dallocx(p, 0);
}
TEST_END

static void
gen_mallctl_str(char *cmd, char *name, unsigned arena_ind) {
	sprintf(cmd, "stats.arenas.%u.bins.0.%s", arena_ind, name);
}

TEST_BEGIN(test_stats_arenas_bins) {
	void *p;
	size_t sz, curslabs, curregs, nonfull_slabs;
	uint64_t epoch, nmalloc, ndalloc, nrequests, nfills, nflushes;
	uint64_t nslabs, nreslabs;
	int expected = config_stats ? 0 : ENOENT;

	/* Make sure allocation below isn't satisfied by tcache. */
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    opt_tcache ? 0 : EFAULT, "Unexpected mallctl() result");

	unsigned arena_ind, old_arena_ind;
	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Arena creation failure");
	sz = sizeof(arena_ind);
	expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
	    (void *)&arena_ind, sizeof(arena_ind)), 0,
	    "Unexpected mallctl() failure");

	p = malloc(bin_infos[0].reg_size);
	expect_ptr_not_null(p, "Unexpected malloc() failure");

	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    opt_tcache ? 0 : EFAULT, "Unexpected mallctl() result");

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	char cmd[128];
	sz = sizeof(uint64_t);
	gen_mallctl_str(cmd, "nmalloc", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&nmalloc, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	gen_mallctl_str(cmd, "ndalloc", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&ndalloc, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	gen_mallctl_str(cmd, "nrequests", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&nrequests, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(size_t);
	gen_mallctl_str(cmd, "curregs", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&curregs, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	sz = sizeof(uint64_t);
	gen_mallctl_str(cmd, "nfills", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&nfills, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	gen_mallctl_str(cmd, "nflushes", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&nflushes, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	gen_mallctl_str(cmd, "nslabs", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&nslabs, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	gen_mallctl_str(cmd, "nreslabs", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&nreslabs, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(size_t);
	gen_mallctl_str(cmd, "curslabs", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&curslabs, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	gen_mallctl_str(cmd, "nonfull_slabs", arena_ind);
	expect_d_eq(mallctl(cmd, (void *)&nonfull_slabs, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");

	if (config_stats) {
		expect_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		expect_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		expect_u64_gt(nrequests, 0,
		    "nrequests should be greater than zero");
		expect_zu_gt(curregs, 0,
		    "allocated should be greater than zero");
		if (opt_tcache) {
			expect_u64_gt(nfills, 0,
			    "At least one fill should have occurred");
			expect_u64_gt(nflushes, 0,
			    "At least one flush should have occurred");
		}
		expect_u64_gt(nslabs, 0,
		    "At least one slab should have been allocated");
		expect_zu_gt(curslabs, 0,
		    "At least one slab should be currently allocated");
		expect_zu_eq(nonfull_slabs, 0,
		    "slabs_nonfull should be empty");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_lextents) {
	void *p;
	uint64_t epoch, nmalloc, ndalloc;
	size_t curlextents, sz, hsize;
	int expected = config_stats ? 0 : ENOENT;

	sz = sizeof(size_t);
	expect_d_eq(mallctl("arenas.lextent.0.size", (void *)&hsize, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");

	p = mallocx(hsize, MALLOCX_ARENA(0));
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(uint64_t);
	expect_d_eq(mallctl("stats.arenas.0.lextents.0.nmalloc",
	    (void *)&nmalloc, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	expect_d_eq(mallctl("stats.arenas.0.lextents.0.ndalloc",
	    (void *)&ndalloc, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(size_t);
	expect_d_eq(mallctl("stats.arenas.0.lextents.0.curlextents",
	    (void *)&curlextents, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	if (config_stats) {
		expect_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		expect_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		expect_u64_gt(curlextents, 0,
		    "At least one extent should be currently allocated");
	}

	dallocx(p, 0);
}
TEST_END

static void
test_tcache_bytes_for_usize(size_t usize) {
	uint64_t epoch;
	size_t tcache_bytes, tcache_stashed_bytes;
	size_t sz = sizeof(tcache_bytes);

	void *ptr = mallocx(usize, 0);

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");
	assert_d_eq(mallctl(
	    "stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL) ".tcache_bytes",
	    &tcache_bytes, &sz, NULL, 0), 0, "Unexpected mallctl failure");
	assert_d_eq(mallctl(
	    "stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL)
	    ".tcache_stashed_bytes", &tcache_stashed_bytes, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");
	size_t tcache_bytes_before = tcache_bytes + tcache_stashed_bytes;
	dallocx(ptr, 0);

	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");
	assert_d_eq(mallctl(
	    "stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL) ".tcache_bytes",
	    &tcache_bytes, &sz, NULL, 0), 0, "Unexpected mallctl failure");
	assert_d_eq(mallctl(
	    "stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL)
	    ".tcache_stashed_bytes", &tcache_stashed_bytes, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");
	size_t tcache_bytes_after = tcache_bytes + tcache_stashed_bytes;
	assert_zu_eq(tcache_bytes_after - tcache_bytes_before,
	    usize, "Incorrectly attributed a free");
}

TEST_BEGIN(test_stats_tcache_bytes_small) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_tcache_max < SC_SMALL_MAXCLASS);

	test_tcache_bytes_for_usize(SC_SMALL_MAXCLASS);
}
TEST_END

TEST_BEGIN(test_stats_tcache_bytes_large) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_tcache_max < SC_LARGE_MINCLASS);

	test_tcache_bytes_for_usize(SC_LARGE_MINCLASS);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_stats_summary,
	    test_stats_large,
	    test_stats_arenas_summary,
	    test_stats_arenas_small,
	    test_stats_arenas_large,
	    test_stats_arenas_bins,
	    test_stats_arenas_lextents,
	    test_stats_tcache_bytes_small,
	    test_stats_tcache_bytes_large);
}
