#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prof_recent.h"
#include "jemalloc/internal/util.h"

/******************************************************************************/

void *
large_malloc(tsdn_t *tsdn, arena_t *arena, size_t usize, bool zero) {
	assert(usize == sz_s2u(usize));

	return large_palloc(tsdn, arena, usize, CACHELINE, zero);
}

void *
large_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero) {
	size_t ausize;
	edata_t *edata;
	UNUSED bool idump JEMALLOC_CC_SILENCE_INIT(false);

	assert(!tsdn_null(tsdn) || arena != NULL);

	ausize = sz_sa2u(usize, alignment);
	if (unlikely(ausize == 0 || ausize > SC_LARGE_MAXCLASS)) {
		return NULL;
	}

	if (likely(!tsdn_null(tsdn))) {
		arena = arena_choose_maybe_huge(tsdn_tsd(tsdn), arena, usize);
	}
	if (unlikely(arena == NULL) || (edata = arena_extent_alloc_large(tsdn,
	    arena, usize, alignment, zero)) == NULL) {
		return NULL;
	}

	/* See comments in arena_bin_slabs_full_insert(). */
	if (!arena_is_auto(arena)) {
		/* Insert edata into large. */
		malloc_mutex_lock(tsdn, &arena->large_mtx);
		edata_list_active_append(&arena->large, edata);
		malloc_mutex_unlock(tsdn, &arena->large_mtx);
	}

	arena_decay_tick(tsdn, arena);
	return edata_addr_get(edata);
}

static bool
large_ralloc_no_move_shrink(tsdn_t *tsdn, edata_t *edata, size_t usize) {
	arena_t *arena = arena_get_from_edata(edata);
	ehooks_t *ehooks = arena_get_ehooks(arena);
	size_t old_size = edata_size_get(edata);
	size_t old_usize = edata_usize_get(edata);

	assert(old_usize > usize);

	if (ehooks_split_will_fail(ehooks)) {
		return true;
	}

	bool deferred_work_generated = false;
	bool err = pa_shrink(tsdn, &arena->pa_shard, edata, old_size,
	    usize + sz_large_pad, sz_size2index(usize),
	    &deferred_work_generated);
	if (err) {
		return true;
	}
	if (deferred_work_generated) {
		arena_handle_deferred_work(tsdn, arena);
	}
	arena_extent_ralloc_large_shrink(tsdn, arena, edata, old_usize);

	return false;
}

static bool
large_ralloc_no_move_expand(tsdn_t *tsdn, edata_t *edata, size_t usize,
    bool zero) {
	arena_t *arena = arena_get_from_edata(edata);

	size_t old_size = edata_size_get(edata);
	size_t old_usize = edata_usize_get(edata);
	size_t new_size = usize + sz_large_pad;

	szind_t szind = sz_size2index(usize);

	bool deferred_work_generated = false;
	bool err = pa_expand(tsdn, &arena->pa_shard, edata, old_size, new_size,
	    szind, zero, &deferred_work_generated);

	if (deferred_work_generated) {
		arena_handle_deferred_work(tsdn, arena);
	}

	if (err) {
		return true;
	}

	if (zero) {
		if (opt_cache_oblivious) {
			assert(sz_large_pad == PAGE);
			/*
			 * Zero the trailing bytes of the original allocation's
			 * last page, since they are in an indeterminate state.
			 * There will always be trailing bytes, because ptr's
			 * offset from the beginning of the extent is a multiple
			 * of CACHELINE in [0 .. PAGE).
			 */
			void *zbase = (void *)
			    ((uintptr_t)edata_addr_get(edata) + old_usize);
			void *zpast = PAGE_ADDR2BASE((void *)((uintptr_t)zbase +
			    PAGE));
			size_t nzero = (uintptr_t)zpast - (uintptr_t)zbase;
			assert(nzero > 0);
			memset(zbase, 0, nzero);
		}
	}
	arena_extent_ralloc_large_expand(tsdn, arena, edata, old_usize);

	return false;
}

bool
large_ralloc_no_move(tsdn_t *tsdn, edata_t *edata, size_t usize_min,
    size_t usize_max, bool zero) {
	size_t oldusize = edata_usize_get(edata);

	/* The following should have been caught by callers. */
	assert(usize_min > 0 && usize_max <= SC_LARGE_MAXCLASS);
	/* Both allocation sizes must be large to avoid a move. */
	assert(oldusize >= SC_LARGE_MINCLASS
	    && usize_max >= SC_LARGE_MINCLASS);

	if (usize_max > oldusize) {
		/* Attempt to expand the allocation in-place. */
		if (!large_ralloc_no_move_expand(tsdn, edata, usize_max,
		    zero)) {
			arena_decay_tick(tsdn, arena_get_from_edata(edata));
			return false;
		}
		/* Try again, this time with usize_min. */
		if (usize_min < usize_max && usize_min > oldusize &&
		    large_ralloc_no_move_expand(tsdn, edata, usize_min, zero)) {
			arena_decay_tick(tsdn, arena_get_from_edata(edata));
			return false;
		}
	}

	/*
	 * Avoid moving the allocation if the existing extent size accommodates
	 * the new size.
	 */
	if (oldusize >= usize_min && oldusize <= usize_max) {
		arena_decay_tick(tsdn, arena_get_from_edata(edata));
		return false;
	}

	/* Attempt to shrink the allocation in-place. */
	if (oldusize > usize_max) {
		if (!large_ralloc_no_move_shrink(tsdn, edata, usize_max)) {
			arena_decay_tick(tsdn, arena_get_from_edata(edata));
			return false;
		}
	}
	return true;
}

