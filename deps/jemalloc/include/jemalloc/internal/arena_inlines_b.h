#ifndef JEMALLOC_INTERNAL_ARENA_INLINES_B_H
#define JEMALLOC_INTERNAL_ARENA_INLINES_B_H

#include "jemalloc/internal/div.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"

static inline arena_t *
arena_get_from_edata(edata_t *edata) {
	return (arena_t *)atomic_load_p(&arenas[edata_arena_ind_get(edata)],
	    ATOMIC_RELAXED);
}

JEMALLOC_ALWAYS_INLINE arena_t *
arena_choose_maybe_huge(tsd_t *tsd, arena_t *arena, size_t size) {
	if (arena != NULL) {
		return arena;
	}

	/*
	 * For huge allocations, use the dedicated huge arena if both are true:
	 * 1) is using auto arena selection (i.e. arena == NULL), and 2) the
	 * thread is not assigned to a manual arena.
	 */
	if (unlikely(size >= oversize_threshold)) {
		arena_t *tsd_arena = tsd_arena_get(tsd);
		if (tsd_arena == NULL || arena_is_auto(tsd_arena)) {
			return arena_choose_huge(tsd);
		}
	}

	return arena_choose(tsd, NULL);
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_info_get(tsd_t *tsd, const void *ptr, emap_alloc_ctx_t *alloc_ctx,
    prof_info_t *prof_info, bool reset_recent) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(prof_info != NULL);

	edata_t *edata = NULL;
	bool is_slab;

	/* Static check. */
	if (alloc_ctx == NULL) {
		edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global,
		    ptr);
		is_slab = edata_slab_get(edata);
	} else if (unlikely(!(is_slab = alloc_ctx->slab))) {
		edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global,
		    ptr);
	}

	if (unlikely(!is_slab)) {
		/* edata must have been initialized at this point. */
		assert(edata != NULL);
		large_prof_info_get(tsd, edata, prof_info, reset_recent);
	} else {
		prof_info->alloc_tctx = (prof_tctx_t *)(uintptr_t)1U;
		/*
		 * No need to set other fields in prof_info; they will never be
		 * accessed if (uintptr_t)alloc_tctx == (uintptr_t)1U.
		 */
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_tctx_reset(tsd_t *tsd, const void *ptr,
    emap_alloc_ctx_t *alloc_ctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	/* Static check. */
	if (alloc_ctx == NULL) {
		edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd),
		    &arena_emap_global, ptr);
		if (unlikely(!edata_slab_get(edata))) {
			large_prof_tctx_reset(edata);
		}
	} else {
		if (unlikely(!alloc_ctx->slab)) {
			edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd),
			    &arena_emap_global, ptr);
			large_prof_tctx_reset(edata);
		}
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_tctx_reset_sampled(tsd_t *tsd, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global,
	    ptr);
	assert(!edata_slab_get(edata));

	large_prof_tctx_reset(edata);
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_info_set(tsd_t *tsd, edata_t *edata, prof_tctx_t *tctx,
    size_t size) {
	cassert(config_prof);

	assert(!edata_slab_get(edata));
	large_prof_info_set(edata, tctx, size);
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_ticks(tsdn_t *tsdn, arena_t *arena, unsigned nticks) {
	if (unlikely(tsdn_null(tsdn))) {
		return;
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	/*
	 * We use the ticker_geom_t to avoid having per-arena state in the tsd.
	 * Instead of having a countdown-until-decay timer running for every
	 * arena in every thread, we flip a coin once per tick, whose
	 * probability of coming up heads is 1/nticks; this is effectively the
	 * operation of the ticker_geom_t.  Each arena has the same chance of a
	 * coinflip coming up heads (1/ARENA_DECAY_NTICKS_PER_UPDATE), so we can
	 * use a single ticker for all of them.
	 */
	ticker_geom_t *decay_ticker = tsd_arena_decay_tickerp_get(tsd);
	uint64_t *prng_state = tsd_prng_statep_get(tsd);
	if (unlikely(ticker_geom_ticks(decay_ticker, prng_state, nticks))) {
		arena_decay(tsdn, arena, false, false);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_tick(tsdn_t *tsdn, arena_t *arena) {
	arena_decay_ticks(tsdn, arena, 1);
}

JEMALLOC_ALWAYS_INLINE void *
arena_malloc(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind, bool zero,
    tcache_t *tcache, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);

	if (likely(tcache != NULL)) {
		if (likely(size <= SC_SMALL_MAXCLASS)) {
			return tcache_alloc_small(tsdn_tsd(tsdn), arena,
			    tcache, size, ind, zero, slow_path);
		}
		if (likely(size <= tcache_maxclass)) {
			return tcache_alloc_large(tsdn_tsd(tsdn), arena,
			    tcache, size, ind, zero, slow_path);
		}
		/* (size > tcache_maxclass) case falls through. */
		assert(size > tcache_maxclass);
	}

	return arena_malloc_hard(tsdn, arena, size, ind, zero);
}

JEMALLOC_ALWAYS_INLINE arena_t *
arena_aalloc(tsdn_t *tsdn, const void *ptr) {
	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	unsigned arena_ind = edata_arena_ind_get(edata);
	return (arena_t *)atomic_load_p(&arenas[arena_ind], ATOMIC_RELAXED);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_salloc(tsdn_t *tsdn, const void *ptr) {
	assert(ptr != NULL);
	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr, &alloc_ctx);
	assert(alloc_ctx.szind != SC_NSIZES);

	return sz_index2size(alloc_ctx.szind);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_vsalloc(tsdn_t *tsdn, const void *ptr) {
	/*
	 * Return 0 if ptr is not within an extent managed by jemalloc.  This
	 * function has two extra costs relative to isalloc():
	 * - The rtree calls cannot claim to be dependent lookups, which induces
	 *   rtree lookup load dependencies.
	 * - The lookup may fail, so there is an extra branch to check for
	 *   failure.
	 */

	emap_full_alloc_ctx_t full_alloc_ctx;
	bool missing = emap_full_alloc_ctx_try_lookup(tsdn, &arena_emap_global,
	    ptr, &full_alloc_ctx);
	if (missing) {
		return 0;
	}

	if (full_alloc_ctx.edata == NULL) {
		return 0;
	}
	assert(edata_state_get(full_alloc_ctx.edata) == extent_state_active);
	/* Only slab members should be looked up via interior pointers. */
	assert(edata_addr_get(full_alloc_ctx.edata) == ptr
	    || edata_slab_get(full_alloc_ctx.edata));

	assert(full_alloc_ctx.szind != SC_NSIZES);

	return sz_index2size(full_alloc_ctx.szind);
}

JEMALLOC_ALWAYS_INLINE bool
large_dalloc_safety_checks(edata_t *edata, void *ptr, szind_t szind) {
	if (!config_opt_safety_checks) {
		return false;
	}

	/*
	 * Eagerly detect double free and sized dealloc bugs for large sizes.
	 * The cost is low enough (as edata will be accessed anyway) to be
	 * enabled all the time.
	 */
	if (unlikely(edata == NULL ||
	    edata_state_get(edata) != extent_state_active)) {
		safety_check_fail("Invalid deallocation detected: "
		    "pages being freed (%p) not currently active, "
		    "possibly caused by double free bugs.",
		    (uintptr_t)edata_addr_get(edata));
		return true;
	}
	size_t input_size = sz_index2size(szind);
	if (unlikely(input_size != edata_usize_get(edata))) {
		safety_check_fail_sized_dealloc(/* current_dealloc */ true, ptr,
		    /* true_size */ edata_usize_get(edata), input_size);
		return true;
	}

	return false;
}

static inline void
arena_dalloc_large_no_tcache(tsdn_t *tsdn, void *ptr, szind_t szind) {
	if (config_prof && unlikely(szind < SC_NBINS)) {
		arena_dalloc_promoted(tsdn, ptr, NULL, true);
	} else {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		if (large_dalloc_safety_checks(edata, ptr, szind)) {
			/* See the comment in isfree. */
			return;
		}
		large_dalloc(tsdn, edata);
	}
}

static inline void
arena_dalloc_no_tcache(tsdn_t *tsdn, void *ptr) {
	assert(ptr != NULL);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr, &alloc_ctx);

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.szind < SC_NSIZES);
		assert(alloc_ctx.slab == edata_slab_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		arena_dalloc_large_no_tcache(tsdn, ptr, alloc_ctx.szind);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc_large(tsdn_t *tsdn, void *ptr, tcache_t *tcache, szind_t szind,
    bool slow_path) {
	if (szind < nhbins) {
		if (config_prof && unlikely(szind < SC_NBINS)) {
			arena_dalloc_promoted(tsdn, ptr, tcache, slow_path);
		} else {
			tcache_dalloc_large(tsdn_tsd(tsdn), tcache, ptr, szind,
			    slow_path);
		}
	} else {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		if (large_dalloc_safety_checks(edata, ptr, szind)) {
			/* See the comment in isfree. */
			return;
		}
		large_dalloc(tsdn, edata);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    emap_alloc_ctx_t *caller_alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);

	if (unlikely(tcache == NULL)) {
		arena_dalloc_no_tcache(tsdn, ptr);
		return;
	}

	emap_alloc_ctx_t alloc_ctx;
	if (caller_alloc_ctx != NULL) {
		alloc_ctx = *caller_alloc_ctx;
	} else {
		util_assume(!tsdn_null(tsdn));
		emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr,
		    &alloc_ctx);
	}

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.szind < SC_NSIZES);
		assert(alloc_ctx.slab == edata_slab_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		tcache_dalloc_small(tsdn_tsd(tsdn), tcache, ptr,
		    alloc_ctx.szind, slow_path);
	} else {
		arena_dalloc_large(tsdn, ptr, tcache, alloc_ctx.szind,
		    slow_path);
	}
}

static inline void
arena_sdalloc_no_tcache(tsdn_t *tsdn, void *ptr, size_t size) {
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	emap_alloc_ctx_t alloc_ctx;
	if (!config_prof || !opt_prof) {
		/*
		 * There is no risk of being confused by a promoted sampled
		 * object, so base szind and slab on the given size.
		 */
		alloc_ctx.szind = sz_size2index(size);
		alloc_ctx.slab = (alloc_ctx.szind < SC_NBINS);
	}

	if ((config_prof && opt_prof) || config_debug) {
		emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr,
		    &alloc_ctx);

		assert(alloc_ctx.szind == sz_size2index(size));
		assert((config_prof && opt_prof)
		    || alloc_ctx.slab == (alloc_ctx.szind < SC_NBINS));

		if (config_debug) {
			edata_t *edata = emap_edata_lookup(tsdn,
			    &arena_emap_global, ptr);
			assert(alloc_ctx.szind == edata_szind_get(edata));
			assert(alloc_ctx.slab == edata_slab_get(edata));
		}
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		arena_dalloc_large_no_tcache(tsdn, ptr, alloc_ctx.szind);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_sdalloc(tsdn_t *tsdn, void *ptr, size_t size, tcache_t *tcache,
    emap_alloc_ctx_t *caller_alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	if (unlikely(tcache == NULL)) {
		arena_sdalloc_no_tcache(tsdn, ptr, size);
		return;
	}

	emap_alloc_ctx_t alloc_ctx;
	if (config_prof && opt_prof) {
		if (caller_alloc_ctx == NULL) {
			/* Uncommon case and should be a static check. */
			emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr,
			    &alloc_ctx);
			assert(alloc_ctx.szind == sz_size2index(size));
		} else {
			alloc_ctx = *caller_alloc_ctx;
		}
	} else {
		/*
		 * There is no risk of being confused by a promoted sampled
		 * object, so base szind and slab on the given size.
		 */
		alloc_ctx.szind = sz_size2index(size);
		alloc_ctx.slab = (alloc_ctx.szind < SC_NBINS);
	}

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.slab == edata_slab_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		tcache_dalloc_small(tsdn_tsd(tsdn), tcache, ptr,
		    alloc_ctx.szind, slow_path);
	} else {
		arena_dalloc_large(tsdn, ptr, tcache, alloc_ctx.szind,
		    slow_path);
	}
}

static inline void
arena_cache_oblivious_randomize(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    size_t alignment) {
	assert(edata_base_get(edata) == edata_addr_get(edata));

	if (alignment < PAGE) {
		unsigned lg_range = LG_PAGE -
		    lg_floor(CACHELINE_CEILING(alignment));
		size_t r;
		if (!tsdn_null(tsdn)) {
			tsd_t *tsd = tsdn_tsd(tsdn);
			r = (size_t)prng_lg_range_u64(
			    tsd_prng_statep_get(tsd), lg_range);
		} else {
			uint64_t stack_value = (uint64_t)(uintptr_t)&r;
			r = (size_t)prng_lg_range_u64(&stack_value, lg_range);
		}
		uintptr_t random_offset = ((uintptr_t)r) << (LG_PAGE -
		    lg_range);
		edata->e_addr = (void *)((uintptr_t)edata->e_addr +
		    random_offset);
		assert(ALIGNMENT_ADDR2BASE(edata->e_addr, alignment) ==
		    edata->e_addr);
	}
}

/*
 * The dalloc bin info contains just the information that the common paths need
 * during tcache flushes.  By force-inlining these paths, and using local copies
 * of data (so that the compiler knows it's constant), we avoid a whole bunch of
 * redundant loads and stores by leaving this information in registers.
 */
typedef struct arena_dalloc_bin_locked_info_s arena_dalloc_bin_locked_info_t;
struct arena_dalloc_bin_locked_info_s {
	div_info_t div_info;
	uint32_t nregs;
	uint64_t ndalloc;
};

JEMALLOC_ALWAYS_INLINE size_t
arena_slab_regind(arena_dalloc_bin_locked_info_t *info, szind_t binind,
    edata_t *slab, const void *ptr) {
	size_t diff, regind;

	/* Freeing a pointer outside the slab can cause assertion failure. */
	assert((uintptr_t)ptr >= (uintptr_t)edata_addr_get(slab));
	assert((uintptr_t)ptr < (uintptr_t)edata_past_get(slab));
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab)) %
	    (uintptr_t)bin_infos[binind].reg_size == 0);

	diff = (size_t)((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab));

	/* Avoid doing division with a variable divisor. */
	regind = div_compute(&info->div_info, diff);

	assert(regind < bin_infos[binind].nregs);

	return regind;
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc_bin_locked_begin(arena_dalloc_bin_locked_info_t *info,
    szind_t binind) {
	info->div_info = arena_binind_div_info[binind];
	info->nregs = bin_infos[binind].nregs;
	info->ndalloc = 0;
}

