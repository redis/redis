#ifndef JEMALLOC_INTERNAL_INLINES_C_H
#define JEMALLOC_INTERNAL_INLINES_C_H

#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/log.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/thread_event.h"
#include "jemalloc/internal/witness.h"

/*
 * Translating the names of the 'i' functions:
 *   Abbreviations used in the first part of the function name (before
 *   alloc/dalloc) describe what that function accomplishes:
 *     a: arena (query)
 *     s: size (query, or sized deallocation)
 *     e: extent (query)
 *     p: aligned (allocates)
 *     vs: size (query, without knowing that the pointer is into the heap)
 *     r: rallocx implementation
 *     x: xallocx implementation
 *   Abbreviations used in the second part of the function name (after
 *   alloc/dalloc) describe the arguments it takes
 *     z: whether to return zeroed memory
 *     t: accepts a tcache_t * parameter
 *     m: accepts an arena_t * parameter
 */

JEMALLOC_ALWAYS_INLINE arena_t *
iaalloc(tsdn_t *tsdn, const void *ptr) {
	assert(ptr != NULL);

	return arena_aalloc(tsdn, ptr);
}

JEMALLOC_ALWAYS_INLINE size_t
isalloc(tsdn_t *tsdn, const void *ptr) {
	assert(ptr != NULL);

	return arena_salloc(tsdn, ptr);
}

