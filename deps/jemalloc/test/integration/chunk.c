#include "test/jemalloc_test.h"

#ifdef JEMALLOC_FILL
const char *malloc_conf = "junk:false";
#endif

static chunk_hooks_t orig_hooks;
static chunk_hooks_t old_hooks;

static bool do_dalloc = true;
static bool do_decommit;

static bool did_alloc;
static bool did_dalloc;
static bool did_commit;
static bool did_decommit;
static bool did_purge;
static bool did_split;
static bool did_merge;

#if 0
#  define TRACE_HOOK(fmt, ...) malloc_printf(fmt, __VA_ARGS__)
#else
#  define TRACE_HOOK(fmt, ...)
#endif

void *
chunk_alloc(void *new_addr, size_t size, size_t alignment, bool *zero,
    bool *commit, unsigned arena_ind)
{

	TRACE_HOOK("%s(new_addr=%p, size=%zu, alignment=%zu, *zero=%s, "
	    "*commit=%s, arena_ind=%u)\n", __func__, new_addr, size, alignment,
	    *zero ?  "true" : "false", *commit ? "true" : "false", arena_ind);
	did_alloc = true;
	return (old_hooks.alloc(new_addr, size, alignment, zero, commit,
	    arena_ind));
}

bool
chunk_dalloc(void *chunk, size_t size, bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(chunk=%p, size=%zu, committed=%s, arena_ind=%u)\n",
	    __func__, chunk, size, committed ? "true" : "false", arena_ind);
	did_dalloc = true;
	if (!do_dalloc)
		return (true);
	return (old_hooks.dalloc(chunk, size, committed, arena_ind));
}

bool
chunk_commit(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(chunk=%p, size=%zu, offset=%zu, length=%zu, "
	    "arena_ind=%u)\n", __func__, chunk, size, offset, length,
	    arena_ind);
	err = old_hooks.commit(chunk, size, offset, length, arena_ind);
	did_commit = !err;
	return (err);
}

bool
chunk_decommit(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(chunk=%p, size=%zu, offset=%zu, length=%zu, "
	    "arena_ind=%u)\n", __func__, chunk, size, offset, length,
	    arena_ind);
	if (!do_decommit)
		return (true);
	err = old_hooks.decommit(chunk, size, offset, length, arena_ind);
	did_decommit = !err;
	return (err);
}

bool
chunk_purge(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	TRACE_HOOK("%s(chunk=%p, size=%zu, offset=%zu, length=%zu "
	    "arena_ind=%u)\n", __func__, chunk, size, offset, length,
	    arena_ind);
	did_purge = true;
	return (old_hooks.purge(chunk, size, offset, length, arena_ind));
}

bool
chunk_split(void *chunk, size_t size, size_t size_a, size_t size_b,
    bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(chunk=%p, size=%zu, size_a=%zu, size_b=%zu, "
	    "committed=%s, arena_ind=%u)\n", __func__, chunk, size, size_a,
	    size_b, committed ? "true" : "false", arena_ind);
	did_split = true;
	return (old_hooks.split(chunk, size, size_a, size_b, committed,
	    arena_ind));
}

bool
chunk_merge(void *chunk_a, size_t size_a, void *chunk_b, size_t size_b,
    bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(chunk_a=%p, size_a=%zu, chunk_b=%p size_b=%zu, "
	    "committed=%s, arena_ind=%u)\n", __func__, chunk_a, size_a, chunk_b,
	    size_b, committed ? "true" : "false", arena_ind);
	did_merge = true;
	return (old_hooks.merge(chunk_a, size_a, chunk_b, size_b,
	    committed, arena_ind));
}

