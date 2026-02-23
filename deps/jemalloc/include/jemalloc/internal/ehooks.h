#ifndef JEMALLOC_INTERNAL_EHOOKS_H
#define JEMALLOC_INTERNAL_EHOOKS_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/extent_mmap.h"

/*
 * This module is the internal interface to the extent hooks (both
 * user-specified and external).  Eventually, this will give us the flexibility
 * to use multiple different versions of user-visible extent-hook APIs under a
 * single user interface.
 *
 * Current API expansions (not available to anyone but the default hooks yet):
 *   - Head state tracking.  Hooks can decide whether or not to merge two
 *     extents based on whether or not one of them is the head (i.e. was
 *     allocated on its own).  The later extent loses its "head" status.
 */

extern const extent_hooks_t ehooks_default_extent_hooks;

typedef struct ehooks_s ehooks_t;
struct ehooks_s {
	/*
	 * The user-visible id that goes with the ehooks (i.e. that of the base
	 * they're a part of, the associated arena's index within the arenas
	 * array).
	 */
	unsigned ind;
	/* Logically an extent_hooks_t *. */
	atomic_p_t ptr;
};

extern const extent_hooks_t ehooks_default_extent_hooks;

/*
 * These are not really part of the public API.  Each hook has a fast-path for
 * the default-hooks case that can avoid various small inefficiencies:
 *   - Forgetting tsd and then calling tsd_get within the hook.
 *   - Getting more state than necessary out of the extent_t.
 *   - Doing arena_ind -> arena -> arena_ind lookups.
 * By making the calls to these functions visible to the compiler, it can move
 * those extra bits of computation down below the fast-paths where they get ignored.
 */
void *ehooks_default_alloc_impl(tsdn_t *tsdn, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind);
bool ehooks_default_dalloc_impl(void *addr, size_t size);
void ehooks_default_destroy_impl(void *addr, size_t size);
bool ehooks_default_commit_impl(void *addr, size_t offset, size_t length);
bool ehooks_default_decommit_impl(void *addr, size_t offset, size_t length);
#ifdef PAGES_CAN_PURGE_LAZY
bool ehooks_default_purge_lazy_impl(void *addr, size_t offset, size_t length);
#endif
#ifdef PAGES_CAN_PURGE_FORCED
bool ehooks_default_purge_forced_impl(void *addr, size_t offset, size_t length);
#endif
bool ehooks_default_split_impl();
/*
 * Merge is the only default extent hook we declare -- see the comment in
 * ehooks_merge.
 */
bool ehooks_default_merge(extent_hooks_t *extent_hooks, void *addr_a,
    size_t size_a, void *addr_b, size_t size_b, bool committed,
    unsigned arena_ind);
bool ehooks_default_merge_impl(tsdn_t *tsdn, void *addr_a, void *addr_b);
void ehooks_default_zero_impl(void *addr, size_t size);
void ehooks_default_guard_impl(void *guard1, void *guard2);
void ehooks_default_unguard_impl(void *guard1, void *guard2);

/*
 * We don't officially support reentrancy from wtihin the extent hooks.  But
 * various people who sit within throwing distance of the jemalloc team want
 * that functionality in certain limited cases.  The default reentrancy guards
 * assert that we're not reentrant from a0 (since it's the bootstrap arena,
 * where reentrant allocations would be redirected), which we would incorrectly
 * trigger in cases where a0 has extent hooks (those hooks themselves can't be
 * reentrant, then, but there are reasonable uses for such functionality, like
 * putting internal metadata on hugepages).  Therefore, we use the raw
 * reentrancy guards.
 *
 * Eventually, we need to think more carefully about whether and where we
 * support allocating from within extent hooks (and what that means for things
 * like profiling, stats collection, etc.), and document what the guarantee is.
 */
static inline void
ehooks_pre_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_pre_reentrancy_raw(tsd);
}

static inline void
ehooks_post_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_post_reentrancy_raw(tsd);
}

/* Beginning of the public API. */
void ehooks_init(ehooks_t *ehooks, extent_hooks_t *extent_hooks, unsigned ind);

static inline unsigned
ehooks_ind_get(const ehooks_t *ehooks) {
	return ehooks->ind;
}

static inline void
ehooks_set_extent_hooks_ptr(ehooks_t *ehooks, extent_hooks_t *extent_hooks) {
	atomic_store_p(&ehooks->ptr, extent_hooks, ATOMIC_RELEASE);
}

static inline extent_hooks_t *
ehooks_get_extent_hooks_ptr(ehooks_t *ehooks) {
	return (extent_hooks_t *)atomic_load_p(&ehooks->ptr, ATOMIC_ACQUIRE);
}

static inline bool
ehooks_are_default(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks) ==
	    &ehooks_default_extent_hooks;
}

/*
 * In some cases, a caller needs to allocate resources before attempting to call
 * a hook.  If that hook is doomed to fail, this is wasteful.  We therefore
 * include some checks for such cases.
 */
static inline bool
ehooks_dalloc_will_fail(ehooks_t *ehooks) {
	if (ehooks_are_default(ehooks)) {
		return opt_retain;
	} else {
		return ehooks_get_extent_hooks_ptr(ehooks)->dalloc == NULL;
	}
}

static inline bool
ehooks_split_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->split == NULL;
}

static inline bool
ehooks_merge_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->merge == NULL;
}

static inline bool
ehooks_guard_will_fail(ehooks_t *ehooks) {
	/*
	 * Before the guard hooks are officially introduced, limit the use to
	 * the default hooks only.
	 */
	return !ehooks_are_default(ehooks);
}

