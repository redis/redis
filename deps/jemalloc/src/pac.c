#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/pac.h"
#include "jemalloc/internal/san.h"

static edata_t *pac_alloc_impl(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero, bool guarded, bool frequent_reuse,
    bool *deferred_work_generated);
static bool pac_expand_impl(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero, bool *deferred_work_generated);
static bool pac_shrink_impl(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool *deferred_work_generated);
static void pac_dalloc_impl(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    bool *deferred_work_generated);
static uint64_t pac_time_until_deferred_work(tsdn_t *tsdn, pai_t *self);

static inline void
pac_decay_data_get(pac_t *pac, extent_state_t state,
    decay_t **r_decay, pac_decay_stats_t **r_decay_stats, ecache_t **r_ecache) {
	switch(state) {
	case extent_state_dirty:
		*r_decay = &pac->decay_dirty;
		*r_decay_stats = &pac->stats->decay_dirty;
		*r_ecache = &pac->ecache_dirty;
		return;
	case extent_state_muzzy:
		*r_decay = &pac->decay_muzzy;
		*r_decay_stats = &pac->stats->decay_muzzy;
		*r_ecache = &pac->ecache_muzzy;
		return;
	default:
		unreachable();
	}
}

bool
pac_init(tsdn_t *tsdn, pac_t *pac, base_t *base, emap_t *emap,
    edata_cache_t *edata_cache, nstime_t *cur_time,
    size_t pac_oversize_threshold, ssize_t dirty_decay_ms,
    ssize_t muzzy_decay_ms, pac_stats_t *pac_stats, malloc_mutex_t *stats_mtx) {
	unsigned ind = base_ind_get(base);
	/*
	 * Delay coalescing for dirty extents despite the disruptive effect on
	 * memory layout for best-fit extent allocation, since cached extents
	 * are likely to be reused soon after deallocation, and the cost of
	 * merging/splitting extents is non-trivial.
	 */
	if (ecache_init(tsdn, &pac->ecache_dirty, extent_state_dirty, ind,
	    /* delay_coalesce */ true)) {
		return true;
	}
	/*
	 * Coalesce muzzy extents immediately, because operations on them are in
	 * the critical path much less often than for dirty extents.
	 */
	if (ecache_init(tsdn, &pac->ecache_muzzy, extent_state_muzzy, ind,
	    /* delay_coalesce */ false)) {
		return true;
	}
	/*
	 * Coalesce retained extents immediately, in part because they will
	 * never be evicted (and therefore there's no opportunity for delayed
	 * coalescing), but also because operations on retained extents are not
	 * in the critical path.
	 */
	if (ecache_init(tsdn, &pac->ecache_retained, extent_state_retained,
	    ind, /* delay_coalesce */ false)) {
		return true;
	}
	exp_grow_init(&pac->exp_grow);
	if (malloc_mutex_init(&pac->grow_mtx, "extent_grow",
	    WITNESS_RANK_EXTENT_GROW, malloc_mutex_rank_exclusive)) {
		return true;
	}
	atomic_store_zu(&pac->oversize_threshold, pac_oversize_threshold,
	    ATOMIC_RELAXED);
	if (decay_init(&pac->decay_dirty, cur_time, dirty_decay_ms)) {
		return true;
	}
	if (decay_init(&pac->decay_muzzy, cur_time, muzzy_decay_ms)) {
		return true;
	}
	if (san_bump_alloc_init(&pac->sba)) {
		return true;
	}

	pac->base = base;
	pac->emap = emap;
	pac->edata_cache = edata_cache;
	pac->stats = pac_stats;
	pac->stats_mtx = stats_mtx;
	atomic_store_zu(&pac->extent_sn_next, 0, ATOMIC_RELAXED);

	pac->pai.alloc = &pac_alloc_impl;
	pac->pai.alloc_batch = &pai_alloc_batch_default;
	pac->pai.expand = &pac_expand_impl;
	pac->pai.shrink = &pac_shrink_impl;
	pac->pai.dalloc = &pac_dalloc_impl;
	pac->pai.dalloc_batch = &pai_dalloc_batch_default;
	pac->pai.time_until_deferred_work = &pac_time_until_deferred_work;

	return false;
}