/*
 * Does the deallocation work associated with freeing a single pointer (a
 * "step") in between a arena_dalloc_bin_locked begin and end call.
 *
 * Returns true if arena_slab_dalloc must be called on slab.  Doesn't do
 * stats updates, which happen during finish (this lets running counts get left
 * in a register).
 */
JEMALLOC_ALWAYS_INLINE bool
arena_dalloc_bin_locked_step(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    arena_dalloc_bin_locked_info_t *info, szind_t binind, edata_t *slab,
    void *ptr) {
	const bin_info_t *bin_info = &bin_infos[binind];
	size_t regind = arena_slab_regind(info, binind, slab, ptr);
	slab_data_t *slab_data = edata_slab_data_get(slab);

	assert(edata_nfree_get(slab) < bin_info->nregs);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(slab_data->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(slab_data->bitmap, &bin_info->bitmap_info, regind);
	edata_nfree_inc(slab);

	if (config_stats) {
		info->ndalloc++;
	}

	unsigned nfree = edata_nfree_get(slab);
	if (nfree == bin_info->nregs) {
		arena_dalloc_bin_locked_handle_newly_empty(tsdn, arena, slab,
		    bin);
		return true;
	} else if (nfree == 1 && slab != bin->slabcur) {
		arena_dalloc_bin_locked_handle_newly_nonempty(tsdn, arena, slab,
		    bin);
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc_bin_locked_finish(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    arena_dalloc_bin_locked_info_t *info) {
	if (config_stats) {
		bin->stats.ndalloc += info->ndalloc;
		assert(bin->stats.curregs >= (size_t)info->ndalloc);
		bin->stats.curregs -= (size_t)info->ndalloc;
	}
}

static inline bin_t *
arena_get_bin(arena_t *arena, szind_t binind, unsigned binshard) {
	bin_t *shard0 = (bin_t *)((uintptr_t)arena + arena_bin_offsets[binind]);
	return shard0 + binshard;
}

#endif /* JEMALLOC_INTERNAL_ARENA_INLINES_B_H */
