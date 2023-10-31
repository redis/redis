#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/decay.h"
#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/util.h"

JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS

/******************************************************************************/
/* Data. */

/*
 * Define names for both unininitialized and initialized phases, so that
 * options and mallctl processing are straightforward.
 */
const char *percpu_arena_mode_names[] = {
	"percpu",
	"phycpu",
	"disabled",
	"percpu",
	"phycpu"
};
percpu_arena_mode_t opt_percpu_arena = PERCPU_ARENA_DEFAULT;

ssize_t opt_dirty_decay_ms = DIRTY_DECAY_MS_DEFAULT;
ssize_t opt_muzzy_decay_ms = MUZZY_DECAY_MS_DEFAULT;

static atomic_zd_t dirty_decay_ms_default;
static atomic_zd_t muzzy_decay_ms_default;

emap_t arena_emap_global;
pa_central_t arena_pa_central_global;

div_info_t arena_binind_div_info[SC_NBINS];

size_t opt_oversize_threshold = OVERSIZE_THRESHOLD_DEFAULT;
size_t oversize_threshold = OVERSIZE_THRESHOLD_DEFAULT;

uint32_t arena_bin_offsets[SC_NBINS];
static unsigned nbins_total;

static unsigned huge_arena_ind;

const arena_config_t arena_config_default = {
	/* .extent_hooks = */ (extent_hooks_t *)&ehooks_default_extent_hooks,
	/* .metadata_use_hooks = */ true,
};

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static bool arena_decay_dirty(tsdn_t *tsdn, arena_t *arena,
    bool is_background_thread, bool all);
static void arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, edata_t *slab,
    bin_t *bin);
static void
arena_maybe_do_deferred_work(tsdn_t *tsdn, arena_t *arena, decay_t *decay,
    size_t npages_new);

/******************************************************************************/

void
arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy) {
	*nthreads += arena_nthreads_get(arena, false);
	*dss = dss_prec_names[arena_dss_prec_get(arena)];
	*dirty_decay_ms = arena_decay_ms_get(arena, extent_state_dirty);
	*muzzy_decay_ms = arena_decay_ms_get(arena, extent_state_muzzy);
	pa_shard_basic_stats_merge(&arena->pa_shard, nactive, ndirty, nmuzzy);
}

void
arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy, arena_stats_t *astats,
    bin_stats_data_t *bstats, arena_stats_large_t *lstats,
    pac_estats_t *estats, hpa_shard_stats_t *hpastats, sec_stats_t *secstats) {
	cassert(config_stats);

	arena_basic_stats_merge(tsdn, arena, nthreads, dss, dirty_decay_ms,
	    muzzy_decay_ms, nactive, ndirty, nmuzzy);

	size_t base_allocated, base_resident, base_mapped, metadata_thp;
	base_stats_get(tsdn, arena->base, &base_allocated, &base_resident,
	    &base_mapped, &metadata_thp);
	size_t pac_mapped_sz = pac_mapped(&arena->pa_shard.pac);
	astats->mapped += base_mapped + pac_mapped_sz;
	astats->resident += base_resident;

	LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);

	astats->base += base_allocated;
	atomic_load_add_store_zu(&astats->internal, arena_internal_get(arena));
	astats->metadata_thp += metadata_thp;

	for (szind_t i = 0; i < SC_NSIZES - SC_NBINS; i++) {
		uint64_t nmalloc = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].nmalloc);
		locked_inc_u64_unsynchronized(&lstats[i].nmalloc, nmalloc);
		astats->nmalloc_large += nmalloc;

		uint64_t ndalloc = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].ndalloc);
		locked_inc_u64_unsynchronized(&lstats[i].ndalloc, ndalloc);
		astats->ndalloc_large += ndalloc;

		uint64_t nrequests = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].nrequests);
		locked_inc_u64_unsynchronized(&lstats[i].nrequests,
		    nmalloc + nrequests);
		astats->nrequests_large += nmalloc + nrequests;

		/* nfill == nmalloc for large currently. */
		locked_inc_u64_unsynchronized(&lstats[i].nfills, nmalloc);
		astats->nfills_large += nmalloc;

		uint64_t nflush = locked_read_u64(tsdn,
		    LOCKEDINT_MTX(arena->stats.mtx),
		    &arena->stats.lstats[i].nflushes);
		locked_inc_u64_unsynchronized(&lstats[i].nflushes, nflush);
		astats->nflushes_large += nflush;

		assert(nmalloc >= ndalloc);
		assert(nmalloc - ndalloc <= SIZE_T_MAX);
		size_t curlextents = (size_t)(nmalloc - ndalloc);
		lstats[i].curlextents += curlextents;
		astats->allocated_large +=
		    curlextents * sz_index2size(SC_NBINS + i);
	}

	pa_shard_stats_merge(tsdn, &arena->pa_shard, &astats->pa_shard_stats,
	    estats, hpastats, secstats, &astats->resident);

	LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);

	/* Currently cached bytes and sanitizer-stashed bytes in tcache. */
	astats->tcache_bytes = 0;
	astats->tcache_stashed_bytes = 0;
	malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
	cache_bin_array_descriptor_t *descriptor;
	ql_foreach(descriptor, &arena->cache_bin_array_descriptor_ql, link) {
		for (szind_t i = 0; i < nhbins; i++) {
			cache_bin_t *cache_bin = &descriptor->bins[i];
			cache_bin_sz_t ncached, nstashed;
			cache_bin_nitems_get_remote(cache_bin,
			    &tcache_bin_info[i], &ncached, &nstashed);

			astats->tcache_bytes += ncached * sz_index2size(i);
			astats->tcache_stashed_bytes += nstashed *
			    sz_index2size(i);
		}
	}
	malloc_mutex_prof_read(tsdn,
	    &astats->mutex_prof_data[arena_prof_mutex_tcache_list],
	    &arena->tcache_ql_mtx);
	malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);

#define READ_ARENA_MUTEX_PROF_DATA(mtx, ind)				\
    malloc_mutex_lock(tsdn, &arena->mtx);				\
    malloc_mutex_prof_read(tsdn, &astats->mutex_prof_data[ind],		\
        &arena->mtx);							\
    malloc_mutex_unlock(tsdn, &arena->mtx);

	/* Gather per arena mutex profiling data. */
	READ_ARENA_MUTEX_PROF_DATA(large_mtx, arena_prof_mutex_large);
	READ_ARENA_MUTEX_PROF_DATA(base->mtx,
	    arena_prof_mutex_base);
#undef READ_ARENA_MUTEX_PROF_DATA
	pa_shard_mtx_stats_read(tsdn, &arena->pa_shard,
	    astats->mutex_prof_data);

	nstime_copy(&astats->uptime, &arena->create_time);
	nstime_update(&astats->uptime);
	nstime_subtract(&astats->uptime, &arena->create_time);

	for (szind_t i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			bin_stats_merge(tsdn, &bstats[i],
			    arena_get_bin(arena, i, j));
		}
	}
}

static void
arena_background_thread_inactivity_check(tsdn_t *tsdn, arena_t *arena,
    bool is_background_thread) {
	if (!background_thread_enabled() || is_background_thread) {
		return;
	}
	background_thread_info_t *info =
	    arena_background_thread_info_get(arena);
	if (background_thread_indefinite_sleep(info)) {
		arena_maybe_do_deferred_work(tsdn, arena,
		    &arena->pa_shard.pac.decay_dirty, 0);
	}
}

/*
 * React to deferred work generated by a PAI function.
 */
void arena_handle_deferred_work(tsdn_t *tsdn, arena_t *arena) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (decay_immediately(&arena->pa_shard.pac.decay_dirty)) {
		arena_decay_dirty(tsdn, arena, false, true);
	}
	arena_background_thread_inactivity_check(tsdn, arena, false);
}

static void *
arena_slab_reg_alloc(edata_t *slab, const bin_info_t *bin_info) {
	void *ret;
	slab_data_t *slab_data = edata_slab_data_get(slab);
	size_t regind;

	assert(edata_nfree_get(slab) > 0);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(slab_data->bitmap, &bin_info->bitmap_info);
	ret = (void *)((uintptr_t)edata_addr_get(slab) +
	    (uintptr_t)(bin_info->reg_size * regind));
	edata_nfree_dec(slab);
	return ret;
}

