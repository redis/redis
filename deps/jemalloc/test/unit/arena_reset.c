#ifndef ARENA_RESET_PROF_C_
#include "test/jemalloc_test.h"
#endif

#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/rtree.h"

#include "test/extent_hooks.h"

static unsigned
get_nsizes_impl(const char *cmd) {
	unsigned ret;
	size_t z;

	z = sizeof(unsigned);
	assert_d_eq(mallctl(cmd, (void *)&ret, &z, NULL, 0), 0,
	    "Unexpected mallctl(\"%s\", ...) failure", cmd);

	return ret;
}

static unsigned
get_nsmall(void) {
	return get_nsizes_impl("arenas.nbins");
}

static unsigned
get_nlarge(void) {
	return get_nsizes_impl("arenas.nlextents");
}

static size_t
get_size_impl(const char *cmd, size_t ind) {
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

	return ret;
}

static size_t
get_small_size(size_t ind) {
	return get_size_impl("arenas.bin.0.size", ind);
}

static size_t
get_large_size(size_t ind) {
	return get_size_impl("arenas.lextent.0.size", ind);
}

/* Like ivsalloc(), but safe to call on discarded allocations. */
static size_t
vsalloc(tsdn_t *tsdn, const void *ptr) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	extent_t *extent;
	szind_t szind;
	if (rtree_extent_szind_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, false, &extent, &szind)) {
		return 0;
	}

	if (extent == NULL) {
		return 0;
	}
	if (extent_state_get(extent) != extent_state_active) {
		return 0;
	}

	if (szind == SC_NSIZES) {
		return 0;
	}

	return sz_index2size(szind);
}

static unsigned
do_arena_create(extent_hooks_t *h) {
	unsigned arena_ind;
	size_t sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz,
	    (void *)(h != NULL ? &h : NULL), (h != NULL ? sizeof(h) : 0)), 0,
	    "Unexpected mallctl() failure");
	return arena_ind;
}

static void
do_arena_reset_pre(unsigned arena_ind, void ***ptrs, unsigned *nptrs) {
#define NLARGE	32
	unsigned nsmall, nlarge, i;
	size_t sz;
	int flags;
	tsdn_t *tsdn;

	flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	nsmall = get_nsmall();
	nlarge = get_nlarge() > NLARGE ? NLARGE : get_nlarge();
	*nptrs = nsmall + nlarge;
	*ptrs = (void **)malloc(*nptrs * sizeof(void *));
	assert_ptr_not_null(*ptrs, "Unexpected malloc() failure");

	/* Allocate objects with a wide range of sizes. */
	for (i = 0; i < nsmall; i++) {
		sz = get_small_size(i);
		(*ptrs)[i] = mallocx(sz, flags);
		assert_ptr_not_null((*ptrs)[i],
		    "Unexpected mallocx(%zu, %#x) failure", sz, flags);
	}
	for (i = 0; i < nlarge; i++) {
		sz = get_large_size(i);
		(*ptrs)[nsmall + i] = mallocx(sz, flags);
		assert_ptr_not_null((*ptrs)[i],
		    "Unexpected mallocx(%zu, %#x) failure", sz, flags);
	}

	tsdn = tsdn_fetch();

	/* Verify allocations. */
	for (i = 0; i < *nptrs; i++) {
		assert_zu_gt(ivsalloc(tsdn, (*ptrs)[i]), 0,
		    "Allocation should have queryable size");
	}
}

static void
do_arena_reset_post(void **ptrs, unsigned nptrs, unsigned arena_ind) {
	tsdn_t *tsdn;
	unsigned i;

	tsdn = tsdn_fetch();

	if (have_background_thread) {
		malloc_mutex_lock(tsdn,
		    &background_thread_info_get(arena_ind)->mtx);
	}
	/* Verify allocations no longer exist. */
	for (i = 0; i < nptrs; i++) {
		assert_zu_eq(vsalloc(tsdn, ptrs[i]), 0,
		    "Allocation should no longer exist");
	}
	if (have_background_thread) {
		malloc_mutex_unlock(tsdn,
		    &background_thread_info_get(arena_ind)->mtx);
	}

	free(ptrs);
}

static void
do_arena_reset_destroy(const char *name, unsigned arena_ind) {
	size_t mib[3];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib(name, mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

static void
do_arena_reset(unsigned arena_ind) {
	do_arena_reset_destroy("arena.0.reset", arena_ind);
}

static void
do_arena_destroy(unsigned arena_ind) {
	do_arena_reset_destroy("arena.0.destroy", arena_ind);
}

TEST_BEGIN(test_arena_reset) {
	unsigned arena_ind;
	void **ptrs;
	unsigned nptrs;

	arena_ind = do_arena_create(NULL);
	do_arena_reset_pre(arena_ind, &ptrs, &nptrs);
	do_arena_reset(arena_ind);
	do_arena_reset_post(ptrs, nptrs, arena_ind);
}
TEST_END

static bool
arena_i_initialized(unsigned arena_ind, bool refresh) {
	bool initialized;
	size_t mib[3];
	size_t miblen, sz;

	if (refresh) {
		uint64_t epoch = 1;
		assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch,
		    sizeof(epoch)), 0, "Unexpected mallctl() failure");
	}

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.initialized", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	sz = sizeof(initialized);
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&initialized, &sz, NULL,
	    0), 0, "Unexpected mallctlbymib() failure");

	return initialized;
}