static void *
large_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero) {
	if (alignment <= CACHELINE) {
		return large_malloc(tsdn, arena, usize, zero);
	}
	return large_palloc(tsdn, arena, usize, alignment, zero);
}

void *
large_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache,
    hook_ralloc_args_t *hook_args) {
	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);

	size_t oldusize = edata_usize_get(edata);
	/* The following should have been caught by callers. */
	assert(usize > 0 && usize <= SC_LARGE_MAXCLASS);
	/* Both allocation sizes must be large to avoid a move. */
	assert(oldusize >= SC_LARGE_MINCLASS
	    && usize >= SC_LARGE_MINCLASS);

	/* Try to avoid moving the allocation. */
	if (!large_ralloc_no_move(tsdn, edata, usize, usize, zero)) {
		hook_invoke_expand(hook_args->is_realloc
		    ? hook_expand_realloc : hook_expand_rallocx, ptr, oldusize,
		    usize, (uintptr_t)ptr, hook_args->args);
		return edata_addr_get(edata);
	}

	/*
	 * usize and old size are different enough that we need to use a
	 * different size class.  In that case, fall back to allocating new
	 * space and copying.
	 */
	void *ret = large_ralloc_move_helper(tsdn, arena, usize, alignment,
	    zero);
	if (ret == NULL) {
		return NULL;
	}

	hook_invoke_alloc(hook_args->is_realloc
	    ? hook_alloc_realloc : hook_alloc_rallocx, ret, (uintptr_t)ret,
	    hook_args->args);
	hook_invoke_dalloc(hook_args->is_realloc
	    ? hook_dalloc_realloc : hook_dalloc_rallocx, ptr, hook_args->args);

	size_t copysize = (usize < oldusize) ? usize : oldusize;
	memcpy(ret, edata_addr_get(edata), copysize);
	isdalloct(tsdn, edata_addr_get(edata), oldusize, tcache, NULL, true);
	return ret;
}

/*
 * locked indicates whether the arena's large_mtx is currently held.
 */
static void
large_dalloc_prep_impl(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    bool locked) {
	if (!locked) {
		/* See comments in arena_bin_slabs_full_insert(). */
		if (!arena_is_auto(arena)) {
			malloc_mutex_lock(tsdn, &arena->large_mtx);
			edata_list_active_remove(&arena->large, edata);
			malloc_mutex_unlock(tsdn, &arena->large_mtx);
		}
	} else {
		/* Only hold the large_mtx if necessary. */
		if (!arena_is_auto(arena)) {
			malloc_mutex_assert_owner(tsdn, &arena->large_mtx);
			edata_list_active_remove(&arena->large, edata);
		}
	}
	arena_extent_dalloc_large_prep(tsdn, arena, edata);
}

static void
large_dalloc_finish_impl(tsdn_t *tsdn, arena_t *arena, edata_t *edata) {
	bool deferred_work_generated = false;
	pa_dalloc(tsdn, &arena->pa_shard, edata, &deferred_work_generated);
	if (deferred_work_generated) {
		arena_handle_deferred_work(tsdn, arena);
	}
}

void
large_dalloc_prep_locked(tsdn_t *tsdn, edata_t *edata) {
	large_dalloc_prep_impl(tsdn, arena_get_from_edata(edata), edata, true);
}

void
large_dalloc_finish(tsdn_t *tsdn, edata_t *edata) {
	large_dalloc_finish_impl(tsdn, arena_get_from_edata(edata), edata);
}

void
large_dalloc(tsdn_t *tsdn, edata_t *edata) {
	arena_t *arena = arena_get_from_edata(edata);
	large_dalloc_prep_impl(tsdn, arena, edata, false);
	large_dalloc_finish_impl(tsdn, arena, edata);
	arena_decay_tick(tsdn, arena);
}

size_t
large_salloc(tsdn_t *tsdn, const edata_t *edata) {
	return edata_usize_get(edata);
}

void
large_prof_info_get(tsd_t *tsd, edata_t *edata, prof_info_t *prof_info,
    bool reset_recent) {
	assert(prof_info != NULL);

	prof_tctx_t *alloc_tctx = edata_prof_tctx_get(edata);
	prof_info->alloc_tctx = alloc_tctx;

	if ((uintptr_t)alloc_tctx > (uintptr_t)1U) {
		nstime_copy(&prof_info->alloc_time,
		    edata_prof_alloc_time_get(edata));
		prof_info->alloc_size = edata_prof_alloc_size_get(edata);
		if (reset_recent) {
			/*
			 * Reset the pointer on the recent allocation record,
			 * so that this allocation is recorded as released.
			 */
			prof_recent_alloc_reset(tsd, edata);
		}
	}
}

static void
large_prof_tctx_set(edata_t *edata, prof_tctx_t *tctx) {
	edata_prof_tctx_set(edata, tctx);
}

void
large_prof_tctx_reset(edata_t *edata) {
	large_prof_tctx_set(edata, (prof_tctx_t *)(uintptr_t)1U);
}

void
large_prof_info_set(edata_t *edata, prof_tctx_t *tctx, size_t size) {
	nstime_t t;
	nstime_prof_init_update(&t);
	edata_prof_alloc_time_set(edata, &t);
	edata_prof_alloc_size_set(edata, size);
	edata_prof_recent_alloc_init(edata);
	large_prof_tctx_set(edata, tctx);
}