static void
arena_slab_reg_alloc_batch(edata_t *slab, const bin_info_t *bin_info,
			   unsigned cnt, void** ptrs) {
	slab_data_t *slab_data = edata_slab_data_get(slab);

	assert(edata_nfree_get(slab) >= cnt);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

#if (! defined JEMALLOC_INTERNAL_POPCOUNTL) || (defined BITMAP_USE_TREE)
	for (unsigned i = 0; i < cnt; i++) {
		size_t regind = bitmap_sfu(slab_data->bitmap,
					   &bin_info->bitmap_info);
		*(ptrs + i) = (void *)((uintptr_t)edata_addr_get(slab) +
		    (uintptr_t)(bin_info->reg_size * regind));
	}
#else
	unsigned group = 0;
	bitmap_t g = slab_data->bitmap[group];
	unsigned i = 0;
	while (i < cnt) {
		while (g == 0) {
			g = slab_data->bitmap[++group];
		}
		size_t shift = group << LG_BITMAP_GROUP_NBITS;
		size_t pop = popcount_lu(g);
		if (pop > (cnt - i)) {
			pop = cnt - i;
		}

		/*
		 * Load from memory locations only once, outside the
		 * hot loop below.
		 */
		uintptr_t base = (uintptr_t)edata_addr_get(slab);
		uintptr_t regsize = (uintptr_t)bin_info->reg_size;
		while (pop--) {
			size_t bit = cfs_lu(&g);
			size_t regind = shift + bit;
			*(ptrs + i) = (void *)(base + regsize * regind);

			i++;
		}
		slab_data->bitmap[group] = g;
	}
#endif
	edata_nfree_sub(slab, cnt);
}

static void
arena_large_malloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < SC_LARGE_MINCLASS) {
		usize = SC_LARGE_MINCLASS;
	}
	index = sz_size2index(usize);
	hindex = (index >= SC_NBINS) ? index - SC_NBINS : 0;

	locked_inc_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->stats.lstats[hindex].nmalloc, 1);
}

static void
arena_large_dalloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < SC_LARGE_MINCLASS) {
		usize = SC_LARGE_MINCLASS;
	}
	index = sz_size2index(usize);
	hindex = (index >= SC_NBINS) ? index - SC_NBINS : 0;

	locked_inc_u64(tsdn, LOCKEDINT_MTX(arena->stats.mtx),
	    &arena->stats.lstats[hindex].ndalloc, 1);
}

static void
arena_large_ralloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t oldusize,
    size_t usize) {
	arena_large_malloc_stats_update(tsdn, arena, usize);
	arena_large_dalloc_stats_update(tsdn, arena, oldusize);
}

edata_t *
arena_extent_alloc_large(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero) {
	bool deferred_work_generated = false;
	szind_t szind = sz_size2index(usize);
	size_t esize = usize + sz_large_pad;

	bool guarded = san_large_extent_decide_guard(tsdn,
	    arena_get_ehooks(arena), esize, alignment);
	edata_t *edata = pa_alloc(tsdn, &arena->pa_shard, esize, alignment,
	    /* slab */ false, szind, zero, guarded, &deferred_work_generated);
	assert(deferred_work_generated == false);

	if (edata != NULL) {
		if (config_stats) {
			LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
			arena_large_malloc_stats_update(tsdn, arena, usize);
			LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
		}
	}

	if (edata != NULL && sz_large_pad != 0) {
		arena_cache_oblivious_randomize(tsdn, arena, edata, alignment);
	}

	return edata;
}

void
arena_extent_dalloc_large_prep(tsdn_t *tsdn, arena_t *arena, edata_t *edata) {
	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
		arena_large_dalloc_stats_update(tsdn, arena,
		    edata_usize_get(edata));
		LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
	}
}

void
arena_extent_ralloc_large_shrink(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    size_t oldusize) {
	size_t usize = edata_usize_get(edata);

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
	}
}

void
arena_extent_ralloc_large_expand(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    size_t oldusize) {
	size_t usize = edata_usize_get(edata);

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, arena->stats.mtx);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		LOCKEDINT_MTX_UNLOCK(tsdn, arena->stats.mtx);
	}
}

/*
 * In situations where we're not forcing a decay (i.e. because the user
 * specifically requested it), should we purge ourselves, or wait for the
 * background thread to get to it.
 */
static pac_purge_eagerness_t
arena_decide_unforced_purge_eagerness(bool is_background_thread) {
	if (is_background_thread) {
		return PAC_PURGE_ALWAYS;
	} else if (!is_background_thread && background_thread_enabled()) {
		return PAC_PURGE_NEVER;
	} else {
		return PAC_PURGE_ON_EPOCH_ADVANCE;
	}
}

bool
arena_decay_ms_set(tsdn_t *tsdn, arena_t *arena, extent_state_t state,
    ssize_t decay_ms) {
	pac_purge_eagerness_t eagerness = arena_decide_unforced_purge_eagerness(
	    /* is_background_thread */ false);
	return pa_decay_ms_set(tsdn, &arena->pa_shard, state, decay_ms,
	    eagerness);
}

ssize_t
arena_decay_ms_get(arena_t *arena, extent_state_t state) {
	return pa_decay_ms_get(&arena->pa_shard, state);
}

static bool
arena_decay_impl(tsdn_t *tsdn, arena_t *arena, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache,
    bool is_background_thread, bool all) {
	if (all) {
		malloc_mutex_lock(tsdn, &decay->mtx);
		pac_decay_all(tsdn, &arena->pa_shard.pac, decay, decay_stats,
		    ecache, /* fully_decay */ all);
		malloc_mutex_unlock(tsdn, &decay->mtx);
		return false;
	}

	if (malloc_mutex_trylock(tsdn, &decay->mtx)) {
		/* No need to wait if another thread is in progress. */
		return true;
	}
	pac_purge_eagerness_t eagerness =
	    arena_decide_unforced_purge_eagerness(is_background_thread);
	bool epoch_advanced = pac_maybe_decay_purge(tsdn, &arena->pa_shard.pac,
	    decay, decay_stats, ecache, eagerness);
	size_t npages_new;
	if (epoch_advanced) {
		/* Backlog is updated on epoch advance. */
		npages_new = decay_epoch_npages_delta(decay);
	}
	malloc_mutex_unlock(tsdn, &decay->mtx);

	if (have_background_thread && background_thread_enabled() &&
	    epoch_advanced && !is_background_thread) {
		arena_maybe_do_deferred_work(tsdn, arena, decay, npages_new);
	}

	return false;
}

static bool
arena_decay_dirty(tsdn_t *tsdn, arena_t *arena, bool is_background_thread,
    bool all) {
	return arena_decay_impl(tsdn, arena, &arena->pa_shard.pac.decay_dirty,
	    &arena->pa_shard.pac.stats->decay_dirty,
	    &arena->pa_shard.pac.ecache_dirty, is_background_thread, all);
}

static bool
arena_decay_muzzy(tsdn_t *tsdn, arena_t *arena, bool is_background_thread,
    bool all) {
	if (pa_shard_dont_decay_muzzy(&arena->pa_shard)) {
		return false;
	}
	return arena_decay_impl(tsdn, arena, &arena->pa_shard.pac.decay_muzzy,
	    &arena->pa_shard.pac.stats->decay_muzzy,
	    &arena->pa_shard.pac.ecache_muzzy, is_background_thread, all);
}

void
arena_decay(tsdn_t *tsdn, arena_t *arena, bool is_background_thread, bool all) {
	if (all) {
		/*
		 * We should take a purge of "all" to mean "save as much memory
		 * as possible", including flushing any caches (for situations
		 * like thread death, or manual purge calls).
		 */
		sec_flush(tsdn, &arena->pa_shard.hpa_sec);
	}
	if (arena_decay_dirty(tsdn, arena, is_background_thread, all)) {
		return;
	}
	arena_decay_muzzy(tsdn, arena, is_background_thread, all);
}

