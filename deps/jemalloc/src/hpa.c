#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpa.h"

#include "jemalloc/internal/fb.h"
#include "jemalloc/internal/witness.h"

#define HPA_EDEN_SIZE (128 * HUGEPAGE)

static edata_t *hpa_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero, bool guarded, bool frequent_reuse,
    bool *deferred_work_generated);
static size_t hpa_alloc_batch(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t nallocs, edata_list_active_t *results, bool *deferred_work_generated);
static bool hpa_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero, bool *deferred_work_generated);
static bool hpa_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool *deferred_work_generated);
static void hpa_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    bool *deferred_work_generated);
static void hpa_dalloc_batch(tsdn_t *tsdn, pai_t *self,
    edata_list_active_t *list, bool *deferred_work_generated);
static uint64_t hpa_time_until_deferred_work(tsdn_t *tsdn, pai_t *self);

bool
hpa_supported() {
#ifdef _WIN32
	/*
	 * At least until the API and implementation is somewhat settled, we
	 * don't want to try to debug the VM subsystem on the hardest-to-test
	 * platform.
	 */
	return false;
#endif
	if (!pages_can_hugify) {
		return false;
	}
	/*
	 * We fundamentally rely on a address-space-hungry growth strategy for
	 * hugepages.
	 */
	if (LG_SIZEOF_PTR != 3) {
		return false;
	}
	/*
	 * If we couldn't detect the value of HUGEPAGE, HUGEPAGE_PAGES becomes
	 * this sentinel value -- see the comment in pages.h.
	 */
	if (HUGEPAGE_PAGES == 1) {
		return false;
	}
	return true;
}

static void
hpa_do_consistency_checks(hpa_shard_t *shard) {
	assert(shard->base != NULL);
}