/*
 * Some hooks are required to return zeroed memory in certain situations.  In
 * debug mode, we do some heuristic checks that they did what they were supposed
 * to.
 *
 * This isn't really ehooks-specific (i.e. anyone can check for zeroed memory).
 * But incorrect zero information indicates an ehook bug.
 */
static inline void
ehooks_debug_zero_check(void *addr, size_t size) {
	assert(((uintptr_t)addr & PAGE_MASK) == 0);
	assert((size & PAGE_MASK) == 0);
	assert(size > 0);
	if (config_debug) {
		/* Check the whole first page. */
		size_t *p = (size_t *)addr;
		for (size_t i = 0; i < PAGE / sizeof(size_t); i++) {
			assert(p[i] == 0);
		}
		/*
		 * And 4 spots within.  There's a tradeoff here; the larger
		 * this number, the more likely it is that we'll catch a bug
		 * where ehooks return a sparsely non-zero range.  But
		 * increasing the number of checks also increases the number of
		 * page faults in debug mode.  FreeBSD does much of their
		 * day-to-day development work in debug mode, so we don't want
		 * even the debug builds to be too slow.
		 */
		const size_t nchecks = 4;
		assert(PAGE >= sizeof(size_t) * nchecks);
		for (size_t i = 0; i < nchecks; ++i) {
			assert(p[i * (size / sizeof(size_t) / nchecks)] == 0);
		}
	}
}


static inline void *
ehooks_alloc(tsdn_t *tsdn, ehooks_t *ehooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit) {
	bool orig_zero = *zero;
	void *ret;
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		ret = ehooks_default_alloc_impl(tsdn, new_addr, size,
		    alignment, zero, commit, ehooks_ind_get(ehooks));
	} else {
		ehooks_pre_reentrancy(tsdn);
		ret = extent_hooks->alloc(extent_hooks, new_addr, size,
		    alignment, zero, commit, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
	}
	assert(new_addr == NULL || ret == NULL || new_addr == ret);
	assert(!orig_zero || *zero);
	if (*zero && ret != NULL) {
		ehooks_debug_zero_check(ret, size);
	}
	return ret;
}

static inline bool
ehooks_dalloc(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    bool committed) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_dalloc_impl(addr, size);
	} else if (extent_hooks->dalloc == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->dalloc(extent_hooks, addr, size,
		    committed, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline void
ehooks_destroy(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    bool committed) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		ehooks_default_destroy_impl(addr, size);
	} else if (extent_hooks->destroy == NULL) {
		/* Do nothing. */
	} else {
		ehooks_pre_reentrancy(tsdn);
		extent_hooks->destroy(extent_hooks, addr, size, committed,
		    ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
	}
}

static inline bool
ehooks_commit(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	bool err;
	if (extent_hooks == &ehooks_default_extent_hooks) {
		err = ehooks_default_commit_impl(addr, offset, length);
	} else if (extent_hooks->commit == NULL) {
		err = true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		err = extent_hooks->commit(extent_hooks, addr, size,
		    offset, length, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
	}
	if (!err) {
		ehooks_debug_zero_check(addr, size);
	}
	return err;
}

static inline bool
ehooks_decommit(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_decommit_impl(addr, offset, length);
	} else if (extent_hooks->decommit == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->decommit(extent_hooks, addr, size,
		    offset, length, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_purge_lazy(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
#ifdef PAGES_CAN_PURGE_LAZY
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_purge_lazy_impl(addr, offset, length);
	}
#endif
	if (extent_hooks->purge_lazy == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->purge_lazy(extent_hooks, addr, size,
		    offset, length, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_purge_forced(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	/*
	 * It would be correct to have a ehooks_debug_zero_check call at the end
	 * of this function; purge_forced is required to zero.  But checking
	 * would touch the page in question, which may have performance
	 * consequences (imagine the hooks are using hugepages, with a global
	 * zero page off).  Even in debug mode, it's usually a good idea to
	 * avoid cases that can dramatically increase memory consumption.
	 */
#ifdef PAGES_CAN_PURGE_FORCED
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_purge_forced_impl(addr, offset, length);
	}
#endif
	if (extent_hooks->purge_forced == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->purge_forced(extent_hooks, addr, size,
		    offset, length, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_split(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (ehooks_are_default(ehooks)) {
		return ehooks_default_split_impl();
	} else if (extent_hooks->split == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->split(extent_hooks, addr, size, size_a,
		    size_b, committed, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_merge(tsdn_t *tsdn, ehooks_t *ehooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_merge_impl(tsdn, addr_a, addr_b);
	} else if (extent_hooks->merge == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->merge(extent_hooks, addr_a, size_a,
		    addr_b, size_b, committed, ehooks_ind_get(ehooks));
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline void
ehooks_zero(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		ehooks_default_zero_impl(addr, size);
	} else {
		/*
		 * It would be correct to try using the user-provided purge
		 * hooks (since they are required to have zeroed the extent if
		 * they indicate success), but we don't necessarily know their
		 * cost.  We'll be conservative and use memset.
		 */
		memset(addr, 0, size);
	}
}

static inline bool
ehooks_guard(tsdn_t *tsdn, ehooks_t *ehooks, void *guard1, void *guard2) {
	bool err;
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);

	if (extent_hooks == &ehooks_default_extent_hooks) {
		ehooks_default_guard_impl(guard1, guard2);
		err = false;
	} else {
		err = true;
	}

	return err;
}

static inline bool
ehooks_unguard(tsdn_t *tsdn, ehooks_t *ehooks, void *guard1, void *guard2) {
	bool err;
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);

	if (extent_hooks == &ehooks_default_extent_hooks) {
		ehooks_default_unguard_impl(guard1, guard2);
		err = false;
	} else {
		err = true;
	}

	return err;
}

#endif /* JEMALLOC_INTERNAL_EHOOKS_H */