static bool
arena_should_decay_early(tsdn_t *tsdn, arena_t *arena, decay_t *decay,
    background_thread_info_t *info, nstime_t *remaining_sleep,
    size_t npages_new) {
	malloc_mutex_assert_owner(tsdn, &info->mtx);

	if (malloc_mutex_trylock(tsdn, &decay->mtx)) {
		return false;
	}

	if (!decay_gradually(decay)) {
		malloc_mutex_unlock(tsdn, &decay->mtx);
		return false;
	}

	nstime_init(remaining_sleep, background_thread_wakeup_time_get(info));
	if (nstime_compare(remaining_sleep, &decay->epoch) <= 0) {
		malloc_mutex_unlock(tsdn, &decay->mtx);
		return false;
	}
	nstime_subtract(remaining_sleep, &decay->epoch);
	if (npages_new > 0) {
		uint64_t npurge_new = decay_npages_purge_in(decay,
		    remaining_sleep, npages_new);
		info->npages_to_purge_new += npurge_new;
	}
	malloc_mutex_unlock(tsdn, &decay->mtx);
	return info->npages_to_purge_new >
	    ARENA_DEFERRED_PURGE_NPAGES_THRESHOLD;
}

/*
 * Check if deferred work needs to be done sooner than planned.
 * For decay we might want to wake up earlier because of an influx of dirty
 * pages. Rather than waiting for previously estimated time, we proactively
 * purge those pages.
 * If background thread sleeps indefinitely, always wake up because some
 * deferred work has been generated.
 */
static void
arena_maybe_do_deferred_work(tsdn_t *tsdn, arena_t *arena, decay_t *decay,
    size_t npages_new) {
	background_thread_info_t *info = arena_background_thread_info_get(
	    arena);
	if (malloc_mutex_trylock(tsdn, &info->mtx)) {
		/*
		 * Background thread may hold the mutex for a long period of
		 * time.  We'd like to avoid the variance on application
		 * threads.  So keep this non-blocking, and leave the work to a
		 * future epoch.
		 */
		return;
	}
	if (!background_thread_is_started(info)) {
		goto label_done;
	}

	nstime_t remaining_sleep;
	if (background_thread_indefinite_sleep(info)) {
		background_thread_wakeup_early(info, NULL);
	} else if (arena_should_decay_early(tsdn, arena, decay, info,
	    &remaining_sleep, npages_new)) {
		info->npages_to_purge_new = 0;
		background_thread_wakeup_early(info, &remaining_sleep);
	}
label_done:
	malloc_mutex_unlock(tsdn, &info->mtx);
}

/* Called from background threads. */
void
arena_do_deferred_work(tsdn_t *tsdn, arena_t *arena) {
	arena_decay(tsdn, arena, true, false);
	pa_shard_do_deferred_work(tsdn, &arena->pa_shard);
}

void
arena_slab_dalloc(tsdn_t *tsdn, arena_t *arena, edata_t *slab) {
	bool deferred_work_generated = false;
	pa_dalloc(tsdn, &arena->pa_shard, slab, &deferred_work_generated);
	if (deferred_work_generated) {
		arena_handle_deferred_work(tsdn, arena);
	}
}

static void
arena_bin_slabs_nonfull_insert(bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) > 0);
	edata_heap_insert(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs++;
	}
}

static void
arena_bin_slabs_nonfull_remove(bin_t *bin, edata_t *slab) {
	edata_heap_remove(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs--;
	}
}

static edata_t *
arena_bin_slabs_nonfull_tryget(bin_t *bin) {
	edata_t *slab = edata_heap_remove_first(&bin->slabs_nonfull);
	if (slab == NULL) {
		return NULL;
	}
	if (config_stats) {
		bin->stats.reslabs++;
		bin->stats.nonfull_slabs--;
	}
	return slab;
}

static void
arena_bin_slabs_full_insert(arena_t *arena, bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) == 0);
	/*
	 *  Tracking extents is required by arena_reset, which is not allowed
	 *  for auto arenas.  Bypass this step to avoid touching the edata
	 *  linkage (often results in cache misses) for auto arenas.
	 */
	if (arena_is_auto(arena)) {
		return;
	}
	edata_list_active_append(&bin->slabs_full, slab);
}

static void
arena_bin_slabs_full_remove(arena_t *arena, bin_t *bin, edata_t *slab) {
	if (arena_is_auto(arena)) {
		return;
	}
	edata_list_active_remove(&bin->slabs_full, slab);
}

static void
arena_bin_reset(tsd_t *tsd, arena_t *arena, bin_t *bin) {
	edata_t *slab;

	malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	if (bin->slabcur != NULL) {
		slab = bin->slabcur;
		bin->slabcur = NULL;
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	while ((slab = edata_heap_remove_first(&bin->slabs_nonfull)) != NULL) {
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	for (slab = edata_list_active_first(&bin->slabs_full); slab != NULL;
	     slab = edata_list_active_first(&bin->slabs_full)) {
		arena_bin_slabs_full_remove(arena, bin, slab);
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
	}
	if (config_stats) {
		bin->stats.curregs = 0;
		bin->stats.curslabs = 0;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
}

void
arena_reset(tsd_t *tsd, arena_t *arena) {
	/*
	 * Locking in this function is unintuitive.  The caller guarantees that
	 * no concurrent operations are happening in this arena, but there are
	 * still reasons that some locking is necessary:
	 *
	 * - Some of the functions in the transitive closure of calls assume
	 *   appropriate locks are held, and in some cases these locks are
	 *   temporarily dropped to avoid lock order reversal or deadlock due to
	 *   reentry.
	 * - mallctl("epoch", ...) may concurrently refresh stats.  While
	 *   strictly speaking this is a "concurrent operation", disallowing
	 *   stats refreshes would impose an inconvenient burden.
	 */

	/* Large allocations. */
	malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);

	for (edata_t *edata = edata_list_active_first(&arena->large);
	    edata != NULL; edata = edata_list_active_first(&arena->large)) {
		void *ptr = edata_base_get(edata);
		size_t usize;

		malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);
		emap_alloc_ctx_t alloc_ctx;
		emap_alloc_ctx_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr,
		    &alloc_ctx);
		assert(alloc_ctx.szind != SC_NSIZES);

		if (config_stats || (config_prof && opt_prof)) {
			usize = sz_index2size(alloc_ctx.szind);
			assert(usize == isalloc(tsd_tsdn(tsd), ptr));
		}
		/* Remove large allocation from prof sample set. */
		if (config_prof && opt_prof) {
			prof_free(tsd, ptr, usize, &alloc_ctx);
		}
		large_dalloc(tsd_tsdn(tsd), edata);
		malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);

	/* Bins. */
	for (unsigned i = 0; i < SC_NBINS; i++) {
		for (unsigned j = 0; j < bin_infos[i].n_shards; j++) {
			arena_bin_reset(tsd, arena, arena_get_bin(arena, i, j));
		}
	}
	pa_shard_reset(tsd_tsdn(tsd), &arena->pa_shard);
}

static void
arena_prepare_base_deletion_sync_finish(tsd_t *tsd, malloc_mutex_t **mutexes,
    unsigned n_mtx) {
	for (unsigned i = 0; i < n_mtx; i++) {
		malloc_mutex_lock(tsd_tsdn(tsd), mutexes[i]);
		malloc_mutex_unlock(tsd_tsdn(tsd), mutexes[i]);
	}
}

#define ARENA_DESTROY_MAX_DELAYED_MTX 32
static void
arena_prepare_base_deletion_sync(tsd_t *tsd, malloc_mutex_t *mtx,
    malloc_mutex_t **delayed_mtx, unsigned *n_delayed) {
	if (!malloc_mutex_trylock(tsd_tsdn(tsd), mtx)) {
		/* No contention. */
		malloc_mutex_unlock(tsd_tsdn(tsd), mtx);
		return;
	}
	unsigned n = *n_delayed;
	assert(n < ARENA_DESTROY_MAX_DELAYED_MTX);
	/* Add another to the batch. */
	delayed_mtx[n++] = mtx;

	if (n == ARENA_DESTROY_MAX_DELAYED_MTX) {
		arena_prepare_base_deletion_sync_finish(tsd, delayed_mtx, n);
		n = 0;
	}
	*n_delayed = n;
}