bool
hpa_central_init(hpa_central_t *central, base_t *base, const hpa_hooks_t *hooks) {
	/* malloc_conf processing should have filtered out these cases. */
	assert(hpa_supported());
	bool err;
	err = malloc_mutex_init(&central->grow_mtx, "hpa_central_grow",
	    WITNESS_RANK_HPA_CENTRAL_GROW, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	err = malloc_mutex_init(&central->mtx, "hpa_central",
	    WITNESS_RANK_HPA_CENTRAL, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	central->base = base;
	central->eden = NULL;
	central->eden_len = 0;
	central->age_counter = 0;
	central->hooks = *hooks;
	return false;
}

static hpdata_t *
hpa_alloc_ps(tsdn_t *tsdn, hpa_central_t *central) {
	return (hpdata_t *)base_alloc(tsdn, central->base, sizeof(hpdata_t),
	    CACHELINE);
}

hpdata_t *
hpa_central_extract(tsdn_t *tsdn, hpa_central_t *central, size_t size,
    bool *oom) {
	/* Don't yet support big allocations; these should get filtered out. */
	assert(size <= HUGEPAGE);
	/*
	 * Should only try to extract from the central allocator if the local
	 * shard is exhausted.  We should hold the grow_mtx on that shard.
	 */
	witness_assert_positive_depth_to_rank(
	    tsdn_witness_tsdp_get(tsdn), WITNESS_RANK_HPA_SHARD_GROW);

	malloc_mutex_lock(tsdn, &central->grow_mtx);
	*oom = false;

	hpdata_t *ps = NULL;

	/* Is eden a perfect fit? */
	if (central->eden != NULL && central->eden_len == HUGEPAGE) {
		ps = hpa_alloc_ps(tsdn, central);
		if (ps == NULL) {
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
		hpdata_init(ps, central->eden, central->age_counter++);
		central->eden = NULL;
		central->eden_len = 0;
		malloc_mutex_unlock(tsdn, &central->grow_mtx);
		return ps;
	}

	/*
	 * We're about to try to allocate from eden by splitting.  If eden is
	 * NULL, we have to allocate it too.  Otherwise, we just have to
	 * allocate an edata_t for the new psset.
	 */
	if (central->eden == NULL) {
		/*
		 * During development, we're primarily concerned with systems
		 * with overcommit.  Eventually, we should be more careful here.
		 */
		bool commit = true;
		/* Allocate address space, bailing if we fail. */
		void *new_eden = pages_map(NULL, HPA_EDEN_SIZE, HUGEPAGE,
		    &commit);
		if (new_eden == NULL) {
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
		ps = hpa_alloc_ps(tsdn, central);
		if (ps == NULL) {
			pages_unmap(new_eden, HPA_EDEN_SIZE);
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
		central->eden = new_eden;
		central->eden_len = HPA_EDEN_SIZE;
	} else {
		/* Eden is already nonempty; only need an edata for ps. */
		ps = hpa_alloc_ps(tsdn, central);
		if (ps == NULL) {
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
	}
	assert(ps != NULL);
	assert(central->eden != NULL);
	assert(central->eden_len > HUGEPAGE);
	assert(central->eden_len % HUGEPAGE == 0);
	assert(HUGEPAGE_ADDR2BASE(central->eden) == central->eden);

	hpdata_init(ps, central->eden, central->age_counter++);

	char *eden_char = (char *)central->eden;
	eden_char += HUGEPAGE;
	central->eden = (void *)eden_char;
	central->eden_len -= HUGEPAGE;

	malloc_mutex_unlock(tsdn, &central->grow_mtx);

	return ps;
}

bool
hpa_shard_init(hpa_shard_t *shard, hpa_central_t *central, emap_t *emap,
    base_t *base, edata_cache_t *edata_cache, unsigned ind,
    const hpa_shard_opts_t *opts) {
	/* malloc_conf processing should have filtered out these cases. */
	assert(hpa_supported());
	bool err;
	err = malloc_mutex_init(&shard->grow_mtx, "hpa_shard_grow",
	    WITNESS_RANK_HPA_SHARD_GROW, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	err = malloc_mutex_init(&shard->mtx, "hpa_shard",
	    WITNESS_RANK_HPA_SHARD, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}

	assert(edata_cache != NULL);
	shard->central = central;
	shard->base = base;
	edata_cache_fast_init(&shard->ecf, edata_cache);
	psset_init(&shard->psset);
	shard->age_counter = 0;
	shard->ind = ind;
	shard->emap = emap;

	shard->opts = *opts;

	shard->npending_purge = 0;
	nstime_init_zero(&shard->last_purge);

	shard->stats.npurge_passes = 0;
	shard->stats.npurges = 0;
	shard->stats.nhugifies = 0;
	shard->stats.ndehugifies = 0;

	/*
	 * Fill these in last, so that if an hpa_shard gets used despite
	 * initialization failing, we'll at least crash instead of just
	 * operating on corrupted data.
	 */
	shard->pai.alloc = &hpa_alloc;
	shard->pai.alloc_batch = &hpa_alloc_batch;
	shard->pai.expand = &hpa_expand;
	shard->pai.shrink = &hpa_shrink;
	shard->pai.dalloc = &hpa_dalloc;
	shard->pai.dalloc_batch = &hpa_dalloc_batch;
	shard->pai.time_until_deferred_work = &hpa_time_until_deferred_work;

	hpa_do_consistency_checks(shard);

	return false;
}

/*
 * Note that the stats functions here follow the usual stats naming conventions;
 * "merge" obtains the stats from some live object of instance, while "accum"
 * only combines the stats from one stats objet to another.  Hence the lack of
 * locking here.
 */
static void
hpa_shard_nonderived_stats_accum(hpa_shard_nonderived_stats_t *dst,
    hpa_shard_nonderived_stats_t *src) {
	dst->npurge_passes += src->npurge_passes;
	dst->npurges += src->npurges;
	dst->nhugifies += src->nhugifies;
	dst->ndehugifies += src->ndehugifies;
}

void
hpa_shard_stats_accum(hpa_shard_stats_t *dst, hpa_shard_stats_t *src) {
	psset_stats_accum(&dst->psset_stats, &src->psset_stats);
	hpa_shard_nonderived_stats_accum(&dst->nonderived_stats,
	    &src->nonderived_stats);
}

void
hpa_shard_stats_merge(tsdn_t *tsdn, hpa_shard_t *shard,
    hpa_shard_stats_t *dst) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_lock(tsdn, &shard->grow_mtx);
	malloc_mutex_lock(tsdn, &shard->mtx);
	psset_stats_accum(&dst->psset_stats, &shard->psset.stats);
	hpa_shard_nonderived_stats_accum(&dst->nonderived_stats, &shard->stats);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	malloc_mutex_unlock(tsdn, &shard->grow_mtx);
}

static bool
hpa_good_hugification_candidate(hpa_shard_t *shard, hpdata_t *ps) {
	/*
	 * Note that this needs to be >= rather than just >, because of the
	 * important special case in which the hugification threshold is exactly
	 * HUGEPAGE.
	 */
	return hpdata_nactive_get(ps) * PAGE
	    >= shard->opts.hugification_threshold;
}

static size_t
hpa_adjusted_ndirty(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	return psset_ndirty(&shard->psset) - shard->npending_purge;
}

static size_t
hpa_ndirty_max(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	if (shard->opts.dirty_mult == (fxp_t)-1) {
		return (size_t)-1;
	}
	return fxp_mul_frac(psset_nactive(&shard->psset),
	    shard->opts.dirty_mult);
}

static bool
hpa_hugify_blocked_by_ndirty(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	hpdata_t *to_hugify = psset_pick_hugify(&shard->psset);
	if (to_hugify == NULL) {
		return false;
	}
	return hpa_adjusted_ndirty(tsdn, shard)
	    + hpdata_nretained_get(to_hugify) > hpa_ndirty_max(tsdn, shard);
}

static bool
hpa_should_purge(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	if (hpa_adjusted_ndirty(tsdn, shard) > hpa_ndirty_max(tsdn, shard)) {
		return true;
	}
	if (hpa_hugify_blocked_by_ndirty(tsdn, shard)) {
		return true;
	}
	return false;
}

static void
hpa_update_purge_hugify_eligibility(tsdn_t *tsdn, hpa_shard_t *shard,
    hpdata_t *ps) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	if (hpdata_changing_state_get(ps)) {
		hpdata_purge_allowed_set(ps, false);
		hpdata_disallow_hugify(ps);
		return;
	}
	/*
	 * Hugepages are distinctly costly to purge, so try to avoid it unless
	 * they're *particularly* full of dirty pages.  Eventually, we should
	 * use a smarter / more dynamic heuristic for situations where we have
	 * to manually hugify.
	 *
	 * In situations where we don't manually hugify, this problem is
	 * reduced.  The "bad" situation we're trying to avoid is one's that's
	 * common in some Linux configurations (where both enabled and defrag
	 * are set to madvise) that can lead to long latency spikes on the first
	 * access after a hugification.  The ideal policy in such configurations
	 * is probably time-based for both purging and hugifying; only hugify a
	 * hugepage if it's met the criteria for some extended period of time,
	 * and only dehugify it if it's failed to meet the criteria for an
	 * extended period of time.  When background threads are on, we should
	 * try to take this hit on one of them, as well.
	 *
	 * I think the ideal setting is THP always enabled, and defrag set to
	 * deferred; in that case we don't need any explicit calls on the
	 * allocator's end at all; we just try to pack allocations in a
	 * hugepage-friendly manner and let the OS hugify in the background.
	 */
	hpdata_purge_allowed_set(ps, hpdata_ndirty_get(ps) > 0);
	if (hpa_good_hugification_candidate(shard, ps)
	    && !hpdata_huge_get(ps)) {
		nstime_t now;
		shard->central->hooks.curtime(&now, /* first_reading */ true);
		hpdata_allow_hugify(ps, now);
	}
	/*
	 * Once a hugepage has become eligible for hugification, we don't mark
	 * it as ineligible just because it stops meeting the criteria (this
	 * could lead to situations where a hugepage that spends most of its
	 * time meeting the criteria never quite getting hugified if there are
	 * intervening deallocations).  The idea is that the hugification delay
	 * will allow them to get purged, reseting their "hugify-allowed" bit.
	 * If they don't get purged, then the hugification isn't hurting and
	 * might help.  As an exception, we don't hugify hugepages that are now
	 * empty; it definitely doesn't help there until the hugepage gets
	 * reused, which is likely not for a while.
	 */
	if (hpdata_nactive_get(ps) == 0) {
		hpdata_disallow_hugify(ps);
	}
}

static bool
hpa_shard_has_deferred_work(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	hpdata_t *to_hugify = psset_pick_hugify(&shard->psset);
	return to_hugify != NULL || hpa_should_purge(tsdn, shard);
}

/* Returns whether or not we purged anything. */
static bool
hpa_try_purge(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);

	hpdata_t *to_purge = psset_pick_purge(&shard->psset);
	if (to_purge == NULL) {
		return false;
	}
	assert(hpdata_purge_allowed_get(to_purge));
	assert(!hpdata_changing_state_get(to_purge));

	/*
	 * Don't let anyone else purge or hugify this page while
	 * we're purging it (allocations and deallocations are
	 * OK).
	 */
	psset_update_begin(&shard->psset, to_purge);
	assert(hpdata_alloc_allowed_get(to_purge));
	hpdata_mid_purge_set(to_purge, true);
	hpdata_purge_allowed_set(to_purge, false);
	hpdata_disallow_hugify(to_purge);
	/*
	 * Unlike with hugification (where concurrent
	 * allocations are allowed), concurrent allocation out
	 * of a hugepage being purged is unsafe; we might hand
	 * out an extent for an allocation and then purge it
	 * (clearing out user data).
	 */
	hpdata_alloc_allowed_set(to_purge, false);
	psset_update_end(&shard->psset, to_purge);

	/* Gather all the metadata we'll need during the purge. */
	bool dehugify = hpdata_huge_get(to_purge);
	hpdata_purge_state_t purge_state;
	size_t num_to_purge = hpdata_purge_begin(to_purge, &purge_state);

	shard->npending_purge += num_to_purge;

	malloc_mutex_unlock(tsdn, &shard->mtx);

	/* Actually do the purging, now that the lock is dropped. */
	if (dehugify) {
		shard->central->hooks.dehugify(hpdata_addr_get(to_purge),
		    HUGEPAGE);
	}
	size_t total_purged = 0;
	uint64_t purges_this_pass = 0;
	void *purge_addr;
	size_t purge_size;
	while (hpdata_purge_next(to_purge, &purge_state, &purge_addr,
	    &purge_size)) {
		total_purged += purge_size;
		assert(total_purged <= HUGEPAGE);
		purges_this_pass++;
		shard->central->hooks.purge(purge_addr, purge_size);
	}

	malloc_mutex_lock(tsdn, &shard->mtx);
	/* The shard updates */
	shard->npending_purge -= num_to_purge;
	shard->stats.npurge_passes++;
	shard->stats.npurges += purges_this_pass;
	shard->central->hooks.curtime(&shard->last_purge,
	    /* first_reading */ false);
	if (dehugify) {
		shard->stats.ndehugifies++;
	}

	/* The hpdata updates. */
	psset_update_begin(&shard->psset, to_purge);
	if (dehugify) {
		hpdata_dehugify(to_purge);
	}
	hpdata_purge_end(to_purge, &purge_state);
	hpdata_mid_purge_set(to_purge, false);

	hpdata_alloc_allowed_set(to_purge, true);
	hpa_update_purge_hugify_eligibility(tsdn, shard, to_purge);

	psset_update_end(&shard->psset, to_purge);

	return true;
}

/* Returns whether or not we hugified anything. */
static bool
hpa_try_hugify(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);

	if (hpa_hugify_blocked_by_ndirty(tsdn, shard)) {
		return false;
	}

	hpdata_t *to_hugify = psset_pick_hugify(&shard->psset);
	if (to_hugify == NULL) {
		return false;
	}
	assert(hpdata_hugify_allowed_get(to_hugify));
	assert(!hpdata_changing_state_get(to_hugify));

	/* Make sure that it's been hugifiable for long enough. */
	nstime_t time_hugify_allowed = hpdata_time_hugify_allowed(to_hugify);
	uint64_t millis = shard->central->hooks.ms_since(&time_hugify_allowed);
	if (millis < shard->opts.hugify_delay_ms) {
		return false;
	}

	/*
	 * Don't let anyone else purge or hugify this page while
	 * we're hugifying it (allocations and deallocations are
	 * OK).
	 */
	psset_update_begin(&shard->psset, to_hugify);
	hpdata_mid_hugify_set(to_hugify, true);
	hpdata_purge_allowed_set(to_hugify, false);
	hpdata_disallow_hugify(to_hugify);
	assert(hpdata_alloc_allowed_get(to_hugify));
	psset_update_end(&shard->psset, to_hugify);

	malloc_mutex_unlock(tsdn, &shard->mtx);

	shard->central->hooks.hugify(hpdata_addr_get(to_hugify), HUGEPAGE);

	malloc_mutex_lock(tsdn, &shard->mtx);
	shard->stats.nhugifies++;

	psset_update_begin(&shard->psset, to_hugify);
	hpdata_hugify(to_hugify);
	hpdata_mid_hugify_set(to_hugify, false);
	hpa_update_purge_hugify_eligibility(tsdn, shard, to_hugify);
	psset_update_end(&shard->psset, to_hugify);

	return true;
}

/*
 * Execution of deferred work is forced if it's triggered by an explicit
 * hpa_shard_do_deferred_work() call.
 */
static void
hpa_shard_maybe_do_deferred_work(tsdn_t *tsdn, hpa_shard_t *shard,
    bool forced) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	if (!forced && shard->opts.deferral_allowed) {
		return;
	}
	/*
	 * If we're on a background thread, do work so long as there's work to
	 * be done.  Otherwise, bound latency to not be *too* bad by doing at
	 * most a small fixed number of operations.
	 */
	bool hugified = false;
	bool purged = false;
	size_t max_ops = (forced ? (size_t)-1 : 16);
	size_t nops = 0;
	do {
		/*
		 * Always purge before hugifying, to make sure we get some
		 * ability to hit our quiescence targets.
		 */
		purged = false;
		while (hpa_should_purge(tsdn, shard) && nops < max_ops) {
			purged = hpa_try_purge(tsdn, shard);
			if (purged) {
				nops++;
			}
		}
		hugified = hpa_try_hugify(tsdn, shard);
		if (hugified) {
			nops++;
		}
		malloc_mutex_assert_owner(tsdn, &shard->mtx);
		malloc_mutex_assert_owner(tsdn, &shard->mtx);
	} while ((hugified || purged) && nops < max_ops);
}

static edata_t *
hpa_try_alloc_one_no_grow(tsdn_t *tsdn, hpa_shard_t *shard, size_t size,
    bool *oom) {
	bool err;
	edata_t *edata = edata_cache_fast_get(tsdn, &shard->ecf);
	if (edata == NULL) {
		*oom = true;
		return NULL;
	}

	hpdata_t *ps = psset_pick_alloc(&shard->psset, size);
	if (ps == NULL) {
		edata_cache_fast_put(tsdn, &shard->ecf, edata);
		return NULL;
	}

	psset_update_begin(&shard->psset, ps);

	if (hpdata_empty(ps)) {
		/*
		 * If the pageslab used to be empty, treat it as though it's
		 * brand new for fragmentation-avoidance purposes; what we're
		 * trying to approximate is the age of the allocations *in* that
		 * pageslab, and the allocations in the new pageslab are
		 * definitionally the youngest in this hpa shard.
		 */
		hpdata_age_set(ps, shard->age_counter++);
	}

	void *addr = hpdata_reserve_alloc(ps, size);
	edata_init(edata, shard->ind, addr, size, /* slab */ false,
	    SC_NSIZES, /* sn */ hpdata_age_get(ps), extent_state_active,
	    /* zeroed */ false, /* committed */ true, EXTENT_PAI_HPA,
	    EXTENT_NOT_HEAD);
	edata_ps_set(edata, ps);

	/*
	 * This could theoretically be moved outside of the critical section,
	 * but that introduces the potential for a race.  Without the lock, the
	 * (initially nonempty, since this is the reuse pathway) pageslab we
	 * allocated out of could become otherwise empty while the lock is
	 * dropped.  This would force us to deal with a pageslab eviction down
	 * the error pathway, which is a pain.
	 */
	err = emap_register_boundary(tsdn, shard->emap, edata,
	    SC_NSIZES, /* slab */ false);
	if (err) {
		hpdata_unreserve(ps, edata_addr_get(edata),
		    edata_size_get(edata));
		/*
		 * We should arguably reset dirty state here, but this would
		 * require some sort of prepare + commit functionality that's a
		 * little much to deal with for now.
		 *
		 * We don't have a do_deferred_work down this pathway, on the
		 * principle that we didn't *really* affect shard state (we
		 * tweaked the stats, but our tweaks weren't really accurate).
		 */
		psset_update_end(&shard->psset, ps);
		edata_cache_fast_put(tsdn, &shard->ecf, edata);
		*oom = true;
		return NULL;
	}

	hpa_update_purge_hugify_eligibility(tsdn, shard, ps);
	psset_update_end(&shard->psset, ps);
	return edata;
}

static size_t
hpa_try_alloc_batch_no_grow(tsdn_t *tsdn, hpa_shard_t *shard, size_t size,
    bool *oom, size_t nallocs, edata_list_active_t *results,
    bool *deferred_work_generated) {
	malloc_mutex_lock(tsdn, &shard->mtx);
	size_t nsuccess = 0;
	for (; nsuccess < nallocs; nsuccess++) {
		edata_t *edata = hpa_try_alloc_one_no_grow(tsdn, shard, size,
		    oom);
		if (edata == NULL) {
			break;
		}
		edata_list_active_append(results, edata);
	}

	hpa_shard_maybe_do_deferred_work(tsdn, shard, /* forced */ false);
	*deferred_work_generated = hpa_shard_has_deferred_work(tsdn, shard);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	return nsuccess;
}

static size_t
hpa_alloc_batch_psset(tsdn_t *tsdn, hpa_shard_t *shard, size_t size,
    size_t nallocs, edata_list_active_t *results,
    bool *deferred_work_generated) {
	assert(size <= shard->opts.slab_max_alloc);
	bool oom = false;

	size_t nsuccess = hpa_try_alloc_batch_no_grow(tsdn, shard, size, &oom,
	    nallocs, results, deferred_work_generated);

	if (nsuccess == nallocs || oom) {
		return nsuccess;
	}

	/*
	 * We didn't OOM, but weren't able to fill everything requested of us;
	 * try to grow.
	 */
	malloc_mutex_lock(tsdn, &shard->grow_mtx);
	/*
	 * Check for grow races; maybe some earlier thread expanded the psset
	 * in between when we dropped the main mutex and grabbed the grow mutex.
	 */
	nsuccess += hpa_try_alloc_batch_no_grow(tsdn, shard, size, &oom,
	    nallocs - nsuccess, results, deferred_work_generated);
	if (nsuccess == nallocs || oom) {
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		return nsuccess;
	}

	/*
	 * Note that we don't hold shard->mtx here (while growing);
	 * deallocations (and allocations of smaller sizes) may still succeed
	 * while we're doing this potentially expensive system call.
	 */
	hpdata_t *ps = hpa_central_extract(tsdn, shard->central, size, &oom);
	if (ps == NULL) {
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		return nsuccess;
	}

	/*
	 * We got the pageslab; allocate from it.  This does an unlock followed
	 * by a lock on the same mutex, and holds the grow mutex while doing
	 * deferred work, but this is an uncommon path; the simplicity is worth
	 * it.
	 */
	malloc_mutex_lock(tsdn, &shard->mtx);
	psset_insert(&shard->psset, ps);
	malloc_mutex_unlock(tsdn, &shard->mtx);

	nsuccess += hpa_try_alloc_batch_no_grow(tsdn, shard, size, &oom,
	    nallocs - nsuccess, results, deferred_work_generated);
	/*
	 * Drop grow_mtx before doing deferred work; other threads blocked on it
	 * should be allowed to proceed while we're working.
	 */
	malloc_mutex_unlock(tsdn, &shard->grow_mtx);

	return nsuccess;
}

static hpa_shard_t *
hpa_from_pai(pai_t *self) {
	assert(self->alloc = &hpa_alloc);
	assert(self->expand = &hpa_expand);
	assert(self->shrink = &hpa_shrink);
	assert(self->dalloc = &hpa_dalloc);
	return (hpa_shard_t *)self;
}

static size_t
hpa_alloc_batch(tsdn_t *tsdn, pai_t *self, size_t size, size_t nallocs,
    edata_list_active_t *results, bool *deferred_work_generated) {
	assert(nallocs > 0);
	assert((size & PAGE_MASK) == 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	hpa_shard_t *shard = hpa_from_pai(self);

	if (size > shard->opts.slab_max_alloc) {
		return 0;
	}

	size_t nsuccess = hpa_alloc_batch_psset(tsdn, shard, size, nallocs,
	    results, deferred_work_generated);

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	/*
	 * Guard the sanity checks with config_debug because the loop cannot be
	 * proven non-circular by the compiler, even if everything within the
	 * loop is optimized away.
	 */
	if (config_debug) {
		edata_t *edata;
		ql_foreach(edata, &results->head, ql_link_active) {
			emap_assert_mapped(tsdn, shard->emap, edata);
			assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
			assert(edata_state_get(edata) == extent_state_active);
			assert(edata_arena_ind_get(edata) == shard->ind);
			assert(edata_szind_get_maybe_invalid(edata) ==
			    SC_NSIZES);
			assert(!edata_slab_get(edata));
			assert(edata_committed_get(edata));
			assert(edata_base_get(edata) == edata_addr_get(edata));
			assert(edata_base_get(edata) != NULL);
		}
	}
	return nsuccess;
}

static edata_t *
hpa_alloc(tsdn_t *tsdn, pai_t *self, size_t size, size_t alignment, bool zero,
    bool guarded, bool frequent_reuse, bool *deferred_work_generated) {
	assert((size & PAGE_MASK) == 0);
	assert(!guarded);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	/* We don't handle alignment or zeroing for now. */
	if (alignment > PAGE || zero) {
		return NULL;
	}
	/*
	 * An alloc with alignment == PAGE and zero == false is equivalent to a
	 * batch alloc of 1.  Just do that, so we can share code.
	 */
	edata_list_active_t results;
	edata_list_active_init(&results);
	size_t nallocs = hpa_alloc_batch(tsdn, self, size, /* nallocs */ 1,
	    &results, deferred_work_generated);
	assert(nallocs == 0 || nallocs == 1);
	edata_t *edata = edata_list_active_first(&results);
	return edata;
}

static bool
hpa_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata, size_t old_size,
    size_t new_size, bool zero, bool *deferred_work_generated) {
	/* Expand not yet supported. */
	return true;
}

static bool
hpa_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool *deferred_work_generated) {
	/* Shrink not yet supported. */
	return true;
}

static void
hpa_dalloc_prepare_unlocked(tsdn_t *tsdn, hpa_shard_t *shard, edata_t *edata) {
	malloc_mutex_assert_not_owner(tsdn, &shard->mtx);

	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(edata_state_get(edata) == extent_state_active);
	assert(edata_arena_ind_get(edata) == shard->ind);
	assert(edata_szind_get_maybe_invalid(edata) == SC_NSIZES);
	assert(edata_committed_get(edata));
	assert(edata_base_get(edata) != NULL);

	/*
	 * Another thread shouldn't be trying to touch the metadata of an
	 * allocation being freed.  The one exception is a merge attempt from a
	 * lower-addressed PAC extent; in this case we have a nominal race on
	 * the edata metadata bits, but in practice the fact that the PAI bits
	 * are different will prevent any further access.  The race is bad, but
	 * benign in practice, and the long term plan is to track enough state
	 * in the rtree to prevent these merge attempts in the first place.
	 */
	edata_addr_set(edata, edata_base_get(edata));
	edata_zeroed_set(edata, false);
	emap_deregister_boundary(tsdn, shard->emap, edata);
}

static void
hpa_dalloc_locked(tsdn_t *tsdn, hpa_shard_t *shard, edata_t *edata) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);

	/*
	 * Release the metadata early, to avoid having to remember to do it
	 * while we're also doing tricky purging logic.  First, we need to grab
	 * a few bits of metadata from it.
	 *
	 * Note that the shard mutex protects ps's metadata too; it wouldn't be
	 * correct to try to read most information out of it without the lock.
	 */
	hpdata_t *ps = edata_ps_get(edata);
	/* Currently, all edatas come from pageslabs. */
	assert(ps != NULL);
	void *unreserve_addr = edata_addr_get(edata);
	size_t unreserve_size = edata_size_get(edata);
	edata_cache_fast_put(tsdn, &shard->ecf, edata);

	psset_update_begin(&shard->psset, ps);
	hpdata_unreserve(ps, unreserve_addr, unreserve_size);
	hpa_update_purge_hugify_eligibility(tsdn, shard, ps);
	psset_update_end(&shard->psset, ps);
}

