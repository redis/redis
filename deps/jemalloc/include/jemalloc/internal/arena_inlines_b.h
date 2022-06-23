#ifndef JEMALLOC_INTERNAL_ARENA_INLINES_B_H
#define JEMALLOC_INTERNAL_ARENA_INLINES_B_H

#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"

JEMALLOC_ALWAYS_INLINE bool
arena_has_default_hooks(arena_t *arena) {
	return (extent_hooks_get(arena) == &extent_hooks_default);
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

JEMALLOC_ALWAYS_INLINE prof_tctx_t *
arena_prof_tctx_get(tsdn_t *tsdn, const void *ptr, alloc_ctx_t *alloc_ctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	/* Static check. */
	if (alloc_ctx == NULL) {
		const extent_t *extent = iealloc(tsdn, ptr);
		if (unlikely(!extent_slab_get(extent))) {
			return large_prof_tctx_get(tsdn, extent);
		}
	} else {
		if (unlikely(!alloc_ctx->slab)) {
			return large_prof_tctx_get(tsdn, iealloc(tsdn, ptr));
		}
	}
	return (prof_tctx_t *)(uintptr_t)1U;
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_tctx_set(tsdn_t *tsdn, const void *ptr, size_t usize,
    alloc_ctx_t *alloc_ctx, prof_tctx_t *tctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	/* Static check. */
	if (alloc_ctx == NULL) {
		extent_t *extent = iealloc(tsdn, ptr);
		if (unlikely(!extent_slab_get(extent))) {
			large_prof_tctx_set(tsdn, extent, tctx);
		}
	} else {
		if (unlikely(!alloc_ctx->slab)) {
			large_prof_tctx_set(tsdn, iealloc(tsdn, ptr), tctx);
		}
	}
}

static inline void
arena_prof_tctx_reset(tsdn_t *tsdn, const void *ptr, prof_tctx_t *tctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	extent_t *extent = iealloc(tsdn, ptr);
	assert(!extent_slab_get(extent));

	large_prof_tctx_reset(tsdn, extent);
}

JEMALLOC_ALWAYS_INLINE nstime_t
arena_prof_alloc_time_get(tsdn_t *tsdn, const void *ptr,
    alloc_ctx_t *alloc_ctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	extent_t *extent = iealloc(tsdn, ptr);
	/*
	 * Unlike arena_prof_prof_tctx_{get, set}, we only call this once we're
	 * sure we have a sampled allocation.
	 */
	assert(!extent_slab_get(extent));
	return large_prof_alloc_time_get(extent);
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_alloc_time_set(tsdn_t *tsdn, const void *ptr, alloc_ctx_t *alloc_ctx,
    nstime_t t) {
	cassert(config_prof);
	assert(ptr != NULL);

	extent_t *extent = iealloc(tsdn, ptr);
	assert(!extent_slab_get(extent));
	large_prof_alloc_time_set(extent, t);
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_ticks(tsdn_t *tsdn, arena_t *arena, unsigned nticks) {
	tsd_t *tsd;
	ticker_t *decay_ticker;

	if (unlikely(tsdn_null(tsdn))) {
		return;
	}
	tsd = tsdn_tsd(tsdn);
	decay_ticker = decay_ticker_get(tsd, arena_ind_get(arena));
	if (unlikely(decay_ticker == NULL)) {
		return;
	}
	if (unlikely(ticker_ticks(decay_ticker, nticks))) {
		arena_decay(tsdn, arena, false, false);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_tick(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_assert_not_owner(tsdn, &arena->decay_dirty.mtx);
	malloc_mutex_assert_not_owner(tsdn, &arena->decay_muzzy.mtx);

	arena_decay_ticks(tsdn, arena, 1);
}

/* Purge a single extent to retained / unmapped directly. */
JEMALLOC_ALWAYS_INLINE void
arena_decay_extent(tsdn_t *tsdn,arena_t *arena, extent_hooks_t **r_extent_hooks,
    extent_t *extent) {
	size_t extent_size = extent_size_get(extent);
	extent_dalloc_wrapper(tsdn, arena,
	    r_extent_hooks, extent);
	if (config_stats) {
		/* Update stats accordingly. */
		arena_stats_lock(tsdn, &arena->stats);
		arena_stats_add_u64(tsdn, &arena->stats,
		    &arena->decay_dirty.stats->nmadvise, 1);
		arena_stats_add_u64(tsdn, &arena->stats,
		    &arena->decay_dirty.stats->purged, extent_size >> LG_PAGE);
		arena_stats_sub_zu(tsdn, &arena->stats, &arena->stats.mapped,
		    extent_size);
		arena_stats_unlock(tsdn, &arena->stats);
	}
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
	return extent_arena_get(iealloc(tsdn, ptr));
}

JEMALLOC_ALWAYS_INLINE size_t
arena_salloc(tsdn_t *tsdn, const void *ptr) {
	assert(ptr != NULL);

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	szind_t szind = rtree_szind_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true);
	assert(szind != SC_NSIZES);

	return sz_index2size(szind);
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
	assert(extent_state_get(extent) == extent_state_active);
	/* Only slab members should be looked up via interior pointers. */
	assert(extent_addr_get(extent) == ptr || extent_slab_get(extent));

	assert(szind != SC_NSIZES);

	return sz_index2size(szind);
}

static inline void
arena_dalloc_large_no_tcache(tsdn_t *tsdn, void *ptr, szind_t szind) {
	if (config_prof && unlikely(szind < SC_NBINS)) {
		arena_dalloc_promoted(tsdn, ptr, NULL, true);
	} else {
		extent_t *extent = iealloc(tsdn, ptr);
		large_dalloc(tsdn, extent);
	}
}

static inline void
arena_dalloc_no_tcache(tsdn_t *tsdn, void *ptr) {
	assert(ptr != NULL);

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	szind_t szind;
	bool slab;
	rtree_szind_slab_read(tsdn, &extents_rtree, rtree_ctx, (uintptr_t)ptr,
	    true, &szind, &slab);

	if (config_debug) {
		extent_t *extent = rtree_extent_read(tsdn, &extents_rtree,
		    rtree_ctx, (uintptr_t)ptr, true);
		assert(szind == extent_szind_get(extent));
		assert(szind < SC_NSIZES);
		assert(slab == extent_slab_get(extent));
	}

	if (likely(slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		arena_dalloc_large_no_tcache(tsdn, ptr, szind);
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
		extent_t *extent = iealloc(tsdn, ptr);
		large_dalloc(tsdn, extent);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    alloc_ctx_t *alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);

	if (unlikely(tcache == NULL)) {
		arena_dalloc_no_tcache(tsdn, ptr);
		return;
	}

	szind_t szind;
	bool slab;
	rtree_ctx_t *rtree_ctx;
	if (alloc_ctx != NULL) {
		szind = alloc_ctx->szind;
		slab = alloc_ctx->slab;
		assert(szind != SC_NSIZES);
	} else {
		rtree_ctx = tsd_rtree_ctx(tsdn_tsd(tsdn));
		rtree_szind_slab_read(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &szind, &slab);
	}

	if (config_debug) {
		rtree_ctx = tsd_rtree_ctx(tsdn_tsd(tsdn));
		extent_t *extent = rtree_extent_read(tsdn, &extents_rtree,
		    rtree_ctx, (uintptr_t)ptr, true);
		assert(szind == extent_szind_get(extent));
		assert(szind < SC_NSIZES);
		assert(slab == extent_slab_get(extent));
	}

	if (likely(slab)) {
		/* Small allocation. */
		tcache_dalloc_small(tsdn_tsd(tsdn), tcache, ptr, szind,
		    slow_path);
	} else {
		arena_dalloc_large(tsdn, ptr, tcache, szind, slow_path);
	}
}

static inline void
arena_sdalloc_no_tcache(tsdn_t *tsdn, void *ptr, size_t size) {
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	szind_t szind;
	bool slab;
	if (!config_prof || !opt_prof) {
		/*
		 * There is no risk of being confused by a promoted sampled
		 * object, so base szind and slab on the given size.
		 */
		szind = sz_size2index(size);
		slab = (szind < SC_NBINS);
	}

	if ((config_prof && opt_prof) || config_debug) {
		rtree_ctx_t rtree_ctx_fallback;
		rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn,
		    &rtree_ctx_fallback);

		rtree_szind_slab_read(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &szind, &slab);

		assert(szind == sz_size2index(size));
		assert((config_prof && opt_prof) || slab == (szind < SC_NBINS));

		if (config_debug) {
			extent_t *extent = rtree_extent_read(tsdn,
			    &extents_rtree, rtree_ctx, (uintptr_t)ptr, true);
			assert(szind == extent_szind_get(extent));
			assert(slab == extent_slab_get(extent));
		}
	}

	if (likely(slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		arena_dalloc_large_no_tcache(tsdn, ptr, szind);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_sdalloc(tsdn_t *tsdn, void *ptr, size_t size, tcache_t *tcache,
    alloc_ctx_t *alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	if (unlikely(tcache == NULL)) {
		arena_sdalloc_no_tcache(tsdn, ptr, size);
		return;
	}

	szind_t szind;
	bool slab;
	alloc_ctx_t local_ctx;
	if (config_prof && opt_prof) {
		if (alloc_ctx == NULL) {
			/* Uncommon case and should be a static check. */
			rtree_ctx_t rtree_ctx_fallback;
			rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn,
			    &rtree_ctx_fallback);
			rtree_szind_slab_read(tsdn, &extents_rtree, rtree_ctx,
			    (uintptr_t)ptr, true, &local_ctx.szind,
			    &local_ctx.slab);
			assert(local_ctx.szind == sz_size2index(size));
			alloc_ctx = &local_ctx;
		}
		slab = alloc_ctx->slab;
		szind = alloc_ctx->szind;
	} else {
		/*
		 * There is no risk of being confused by a promoted sampled
		 * object, so base szind and slab on the given size.
		 */
		szind = sz_size2index(size);
		slab = (szind < SC_NBINS);
	}

	if (config_debug) {
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsdn_tsd(tsdn));
		rtree_szind_slab_read(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &szind, &slab);
		extent_t *extent = rtree_extent_read(tsdn,
		    &extents_rtree, rtree_ctx, (uintptr_t)ptr, true);
		assert(szind == extent_szind_get(extent));
		assert(slab == extent_slab_get(extent));
	}

	if (likely(slab)) {
		/* Small allocation. */
		tcache_dalloc_small(tsdn_tsd(tsdn), tcache, ptr, szind,
		    slow_path);
	} else {
		arena_dalloc_large(tsdn, ptr, tcache, szind, slow_path);
	}
}

#endif /* JEMALLOC_INTERNAL_ARENA_INLINES_B_H */