static void
arena_prepare_base_deletion(tsd_t *tsd, base_t *base_to_destroy) {
	/*
	 * In order to coalesce, emap_try_acquire_edata_neighbor will attempt to
	 * check neighbor edata's state to determine eligibility.  This means
	 * under certain conditions, the metadata from an arena can be accessed
	 * w/o holding any locks from that arena.  In order to guarantee safe
	 * memory access, the metadata and the underlying base allocator needs
	 * to be kept alive, until all pending accesses are done.
	 *
	 * 1) with opt_retain, the arena boundary implies the is_head state
	 * (tracked in the rtree leaf), and the coalesce flow will stop at the
	 * head state branch.  Therefore no cross arena metadata access
	 * possible.
	 *
	 * 2) w/o opt_retain, the arena id needs to be read from the edata_t,
	 * meaning read only cross-arena metadata access is possible.  The
	 * coalesce attempt will stop at the arena_id mismatch, and is always
	 * under one of the ecache locks.  To allow safe passthrough of such
	 * metadata accesses, the loop below will iterate through all manual
	 * arenas' ecache locks.  As all the metadata from this base allocator
	 * have been unlinked from the rtree, after going through all the
	 * relevant ecache locks, it's safe to say that a) pending accesses are
	 * all finished, and b) no new access will be generated.
	 */
	if (opt_retain) {
		return;
	}
	unsigned destroy_ind = base_ind_get(base_to_destroy);
	assert(destroy_ind >= manual_arena_base);

	tsdn_t *tsdn = tsd_tsdn(tsd);
	malloc_mutex_t *delayed_mtx[ARENA_DESTROY_MAX_DELAYED_MTX];
	unsigned n_delayed = 0, total = narenas_total_get();
	for (unsigned i = 0; i < total; i++) {
		if (i == destroy_ind) {
			continue;
		}
		arena_t *arena = arena_get(tsdn, i, false);
		if (arena == NULL) {
			continue;
		}
		pac_t *pac = &arena->pa_shard.pac;
		arena_prepare_base_deletion_sync(tsd, &pac->ecache_dirty.mtx,
		    delayed_mtx, &n_delayed);
		arena_prepare_base_deletion_sync(tsd, &pac->ecache_muzzy.mtx,
		    delayed_mtx, &n_delayed);
		arena_prepare_base_deletion_sync(tsd, &pac->ecache_retained.mtx,
		    delayed_mtx, &n_delayed);
	}
	arena_prepare_base_deletion_sync_finish(tsd, delayed_mtx, n_delayed);
}
#undef ARENA_DESTROY_MAX_DELAYED_MTX

void
arena_destroy(tsd_t *tsd, arena_t *arena) {
	assert(base_ind_get(arena->base) >= narenas_auto);
	assert(arena_nthreads_get(arena, false) == 0);
	assert(arena_nthreads_get(arena, true) == 0);

	/*
	 * No allocations have occurred since arena_reset() was called.
	 * Furthermore, the caller (arena_i_destroy_ctl()) purged all cached
	 * extents, so only retained extents may remain and it's safe to call
	 * pa_shard_destroy_retained.
	 */
	pa_shard_destroy(tsd_tsdn(tsd), &arena->pa_shard);

	/*
	 * Remove the arena pointer from the arenas array.  We rely on the fact
	 * that there is no way for the application to get a dirty read from the
	 * arenas array unless there is an inherent race in the application
	 * involving access of an arena being concurrently destroyed.  The
	 * application must synchronize knowledge of the arena's validity, so as
	 * long as we use an atomic write to update the arenas array, the
	 * application will get a clean read any time after it synchronizes
	 * knowledge that the arena is no longer valid.
	 */
	arena_set(base_ind_get(arena->base), NULL);

	/*
	 * Destroy the base allocator, which manages all metadata ever mapped by
	 * this arena.  The prepare function will make sure no pending access to
	 * the metadata in this base anymore.
	 */
	arena_prepare_base_deletion(tsd, arena->base);
	base_delete(tsd_tsdn(tsd), arena->base);
}

static edata_t *
arena_slab_alloc(tsdn_t *tsdn, arena_t *arena, szind_t binind, unsigned binshard,
    const bin_info_t *bin_info) {
	bool deferred_work_generated = false;
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	bool guarded = san_slab_extent_decide_guard(tsdn,
	    arena_get_ehooks(arena));
	edata_t *slab = pa_alloc(tsdn, &arena->pa_shard, bin_info->slab_size,
	    /* alignment */ PAGE, /* slab */ true, /* szind */ binind,
	     /* zero */ false, guarded, &deferred_work_generated);

	if (deferred_work_generated) {
		arena_handle_deferred_work(tsdn, arena);
	}

	if (slab == NULL) {
		return NULL;
	}
	assert(edata_slab_get(slab));

	/* Initialize slab internals. */
	slab_data_t *slab_data = edata_slab_data_get(slab);
	edata_nfree_binshard_set(slab, bin_info->nregs, binshard);
	bitmap_init(slab_data->bitmap, &bin_info->bitmap_info, false);

	return slab;
}

/*
 * Before attempting the _with_fresh_slab approaches below, the _no_fresh_slab
 * variants (i.e. through slabcur and nonfull) must be tried first.
 */
static void
arena_bin_refill_slabcur_with_fresh_slab(tsdn_t *tsdn, arena_t *arena,
    bin_t *bin, szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after slabcur and nonfull both failed. */
	assert(bin->slabcur == NULL);
	assert(edata_heap_first(&bin->slabs_nonfull) == NULL);
	assert(fresh_slab != NULL);

	/* A new slab from arena_slab_alloc() */
	assert(edata_nfree_get(fresh_slab) == bin_infos[binind].nregs);
	if (config_stats) {
		bin->stats.nslabs++;
		bin->stats.curslabs++;
	}
	bin->slabcur = fresh_slab;
}

/* Refill slabcur and then alloc using the fresh slab */
static void *
arena_bin_malloc_with_fresh_slab(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	arena_bin_refill_slabcur_with_fresh_slab(tsdn, arena, bin, binind,
	    fresh_slab);

	return arena_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

static bool
arena_bin_refill_slabcur_no_fresh_slab(tsdn_t *tsdn, arena_t *arena,
    bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after arena_slab_reg_alloc[_batch] failed. */
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0);

	if (bin->slabcur != NULL) {
		arena_bin_slabs_full_insert(arena, bin, bin->slabcur);
	}

	/* Look for a usable slab. */
	bin->slabcur = arena_bin_slabs_nonfull_tryget(bin);
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) > 0);

	return (bin->slabcur == NULL);
}

bin_t *
arena_bin_choose(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    unsigned *binshard_p) {
	unsigned binshard;
	if (tsdn_null(tsdn) || tsd_arena_get(tsdn_tsd(tsdn)) == NULL) {
		binshard = 0;
	} else {
		binshard = tsd_binshardsp_get(tsdn_tsd(tsdn))->binshard[binind];
	}
	assert(binshard < bin_infos[binind].n_shards);
	if (binshard_p != NULL) {
		*binshard_p = binshard;
	}
	return arena_get_bin(arena, binind, binshard);
}

