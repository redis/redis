#ifndef JEMALLOC_INTERNAL_PA_H
#define JEMALLOC_INTERNAL_PA_H

#include "jemalloc/internal/base.h"
#include "jemalloc/internal/decay.h"
#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/lockedint.h"
#include "jemalloc/internal/pac.h"
#include "jemalloc/internal/pai.h"
#include "jemalloc/internal/sec.h"

/*
 * The page allocator; responsible for acquiring pages of memory for
 * allocations.  It picks the implementation of the page allocator interface
 * (i.e. a pai_t) to handle a given page-level allocation request.  For now, the
 * only such implementation is the PAC code ("page allocator classic"), but
 * others will be coming soon.
 */

typedef struct pa_central_s pa_central_t;
struct pa_central_s {
	hpa_central_t hpa;
};

/*
 * The stats for a particular pa_shard.  Because of the way the ctl module
 * handles stats epoch data collection (it has its own arena_stats, and merges
 * the stats from each arena into it), this needs to live in the arena_stats_t;
 * hence we define it here and let the pa_shard have a pointer (rather than the
 * more natural approach of just embedding it in the pa_shard itself).
 *
 * We follow the arena_stats_t approach of marking the derived fields.  These
 * are the ones that are not maintained on their own; instead, their values are
 * derived during those stats merges.
 */
typedef struct pa_shard_stats_s pa_shard_stats_t;
struct pa_shard_stats_s {
	/* Number of edata_t structs allocated by base, but not being used. */
	size_t edata_avail; /* Derived. */
	/*
	 * Stats specific to the PAC.  For now, these are the only stats that
	 * exist, but there will eventually be other page allocators.  Things
	 * like edata_avail make sense in a cross-PA sense, but things like
	 * npurges don't.
	 */
	pac_stats_t pac_stats;
};

/*
 * The local allocator handle.  Keeps the state necessary to satisfy page-sized
 * allocations.
 *
 * The contents are mostly internal to the PA module.  The key exception is that
 * arena decay code is allowed to grab pointers to the dirty and muzzy ecaches
 * decay_ts, for a couple of queries, passing them back to a PA function, or
 * acquiring decay.mtx and looking at decay.purging.  The reasoning is that,
 * while PA decides what and how to purge, the arena code decides when and where
 * (e.g. on what thread).  It's allowed to use the presence of another purger to
 * decide.
 * (The background thread code also touches some other decay internals, but
 * that's not fundamental; its' just an artifact of a partial refactoring, and
 * its accesses could be straightforwardly moved inside the decay module).
 */
typedef struct pa_shard_s pa_shard_t;
struct pa_shard_s {
	/* The central PA this shard is associated with. */
	pa_central_t *central;

	/*
	 * Number of pages in active extents.
	 *
	 * Synchronization: atomic.
	 */
	atomic_zu_t nactive;

	/*
	 * Whether or not we should prefer the hugepage allocator.  Atomic since
	 * it may be concurrently modified by a thread setting extent hooks.
	 * Note that we still may do HPA operations in this arena; if use_hpa is
	 * changed from true to false, we'll free back to the hugepage allocator
	 * for those allocations.
	 */
	atomic_b_t use_hpa;

	/*
	 * If we never used the HPA to begin with, it wasn't initialized, and so
	 * we shouldn't try to e.g. acquire its mutexes during fork.  This
	 * tracks that knowledge.
	 */
	bool ever_used_hpa;

	/* Allocates from a PAC. */
	pac_t pac;

	/*
	 * We place a small extent cache in front of the HPA, since we intend
	 * these configurations to use many fewer arenas, and therefore have a
	 * higher risk of hot locks.
	 */
	sec_t hpa_sec;
	hpa_shard_t hpa_shard;

	/* The source of edata_t objects. */
	edata_cache_t edata_cache;

	unsigned ind;

	malloc_mutex_t *stats_mtx;
	pa_shard_stats_t *stats;

	/* The emap this shard is tied to. */
	emap_t *emap;

	/* The base from which we get the ehooks and allocate metadat. */
	base_t *base;
};

static inline bool
pa_shard_dont_decay_muzzy(pa_shard_t *shard) {
	return ecache_npages_get(&shard->pac.ecache_muzzy) == 0 &&
	    pac_decay_ms_get(&shard->pac, extent_state_muzzy) <= 0;
}

static inline ehooks_t *
pa_shard_ehooks_get(pa_shard_t *shard) {
	return base_ehooks_get(shard->base);
}

/* Returns true on error. */
bool pa_central_init(pa_central_t *central, base_t *base, bool hpa,
    hpa_hooks_t *hpa_hooks);