static inline bool
pac_may_have_muzzy(pac_t *pac) {
	return pac_decay_ms_get(pac, extent_state_muzzy) != 0;
}

static edata_t *
pac_alloc_real(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks, size_t size,
    size_t alignment, bool zero, bool guarded) {
	assert(!guarded || alignment <= PAGE);

	edata_t *edata = ecache_alloc(tsdn, pac, ehooks, &pac->ecache_dirty,
	    NULL, size, alignment, zero, guarded);

	if (edata == NULL && pac_may_have_muzzy(pac)) {
		edata = ecache_alloc(tsdn, pac, ehooks, &pac->ecache_muzzy,
		    NULL, size, alignment, zero, guarded);
	}
	if (edata == NULL) {
		edata = ecache_alloc_grow(tsdn, pac, ehooks,
		    &pac->ecache_retained, NULL, size, alignment, zero,
		    guarded);
		if (config_stats && edata != NULL) {
			atomic_fetch_add_zu(&pac->stats->pac_mapped, size,
			    ATOMIC_RELAXED);
		}
	}

	return edata;
}

static edata_t *
pac_alloc_new_guarded(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks, size_t size,
    size_t alignment, bool zero, bool frequent_reuse) {
	assert(alignment <= PAGE);

	edata_t *edata;
	if (san_bump_enabled() && frequent_reuse) {
		edata = san_bump_alloc(tsdn, &pac->sba, pac, ehooks, size,
		    zero);
	} else {
		size_t size_with_guards = san_two_side_guarded_sz(size);
		/* Alloc a non-guarded extent first.*/
		edata = pac_alloc_real(tsdn, pac, ehooks, size_with_guards,
		    /* alignment */ PAGE, zero, /* guarded */ false);
		if (edata != NULL) {
			/* Add guards around it. */
			assert(edata_size_get(edata) == size_with_guards);
			san_guard_pages_two_sided(tsdn, ehooks, edata,
			    pac->emap, true);
		}
	}
	assert(edata == NULL || (edata_guarded_get(edata) &&
	    edata_size_get(edata) == size));

	return edata;
}

static edata_t *
pac_alloc_impl(tsdn_t *tsdn, pai_t *self, size_t size, size_t alignment,
    bool zero, bool guarded, bool frequent_reuse,
    bool *deferred_work_generated) {
	pac_t *pac = (pac_t *)self;
	ehooks_t *ehooks = pac_ehooks_get(pac);

	edata_t *edata = NULL;
	/*
	 * The condition is an optimization - not frequently reused guarded
	 * allocations are never put in the ecache.  pac_alloc_real also
	 * doesn't grow retained for guarded allocations.  So pac_alloc_real
	 * for such allocations would always return NULL.
	 * */
	if (!guarded || frequent_reuse) {
		edata =	pac_alloc_real(tsdn, pac, ehooks, size, alignment,
		    zero, guarded);
	}
	if (edata == NULL && guarded) {
		/* No cached guarded extents; creating a new one. */
		edata = pac_alloc_new_guarded(tsdn, pac, ehooks, size,
		    alignment, zero, frequent_reuse);
	}

	return edata;
}

static bool
pac_expand_impl(tsdn_t *tsdn, pai_t *self, edata_t *edata, size_t old_size,
    size_t new_size, bool zero, bool *deferred_work_generated) {
	pac_t *pac = (pac_t *)self;
	ehooks_t *ehooks = pac_ehooks_get(pac);

	size_t mapped_add = 0;
	size_t expand_amount = new_size - old_size;

	if (ehooks_merge_will_fail(ehooks)) {
		return true;
	}
	edata_t *trail = ecache_alloc(tsdn, pac, ehooks, &pac->ecache_dirty,
	    edata, expand_amount, PAGE, zero, /* guarded*/ false);
	if (trail == NULL) {
		trail = ecache_alloc(tsdn, pac, ehooks, &pac->ecache_muzzy,
		    edata, expand_amount, PAGE, zero, /* guarded*/ false);
	}
	if (trail == NULL) {
		trail = ecache_alloc_grow(tsdn, pac, ehooks,
		    &pac->ecache_retained, edata, expand_amount, PAGE, zero,
		    /* guarded */ false);
		mapped_add = expand_amount;
	}
	if (trail == NULL) {
		return true;
	}
	if (extent_merge_wrapper(tsdn, pac, ehooks, edata, trail)) {
		extent_dalloc_wrapper(tsdn, pac, ehooks, trail);
		return true;
	}
	if (config_stats && mapped_add > 0) {
		atomic_fetch_add_zu(&pac->stats->pac_mapped, mapped_add,
		    ATOMIC_RELAXED);
	}
	return false;
}