static void
hpa_dalloc_batch(tsdn_t *tsdn, pai_t *self, edata_list_active_t *list,
    bool *deferred_work_generated) {
	hpa_shard_t *shard = hpa_from_pai(self);

	edata_t *edata;
	ql_foreach(edata, &list->head, ql_link_active) {
		hpa_dalloc_prepare_unlocked(tsdn, shard, edata);
	}

	malloc_mutex_lock(tsdn, &shard->mtx);
	/* Now, remove from the list. */
	while ((edata = edata_list_active_first(list)) != NULL) {
		edata_list_active_remove(list, edata);
		hpa_dalloc_locked(tsdn, shard, edata);
	}
	hpa_shard_maybe_do_deferred_work(tsdn, shard, /* forced */ false);
	*deferred_work_generated =
	    hpa_shard_has_deferred_work(tsdn, shard);

	malloc_mutex_unlock(tsdn, &shard->mtx);
}

static void
hpa_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    bool *deferred_work_generated) {
	assert(!edata_guarded_get(edata));
	/* Just a dalloc_batch of size 1; this lets us share logic. */
	edata_list_active_t dalloc_list;
	edata_list_active_init(&dalloc_list);
	edata_list_active_append(&dalloc_list, edata);
	hpa_dalloc_batch(tsdn, self, &dalloc_list, deferred_work_generated);
}