TEST_BEGIN(test_arena_destroy_initial) {
	assert_false(arena_i_initialized(MALLCTL_ARENAS_DESTROYED, false),
	    "Destroyed arena stats should not be initialized");
}
TEST_END

TEST_BEGIN(test_arena_destroy_hooks_default) {
	unsigned arena_ind, arena_ind_another, arena_ind_prev;
	void **ptrs;
	unsigned nptrs;

	arena_ind = do_arena_create(NULL);
	do_arena_reset_pre(arena_ind, &ptrs, &nptrs);

	assert_false(arena_i_initialized(arena_ind, false),
	    "Arena stats should not be initialized");
	assert_true(arena_i_initialized(arena_ind, true),
	    "Arena stats should be initialized");

	/*
	 * Create another arena before destroying one, to better verify arena
	 * index reuse.
	 */
	arena_ind_another = do_arena_create(NULL);

	do_arena_destroy(arena_ind);

	assert_false(arena_i_initialized(arena_ind, true),
	    "Arena stats should not be initialized");
	assert_true(arena_i_initialized(MALLCTL_ARENAS_DESTROYED, false),
	    "Destroyed arena stats should be initialized");

	do_arena_reset_post(ptrs, nptrs, arena_ind);

	arena_ind_prev = arena_ind;
	arena_ind = do_arena_create(NULL);
	do_arena_reset_pre(arena_ind, &ptrs, &nptrs);
	assert_u_eq(arena_ind, arena_ind_prev,
	    "Arena index should have been recycled");
	do_arena_destroy(arena_ind);
	do_arena_reset_post(ptrs, nptrs, arena_ind);

	do_arena_destroy(arena_ind_another);
}
TEST_END

/*
 * Actually unmap extents, regardless of opt_retain, so that attempts to access
 * a destroyed arena's memory will segfault.
 */
static bool
extent_dalloc_unmap(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, committed=%s, "
	    "arena_ind=%u)\n", __func__, extent_hooks, addr, size, committed ?
	    "true" : "false", arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->dalloc, extent_dalloc_unmap,
	    "Wrong hook function");
	called_dalloc = true;
	if (!try_dalloc) {
		return true;
	}
	did_dalloc = true;
	if (!maps_coalesce && opt_retain) {
		return true;
	}
	pages_unmap(addr, size);
	return false;
}

static extent_hooks_t hooks_orig;

static extent_hooks_t hooks_unmap = {
	extent_alloc_hook,
	extent_dalloc_unmap, /* dalloc */
	extent_destroy_hook,
	extent_commit_hook,
	extent_decommit_hook,
	extent_purge_lazy_hook,
	extent_purge_forced_hook,
	extent_split_hook,
	extent_merge_hook
};

TEST_BEGIN(test_arena_destroy_hooks_unmap) {
	unsigned arena_ind;
	void **ptrs;
	unsigned nptrs;

	extent_hooks_prep();
	if (maps_coalesce) {
		try_decommit = false;
	}
	memcpy(&hooks_orig, &hooks, sizeof(extent_hooks_t));
	memcpy(&hooks, &hooks_unmap, sizeof(extent_hooks_t));

	did_alloc = false;
	arena_ind = do_arena_create(&hooks);
	do_arena_reset_pre(arena_ind, &ptrs, &nptrs);

	assert_true(did_alloc, "Expected alloc");

	assert_false(arena_i_initialized(arena_ind, false),
	    "Arena stats should not be initialized");
	assert_true(arena_i_initialized(arena_ind, true),
	    "Arena stats should be initialized");

	did_dalloc = false;
	do_arena_destroy(arena_ind);
	assert_true(did_dalloc, "Expected dalloc");

	assert_false(arena_i_initialized(arena_ind, true),
	    "Arena stats should not be initialized");
	assert_true(arena_i_initialized(MALLCTL_ARENAS_DESTROYED, false),
	    "Destroyed arena stats should be initialized");

	do_arena_reset_post(ptrs, nptrs, arena_ind);

	memcpy(&hooks, &hooks_orig, sizeof(extent_hooks_t));
}
TEST_END

int
main(void) {
	return test(
	    test_arena_reset,
	    test_arena_destroy_initial,
	    test_arena_destroy_hooks_default,
	    test_arena_destroy_hooks_unmap);
}