static bool
pac_shrink_impl(tsdn_t *tsdn, pai_t *self, edata_t *edata, size_t old_size,
    size_t new_size, bool *deferred_work_generated) {
	pac_t *pac = (pac_t *)self;
	ehooks_t *ehooks = pac_ehooks_get(pac);

	size_t shrink_amount = old_size - new_size;

	if (ehooks_split_will_fail(ehooks)) {
		return true;
	}

	edata_t *trail = extent_split_wrapper(tsdn, pac, ehooks, edata,
	    new_size, shrink_amount, /* holding_core_locks */ false);
	if (trail == NULL) {
		return true;
	}
	ecache_dalloc(tsdn, pac, ehooks, &pac->ecache_dirty, trail);
	*deferred_work_generated = true;
	return false;
}

static void
pac_dalloc_impl(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    bool *deferred_work_generated) {
	pac_t *pac = (pac_t *)self;
	ehooks_t *ehooks = pac_ehooks_get(pac);

	if (edata_guarded_get(edata)) {
		/*
		 * Because cached guarded extents do exact fit only, large
		 * guarded extents are restored on dalloc eagerly (otherwise
		 * they will not be reused efficiently).  Slab sizes have a
		 * limited number of size classes, and tend to cycle faster.
		 *
		 * In the case where coalesce is restrained (VirtualFree on
		 * Windows), guarded extents are also not cached -- otherwise
		 * during arena destroy / reset, the retained extents would not
		 * be whole regions (i.e. they are split between regular and
		 * guarded).
		 */
		if (!edata_slab_get(edata) || !maps_coalesce) {
			assert(edata_size_get(edata) >= SC_LARGE_MINCLASS ||
			    !maps_coalesce);
			san_unguard_pages_two_sided(tsdn, ehooks, edata,
			    pac->emap);
		}
	}

	ecache_dalloc(tsdn, pac, ehooks, &pac->ecache_dirty, edata);
	/* Purging of deallocated pages is deferred */
	*deferred_work_generated = true;
}

static inline uint64_t
pac_ns_until_purge(tsdn_t *tsdn, decay_t *decay, size_t npages) {
	if (malloc_mutex_trylock(tsdn, &decay->mtx)) {
		/* Use minimal interval if decay is contended. */
		return BACKGROUND_THREAD_DEFERRED_MIN;
	}
	uint64_t result = decay_ns_until_purge(decay, npages,
	    ARENA_DEFERRED_PURGE_NPAGES_THRESHOLD);

	malloc_mutex_unlock(tsdn, &decay->mtx);
	return result;
}

static uint64_t
pac_time_until_deferred_work(tsdn_t *tsdn, pai_t *self) {
	uint64_t time;
	pac_t *pac = (pac_t *)self;

	time = pac_ns_until_purge(tsdn,
	    &pac->decay_dirty,
	    ecache_npages_get(&pac->ecache_dirty));
	if (time == BACKGROUND_THREAD_DEFERRED_MIN) {
		return time;
	}

	uint64_t muzzy = pac_ns_until_purge(tsdn,
	    &pac->decay_muzzy,
	    ecache_npages_get(&pac->ecache_muzzy));
	if (muzzy < time) {
		time = muzzy;
	}
	return time;
}

bool
pac_retain_grow_limit_get_set(tsdn_t *tsdn, pac_t *pac, size_t *old_limit,
    size_t *new_limit) {
	pszind_t new_ind JEMALLOC_CC_SILENCE_INIT(0);
	if (new_limit != NULL) {
		size_t limit = *new_limit;
		/* Grow no more than the new limit. */
		if ((new_ind = sz_psz2ind(limit + 1) - 1) >= SC_NPSIZES) {
			return true;
		}
	}

	malloc_mutex_lock(tsdn, &pac->grow_mtx);
	if (old_limit != NULL) {
		*old_limit = sz_pind2sz(pac->exp_grow.limit);
	}
	if (new_limit != NULL) {
		pac->exp_grow.limit = new_ind;
	}
	malloc_mutex_unlock(tsdn, &pac->grow_mtx);

	return false;
}