/*
 * Calculate time until either purging or hugification ought to happen.
 * Called by background threads.
 */
static uint64_t
hpa_time_until_deferred_work(tsdn_t *tsdn, pai_t *self) {
	hpa_shard_t *shard = hpa_from_pai(self);
	uint64_t time_ns = BACKGROUND_THREAD_DEFERRED_MAX;

	malloc_mutex_lock(tsdn, &shard->mtx);

	hpdata_t *to_hugify = psset_pick_hugify(&shard->psset);
	if (to_hugify != NULL) {
		nstime_t time_hugify_allowed =
		    hpdata_time_hugify_allowed(to_hugify);
		uint64_t since_hugify_allowed_ms =
		    shard->central->hooks.ms_since(&time_hugify_allowed);
		/*
		 * If not enough time has passed since hugification was allowed,
		 * sleep for the rest.
		 */
		if (since_hugify_allowed_ms < shard->opts.hugify_delay_ms) {
			time_ns = shard->opts.hugify_delay_ms -
			    since_hugify_allowed_ms;
			time_ns *= 1000 * 1000;
		} else {
			malloc_mutex_unlock(tsdn, &shard->mtx);
			return BACKGROUND_THREAD_DEFERRED_MIN;
		}
	}

	if (hpa_should_purge(tsdn, shard)) {
		/*
		 * If we haven't purged before, no need to check interval
		 * between purges. Simply purge as soon as possible.
		 */
		if (shard->stats.npurge_passes == 0) {
			malloc_mutex_unlock(tsdn, &shard->mtx);
			return BACKGROUND_THREAD_DEFERRED_MIN;
		}
		uint64_t since_last_purge_ms = shard->central->hooks.ms_since(
		    &shard->last_purge);

		if (since_last_purge_ms < shard->opts.min_purge_interval_ms) {
			uint64_t until_purge_ns;
			until_purge_ns = shard->opts.min_purge_interval_ms -
			    since_last_purge_ms;
			until_purge_ns *= 1000 * 1000;

			if (until_purge_ns < time_ns) {
				time_ns = until_purge_ns;
			}
		} else {
			time_ns = BACKGROUND_THREAD_DEFERRED_MIN;
		}
	}
	malloc_mutex_unlock(tsdn, &shard->mtx);
	return time_ns;
}

