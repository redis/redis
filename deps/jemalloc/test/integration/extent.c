#include "test/jemalloc_test.h"

#include "test/extent_hooks.h"

#include "jemalloc/internal/arena_types.h"

static void
test_extent_body(unsigned arena_ind) {
	void *p;
	size_t large0, large1, large2, sz;
	size_t purge_mib[3];
	size_t purge_miblen;
	int flags;
	bool xallocx_success_a, xallocx_success_b, xallocx_success_c;

	flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/* Get large size classes. */
	sz = sizeof(size_t);
	expect_d_eq(mallctl("arenas.lextent.0.size", (void *)&large0, &sz, NULL,
	    0), 0, "Unexpected arenas.lextent.0.size failure");
	expect_d_eq(mallctl("arenas.lextent.1.size", (void *)&large1, &sz, NULL,
	    0), 0, "Unexpected arenas.lextent.1.size failure");
	expect_d_eq(mallctl("arenas.lextent.2.size", (void *)&large2, &sz, NULL,
	    0), 0, "Unexpected arenas.lextent.2.size failure");

	/* Test dalloc/decommit/purge cascade. */
	purge_miblen = sizeof(purge_mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.purge", purge_mib, &purge_miblen),
	    0, "Unexpected mallctlnametomib() failure");
	purge_mib[1] = (size_t)arena_ind;
	called_alloc = false;
	try_alloc = true;
	try_dalloc = false;
	try_decommit = false;
	p = mallocx(large0 * 2, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	expect_true(called_alloc, "Expected alloc call");
	called_dalloc = false;
	called_decommit = false;
	did_purge_lazy = false;
	did_purge_forced = false;
	called_split = false;
	xallocx_success_a = (xallocx(p, large0, 0, flags) == large0);
	expect_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_a) {
		expect_true(called_dalloc, "Expected dalloc call");
		expect_true(called_decommit, "Expected decommit call");
		expect_true(did_purge_lazy || did_purge_forced,
		    "Expected purge");
		expect_true(called_split, "Expected split call");
	}
	dallocx(p, flags);
	try_dalloc = true;

	/* Test decommit/commit and observe split/merge. */
	try_dalloc = false;
	try_decommit = true;
	p = mallocx(large0 * 2, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	did_decommit = false;
	did_commit = false;
	called_split = false;
	did_split = false;
	did_merge = false;
	xallocx_success_b = (xallocx(p, large0, 0, flags) == large0);
	expect_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_b) {
		expect_true(did_split, "Expected split");
	}
	xallocx_success_c = (xallocx(p, large0 * 2, 0, flags) == large0 * 2);
	if (did_split) {
		expect_b_eq(did_decommit, did_commit,
		    "Expected decommit/commit match");
	}
	if (xallocx_success_b && xallocx_success_c) {
		expect_true(did_merge, "Expected merge");
	}
	dallocx(p, flags);
	try_dalloc = true;
	try_decommit = false;

	/* Make sure non-large allocation succeeds. */
	p = mallocx(42, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() error");
	dallocx(p, flags);
}

static void
test_manual_hook_auto_arena(void) {
	unsigned narenas;
	size_t old_size, new_size, sz;
	size_t hooks_mib[3];
	size_t hooks_miblen;
	extent_hooks_t *new_hooks, *old_hooks;

	extent_hooks_prep();

	sz = sizeof(unsigned);
	/* Get number of auto arenas. */
	expect_d_eq(mallctl("opt.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	if (narenas == 1) {
		return;
	}

	/* Install custom extent hooks on arena 1 (might not be initialized). */
	hooks_miblen = sizeof(hooks_mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.extent_hooks", hooks_mib,
	    &hooks_miblen), 0, "Unexpected mallctlnametomib() failure");
	hooks_mib[1] = 1;
	old_size = sizeof(extent_hooks_t *);
	new_hooks = &hooks;
	new_size = sizeof(extent_hooks_t *);
	expect_d_eq(mallctlbymib(hooks_mib, hooks_miblen, (void *)&old_hooks,
	    &old_size, (void *)&new_hooks, new_size), 0,
	    "Unexpected extent_hooks error");
	static bool auto_arena_created = false;
	if (old_hooks != &hooks) {
		expect_b_eq(auto_arena_created, false,
		    "Expected auto arena 1 created only once.");
		auto_arena_created = true;
	}
}

static void
test_manual_hook_body(void) {
	unsigned arena_ind;
	size_t old_size, new_size, sz;
	size_t hooks_mib[3];
	size_t hooks_miblen;
	extent_hooks_t *new_hooks, *old_hooks;

	extent_hooks_prep();

	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	/* Install custom extent hooks. */
	hooks_miblen = sizeof(hooks_mib)/sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.extent_hooks", hooks_mib,
	    &hooks_miblen), 0, "Unexpected mallctlnametomib() failure");
	hooks_mib[1] = (size_t)arena_ind;
	old_size = sizeof(extent_hooks_t *);
	new_hooks = &hooks;
	new_size = sizeof(extent_hooks_t *);
	expect_d_eq(mallctlbymib(hooks_mib, hooks_miblen, (void *)&old_hooks,
	    &old_size, (void *)&new_hooks, new_size), 0,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->alloc, extent_alloc_hook,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->dalloc, extent_dalloc_hook,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->commit, extent_commit_hook,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->decommit, extent_decommit_hook,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->purge_lazy, extent_purge_lazy_hook,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->purge_forced, extent_purge_forced_hook,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->split, extent_split_hook,
	    "Unexpected extent_hooks error");
	expect_ptr_ne(old_hooks->merge, extent_merge_hook,
	    "Unexpected extent_hooks error");

	if (!is_background_thread_enabled()) {
		test_extent_body(arena_ind);
	}

	/* Restore extent hooks. */
	expect_d_eq(mallctlbymib(hooks_mib, hooks_miblen, NULL, NULL,
	    (void *)&old_hooks, new_size), 0, "Unexpected extent_hooks error");
	expect_d_eq(mallctlbymib(hooks_mib, hooks_miblen, (void *)&old_hooks,
	    &old_size, NULL, 0), 0, "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks, default_hooks, "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->alloc, default_hooks->alloc,
	    "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->dalloc, default_hooks->dalloc,
	    "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->commit, default_hooks->commit,
	    "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->decommit, default_hooks->decommit,
	    "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->purge_lazy, default_hooks->purge_lazy,
	    "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->purge_forced, default_hooks->purge_forced,
	    "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->split, default_hooks->split,
	    "Unexpected extent_hooks error");
	expect_ptr_eq(old_hooks->merge, default_hooks->merge,
	    "Unexpected extent_hooks error");
}