void
arena_cache_bin_fill_small(tsdn_t *tsdn, arena_t *arena,
    cache_bin_t *cache_bin, cache_bin_info_t *cache_bin_info, szind_t binind,
    const unsigned nfill) {
	assert(cache_bin_ncached_get_local(cache_bin, cache_bin_info) == 0);

	const bin_info_t *bin_info = &bin_infos[binind];

	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nfill);
	cache_bin_init_ptr_array_for_fill(cache_bin, cache_bin_info, &ptrs,
	    nfill);
	/*
	 * Bin-local resources are used first: 1) bin->slabcur, and 2) nonfull
	 * slabs.  After both are exhausted, new slabs will be allocated through
	 * arena_slab_alloc().
	 *
	 * Bin lock is only taken / released right before / after the while(...)
	 * refill loop, with new slab allocation (which has its own locking)
	 * kept outside of the loop.  This setup facilitates flat combining, at
	 * the cost of the nested loop (through goto label_refill).
	 *
	 * To optimize for cases with contention and limited resources
	 * (e.g. hugepage-backed or non-overcommit arenas), each fill-iteration
	 * gets one chance of slab_alloc, and a retry of bin local resources
	 * after the slab allocation (regardless if slab_alloc failed, because
	 * the bin lock is dropped during the slab allocation).
	 *
	 * In other words, new slab allocation is allowed, as long as there was
	 * progress since the previous slab_alloc.  This is tracked with
	 * made_progress below, initialized to true to jump start the first
	 * iteration.
	 *
	 * In other words (again), the loop will only terminate early (i.e. stop
	 * with filled < nfill) after going through the three steps: a) bin
	 * local exhausted, b) unlock and slab_alloc returns null, c) re-lock
	 * and bin local fails again.
	 */
	bool made_progress = true;
	edata_t *fresh_slab = NULL;
	bool alloc_and_retry = false;
	unsigned filled = 0;
	unsigned binshard;
	bin_t *bin = arena_bin_choose(tsdn, arena, binind, &binshard);

label_refill:
	malloc_mutex_lock(tsdn, &bin->lock);

	while (filled < nfill) {
		/* Try batch-fill from slabcur first. */
		edata_t *slabcur = bin->slabcur;
		if (slabcur != NULL && edata_nfree_get(slabcur) > 0) {
			unsigned tofill = nfill - filled;
			unsigned nfree = edata_nfree_get(slabcur);
			unsigned cnt = tofill < nfree ? tofill : nfree;

			arena_slab_reg_alloc_batch(slabcur, bin_info, cnt,
			    &ptrs.ptr[filled]);
			made_progress = true;
			filled += cnt;
			continue;
		}
		/* Next try refilling slabcur from nonfull slabs. */
		if (!arena_bin_refill_slabcur_no_fresh_slab(tsdn, arena, bin)) {
			assert(bin->slabcur != NULL);
			continue;
		}

		/* Then see if a new slab was reserved already. */
		if (fresh_slab != NULL) {
			arena_bin_refill_slabcur_with_fresh_slab(tsdn, arena,
			    bin, binind, fresh_slab);
			assert(bin->slabcur != NULL);
			fresh_slab = NULL;
			continue;
		}

		/* Try slab_alloc if made progress (or never did slab_alloc). */
		if (made_progress) {
			assert(bin->slabcur == NULL);
			assert(fresh_slab == NULL);
			alloc_and_retry = true;
			/* Alloc a new slab then come back. */
			break;
		}

		/* OOM. */

		assert(fresh_slab == NULL);
		assert(!alloc_and_retry);
		break;
	} /* while (filled < nfill) loop. */

	if (config_stats && !alloc_and_retry) {
		bin->stats.nmalloc += filled;
		bin->stats.nrequests += cache_bin->tstats.nrequests;
		bin->stats.curregs += filled;
		bin->stats.nfills++;
		cache_bin->tstats.nrequests = 0;
	}

	malloc_mutex_unlock(tsdn, &bin->lock);

	if (alloc_and_retry) {
		assert(fresh_slab == NULL);
		assert(filled < nfill);
		assert(made_progress);

		fresh_slab = arena_slab_alloc(tsdn, arena, binind, binshard,
		    bin_info);
		/* fresh_slab NULL case handled in the for loop. */

		alloc_and_retry = false;
		made_progress = false;
		goto label_refill;
	}
	assert(filled == nfill || (fresh_slab == NULL && !made_progress));

	/* Release if allocated but not used. */
	if (fresh_slab != NULL) {
		assert(edata_nfree_get(fresh_slab) == bin_info->nregs);
		arena_slab_dalloc(tsdn, arena, fresh_slab);
		fresh_slab = NULL;
	}

	cache_bin_finish_fill(cache_bin, cache_bin_info, &ptrs, filled);
	arena_decay_tick(tsdn, arena);
}

size_t
arena_fill_small_fresh(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    void **ptrs, size_t nfill, bool zero) {
	assert(binind < SC_NBINS);
	const bin_info_t *bin_info = &bin_infos[binind];
	const size_t nregs = bin_info->nregs;
	assert(nregs > 0);
	const size_t usize = bin_info->reg_size;

	const bool manual_arena = !arena_is_auto(arena);
	unsigned binshard;
	bin_t *bin = arena_bin_choose(tsdn, arena, binind, &binshard);

	size_t nslab = 0;
	size_t filled = 0;
	edata_t *slab = NULL;
	edata_list_active_t fulls;
	edata_list_active_init(&fulls);

	while (filled < nfill && (slab = arena_slab_alloc(tsdn, arena, binind,
	    binshard, bin_info)) != NULL) {
		assert((size_t)edata_nfree_get(slab) == nregs);
		++nslab;
		size_t batch = nfill - filled;
		if (batch > nregs) {
			batch = nregs;
		}
		assert(batch > 0);
		arena_slab_reg_alloc_batch(slab, bin_info, (unsigned)batch,
		    &ptrs[filled]);
		assert(edata_addr_get(slab) == ptrs[filled]);
		if (zero) {
			memset(ptrs[filled], 0, batch * usize);
		}
		filled += batch;
		if (batch == nregs) {
			if (manual_arena) {
				edata_list_active_append(&fulls, slab);
			}
			slab = NULL;
		}
	}

	malloc_mutex_lock(tsdn, &bin->lock);
	/*
	 * Only the last slab can be non-empty, and the last slab is non-empty
	 * iff slab != NULL.
	 */
	if (slab != NULL) {
		arena_bin_lower_slab(tsdn, arena, slab, bin);
	}
	if (manual_arena) {
		edata_list_active_concat(&bin->slabs_full, &fulls);
	}
	assert(edata_list_active_empty(&fulls));
	if (config_stats) {
		bin->stats.nslabs += nslab;
		bin->stats.curslabs += nslab;
		bin->stats.nmalloc += filled;
		bin->stats.nrequests += filled;
		bin->stats.curregs += filled;
	}
	malloc_mutex_unlock(tsdn, &bin->lock);

	arena_decay_tick(tsdn, arena);
	return filled;
}

/*
 * Without allocating a new slab, try arena_slab_reg_alloc() and re-fill
 * bin->slabcur if necessary.
 */
static void *
arena_bin_malloc_no_fresh_slab(tsdn_t *tsdn, arena_t *arena, bin_t *bin,
    szind_t binind) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	if (bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0) {
		if (arena_bin_refill_slabcur_no_fresh_slab(tsdn, arena, bin)) {
			return NULL;
		}
	}

	assert(bin->slabcur != NULL && edata_nfree_get(bin->slabcur) > 0);
	return arena_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

static void *
arena_malloc_small(tsdn_t *tsdn, arena_t *arena, szind_t binind, bool zero) {
	assert(binind < SC_NBINS);
	const bin_info_t *bin_info = &bin_infos[binind];
	size_t usize = sz_index2size(binind);
	unsigned binshard;
	bin_t *bin = arena_bin_choose(tsdn, arena, binind, &binshard);

	malloc_mutex_lock(tsdn, &bin->lock);
	edata_t *fresh_slab = NULL;
	void *ret = arena_bin_malloc_no_fresh_slab(tsdn, arena, bin, binind);
	if (ret == NULL) {
		malloc_mutex_unlock(tsdn, &bin->lock);
		/******************************/
		fresh_slab = arena_slab_alloc(tsdn, arena, binind, binshard,
		    bin_info);
		/********************************/
		malloc_mutex_lock(tsdn, &bin->lock);
		/* Retry since the lock was dropped. */
		ret = arena_bin_malloc_no_fresh_slab(tsdn, arena, bin, binind);
		if (ret == NULL) {
			if (fresh_slab == NULL) {
				/* OOM */
				malloc_mutex_unlock(tsdn, &bin->lock);
				return NULL;
			}
			ret = arena_bin_malloc_with_fresh_slab(tsdn, arena, bin,
			    binind, fresh_slab);
			fresh_slab = NULL;
		}
	}
	if (config_stats) {
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
		bin->stats.curregs++;
	}
	malloc_mutex_unlock(tsdn, &bin->lock);

	if (fresh_slab != NULL) {
		arena_slab_dalloc(tsdn, arena, fresh_slab);
	}
	if (zero) {
		memset(ret, 0, usize);
	}
	arena_decay_tick(tsdn, arena);

	return ret;
}