void
hpa_shard_disable(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_lock(tsdn, &shard->mtx);
	edata_cache_fast_disable(tsdn, &shard->ecf);
	malloc_mutex_unlock(tsdn, &shard->mtx);
}

static void
hpa_shard_assert_stats_empty(psset_bin_stats_t *bin_stats) {
	assert(bin_stats->npageslabs == 0);
	assert(bin_stats->nactive == 0);
}

static void
hpa_assert_empty(tsdn_t *tsdn, hpa_shard_t *shard, psset_t *psset) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	for (int huge = 0; huge <= 1; huge++) {
		hpa_shard_assert_stats_empty(&psset->stats.full_slabs[huge]);
		for (pszind_t i = 0; i < PSSET_NPSIZES; i++) {
			hpa_shard_assert_stats_empty(
			    &psset->stats.nonfull_slabs[i][huge]);
		}
	}
}

void
hpa_shard_destroy(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_do_consistency_checks(shard);
	/*
	 * By the time we're here, the arena code should have dalloc'd all the
	 * active extents, which means we should have eventually evicted
	 * everything from the psset, so it shouldn't be able to serve even a
	 * 1-page allocation.
	 */
	if (config_debug) {
		malloc_mutex_lock(tsdn, &shard->mtx);
		hpa_assert_empty(tsdn, shard, &shard->psset);
		malloc_mutex_unlock(tsdn, &shard->mtx);
	}
	hpdata_t *ps;
	while ((ps = psset_pick_alloc(&shard->psset, PAGE)) != NULL) {
		/* There should be no allocations anywhere. */
		assert(hpdata_empty(ps));
		psset_remove(&shard->psset, ps);
		shard->central->hooks.unmap(hpdata_addr_get(ps), HUGEPAGE);
	}
}