TEST_BEGIN(test_extent_manual_hook) {
	test_manual_hook_auto_arena();
	test_manual_hook_body();

	/* Test failure paths. */
	try_split = false;
	test_manual_hook_body();
	try_merge = false;
	test_manual_hook_body();
	try_purge_lazy = false;
	try_purge_forced = false;
	test_manual_hook_body();

	try_split = try_merge = try_purge_lazy = try_purge_forced = true;
}
TEST_END

TEST_BEGIN(test_extent_auto_hook) {
	unsigned arena_ind;
	size_t new_size, sz;
	extent_hooks_t *new_hooks;

	extent_hooks_prep();

	sz = sizeof(unsigned);
	new_hooks = &hooks;
	new_size = sizeof(extent_hooks_t *);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz,
	    (void *)&new_hooks, new_size), 0, "Unexpected mallctl() failure");

	test_skip_if(is_background_thread_enabled());
	test_extent_body(arena_ind);
}
TEST_END

static void
test_arenas_create_ext_base(arena_config_t config,
	bool expect_hook_data, bool expect_hook_metadata)
{
	unsigned arena, arena1;
	void *ptr;
	size_t sz = sizeof(unsigned);

	extent_hooks_prep();

	called_alloc = false;
	expect_d_eq(mallctl("experimental.arenas_create_ext",
	    (void *)&arena, &sz, &config, sizeof(arena_config_t)), 0,
	    "Unexpected mallctl() failure");
	expect_b_eq(called_alloc, expect_hook_metadata,
	    "expected hook metadata alloc mismatch");

	called_alloc = false;
	ptr = mallocx(42, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
	expect_b_eq(called_alloc, expect_hook_data,
	    "expected hook data alloc mismatch");

	expect_ptr_not_null(ptr, "Unexpected mallocx() failure");
	expect_d_eq(mallctl("arenas.lookup", &arena1, &sz, &ptr, sizeof(ptr)),
	    0, "Unexpected mallctl() failure");
	expect_u_eq(arena, arena1, "Unexpected arena index");
	dallocx(ptr, 0);
}

TEST_BEGIN(test_arenas_create_ext_with_ehooks_no_metadata) {
	arena_config_t config;
	config.extent_hooks = &hooks;
	config.metadata_use_hooks = false;

	test_arenas_create_ext_base(config, true, false);
}
TEST_END

TEST_BEGIN(test_arenas_create_ext_with_ehooks_with_metadata) {
	arena_config_t config;
	config.extent_hooks = &hooks;
	config.metadata_use_hooks = true;

	test_arenas_create_ext_base(config, true, true);
}
TEST_END

int
main(void) {
	return test(
	    test_extent_manual_hook,
	    test_extent_auto_hook,
	    test_arenas_create_ext_with_ehooks_no_metadata,
	    test_arenas_create_ext_with_ehooks_with_metadata);
}