static size_t
pac_stash_decayed(tsdn_t *tsdn, pac_t *pac, ecache_t *ecache,
    size_t npages_limit, size_t npages_decay_max,
    edata_list_inactive_t *result) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	ehooks_t *ehooks = pac_ehooks_get(pac);

	/* Stash extents according to npages_limit. */
	size_t nstashed = 0;
	while (nstashed < npages_decay_max) {
		edata_t *edata = ecache_evict(tsdn, pac, ehooks, ecache,
		    npages_limit);
		if (edata == NULL) {
			break;
		}
		edata_list_inactive_append(result, edata);
		nstashed += edata_size_get(edata) >> LG_PAGE;
	}
	return nstashed;
}

static size_t
pac_decay_stashed(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    edata_list_inactive_t *decay_extents) {
	bool err;

	size_t nmadvise = 0;
	size_t nunmapped = 0;
	size_t npurged = 0;

	ehooks_t *ehooks = pac_ehooks_get(pac);

	bool try_muzzy = !fully_decay
	    && pac_decay_ms_get(pac, extent_state_muzzy) != 0;

	for (edata_t *edata = edata_list_inactive_first(decay_extents); edata !=
	    NULL; edata = edata_list_inactive_first(decay_extents)) {
		edata_list_inactive_remove(decay_extents, edata);

		size_t size = edata_size_get(edata);
		size_t npages = size >> LG_PAGE;

		nmadvise++;
		npurged += npages;

		switch (ecache->state) {
		case extent_state_active:
			not_reached();
		case extent_state_dirty:
			if (try_muzzy) {
				err = extent_purge_lazy_wrapper(tsdn, ehooks,
				    edata, /* offset */ 0, size);
				if (!err) {
					ecache_dalloc(tsdn, pac, ehooks,
					    &pac->ecache_muzzy, edata);
					break;
				}
			}
			JEMALLOC_FALLTHROUGH;
		case extent_state_muzzy:
			extent_dalloc_wrapper(tsdn, pac, ehooks, edata);
			nunmapped += npages;
			break;
		case extent_state_retained:
		default:
			not_reached();
		}
	}

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, *pac->stats_mtx);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*pac->stats_mtx),
		    &decay_stats->npurge, 1);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*pac->stats_mtx),
		    &decay_stats->nmadvise, nmadvise);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*pac->stats_mtx),
		    &decay_stats->purged, npurged);
		LOCKEDINT_MTX_UNLOCK(tsdn, *pac->stats_mtx);
		atomic_fetch_sub_zu(&pac->stats->pac_mapped,
		    nunmapped << LG_PAGE, ATOMIC_RELAXED);
	}

	return npurged;
}

/*
 * npages_limit: Decay at most npages_decay_max pages without violating the
 * invariant: (ecache_npages_get(ecache) >= npages_limit).  We need an upper
 * bound on number of pages in order to prevent unbounded growth (namely in
 * stashed), otherwise unbounded new pages could be added to extents during the
 * current decay run, so that the purging thread never finishes.
 */
static void
pac_decay_to_limit(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    size_t npages_limit, size_t npages_decay_max) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 1);

	if (decay->purging || npages_decay_max == 0) {
		return;
	}
	decay->purging = true;
	malloc_mutex_unlock(tsdn, &decay->mtx);

	edata_list_inactive_t decay_extents;
	edata_list_inactive_init(&decay_extents);
	size_t npurge = pac_stash_decayed(tsdn, pac, ecache, npages_limit,
	    npages_decay_max, &decay_extents);
	if (npurge != 0) {
		size_t npurged = pac_decay_stashed(tsdn, pac, decay,
		    decay_stats, ecache, fully_decay, &decay_extents);
		assert(npurged == npurge);
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	decay->purging = false;
}

void
pac_decay_all(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);
	pac_decay_to_limit(tsdn, pac, decay, decay_stats, ecache, fully_decay,
	    /* npages_limit */ 0, ecache_npages_get(ecache));
}