/* Returns true on error. */
bool pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, pa_central_t *central,
    emap_t *emap, base_t *base, unsigned ind, pa_shard_stats_t *stats,
    malloc_mutex_t *stats_mtx, nstime_t *cur_time, size_t oversize_threshold,
    ssize_t dirty_decay_ms, ssize_t muzzy_decay_ms);

/*
 * This isn't exposed to users; we allow late enablement of the HPA shard so
 * that we can boot without worrying about the HPA, then turn it on in a0.
 */
bool pa_shard_enable_hpa(tsdn_t *tsdn, pa_shard_t *shard,
    const hpa_shard_opts_t *hpa_opts, const sec_opts_t *hpa_sec_opts);

/*
 * We stop using the HPA when custom extent hooks are installed, but still
 * redirect deallocations to it.
 */
void pa_shard_disable_hpa(tsdn_t *tsdn, pa_shard_t *shard);

/*
 * This does the PA-specific parts of arena reset (i.e. freeing all active
 * allocations).
 */
void pa_shard_reset(tsdn_t *tsdn, pa_shard_t *shard);

/*
 * Destroy all the remaining retained extents.  Should only be called after
 * decaying all active, dirty, and muzzy extents to the retained state, as the
 * last step in destroying the shard.
 */
void pa_shard_destroy(tsdn_t *tsdn, pa_shard_t *shard);

/* Gets an edata for the given allocation. */
edata_t *pa_alloc(tsdn_t *tsdn, pa_shard_t *shard, size_t size,
    size_t alignment, bool slab, szind_t szind, bool zero, bool guarded,
    bool *deferred_work_generated);
/* Returns true on error, in which case nothing changed. */
bool pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool zero, bool *deferred_work_generated);
/*
 * The same.  Sets *generated_dirty to true if we produced new dirty pages, and
 * false otherwise.
 */
bool pa_shrink(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool *deferred_work_generated);
/*
 * Frees the given edata back to the pa.  Sets *generated_dirty if we produced
 * new dirty pages (well, we always set it for now; but this need not be the
 * case).
 * (We could make generated_dirty the return value of course, but this is more
 * consistent with the shrink pathway and our error codes here).
 */
void pa_dalloc(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata,
    bool *deferred_work_generated);
bool pa_decay_ms_set(tsdn_t *tsdn, pa_shard_t *shard, extent_state_t state,
    ssize_t decay_ms, pac_purge_eagerness_t eagerness);
ssize_t pa_decay_ms_get(pa_shard_t *shard, extent_state_t state);

/*
 * Do deferred work on this PA shard.
 *
 * Morally, this should do both PAC decay and the HPA deferred work.  For now,
 * though, the arena, background thread, and PAC modules are tightly interwoven
 * in a way that's tricky to extricate, so we only do the HPA-specific parts.
 */
void pa_shard_set_deferral_allowed(tsdn_t *tsdn, pa_shard_t *shard,
    bool deferral_allowed);
void pa_shard_do_deferred_work(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_try_deferred_work(tsdn_t *tsdn, pa_shard_t *shard);
uint64_t pa_shard_time_until_deferred_work(tsdn_t *tsdn, pa_shard_t *shard);

/******************************************************************************/
/*
 * Various bits of "boring" functionality that are still part of this module,
 * but that we relegate to pa_extra.c, to keep the core logic in pa.c as
 * readable as possible.
 */

/*
 * These fork phases are synchronized with the arena fork phase numbering to
 * make it easy to keep straight. That's why there's no prefork1.
 */
void pa_shard_prefork0(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_prefork2(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_prefork3(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_prefork4(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_prefork5(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_postfork_parent(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_postfork_child(tsdn_t *tsdn, pa_shard_t *shard);

void pa_shard_basic_stats_merge(pa_shard_t *shard, size_t *nactive,
    size_t *ndirty, size_t *nmuzzy);

void pa_shard_stats_merge(tsdn_t *tsdn, pa_shard_t *shard,
    pa_shard_stats_t *pa_shard_stats_out, pac_estats_t *estats_out,
    hpa_shard_stats_t *hpa_stats_out, sec_stats_t *sec_stats_out,
    size_t *resident);

/*
 * Reads the PA-owned mutex stats into the output stats array, at the
 * appropriate positions.  Morally, these stats should really live in
 * pa_shard_stats_t, but the indices are sort of baked into the various mutex
 * prof macros.  This would be a good thing to do at some point.
 */
void pa_shard_mtx_stats_read(tsdn_t *tsdn, pa_shard_t *shard,
    mutex_prof_data_t mutex_prof_data[mutex_prof_num_arena_mutexes]);

#endif /* JEMALLOC_INTERNAL_PA_H */
