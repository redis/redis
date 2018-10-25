/*
 * Boilerplate code used for testing extent hooks via interception and
 * passthrough.
 */

static void	*extent_alloc_hook(extent_hooks_t *extent_hooks, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind);
static bool	extent_dalloc_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, bool committed, unsigned arena_ind);
static void	extent_destroy_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, bool committed, unsigned arena_ind);
static bool	extent_commit_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_decommit_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_purge_lazy_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_purge_forced_hook(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_split_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t size_a, size_t size_b, bool committed,
    unsigned arena_ind);
static bool	extent_merge_hook(extent_hooks_t *extent_hooks, void *addr_a,
    size_t size_a, void *addr_b, size_t size_b, bool committed,
    unsigned arena_ind);

static extent_hooks_t *default_hooks;
static extent_hooks_t hooks = {
	extent_alloc_hook,
	extent_dalloc_hook,
	extent_destroy_hook,
	extent_commit_hook,
	extent_decommit_hook,
	extent_purge_lazy_hook,
	extent_purge_forced_hook,
	extent_split_hook,
	extent_merge_hook
};

/* Control whether hook functions pass calls through to default hooks. */
static bool try_alloc = true;
static bool try_dalloc = true;
static bool try_destroy = true;
static bool try_commit = true;
static bool try_decommit = true;
static bool try_purge_lazy = true;
static bool try_purge_forced = true;
static bool try_split = true;
static bool try_merge = true;

/* Set to false prior to operations, then introspect after operations. */
static bool called_alloc;
static bool called_dalloc;
static bool called_destroy;
static bool called_commit;
static bool called_decommit;
static bool called_purge_lazy;
static bool called_purge_forced;
static bool called_split;
static bool called_merge;

/* Set to false prior to operations, then introspect after operations. */
static bool did_alloc;
static bool did_dalloc;
static bool did_destroy;
static bool did_commit;
static bool did_decommit;
static bool did_purge_lazy;
static bool did_purge_forced;
static bool did_split;
static bool did_merge;

#if 0
#  define TRACE_HOOK(fmt, ...) malloc_printf(fmt, __VA_ARGS__)
#else
#  define TRACE_HOOK(fmt, ...)
#endif

static void *
extent_alloc_hook(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	void *ret;

	TRACE_HOOK("%s(extent_hooks=%p, new_addr=%p, size=%zu, alignment=%zu, "
	    "*zero=%s, *commit=%s, arena_ind=%u)\n", __func__, extent_hooks,
	    new_addr, size, alignment, *zero ?  "true" : "false", *commit ?
	    "true" : "false", arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->alloc, extent_alloc_hook,
	    "Wrong hook function");
	called_alloc = true;
	if (!try_alloc) {
		return NULL;
	}
	ret = default_hooks->alloc(default_hooks, new_addr, size, alignment,
	    zero, commit, 0);
	did_alloc = (ret != NULL);
	return ret;
}

static bool
extent_dalloc_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, committed=%s, "
	    "arena_ind=%u)\n", __func__, extent_hooks, addr, size, committed ?
	    "true" : "false", arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->dalloc, extent_dalloc_hook,
	    "Wrong hook function");
	called_dalloc = true;
	if (!try_dalloc) {
		return true;
	}
	err = default_hooks->dalloc(default_hooks, addr, size, committed, 0);
	did_dalloc = !err;
	return err;
}

static void
extent_destroy_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, committed=%s, "
	    "arena_ind=%u)\n", __func__, extent_hooks, addr, size, committed ?
	    "true" : "false", arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->destroy, extent_destroy_hook,
	    "Wrong hook function");
	called_destroy = true;
	if (!try_destroy) {
		return;
	}
	default_hooks->destroy(default_hooks, addr, size, committed, 0);
	did_destroy = true;
}

static bool
extent_commit_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu, arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->commit, extent_commit_hook,
	    "Wrong hook function");
	called_commit = true;
	if (!try_commit) {
		return true;
	}
	err = default_hooks->commit(default_hooks, addr, size, offset, length,
	    0);
	did_commit = !err;
	return err;
}

static bool
extent_decommit_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu, arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->decommit, extent_decommit_hook,
	    "Wrong hook function");
	called_decommit = true;
	if (!try_decommit) {
		return true;
	}
	err = default_hooks->decommit(default_hooks, addr, size, offset, length,
	    0);
	did_decommit = !err;
	return err;
}

static bool
extent_purge_lazy_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->purge_lazy, extent_purge_lazy_hook,
	    "Wrong hook function");
	called_purge_lazy = true;
	if (!try_purge_lazy) {
		return true;
	}
	err = default_hooks->purge_lazy == NULL ||
	    default_hooks->purge_lazy(default_hooks, addr, size, offset, length,
	    0);
	did_purge_lazy = !err;
	return err;
}

static bool
extent_purge_forced_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->purge_forced, extent_purge_forced_hook,
	    "Wrong hook function");
	called_purge_forced = true;
	if (!try_purge_forced) {
		return true;
	}
	err = default_hooks->purge_forced == NULL ||
	    default_hooks->purge_forced(default_hooks, addr, size, offset,
	    length, 0);
	did_purge_forced = !err;
	return err;
}

static bool
extent_split_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, size_a=%zu, "
	    "size_b=%zu, committed=%s, arena_ind=%u)\n", __func__, extent_hooks,
	    addr, size, size_a, size_b, committed ? "true" : "false",
	    arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->split, extent_split_hook,
	    "Wrong hook function");
	called_split = true;
	if (!try_split) {
		return true;
	}
	err = (default_hooks->split == NULL ||
	    default_hooks->split(default_hooks, addr, size, size_a, size_b,
	    committed, 0));
	did_split = !err;
	return err;
}

static bool
extent_merge_hook(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr_a=%p, size_a=%zu, addr_b=%p "
	    "size_b=%zu, committed=%s, arena_ind=%u)\n", __func__, extent_hooks,
	    addr_a, size_a, addr_b, size_b, committed ? "true" : "false",
	    arena_ind);
	assert_ptr_eq(extent_hooks, &hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->merge, extent_merge_hook,
	    "Wrong hook function");
	assert_ptr_eq((void *)((uintptr_t)addr_a + size_a), addr_b,
	    "Extents not mergeable");
	called_merge = true;
	if (!try_merge) {
		return true;
	}
	err = (default_hooks->merge == NULL ||
	    default_hooks->merge(default_hooks, addr_a, size_a, addr_b, size_b,
	    committed, 0));
	did_merge = !err;
	return err;
}

static void
extent_hooks_prep(void) {
	size_t sz;

	sz = sizeof(default_hooks);
	assert_d_eq(mallctl("arena.0.extent_hooks", (void *)&default_hooks, &sz,
	    NULL, 0), 0, "Unexpected mallctl() error");
}