void *
arena_malloc_hard(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind,
    bool zero) {
	assert(!tsdn_null(tsdn) || arena != NULL);

	if (likely(!tsdn_null(tsdn))) {
		arena = arena_choose_maybe_huge(tsdn_tsd(tsdn), arena, size);
	}
	if (unlikely(arena == NULL)) {
		return NULL;
	}

	if (likely(size <= SC_SMALL_MAXCLASS)) {
		return arena_malloc_small(tsdn, arena, ind, zero);
	}
	return large_malloc(tsdn, arena, sz_index2size(ind), zero);
}

void *
arena_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero, tcache_t *tcache) {
	void *ret;

	if (usize <= SC_SMALL_MAXCLASS) {
		/* Small; alignment doesn't require special slab placement. */

		/* usize should be a result of sz_sa2u() */
		assert((usize & (alignment - 1)) == 0);

		/*
		 * Small usize can't come from an alignment larger than a page.
		 */
		assert(alignment <= PAGE);

		ret = arena_malloc(tsdn, arena, usize, sz_size2index(usize),
		    zero, tcache, true);
	} else {
		if (likely(alignment <= CACHELINE)) {
			ret = large_malloc(tsdn, arena, usize, zero);
		} else {
			ret = large_palloc(tsdn, arena, usize, alignment, zero);
		}
	}
	return ret;
}

void
arena_prof_promote(tsdn_t *tsdn, void *ptr, size_t usize) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(isalloc(tsdn, ptr) == SC_LARGE_MINCLASS);
	assert(usize <= SC_SMALL_MAXCLASS);

	if (config_opt_safety_checks) {
		safety_check_set_redzone(ptr, usize, SC_LARGE_MINCLASS);
	}

	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);

	szind_t szind = sz_size2index(usize);
	edata_szind_set(edata, szind);
	emap_remap(tsdn, &arena_emap_global, edata, szind, /* slab */ false);

	assert(isalloc(tsdn, ptr) == usize);
}

static size_t
arena_prof_demote(tsdn_t *tsdn, edata_t *edata, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	edata_szind_set(edata, SC_NBINS);
	emap_remap(tsdn, &arena_emap_global, edata, SC_NBINS, /* slab */ false);

	assert(isalloc(tsdn, ptr) == SC_LARGE_MINCLASS);

	return SC_LARGE_MINCLASS;
}

void
arena_dalloc_promoted(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    bool slow_path) {
	cassert(config_prof);
	assert(opt_prof);

	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	size_t usize = edata_usize_get(edata);
	size_t bumped_usize = arena_prof_demote(tsdn, edata, ptr);
	if (config_opt_safety_checks && usize < SC_LARGE_MINCLASS) {
		/*
		 * Currently, we only do redzoning for small sampled
		 * allocations.
		 */
		assert(bumped_usize == SC_LARGE_MINCLASS);
		safety_check_verify_redzone(ptr, usize, bumped_usize);
	}
	if (bumped_usize <= tcache_maxclass && tcache != NULL) {
		tcache_dalloc_large(tsdn_tsd(tsdn), tcache, ptr,
		    sz_size2index(bumped_usize), slow_path);
	} else {
		large_dalloc(tsdn, edata);
	}
}

static void
arena_dissociate_bin_slab(arena_t *arena, edata_t *slab, bin_t *bin) {
	/* Dissociate slab from bin. */
	if (slab == bin->slabcur) {
		bin->slabcur = NULL;
	} else {
		szind_t binind = edata_szind_get(slab);
		const bin_info_t *bin_info = &bin_infos[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * slab only contains one region, then it never gets inserted
		 * into the non-full slabs heap.
		 */
		if (bin_info->nregs == 1) {
			arena_bin_slabs_full_remove(arena, bin, slab);
		} else {
			arena_bin_slabs_nonfull_remove(bin, slab);
		}
	}
}

static void
arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, edata_t *slab,
    bin_t *bin) {
	assert(edata_nfree_get(slab) > 0);

	/*
	 * Make sure that if bin->slabcur is non-NULL, it refers to the
	 * oldest/lowest non-full slab.  It is okay to NULL slabcur out rather
	 * than proactively keeping it pointing at the oldest/lowest non-full
	 * slab.
	 */
	if (bin->slabcur != NULL && edata_snad_comp(bin->slabcur, slab) > 0) {
		/* Switch slabcur. */
		if (edata_nfree_get(bin->slabcur) > 0) {
			arena_bin_slabs_nonfull_insert(bin, bin->slabcur);
		} else {
			arena_bin_slabs_full_insert(arena, bin, bin->slabcur);
		}
		bin->slabcur = slab;
		if (config_stats) {
			bin->stats.reslabs++;
		}
	} else {
		arena_bin_slabs_nonfull_insert(bin, slab);
	}
}

static void
arena_dalloc_bin_slab_prepare(tsdn_t *tsdn, edata_t *slab, bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);

	assert(slab != bin->slabcur);
	if (config_stats) {
		bin->stats.curslabs--;
	}
}

void
arena_dalloc_bin_locked_handle_newly_empty(tsdn_t *tsdn, arena_t *arena,
    edata_t *slab, bin_t *bin) {
	arena_dissociate_bin_slab(arena, slab, bin);
	arena_dalloc_bin_slab_prepare(tsdn, slab, bin);
}

void
arena_dalloc_bin_locked_handle_newly_nonempty(tsdn_t *tsdn, arena_t *arena,
    edata_t *slab, bin_t *bin) {
	arena_bin_slabs_full_remove(arena, bin, slab);
	arena_bin_lower_slab(tsdn, arena, slab, bin);
}

static void
arena_dalloc_bin(tsdn_t *tsdn, arena_t *arena, edata_t *edata, void *ptr) {
	szind_t binind = edata_szind_get(edata);
	unsigned binshard = edata_binshard_get(edata);
	bin_t *bin = arena_get_bin(arena, binind, binshard);

	malloc_mutex_lock(tsdn, &bin->lock);
	arena_dalloc_bin_locked_info_t info;
	arena_dalloc_bin_locked_begin(&info, binind);
	bool ret = arena_dalloc_bin_locked_step(tsdn, arena, bin,
	    &info, binind, edata, ptr);
	arena_dalloc_bin_locked_finish(tsdn, arena, bin, &info);
	malloc_mutex_unlock(tsdn, &bin->lock);

	if (ret) {
		arena_slab_dalloc(tsdn, arena, edata);
	}
}

void
arena_dalloc_small(tsdn_t *tsdn, void *ptr) {
	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	arena_t *arena = arena_get_from_edata(edata);

	arena_dalloc_bin(tsdn, arena, edata, ptr);
	arena_decay_tick(tsdn, arena);
}

bool
arena_ralloc_no_move(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size,
    size_t extra, bool zero, size_t *newsize) {
	bool ret;
	/* Calls with non-zero extra had to clamp extra. */
	assert(extra == 0 || size + extra <= SC_LARGE_MAXCLASS);

	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	if (unlikely(size > SC_LARGE_MAXCLASS)) {
		ret = true;
		goto done;
	}

	size_t usize_min = sz_s2u(size);
	size_t usize_max = sz_s2u(size + extra);
	if (likely(oldsize <= SC_SMALL_MAXCLASS && usize_min
	    <= SC_SMALL_MAXCLASS)) {
		/*
		 * Avoid moving the allocation if the size class can be left the
		 * same.
		 */
		assert(bin_infos[sz_size2index(oldsize)].reg_size ==
		    oldsize);
		if ((usize_max > SC_SMALL_MAXCLASS
		    || sz_size2index(usize_max) != sz_size2index(oldsize))
		    && (size > oldsize || usize_max < oldsize)) {
			ret = true;
			goto done;
		}

		arena_t *arena = arena_get_from_edata(edata);
		arena_decay_tick(tsdn, arena);
		ret = false;
	} else if (oldsize >= SC_LARGE_MINCLASS
	    && usize_max >= SC_LARGE_MINCLASS) {
		ret = large_ralloc_no_move(tsdn, edata, usize_min, usize_max,
		    zero);
	} else {
		ret = true;
	}
done:
	assert(edata == emap_edata_lookup(tsdn, &arena_emap_global, ptr));
	*newsize = edata_usize_get(edata);

	return ret;
}