TEST_BEGIN(test_chunk)
{
	void *p;
	size_t old_size, new_size, large0, large1, huge0, huge1, huge2, sz;
	unsigned arena_ind;
	int flags;
	size_t hooks_mib[3], purge_mib[3];
	size_t hooks_miblen, purge_miblen;
	chunk_hooks_t new_hooks = {
		chunk_alloc,
		chunk_dalloc,
		chunk_commit,
		chunk_decommit,
		chunk_purge,
		chunk_split,
		chunk_merge
	};
	bool xallocx_success_a, xallocx_success_b, xallocx_success_c;

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.extend", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/* Install custom chunk hooks. */
	hooks_miblen = sizeof(hooks_mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.chunk_hooks", hooks_mib,
	    &hooks_miblen), 0, "Unexpected mallctlnametomib() failure");
	hooks_mib[1] = (size_t)arena_ind;
	old_size = sizeof(chunk_hooks_t);
	new_size = sizeof(chunk_hooks_t);
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, (void *)&old_hooks,
	    &old_size, (void *)&new_hooks, new_size), 0,
	    "Unexpected chunk_hooks error");
	orig_hooks = old_hooks;
	assert_ptr_ne(old_hooks.alloc, chunk_alloc, "Unexpected alloc error");
	assert_ptr_ne(old_hooks.dalloc, chunk_dalloc,
	    "Unexpected dalloc error");
	assert_ptr_ne(old_hooks.commit, chunk_commit,
	    "Unexpected commit error");
	assert_ptr_ne(old_hooks.decommit, chunk_decommit,
	    "Unexpected decommit error");
	assert_ptr_ne(old_hooks.purge, chunk_purge, "Unexpected purge error");
	assert_ptr_ne(old_hooks.split, chunk_split, "Unexpected split error");
	assert_ptr_ne(old_hooks.merge, chunk_merge, "Unexpected merge error");

	/* Get large size classes. */
	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.lrun.0.size", (void *)&large0, &sz, NULL,
	    0), 0, "Unexpected arenas.lrun.0.size failure");
	assert_d_eq(mallctl("arenas.lrun.1.size", (void *)&large1, &sz, NULL,
	    0), 0, "Unexpected arenas.lrun.1.size failure");

	/* Get huge size classes. */
	assert_d_eq(mallctl("arenas.hchunk.0.size", (void *)&huge0, &sz, NULL,
	    0), 0, "Unexpected arenas.hchunk.0.size failure");
	assert_d_eq(mallctl("arenas.hchunk.1.size", (void *)&huge1, &sz, NULL,
	    0), 0, "Unexpected arenas.hchunk.1.size failure");
	assert_d_eq(mallctl("arenas.hchunk.2.size", (void *)&huge2, &sz, NULL,
	    0), 0, "Unexpected arenas.hchunk.2.size failure");

	/* Test dalloc/decommit/purge cascade. */
	purge_miblen = sizeof(purge_mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.purge", purge_mib, &purge_miblen),
	    0, "Unexpected mallctlnametomib() failure");
	purge_mib[1] = (size_t)arena_ind;
	do_dalloc = false;
	do_decommit = false;
	p = mallocx(huge0 * 2, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	did_dalloc = false;
	did_decommit = false;
	did_purge = false;
	did_split = false;
	xallocx_success_a = (xallocx(p, huge0, 0, flags) == huge0);
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_a) {
		assert_true(did_dalloc, "Expected dalloc");
		assert_false(did_decommit, "Unexpected decommit");
		assert_true(did_purge, "Expected purge");
	}
	assert_true(did_split, "Expected split");
	dallocx(p, flags);
	do_dalloc = true;

	/* Test decommit/commit and observe split/merge. */
	do_dalloc = false;
	do_decommit = true;
	p = mallocx(huge0 * 2, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	did_decommit = false;
	did_commit = false;
	did_split = false;
	did_merge = false;
	xallocx_success_b = (xallocx(p, huge0, 0, flags) == huge0);
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_b)
		assert_true(did_split, "Expected split");
	xallocx_success_c = (xallocx(p, huge0 * 2, 0, flags) == huge0 * 2);
	assert_b_eq(did_decommit, did_commit, "Expected decommit/commit match");
	if (xallocx_success_b && xallocx_success_c)
		assert_true(did_merge, "Expected merge");
	dallocx(p, flags);
	do_dalloc = true;
	do_decommit = false;

	/* Test purge for partial-chunk huge allocations. */
	if (huge0 * 2 > huge2) {
		/*
		 * There are at least four size classes per doubling, so a
		 * successful xallocx() from size=huge2 to size=huge1 is
		 * guaranteed to leave trailing purgeable memory.
		 */
		p = mallocx(huge2, flags);
		assert_ptr_not_null(p, "Unexpected mallocx() error");
		did_purge = false;
		assert_zu_eq(xallocx(p, huge1, 0, flags), huge1,
		    "Unexpected xallocx() failure");
		assert_true(did_purge, "Expected purge");
		dallocx(p, flags);
	}

	/* Test decommit for large allocations. */
	do_decommit = true;
	p = mallocx(large1, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	did_decommit = false;
	assert_zu_eq(xallocx(p, large0, 0, flags), large0,
	    "Unexpected xallocx() failure");
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	did_commit = false;
	assert_zu_eq(xallocx(p, large1, 0, flags), large1,
	    "Unexpected xallocx() failure");
	assert_b_eq(did_decommit, did_commit, "Expected decommit/commit match");
	dallocx(p, flags);
	do_decommit = false;

	/* Make sure non-huge allocation succeeds. */
	p = mallocx(42, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	dallocx(p, flags);

	/* Restore chunk hooks. */
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, NULL, NULL,
	    (void *)&old_hooks, new_size), 0, "Unexpected chunk_hooks error");
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, (void *)&old_hooks,
	    &old_size, NULL, 0), 0, "Unexpected chunk_hooks error");
	assert_ptr_eq(old_hooks.alloc, orig_hooks.alloc,
	    "Unexpected alloc error");
	assert_ptr_eq(old_hooks.dalloc, orig_hooks.dalloc,
	    "Unexpected dalloc error");
	assert_ptr_eq(old_hooks.commit, orig_hooks.commit,
	    "Unexpected commit error");
	assert_ptr_eq(old_hooks.decommit, orig_hooks.decommit,
	    "Unexpected decommit error");
	assert_ptr_eq(old_hooks.purge, orig_hooks.purge,
	    "Unexpected purge error");
	assert_ptr_eq(old_hooks.split, orig_hooks.split,
	    "Unexpected split error");
	assert_ptr_eq(old_hooks.merge, orig_hooks.merge,
	    "Unexpected merge error");
}
TEST_END

int
main(void)
{

	return (test(test_chunk));
}