JEMALLOC_ALWAYS_INLINE void *
iallocztm(tsdn_t *tsdn, size_t size, szind_t ind, bool zero, tcache_t *tcache,
    bool is_internal, arena_t *arena, bool slow_path) {
	void *ret;

	assert(!is_internal || tcache == NULL);
	assert(!is_internal || arena == NULL || arena_is_auto(arena));
	if (!tsdn_null(tsdn) && tsd_reentrancy_level_get(tsdn_tsd(tsdn)) == 0) {
		witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
		    WITNESS_RANK_CORE, 0);
	}

	ret = arena_malloc(tsdn, arena, size, ind, zero, tcache, slow_path);
	if (config_stats && is_internal && likely(ret != NULL)) {
		arena_internal_add(iaalloc(tsdn, ret), isalloc(tsdn, ret));
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
ialloc(tsd_t *tsd, size_t size, szind_t ind, bool zero, bool slow_path) {
	return iallocztm(tsd_tsdn(tsd), size, ind, zero, tcache_get(tsd), false,
	    NULL, slow_path);
}

JEMALLOC_ALWAYS_INLINE void *
ipallocztm(tsdn_t *tsdn, size_t usize, size_t alignment, bool zero,
    tcache_t *tcache, bool is_internal, arena_t *arena) {
	void *ret;

	assert(usize != 0);
	assert(usize == sz_sa2u(usize, alignment));
	assert(!is_internal || tcache == NULL);
	assert(!is_internal || arena == NULL || arena_is_auto(arena));
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	ret = arena_palloc(tsdn, arena, usize, alignment, zero, tcache);
	assert(ALIGNMENT_ADDR2BASE(ret, alignment) == ret);
	if (config_stats && is_internal && likely(ret != NULL)) {
		arena_internal_add(iaalloc(tsdn, ret), isalloc(tsdn, ret));
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
ipalloct(tsdn_t *tsdn, size_t usize, size_t alignment, bool zero,
    tcache_t *tcache, arena_t *arena) {
	return ipallocztm(tsdn, usize, alignment, zero, tcache, false, arena);
}

JEMALLOC_ALWAYS_INLINE void *
ipalloc(tsd_t *tsd, size_t usize, size_t alignment, bool zero) {
	return ipallocztm(tsd_tsdn(tsd), usize, alignment, zero,
	    tcache_get(tsd), false, NULL);
}

JEMALLOC_ALWAYS_INLINE size_t
ivsalloc(tsdn_t *tsdn, const void *ptr) {
	return arena_vsalloc(tsdn, ptr);
}

JEMALLOC_ALWAYS_INLINE void
idalloctm(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    emap_alloc_ctx_t *alloc_ctx, bool is_internal, bool slow_path) {
	assert(ptr != NULL);
	assert(!is_internal || tcache == NULL);
	assert(!is_internal || arena_is_auto(iaalloc(tsdn, ptr)));
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	if (config_stats && is_internal) {
		arena_internal_sub(iaalloc(tsdn, ptr), isalloc(tsdn, ptr));
	}
	if (!is_internal && !tsdn_null(tsdn) &&
	    tsd_reentrancy_level_get(tsdn_tsd(tsdn)) != 0) {
		assert(tcache == NULL);
	}
	arena_dalloc(tsdn, ptr, tcache, alloc_ctx, slow_path);
}

JEMALLOC_ALWAYS_INLINE void
idalloc(tsd_t *tsd, void *ptr) {
	idalloctm(tsd_tsdn(tsd), ptr, tcache_get(tsd), NULL, false, true);
}

JEMALLOC_ALWAYS_INLINE void
isdalloct(tsdn_t *tsdn, void *ptr, size_t size, tcache_t *tcache,
    emap_alloc_ctx_t *alloc_ctx, bool slow_path) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	arena_sdalloc(tsdn, ptr, size, tcache, alloc_ctx, slow_path);
}

JEMALLOC_ALWAYS_INLINE void *
iralloct_realign(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size,
    size_t alignment, bool zero, tcache_t *tcache, arena_t *arena,
    hook_ralloc_args_t *hook_args) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	void *p;
	size_t usize, copysize;

	usize = sz_sa2u(size, alignment);
	if (unlikely(usize == 0 || usize > SC_LARGE_MAXCLASS)) {
		return NULL;
	}
	p = ipalloct(tsdn, usize, alignment, zero, tcache, arena);
	if (p == NULL) {
		return NULL;
	}
	/*
	 * Copy at most size bytes (not size+extra), since the caller has no
	 * expectation that the extra bytes will be reliably preserved.
	 */
	copysize = (size < oldsize) ? size : oldsize;
	memcpy(p, ptr, copysize);
	hook_invoke_alloc(hook_args->is_realloc
	    ? hook_alloc_realloc : hook_alloc_rallocx, p, (uintptr_t)p,
	    hook_args->args);
	hook_invoke_dalloc(hook_args->is_realloc
	    ? hook_dalloc_realloc : hook_dalloc_rallocx, ptr, hook_args->args);
	isdalloct(tsdn, ptr, oldsize, tcache, NULL, true);
	return p;
}

/*
 * is_realloc threads through the knowledge of whether or not this call comes
 * from je_realloc (as opposed to je_rallocx); this ensures that we pass the
 * correct entry point into any hooks.
 * Note that these functions are all force-inlined, so no actual bool gets
 * passed-around anywhere.
 */
JEMALLOC_ALWAYS_INLINE void *
iralloct(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size, size_t alignment,
    bool zero, tcache_t *tcache, arena_t *arena, hook_ralloc_args_t *hook_args)
{
	assert(ptr != NULL);
	assert(size != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (alignment != 0 && ((uintptr_t)ptr & ((uintptr_t)alignment-1))
	    != 0) {
		/*
		 * Existing object alignment is inadequate; allocate new space
		 * and copy.
		 */
		return iralloct_realign(tsdn, ptr, oldsize, size, alignment,
		    zero, tcache, arena, hook_args);
	}

	return arena_ralloc(tsdn, arena, ptr, oldsize, size, alignment, zero,
	    tcache, hook_args);
}

JEMALLOC_ALWAYS_INLINE void *
iralloc(tsd_t *tsd, void *ptr, size_t oldsize, size_t size, size_t alignment,
    bool zero, hook_ralloc_args_t *hook_args) {
	return iralloct(tsd_tsdn(tsd), ptr, oldsize, size, alignment, zero,
	    tcache_get(tsd), NULL, hook_args);
}

JEMALLOC_ALWAYS_INLINE bool
ixalloc(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size, size_t extra,
    size_t alignment, bool zero, size_t *newsize) {
	assert(ptr != NULL);
	assert(size != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (alignment != 0 && ((uintptr_t)ptr & ((uintptr_t)alignment-1))
	    != 0) {
		/* Existing object alignment is inadequate. */
		*newsize = oldsize;
		return true;
	}

	return arena_ralloc_no_move(tsdn, ptr, oldsize, size, extra, zero,
	    newsize);
}

JEMALLOC_ALWAYS_INLINE void
fastpath_success_finish(tsd_t *tsd, uint64_t allocated_after,
    cache_bin_t *bin, void *ret) {
	thread_allocated_set(tsd, allocated_after);
	if (config_stats) {
		bin->tstats.nrequests++;
	}

	LOG("core.malloc.exit", "result: %p", ret);
}

JEMALLOC_ALWAYS_INLINE bool
malloc_initialized(void) {
	return (malloc_init_state == malloc_init_initialized);
}

/*
 * malloc() fastpath.  Included here so that we can inline it into operator new;
 * function call overhead there is non-negligible as a fraction of total CPU in
 * allocation-heavy C++ programs.  We take the fallback alloc to allow malloc
 * (which can return NULL) to differ in its behavior from operator new (which
 * can't).  It matches the signature of malloc / operator new so that we can
 * tail-call the fallback allocator, allowing us to avoid setting up the call
 * frame in the common case.
 *
 * Fastpath assumes size <= SC_LOOKUP_MAXCLASS, and that we hit
 * tcache.  If either of these is false, we tail-call to the slowpath,
 * malloc_default().  Tail-calling is used to avoid any caller-saved
 * registers.
 *
 * fastpath supports ticker and profiling, both of which will also
 * tail-call to the slowpath if they fire.
 */
JEMALLOC_ALWAYS_INLINE void *
imalloc_fastpath(size_t size, void *(fallback_alloc)(size_t)) {
	LOG("core.malloc.entry", "size: %zu", size);
	if (tsd_get_allocates() && unlikely(!malloc_initialized())) {
		return fallback_alloc(size);
	}

	tsd_t *tsd = tsd_get(false);
	if (unlikely((size > SC_LOOKUP_MAXCLASS) || tsd == NULL)) {
		return fallback_alloc(size);
	}
	/*
	 * The code below till the branch checking the next_event threshold may
	 * execute before malloc_init(), in which case the threshold is 0 to
	 * trigger slow path and initialization.
	 *
	 * Note that when uninitialized, only the fast-path variants of the sz /
	 * tsd facilities may be called.
	 */
	szind_t ind;
	/*
	 * The thread_allocated counter in tsd serves as a general purpose
	 * accumulator for bytes of allocation to trigger different types of
	 * events.  usize is always needed to advance thread_allocated, though
	 * it's not always needed in the core allocation logic.
	 */
	size_t usize;
	sz_size2index_usize_fastpath(size, &ind, &usize);
	/* Fast path relies on size being a bin. */
	assert(ind < SC_NBINS);
	assert((SC_LOOKUP_MAXCLASS < SC_SMALL_MAXCLASS) &&
	    (size <= SC_SMALL_MAXCLASS));

	uint64_t allocated, threshold;
	te_malloc_fastpath_ctx(tsd, &allocated, &threshold);
	uint64_t allocated_after = allocated + usize;
	/*
	 * The ind and usize might be uninitialized (or partially) before
	 * malloc_init().  The assertions check for: 1) full correctness (usize
	 * & ind) when initialized; and 2) guaranteed slow-path (threshold == 0)
	 * when !initialized.
	 */
	if (!malloc_initialized()) {
		assert(threshold == 0);
	} else {
		assert(ind == sz_size2index(size));
		assert(usize > 0 && usize == sz_index2size(ind));
	}
	/*
	 * Check for events and tsd non-nominal (fast_threshold will be set to
	 * 0) in a single branch.
	 */
	if (unlikely(allocated_after >= threshold)) {
		return fallback_alloc(size);
	}
	assert(tsd_fast(tsd));

	tcache_t *tcache = tsd_tcachep_get(tsd);
	assert(tcache == tcache_get(tsd));
	cache_bin_t *bin = &tcache->bins[ind];
	bool tcache_success;
	void *ret;

	/*
	 * We split up the code this way so that redundant low-water
	 * computation doesn't happen on the (more common) case in which we
	 * don't touch the low water mark.  The compiler won't do this
	 * duplication on its own.
	 */
	ret = cache_bin_alloc_easy(bin, &tcache_success);
	if (tcache_success) {
		fastpath_success_finish(tsd, allocated_after, bin, ret);
		return ret;
	}
	ret = cache_bin_alloc(bin, &tcache_success);
	if (tcache_success) {
		fastpath_success_finish(tsd, allocated_after, bin, ret);
		return ret;
	}

	return fallback_alloc(size);
}

JEMALLOC_ALWAYS_INLINE int
iget_defrag_hint(tsdn_t *tsdn, void* ptr) {
	int defrag = 0;
	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr, &alloc_ctx);
	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		edata_t *slab = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
		arena_t *arena = arena_get_from_edata(slab);
		szind_t binind = edata_szind_get(slab);
		unsigned binshard = edata_binshard_get(slab);
		bin_t *bin = arena_get_bin(arena, binind, binshard);
		malloc_mutex_lock(tsdn, &bin->lock);
		arena_dalloc_bin_locked_info_t info;
		arena_dalloc_bin_locked_begin(&info, binind);
		/* Don't bother moving allocations from the slab currently used for new allocations */
		if (slab != bin->slabcur) {
			int free_in_slab = edata_nfree_get(slab);
			if (free_in_slab) {
				const bin_info_t *bin_info = &bin_infos[binind];
				/* Find number of non-full slabs and the number of regs in them */
				unsigned long curslabs = 0;
				size_t curregs = 0;
				/* Run on all bin shards (usually just one) */
				for (uint32_t i=0; i< bin_info->n_shards; i++) {
					bin_t *bb = arena_get_bin(arena, binind, i);
					curslabs += bb->stats.nonfull_slabs;
					/* Deduct the regs in full slabs (they're not part of the game) */
					unsigned long full_slabs = bb->stats.curslabs - bb->stats.nonfull_slabs;
					curregs += bb->stats.curregs - full_slabs * bin_info->nregs;
					if (bb->slabcur) {
						/* Remove slabcur from the overall utilization (not a candidate to nove from) */
						curregs -= bin_info->nregs - edata_nfree_get(bb->slabcur);
						curslabs -= 1;
					}
				}
				/* Compare the utilization ratio of the slab in question to the total average
				 * among non-full slabs. To avoid precision loss in division, we do that by
				 * extrapolating the usage of the slab as if all slabs have the same usage.
				 * If this slab is less used than the average, we'll prefer to move the data
				 * to hopefully more used ones. To avoid stagnation when all slabs have the same
				 * utilization, we give additional 12.5% weight to the decision to defrag. */
				defrag = (bin_info->nregs - free_in_slab) * curslabs <= curregs + curregs / 8;
			}
		}
		arena_dalloc_bin_locked_finish(tsdn, arena, bin, &info);
		malloc_mutex_unlock(tsdn, &bin->lock);
	}
	return defrag;
}

#endif /* JEMALLOC_INTERNAL_INLINES_C_H */