void
hpa_shard_set_deferral_allowed(tsdn_t *tsdn, hpa_shard_t *shard,
    bool deferral_allowed) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_lock(tsdn, &shard->mtx);
	bool deferral_previously_allowed = shard->opts.deferral_allowed;
	shard->opts.deferral_allowed = deferral_allowed;
	if (deferral_previously_allowed && !deferral_allowed) {
		hpa_shard_maybe_do_deferred_work(tsdn, shard,
		    /* forced */ true);
	}
	malloc_mutex_unlock(tsdn, &shard->mtx);
}

void
hpa_shard_do_deferred_work(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_lock(tsdn, &shard->mtx);
	hpa_shard_maybe_do_deferred_work(tsdn, shard, /* forced */ true);
	malloc_mutex_unlock(tsdn, &shard->mtx);
}

void
hpa_shard_prefork3(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_prefork(tsdn, &shard->grow_mtx);
}

void
hpa_shard_prefork4(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_prefork(tsdn, &shard->mtx);
}

void
hpa_shard_postfork_parent(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_postfork_parent(tsdn, &shard->grow_mtx);
	malloc_mutex_postfork_parent(tsdn, &shard->mtx);
}

void
hpa_shard_postfork_child(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_do_consistency_checks(shard);

	malloc_mutex_postfork_child(tsdn, &shard->grow_mtx);
	malloc_mutex_postfork_child(tsdn, &shard->mtx);
}