static void *
arena_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache) {
	if (alignment == 0) {
		return arena_malloc(tsdn, arena, usize, sz_size2index(usize),
		    zero, tcache, true);
	}
	usize = sz_sa2u(usize, alignment);
	if (unlikely(usize == 0 || usize > SC_LARGE_MAXCLASS)) {
		return NULL;
	}
	return ipalloct(tsdn, usize, alignment, zero, tcache, arena);
}

void *
arena_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t oldsize,
    size_t size, size_t alignment, bool zero, tcache_t *tcache,
    hook_ralloc_args_t *hook_args) {
	size_t usize = alignment == 0 ? sz_s2u(size) : sz_sa2u(size, alignment);
	if (unlikely(usize == 0 || size > SC_LARGE_MAXCLASS)) {
		return NULL;
	}

	if (likely(usize <= SC_SMALL_MAXCLASS)) {
		/* Try to avoid moving the allocation. */
		UNUSED size_t newsize;
		if (!arena_ralloc_no_move(tsdn, ptr, oldsize, usize, 0, zero,
		    &newsize)) {
			hook_invoke_expand(hook_args->is_realloc
			    ? hook_expand_realloc : hook_expand_rallocx,
			    ptr, oldsize, usize, (uintptr_t)ptr,
			    hook_args->args);
			return ptr;
		}
	}

	if (oldsize >= SC_LARGE_MINCLASS
	    && usize >= SC_LARGE_MINCLASS) {
		return large_ralloc(tsdn, arena, ptr, usize,
		    alignment, zero, tcache, hook_args);
	}

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and copying.
	 */
	void *ret = arena_ralloc_move_helper(tsdn, arena, usize, alignment,
	    zero, tcache);
	if (ret == NULL) {
		return NULL;
	}

	hook_invoke_alloc(hook_args->is_realloc
	    ? hook_alloc_realloc : hook_alloc_rallocx, ret, (uintptr_t)ret,
	    hook_args->args);
	hook_invoke_dalloc(hook_args->is_realloc
	    ? hook_dalloc_realloc : hook_dalloc_rallocx, ptr, hook_args->args);

	/*
	 * Junk/zero-filling were already done by
	 * ipalloc()/arena_malloc().
	 */
	size_t copysize = (usize < oldsize) ? usize : oldsize;
	memcpy(ret, ptr, copysize);
	isdalloct(tsdn, ptr, oldsize, tcache, NULL, true);
	return ret;
}

ehooks_t *
arena_get_ehooks(arena_t *arena) {
	return base_ehooks_get(arena->base);
}

extent_hooks_t *
arena_set_extent_hooks(tsd_t *tsd, arena_t *arena,
    extent_hooks_t *extent_hooks) {
	background_thread_info_t *info;
	if (have_background_thread) {
		info = arena_background_thread_info_get(arena);
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	}
	/* No using the HPA now that we have the custom hooks. */
	pa_shard_disable_hpa(tsd_tsdn(tsd), &arena->pa_shard);
	extent_hooks_t *ret = base_extent_hooks_set(arena->base, extent_hooks);
	if (have_background_thread) {
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
	}

	return ret;
}

dss_prec_t
arena_dss_prec_get(arena_t *arena) {
	return (dss_prec_t)atomic_load_u(&arena->dss_prec, ATOMIC_ACQUIRE);
}

bool
arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec) {
	if (!have_dss) {
		return (dss_prec != dss_prec_disabled);
	}
	atomic_store_u(&arena->dss_prec, (unsigned)dss_prec, ATOMIC_RELEASE);
	return false;
}

ssize_t
arena_dirty_decay_ms_default_get(void) {
	return atomic_load_zd(&dirty_decay_ms_default, ATOMIC_RELAXED);
}

bool
arena_dirty_decay_ms_default_set(ssize_t decay_ms) {
	if (!decay_ms_valid(decay_ms)) {
		return true;
	}
	atomic_store_zd(&dirty_decay_ms_default, decay_ms, ATOMIC_RELAXED);
	return false;
}

ssize_t
arena_muzzy_decay_ms_default_get(void) {
	return atomic_load_zd(&muzzy_decay_ms_default, ATOMIC_RELAXED);
}

bool
arena_muzzy_decay_ms_default_set(ssize_t decay_ms) {
	if (!decay_ms_valid(decay_ms)) {
		return true;
	}
	atomic_store_zd(&muzzy_decay_ms_default, decay_ms, ATOMIC_RELAXED);
	return false;
}

bool
arena_retain_grow_limit_get_set(tsd_t *tsd, arena_t *arena, size_t *old_limit,
    size_t *new_limit) {
	assert(opt_retain);
	return pac_retain_grow_limit_get_set(tsd_tsdn(tsd),
	    &arena->pa_shard.pac, old_limit, new_limit);
}

unsigned
arena_nthreads_get(arena_t *arena, bool internal) {
	return atomic_load_u(&arena->nthreads[internal], ATOMIC_RELAXED);
}

void
arena_nthreads_inc(arena_t *arena, bool internal) {
	atomic_fetch_add_u(&arena->nthreads[internal], 1, ATOMIC_RELAXED);
}

void
arena_nthreads_dec(arena_t *arena, bool internal) {
	atomic_fetch_sub_u(&arena->nthreads[internal], 1, ATOMIC_RELAXED);
}

arena_t *
arena_new(tsdn_t *tsdn, unsigned ind, const arena_config_t *config) {
	arena_t *arena;
	base_t *base;
	unsigned i;

	if (ind == 0) {
		base = b0get();
	} else {
		base = base_new(tsdn, ind, config->extent_hooks,
		    config->metadata_use_hooks);
		if (base == NULL) {
			return NULL;
		}
	}

	size_t arena_size = sizeof(arena_t) + sizeof(bin_t) * nbins_total;
	arena = (arena_t *)base_alloc(tsdn, base, arena_size, CACHELINE);
	if (arena == NULL) {
		goto label_error;
	}

	atomic_store_u(&arena->nthreads[0], 0, ATOMIC_RELAXED);
	atomic_store_u(&arena->nthreads[1], 0, ATOMIC_RELAXED);
	arena->last_thd = NULL;

	if (config_stats) {
		if (arena_stats_init(tsdn, &arena->stats)) {
			goto label_error;
		}

		ql_new(&arena->tcache_ql);
		ql_new(&arena->cache_bin_array_descriptor_ql);
		if (malloc_mutex_init(&arena->tcache_ql_mtx, "tcache_ql",
		    WITNESS_RANK_TCACHE_QL, malloc_mutex_rank_exclusive)) {
			goto label_error;
		}
	}

	atomic_store_u(&arena->dss_prec, (unsigned)extent_dss_prec_get(),
	    ATOMIC_RELAXED);

	edata_list_active_init(&arena->large);
	if (malloc_mutex_init(&arena->large_mtx, "arena_large",
	    WITNESS_RANK_ARENA_LARGE, malloc_mutex_rank_exclusive)) {
		goto label_error;
	}

	nstime_t cur_time;
	nstime_init_update(&cur_time);
	if (pa_shard_init(tsdn, &arena->pa_shard, &arena_pa_central_global,
	    &arena_emap_global, base, ind, &arena->stats.pa_shard_stats,
	    LOCKEDINT_MTX(arena->stats.mtx), &cur_time, oversize_threshold,
	    arena_dirty_decay_ms_default_get(),
	    arena_muzzy_decay_ms_default_get())) {
		goto label_error;
	}

	/* Initialize bins. */
	atomic_store_u(&arena->binshard_next, 0, ATOMIC_RELEASE);
	for (i = 0; i < nbins_total; i++) {
		bool err = bin_init(&arena->bins[i]);
		if (err) {
			goto label_error;
		}
	}

	arena->base = base;
	/* Set arena before creating background threads. */
	arena_set(ind, arena);
	arena->ind = ind;

	nstime_init_update(&arena->create_time);

	/*
	 * We turn on the HPA if set to.  There are two exceptions:
	 * - Custom extent hooks (we should only return memory allocated from
	 *   them in that case).
	 * - Arena 0 initialization.  In this case, we're mid-bootstrapping, and
	 *   so arena_hpa_global is not yet initialized.
	 */
	if (opt_hpa && ehooks_are_default(base_ehooks_get(base)) && ind != 0) {
		hpa_shard_opts_t hpa_shard_opts = opt_hpa_opts;
		hpa_shard_opts.deferral_allowed = background_thread_enabled();
		if (pa_shard_enable_hpa(tsdn, &arena->pa_shard,
		    &hpa_shard_opts, &opt_hpa_sec_opts)) {
			goto label_error;
		}
	}

	/* We don't support reentrancy for arena 0 bootstrapping. */
	if (ind != 0) {
		/*
		 * If we're here, then arena 0 already exists, so bootstrapping
		 * is done enough that we should have tsd.
		 */
		assert(!tsdn_null(tsdn));
		pre_reentrancy(tsdn_tsd(tsdn), arena);
		if (test_hooks_arena_new_hook) {
			test_hooks_arena_new_hook();
		}
		post_reentrancy(tsdn_tsd(tsdn));
	}

	return arena;
label_error:
	if (ind != 0) {
		base_delete(tsdn, base);
	}
	return NULL;
}