static void
pac_decay_try_purge(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache,
    size_t current_npages, size_t npages_limit) {
	if (current_npages > npages_limit) {
		pac_decay_to_limit(tsdn, pac, decay, decay_stats, ecache,
		    /* fully_decay */ false, npages_limit,
		    current_npages - npages_limit);
	}
}

bool
pac_maybe_decay_purge(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache,
    pac_purge_eagerness_t eagerness) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);

	/* Purge all or nothing if the option is disabled. */
	ssize_t decay_ms = decay_ms_read(decay);
	if (decay_ms <= 0) {
		if (decay_ms == 0) {
			pac_decay_to_limit(tsdn, pac, decay, decay_stats,
			    ecache, /* fully_decay */ false,
			    /* npages_limit */ 0, ecache_npages_get(ecache));
		}
		return false;
	}

	/*
	 * If the deadline has been reached, advance to the current epoch and
	 * purge to the new limit if necessary.  Note that dirty pages created
	 * during the current epoch are not subject to purge until a future
	 * epoch, so as a result purging only happens during epoch advances, or
	 * being triggered by background threads (scheduled event).
	 */
	nstime_t time;
	nstime_init_update(&time);
	size_t npages_current = ecache_npages_get(ecache);
	bool epoch_advanced = decay_maybe_advance_epoch(decay, &time,
	    npages_current);
	if (eagerness == PAC_PURGE_ALWAYS
	    || (epoch_advanced && eagerness == PAC_PURGE_ON_EPOCH_ADVANCE)) {
		size_t npages_limit = decay_npages_limit_get(decay);
		pac_decay_try_purge(tsdn, pac, decay, decay_stats, ecache,
		    npages_current, npages_limit);
	}

	return epoch_advanced;
}

bool
pac_decay_ms_set(tsdn_t *tsdn, pac_t *pac, extent_state_t state,
    ssize_t decay_ms, pac_purge_eagerness_t eagerness) {
	decay_t *decay;
	pac_decay_stats_t *decay_stats;
	ecache_t *ecache;
	pac_decay_data_get(pac, state, &decay, &decay_stats, &ecache);

	if (!decay_ms_valid(decay_ms)) {
		return true;
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	/*
	 * Restart decay backlog from scratch, which may cause many dirty pages
	 * to be immediately purged.  It would conceptually be possible to map
	 * the old backlog onto the new backlog, but there is no justification
	 * for such complexity since decay_ms changes are intended to be
	 * infrequent, either between the {-1, 0, >0} states, or a one-time
	 * arbitrary change during initial arena configuration.
	 */
	nstime_t cur_time;
	nstime_init_update(&cur_time);
	decay_reinit(decay, &cur_time, decay_ms);
	pac_maybe_decay_purge(tsdn, pac, decay, decay_stats, ecache, eagerness);
	malloc_mutex_unlock(tsdn, &decay->mtx);

	return false;
}

ssize_t
pac_decay_ms_get(pac_t *pac, extent_state_t state) {
	decay_t *decay;
	pac_decay_stats_t *decay_stats;
	ecache_t *ecache;
	pac_decay_data_get(pac, state, &decay, &decay_stats, &ecache);
	return decay_ms_read(decay);
}

void
pac_reset(tsdn_t *tsdn, pac_t *pac) {
	/*
	 * No-op for now; purging is still done at the arena-level.  It should
	 * get moved in here, though.
	 */
	(void)tsdn;
	(void)pac;
}

void
pac_destroy(tsdn_t *tsdn, pac_t *pac) {
	assert(ecache_npages_get(&pac->ecache_dirty) == 0);
	assert(ecache_npages_get(&pac->ecache_muzzy) == 0);
	/*
	 * Iterate over the retained extents and destroy them.  This gives the
	 * extent allocator underlying the extent hooks an opportunity to unmap
	 * all retained memory without having to keep its own metadata
	 * structures.  In practice, virtual memory for dss-allocated extents is
	 * leaked here, so best practice is to avoid dss for arenas to be
	 * destroyed, or provide custom extent hooks that track retained
	 * dss-based extents for later reuse.
	 */
	ehooks_t *ehooks = pac_ehooks_get(pac);
	edata_t *edata;
	while ((edata = ecache_evict(tsdn, pac, ehooks,
	    &pac->ecache_retained, 0)) != NULL) {
		extent_destroy_wrapper(tsdn, pac, ehooks, edata);
	}
}