arena_t *
arena_choose_huge(tsd_t *tsd) {
	/* huge_arena_ind can be 0 during init (will use a0). */
	if (huge_arena_ind == 0) {
		assert(!malloc_initialized());
	}

	arena_t *huge_arena = arena_get(tsd_tsdn(tsd), huge_arena_ind, false);
	if (huge_arena == NULL) {
		/* Create the huge arena on demand. */
		assert(huge_arena_ind != 0);
		huge_arena = arena_get(tsd_tsdn(tsd), huge_arena_ind, true);
		if (huge_arena == NULL) {
			return NULL;
		}
		/*
		 * Purge eagerly for huge allocations, because: 1) number of
		 * huge allocations is usually small, which means ticker based
		 * decay is not reliable; and 2) less immediate reuse is
		 * expected for huge allocations.
		 */
		if (arena_dirty_decay_ms_default_get() > 0) {
			arena_decay_ms_set(tsd_tsdn(tsd), huge_arena,
			    extent_state_dirty, 0);
		}
		if (arena_muzzy_decay_ms_default_get() > 0) {
			arena_decay_ms_set(tsd_tsdn(tsd), huge_arena,
			    extent_state_muzzy, 0);
		}
	}

	return huge_arena;
}

bool
arena_init_huge(void) {
	bool huge_enabled;

	/* The threshold should be large size class. */
	if (opt_oversize_threshold > SC_LARGE_MAXCLASS ||
	    opt_oversize_threshold < SC_LARGE_MINCLASS) {
		opt_oversize_threshold = 0;
		oversize_threshold = SC_LARGE_MAXCLASS + PAGE;
		huge_enabled = false;
	} else {
		/* Reserve the index for the huge arena. */
		huge_arena_ind = narenas_total_get();
		oversize_threshold = opt_oversize_threshold;
		huge_enabled = true;
	}

	return huge_enabled;
}

bool
arena_is_huge(unsigned arena_ind) {
	if (huge_arena_ind == 0) {
		return false;
	}
	return (arena_ind == huge_arena_ind);
}

bool
arena_boot(sc_data_t *sc_data, base_t *base, bool hpa) {
	arena_dirty_decay_ms_default_set(opt_dirty_decay_ms);
	arena_muzzy_decay_ms_default_set(opt_muzzy_decay_ms);
	for (unsigned i = 0; i < SC_NBINS; i++) {
		sc_t *sc = &sc_data->sc[i];
		div_init(&arena_binind_div_info[i],
		    (1U << sc->lg_base) + (sc->ndelta << sc->lg_delta));
	}

	uint32_t cur_offset = (uint32_t)offsetof(arena_t, bins);
	for (szind_t i = 0; i < SC_NBINS; i++) {
		arena_bin_offsets[i] = cur_offset;
		nbins_total += bin_infos[i].n_shards;
		cur_offset += (uint32_t)(bin_infos[i].n_shards * sizeof(bin_t));
	}
	return pa_central_init(&arena_pa_central_global, base, hpa,
	    &hpa_hooks_default);
}

void
arena_prefork0(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork0(tsdn, &arena->pa_shard);
}

void
arena_prefork1(tsdn_t *tsdn, arena_t *arena) {
	if (config_stats) {
		malloc_mutex_prefork(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_prefork2(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork2(tsdn, &arena->pa_shard);
}

void
arena_prefork3(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork3(tsdn, &arena->pa_shard);
}

void
arena_prefork4(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork4(tsdn, &arena->pa_shard);
}

void
arena_prefork5(tsdn_t *tsdn, arena_t *arena) {
	pa_shard_prefork5(tsdn, &arena->pa_shard);
}

void
arena_prefork6(tsdn_t *tsdn, arena_t *arena) {
	base_prefork(tsdn, arena->base);
}

void
arena_prefork7(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->large_mtx);
}

void
arena_prefork8(tsdn_t *tsdn, arena_t *arena) {
	for (unsigned i = 0; i < nbins_total; i++) {
		bin_prefork(tsdn, &arena->bins[i]);
	}
}

void
arena_postfork_parent(tsdn_t *tsdn, arena_t *arena) {
	for (unsigned i = 0; i < nbins_total; i++) {
		bin_postfork_parent(tsdn, &arena->bins[i]);
	}

	malloc_mutex_postfork_parent(tsdn, &arena->large_mtx);
	base_postfork_parent(tsdn, arena->base);
	pa_shard_postfork_parent(tsdn, &arena->pa_shard);
	if (config_stats) {
		malloc_mutex_postfork_parent(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_postfork_child(tsdn_t *tsdn, arena_t *arena) {
	atomic_store_u(&arena->nthreads[0], 0, ATOMIC_RELAXED);
	atomic_store_u(&arena->nthreads[1], 0, ATOMIC_RELAXED);
	if (tsd_arena_get(tsdn_tsd(tsdn)) == arena) {
		arena_nthreads_inc(arena, false);
	}
	if (tsd_iarena_get(tsdn_tsd(tsdn)) == arena) {
		arena_nthreads_inc(arena, true);
	}
	if (config_stats) {
		ql_new(&arena->tcache_ql);
		ql_new(&arena->cache_bin_array_descriptor_ql);
		tcache_slow_t *tcache_slow = tcache_slow_get(tsdn_tsd(tsdn));
		if (tcache_slow != NULL && tcache_slow->arena == arena) {
			tcache_t *tcache = tcache_slow->tcache;
			ql_elm_new(tcache_slow, link);
			ql_tail_insert(&arena->tcache_ql, tcache_slow, link);
			cache_bin_array_descriptor_init(
			    &tcache_slow->cache_bin_array_descriptor,
			    tcache->bins);
			ql_tail_insert(&arena->cache_bin_array_descriptor_ql,
			    &tcache_slow->cache_bin_array_descriptor, link);
		}
	}

	for (unsigned i = 0; i < nbins_total; i++) {
		bin_postfork_child(tsdn, &arena->bins[i]);
	}

	malloc_mutex_postfork_child(tsdn, &arena->large_mtx);
	base_postfork_child(tsdn, arena->base);
	pa_shard_postfork_child(tsdn, &arena->pa_shard);
	if (config_stats) {
		malloc_mutex_postfork_child(tsdn, &arena->tcache_ql_mtx);
	}
}
