#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/sc.h"

/******************************************************************************/
/* Data. */

bool opt_tcache = true;

/* tcache_maxclass is set to 32KB by default.  */
size_t opt_tcache_max = ((size_t)1) << 15;

/* Reasonable defaults for min and max values. */
unsigned opt_tcache_nslots_small_min = 20;
unsigned opt_tcache_nslots_small_max = 200;
unsigned opt_tcache_nslots_large = 20;

/*
 * We attempt to make the number of slots in a tcache bin for a given size class
 * equal to the number of objects in a slab times some multiplier.  By default,
 * the multiplier is 2 (i.e. we set the maximum number of objects in the tcache
 * to twice the number of objects in a slab).
 * This is bounded by some other constraints as well, like the fact that it
 * must be even, must be less than opt_tcache_nslots_small_max, etc..
 */
ssize_t	opt_lg_tcache_nslots_mul = 1;

/*
 * Number of allocation bytes between tcache incremental GCs.  Again, this
 * default just seems to work well; more tuning is possible.
 */
size_t opt_tcache_gc_incr_bytes = 65536;

/*
 * With default settings, we may end up flushing small bins frequently with
 * small flush amounts.  To limit this tendency, we can set a number of bytes to
 * "delay" by.  If we try to flush N M-byte items, we decrease that size-class's
 * delay by N * M.  So, if delay is 1024 and we're looking at the 64-byte size
 * class, we won't do any flushing until we've been asked to flush 1024/64 == 16
 * items.  This can happen in any configuration (i.e. being asked to flush 16
 * items once, or 4 items 4 times).
 *
 * Practically, this is stored as a count of items in a uint8_t, so the
 * effective maximum value for a size class is 255 * sz.
 */
size_t opt_tcache_gc_delay_bytes = 0;

/*
 * When a cache bin is flushed because it's full, how much of it do we flush?
 * By default, we flush half the maximum number of items.
 */
unsigned opt_lg_tcache_flush_small_div = 1;
unsigned opt_lg_tcache_flush_large_div = 1;

cache_bin_info_t	*tcache_bin_info;

/* Total stack size required (per tcache).  Include the padding above. */
static size_t tcache_bin_alloc_size;
static size_t tcache_bin_alloc_alignment;

/* Number of cache bins enabled, including both large and small. */
unsigned		nhbins;
/* Max size class to be cached (can be small or large). */
size_t			tcache_maxclass;

tcaches_t		*tcaches;

/* Index of first element within tcaches that has never been used. */
static unsigned		tcaches_past;

/* Head of singly linked list tracking available tcaches elements. */
static tcaches_t	*tcaches_avail;

/* Protects tcaches{,_past,_avail}. */
static malloc_mutex_t	tcaches_mtx;

/******************************************************************************/

size_t
tcache_salloc(tsdn_t *tsdn, const void *ptr) {
	return arena_salloc(tsdn, ptr);
}

uint64_t
tcache_gc_new_event_wait(tsd_t *tsd) {
	return opt_tcache_gc_incr_bytes;
}

uint64_t
tcache_gc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

uint64_t
tcache_gc_dalloc_new_event_wait(tsd_t *tsd) {
	return opt_tcache_gc_incr_bytes;
}

uint64_t
tcache_gc_dalloc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

static uint8_t
tcache_gc_item_delay_compute(szind_t szind) {
	assert(szind < SC_NBINS);
	size_t sz = sz_index2size(szind);
	size_t item_delay = opt_tcache_gc_delay_bytes / sz;
	size_t delay_max = ZU(1)
	    << (sizeof(((tcache_slow_t *)NULL)->bin_flush_delay_items[0]) * 8);
	if (item_delay >= delay_max) {
		item_delay = delay_max - 1;
	}
	return (uint8_t)item_delay;
}

static void
tcache_gc_small(tsd_t *tsd, tcache_slow_t *tcache_slow, tcache_t *tcache,
    szind_t szind) {
	/* Aim to flush 3/4 of items below low-water. */
	assert(szind < SC_NBINS);

	cache_bin_t *cache_bin = &tcache->bins[szind];
	cache_bin_sz_t ncached = cache_bin_ncached_get_local(cache_bin,
	    &tcache_bin_info[szind]);
	cache_bin_sz_t low_water = cache_bin_low_water_get(cache_bin,
	    &tcache_bin_info[szind]);
	assert(!tcache_slow->bin_refilled[szind]);

	size_t nflush = low_water - (low_water >> 2);
	if (nflush < tcache_slow->bin_flush_delay_items[szind]) {
		/* Workaround for a conversion warning. */
		uint8_t nflush_uint8 = (uint8_t)nflush;
		assert(sizeof(tcache_slow->bin_flush_delay_items[0]) ==
		    sizeof(nflush_uint8));
		tcache_slow->bin_flush_delay_items[szind] -= nflush_uint8;
		return;
	} else {
		tcache_slow->bin_flush_delay_items[szind]
		    = tcache_gc_item_delay_compute(szind);
	}

	tcache_bin_flush_small(tsd, tcache, cache_bin, szind,
	    (unsigned)(ncached - nflush));

	/*
	 * Reduce fill count by 2X.  Limit lg_fill_div such that
	 * the fill count is always at least 1.
	 */
	if ((cache_bin_info_ncached_max(&tcache_bin_info[szind])
	    >> (tcache_slow->lg_fill_div[szind] + 1)) >= 1) {
		tcache_slow->lg_fill_div[szind]++;
	}
}

static void
tcache_gc_large(tsd_t *tsd, tcache_slow_t *tcache_slow, tcache_t *tcache,
    szind_t szind) {
	/* Like the small GC; flush 3/4 of untouched items. */
	assert(szind >= SC_NBINS);
	cache_bin_t *cache_bin = &tcache->bins[szind];
	cache_bin_sz_t ncached = cache_bin_ncached_get_local(cache_bin,
	    &tcache_bin_info[szind]);
	cache_bin_sz_t low_water = cache_bin_low_water_get(cache_bin,
	    &tcache_bin_info[szind]);
	tcache_bin_flush_large(tsd, tcache, cache_bin, szind,
	    (unsigned)(ncached - low_water + (low_water >> 2)));
}

static void
tcache_event(tsd_t *tsd) {
	tcache_t *tcache = tcache_get(tsd);
	if (tcache == NULL) {
		return;
	}

	tcache_slow_t *tcache_slow = tsd_tcache_slowp_get(tsd);
	szind_t szind = tcache_slow->next_gc_bin;
	bool is_small = (szind < SC_NBINS);
	cache_bin_t *cache_bin = &tcache->bins[szind];

	tcache_bin_flush_stashed(tsd, tcache, cache_bin, szind, is_small);

	cache_bin_sz_t low_water = cache_bin_low_water_get(cache_bin,
	    &tcache_bin_info[szind]);
	if (low_water > 0) {
		if (is_small) {
			tcache_gc_small(tsd, tcache_slow, tcache, szind);
		} else {
			tcache_gc_large(tsd, tcache_slow, tcache, szind);
		}
	} else if (is_small && tcache_slow->bin_refilled[szind]) {
		assert(low_water == 0);
		/*
		 * Increase fill count by 2X for small bins.  Make sure
		 * lg_fill_div stays greater than 0.
		 */
		if (tcache_slow->lg_fill_div[szind] > 1) {
			tcache_slow->lg_fill_div[szind]--;
		}
		tcache_slow->bin_refilled[szind] = false;
	}
	cache_bin_low_water_set(cache_bin);

	tcache_slow->next_gc_bin++;
	if (tcache_slow->next_gc_bin == nhbins) {
		tcache_slow->next_gc_bin = 0;
	}
}

void
tcache_gc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	assert(elapsed == TE_INVALID_ELAPSED);
	tcache_event(tsd);
}

void
tcache_gc_dalloc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	assert(elapsed == TE_INVALID_ELAPSED);
	tcache_event(tsd);
}

void *
tcache_alloc_small_hard(tsdn_t *tsdn, arena_t *arena,
    tcache_t *tcache, cache_bin_t *cache_bin, szind_t binind,
    bool *tcache_success) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	void *ret;

	assert(tcache_slow->arena != NULL);
	unsigned nfill = cache_bin_info_ncached_max(&tcache_bin_info[binind])
	    >> tcache_slow->lg_fill_div[binind];
	arena_cache_bin_fill_small(tsdn, arena, cache_bin,
	    &tcache_bin_info[binind], binind, nfill);
	tcache_slow->bin_refilled[binind] = true;
	ret = cache_bin_alloc(cache_bin, tcache_success);

	return ret;
}

static const void *
tcache_bin_flush_ptr_getter(void *arr_ctx, size_t ind) {
	cache_bin_ptr_array_t *arr = (cache_bin_ptr_array_t *)arr_ctx;
	return arr->ptr[ind];
}

static void
tcache_bin_flush_metadata_visitor(void *szind_sum_ctx,
    emap_full_alloc_ctx_t *alloc_ctx) {
	size_t *szind_sum = (size_t *)szind_sum_ctx;
	*szind_sum -= alloc_ctx->szind;
	util_prefetch_write_range(alloc_ctx->edata, sizeof(edata_t));
}

JEMALLOC_NOINLINE static void
tcache_bin_flush_size_check_fail(cache_bin_ptr_array_t *arr, szind_t szind,
    size_t nptrs, emap_batch_lookup_result_t *edatas) {
	bool found_mismatch = false;
	for (size_t i = 0; i < nptrs; i++) {
		szind_t true_szind = edata_szind_get(edatas[i].edata);
		if (true_szind != szind) {
			found_mismatch = true;
			safety_check_fail_sized_dealloc(
			    /* current_dealloc */ false,
			    /* ptr */ tcache_bin_flush_ptr_getter(arr, i),
			    /* true_size */ sz_index2size(true_szind),
			    /* input_size */ sz_index2size(szind));
		}
	}
	assert(found_mismatch);
}

static void
tcache_bin_flush_edatas_lookup(tsd_t *tsd, cache_bin_ptr_array_t *arr,
    szind_t binind, size_t nflush, emap_batch_lookup_result_t *edatas) {

	/*
	 * This gets compiled away when config_opt_safety_checks is false.
	 * Checks for sized deallocation bugs, failing early rather than
	 * corrupting metadata.
	 */
	size_t szind_sum = binind * nflush;
	emap_edata_lookup_batch(tsd, &arena_emap_global, nflush,
	    &tcache_bin_flush_ptr_getter, (void *)arr,
	    &tcache_bin_flush_metadata_visitor, (void *)&szind_sum,
	    edatas);
	if (config_opt_safety_checks && unlikely(szind_sum != 0)) {
		tcache_bin_flush_size_check_fail(arr, binind, nflush, edatas);
	}
}

JEMALLOC_ALWAYS_INLINE bool
tcache_bin_flush_match(edata_t *edata, unsigned cur_arena_ind,
    unsigned cur_binshard, bool small) {
	if (small) {
		return edata_arena_ind_get(edata) == cur_arena_ind
		    && edata_binshard_get(edata) == cur_binshard;
	} else {
		return edata_arena_ind_get(edata) == cur_arena_ind;
	}
}

JEMALLOC_ALWAYS_INLINE void
tcache_bin_flush_impl(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, cache_bin_ptr_array_t *ptrs, unsigned nflush, bool small) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	/*
	 * A couple lookup calls take tsdn; declare it once for convenience
	 * instead of calling tsd_tsdn(tsd) all the time.
	 */
	tsdn_t *tsdn = tsd_tsdn(tsd);

	if (small) {
		assert(binind < SC_NBINS);
	} else {
		assert(binind < nhbins);
	}
	arena_t *tcache_arena = tcache_slow->arena;
	assert(tcache_arena != NULL);

	/*
	 * Variable length array must have > 0 length; the last element is never
	 * touched (it's just included to satisfy the no-zero-length rule).
	 */
	VARIABLE_ARRAY(emap_batch_lookup_result_t, item_edata, nflush + 1);
	tcache_bin_flush_edatas_lookup(tsd, ptrs, binind, nflush, item_edata);

	/*
	 * The slabs where we freed the last remaining object in the slab (and
	 * so need to free the slab itself).
	 * Used only if small == true.
	 */
	unsigned dalloc_count = 0;
	VARIABLE_ARRAY(edata_t *, dalloc_slabs, nflush + 1);

	/*
	 * We're about to grab a bunch of locks.  If one of them happens to be
	 * the one guarding the arena-level stats counters we flush our
	 * thread-local ones to, we do so under one critical section.
	 */
	bool merged_stats = false;
	while (nflush > 0) {
		/* Lock the arena, or bin, associated with the first object. */
		edata_t *edata = item_edata[0].edata;
		unsigned cur_arena_ind = edata_arena_ind_get(edata);
		arena_t *cur_arena = arena_get(tsdn, cur_arena_ind, false);

		/*
		 * These assignments are always overwritten when small is true,
		 * and their values are always ignored when small is false, but
		 * to avoid the technical UB when we pass them as parameters, we
		 * need to intialize them.
		 */
		unsigned cur_binshard = 0;
		bin_t *cur_bin = NULL;
		if (small) {
			cur_binshard = edata_binshard_get(edata);
			cur_bin = arena_get_bin(cur_arena, binind,
			    cur_binshard);
			assert(cur_binshard < bin_infos[binind].n_shards);
			/*
			 * If you're looking at profiles, you might think this
			 * is a good place to prefetch the bin stats, which are
			 * often a cache miss.  This turns out not to be
			 * helpful on the workloads we've looked at, with moving
			 * the bin stats next to the lock seeming to do better.
			 */
		}

		if (small) {
			malloc_mutex_lock(tsdn, &cur_bin->lock);
		}
		if (!small && !arena_is_auto(cur_arena)) {
			malloc_mutex_lock(tsdn, &cur_arena->large_mtx);
		}

		/*
		 * If we acquired the right lock and have some stats to flush,
		 * flush them.
		 */
		if (config_stats && tcache_arena == cur_arena
		    && !merged_stats) {
			merged_stats = true;
			if (small) {
				cur_bin->stats.nflushes++;
				cur_bin->stats.nrequests +=
				    cache_bin->tstats.nrequests;
				cache_bin->tstats.nrequests = 0;
			} else {
				arena_stats_large_flush_nrequests_add(tsdn,
				    &tcache_arena->stats, binind,
				    cache_bin->tstats.nrequests);
				cache_bin->tstats.nrequests = 0;
			}
		}

		/*
		 * Large allocations need special prep done.  Afterwards, we can
		 * drop the large lock.
		 */
		if (!small) {
			for (unsigned i = 0; i < nflush; i++) {
				void *ptr = ptrs->ptr[i];
				edata = item_edata[i].edata;
				assert(ptr != NULL && edata != NULL);

				if (tcache_bin_flush_match(edata, cur_arena_ind,
				    cur_binshard, small)) {
					large_dalloc_prep_locked(tsdn,
					    edata);
				}
			}
		}
		if (!small && !arena_is_auto(cur_arena)) {
			malloc_mutex_unlock(tsdn, &cur_arena->large_mtx);
		}

		/* Deallocate whatever we can. */
		unsigned ndeferred = 0;
		/* Init only to avoid used-uninitialized warning. */
		arena_dalloc_bin_locked_info_t dalloc_bin_info = {0};
		if (small) {
			arena_dalloc_bin_locked_begin(&dalloc_bin_info, binind);
		}
		for (unsigned i = 0; i < nflush; i++) {
			void *ptr = ptrs->ptr[i];
			edata = item_edata[i].edata;
			assert(ptr != NULL && edata != NULL);
			if (!tcache_bin_flush_match(edata, cur_arena_ind,
			    cur_binshard, small)) {
				/*
				 * The object was allocated either via a
				 * different arena, or a different bin in this
				 * arena.  Either way, stash the object so that
				 * it can be handled in a future pass.
				 */
				ptrs->ptr[ndeferred] = ptr;
				item_edata[ndeferred].edata = edata;
				ndeferred++;
				continue;
			}
			if (small) {
				if (arena_dalloc_bin_locked_step(tsdn,
				    cur_arena, cur_bin, &dalloc_bin_info,
				    binind, edata, ptr)) {
					dalloc_slabs[dalloc_count] = edata;
					dalloc_count++;
				}
			} else {
				if (large_dalloc_safety_checks(edata, ptr,
				    binind)) {
					/* See the comment in isfree. */
					continue;
				}
				large_dalloc_finish(tsdn, edata);
			}
		}

		if (small) {
			arena_dalloc_bin_locked_finish(tsdn, cur_arena, cur_bin,
			    &dalloc_bin_info);
			malloc_mutex_unlock(tsdn, &cur_bin->lock);
		}
		arena_decay_ticks(tsdn, cur_arena, nflush - ndeferred);
		nflush = ndeferred;
	}

	/* Handle all deferred slab dalloc. */
	assert(small || dalloc_count == 0);
	for (unsigned i = 0; i < dalloc_count; i++) {
		edata_t *slab = dalloc_slabs[i];
		arena_slab_dalloc(tsdn, arena_get_from_edata(slab), slab);

	}

	if (config_stats && !merged_stats) {
		if (small) {
			/*
			 * The flush loop didn't happen to flush to this
			 * thread's arena, so the stats didn't get merged.
			 * Manually do so now.
			 */
			bin_t *bin = arena_bin_choose(tsdn, tcache_arena,
			    binind, NULL);
			malloc_mutex_lock(tsdn, &bin->lock);
			bin->stats.nflushes++;
			bin->stats.nrequests += cache_bin->tstats.nrequests;
			cache_bin->tstats.nrequests = 0;
			malloc_mutex_unlock(tsdn, &bin->lock);
		} else {
			arena_stats_large_flush_nrequests_add(tsdn,
			    &tcache_arena->stats, binind,
			    cache_bin->tstats.nrequests);
			cache_bin->tstats.nrequests = 0;
		}
	}

}

JEMALLOC_ALWAYS_INLINE void
tcache_bin_flush_bottom(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, unsigned rem, bool small) {
	tcache_bin_flush_stashed(tsd, tcache, cache_bin, binind, small);

	cache_bin_sz_t ncached = cache_bin_ncached_get_local(cache_bin,
	    &tcache_bin_info[binind]);
	assert((cache_bin_sz_t)rem <= ncached);
	unsigned nflush = ncached - rem;

	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nflush);
	cache_bin_init_ptr_array_for_flush(cache_bin, &tcache_bin_info[binind],
	    &ptrs, nflush);

	tcache_bin_flush_impl(tsd, tcache, cache_bin, binind, &ptrs, nflush,
	    small);

	cache_bin_finish_flush(cache_bin, &tcache_bin_info[binind], &ptrs,
	    ncached - rem);
}

void
tcache_bin_flush_small(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, unsigned rem) {
	tcache_bin_flush_bottom(tsd, tcache, cache_bin, binind, rem, true);
}

void
tcache_bin_flush_large(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, unsigned rem) {
	tcache_bin_flush_bottom(tsd, tcache, cache_bin, binind, rem, false);
}

/*
 * Flushing stashed happens when 1) tcache fill, 2) tcache flush, or 3) tcache
 * GC event.  This makes sure that the stashed items do not hold memory for too
 * long, and new buffers can only be allocated when nothing is stashed.
 *
 * The downside is, the time between stash and flush may be relatively short,
 * especially when the request rate is high.  It lowers the chance of detecting
 * write-after-free -- however that is a delayed detection anyway, and is less
 * of a focus than the memory overhead.
 */
void
tcache_bin_flush_stashed(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, bool is_small) {
	cache_bin_info_t *info = &tcache_bin_info[binind];
	/*
	 * The two below are for assertion only.  The content of original cached
	 * items remain unchanged -- the stashed items reside on the other end
	 * of the stack.  Checking the stack head and ncached to verify.
	 */
	void *head_content = *cache_bin->stack_head;
	cache_bin_sz_t orig_cached = cache_bin_ncached_get_local(cache_bin,
	    info);

	cache_bin_sz_t nstashed = cache_bin_nstashed_get_local(cache_bin, info);
	assert(orig_cached + nstashed <= cache_bin_info_ncached_max(info));
	if (nstashed == 0) {
		return;
	}

	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nstashed);
	cache_bin_init_ptr_array_for_stashed(cache_bin, binind, info, &ptrs,
	    nstashed);
	san_check_stashed_ptrs(ptrs.ptr, nstashed, sz_index2size(binind));
	tcache_bin_flush_impl(tsd, tcache, cache_bin, binind, &ptrs, nstashed,
	    is_small);
	cache_bin_finish_flush_stashed(cache_bin, info);

	assert(cache_bin_nstashed_get_local(cache_bin, info) == 0);
	assert(cache_bin_ncached_get_local(cache_bin, info) == orig_cached);
	assert(head_content == *cache_bin->stack_head);
}

void
tcache_arena_associate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena) {
	assert(tcache_slow->arena == NULL);
	tcache_slow->arena = arena;

	if (config_stats) {
		/* Link into list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);

		ql_elm_new(tcache_slow, link);
		ql_tail_insert(&arena->tcache_ql, tcache_slow, link);
		cache_bin_array_descriptor_init(
		    &tcache_slow->cache_bin_array_descriptor, tcache->bins);
		ql_tail_insert(&arena->cache_bin_array_descriptor_ql,
		    &tcache_slow->cache_bin_array_descriptor, link);

		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
}

static void
tcache_arena_dissociate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache) {
	arena_t *arena = tcache_slow->arena;
	assert(arena != NULL);
	if (config_stats) {
		/* Unlink from list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
		if (config_debug) {
			bool in_ql = false;
			tcache_slow_t *iter;
			ql_foreach(iter, &arena->tcache_ql, link) {
				if (iter == tcache_slow) {
					in_ql = true;
					break;
				}
			}
			assert(in_ql);
		}
		ql_remove(&arena->tcache_ql, tcache_slow, link);
		ql_remove(&arena->cache_bin_array_descriptor_ql,
		    &tcache_slow->cache_bin_array_descriptor, link);
		tcache_stats_merge(tsdn, tcache_slow->tcache, arena);
		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
	tcache_slow->arena = NULL;
}

void
tcache_arena_reassociate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena) {
	tcache_arena_dissociate(tsdn, tcache_slow, tcache);
	tcache_arena_associate(tsdn, tcache_slow, tcache, arena);
}

bool
tsd_tcache_enabled_data_init(tsd_t *tsd) {
	/* Called upon tsd initialization. */
	tsd_tcache_enabled_set(tsd, opt_tcache);
	tsd_slow_update(tsd);

	if (opt_tcache) {
		/* Trigger tcache init. */
		tsd_tcache_data_init(tsd);
	}

	return false;
}

static void
tcache_init(tsd_t *tsd, tcache_slow_t *tcache_slow, tcache_t *tcache,
    void *mem) {
	tcache->tcache_slow = tcache_slow;
	tcache_slow->tcache = tcache;

	memset(&tcache_slow->link, 0, sizeof(ql_elm(tcache_t)));
	tcache_slow->next_gc_bin = 0;
	tcache_slow->arena = NULL;
	tcache_slow->dyn_alloc = mem;

	/*
	 * We reserve cache bins for all small size classes, even if some may
	 * not get used (i.e. bins higher than nhbins).  This allows the fast
	 * and common paths to access cache bin metadata safely w/o worrying
	 * about which ones are disabled.
	 */
	unsigned n_reserved_bins = nhbins < SC_NBINS ? SC_NBINS : nhbins;
	memset(tcache->bins, 0, sizeof(cache_bin_t) * n_reserved_bins);

	size_t cur_offset = 0;
	cache_bin_preincrement(tcache_bin_info, nhbins, mem,
	    &cur_offset);
	for (unsigned i = 0; i < nhbins; i++) {
		if (i < SC_NBINS) {
			tcache_slow->lg_fill_div[i] = 1;
			tcache_slow->bin_refilled[i] = false;
			tcache_slow->bin_flush_delay_items[i]
			    = tcache_gc_item_delay_compute(i);
		}
		cache_bin_t *cache_bin = &tcache->bins[i];
		cache_bin_init(cache_bin, &tcache_bin_info[i], mem,
		    &cur_offset);
	}
	/*
	 * For small size classes beyond tcache_maxclass (i.e. nhbins < NBINS),
	 * their cache bins are initialized to a state to safely and efficiently
	 * fail all fastpath alloc / free, so that no additional check around
	 * nhbins is needed on fastpath.
	 */
	for (unsigned i = nhbins; i < SC_NBINS; i++) {
		/* Disabled small bins. */
		cache_bin_t *cache_bin = &tcache->bins[i];
		void *fake_stack = mem;
		size_t fake_offset = 0;

		cache_bin_init(cache_bin, &tcache_bin_info[i], fake_stack,
		    &fake_offset);
		assert(tcache_small_bin_disabled(i, cache_bin));
	}

	cache_bin_postincrement(tcache_bin_info, nhbins, mem,
	    &cur_offset);
	/* Sanity check that the whole stack is used. */
	assert(cur_offset == tcache_bin_alloc_size);
}

/* Initialize auto tcache (embedded in TSD). */
bool
tsd_tcache_data_init(tsd_t *tsd) {
	tcache_slow_t *tcache_slow = tsd_tcache_slowp_get_unsafe(tsd);
	tcache_t *tcache = tsd_tcachep_get_unsafe(tsd);

	assert(cache_bin_still_zero_initialized(&tcache->bins[0]));
	size_t alignment = tcache_bin_alloc_alignment;
	size_t size = sz_sa2u(tcache_bin_alloc_size, alignment);

	void *mem = ipallocztm(tsd_tsdn(tsd), size, alignment, true, NULL,
	    true, arena_get(TSDN_NULL, 0, true));
	if (mem == NULL) {
		return true;
	}

	tcache_init(tsd, tcache_slow, tcache, mem);
	/*
	 * Initialization is a bit tricky here.  After malloc init is done, all
	 * threads can rely on arena_choose and associate tcache accordingly.
	 * However, the thread that does actual malloc bootstrapping relies on
	 * functional tsd, and it can only rely on a0.  In that case, we
	 * associate its tcache to a0 temporarily, and later on
	 * arena_choose_hard() will re-associate properly.
	 */
	tcache_slow->arena = NULL;
	arena_t *arena;
	if (!malloc_initialized()) {
		/* If in initialization, assign to a0. */
		arena = arena_get(tsd_tsdn(tsd), 0, false);
		tcache_arena_associate(tsd_tsdn(tsd), tcache_slow, tcache,
		    arena);
	} else {
		arena = arena_choose(tsd, NULL);
		/* This may happen if thread.tcache.enabled is used. */
		if (tcache_slow->arena == NULL) {
			tcache_arena_associate(tsd_tsdn(tsd), tcache_slow,
			    tcache, arena);
		}
	}
	assert(arena == tcache_slow->arena);

	return false;
}

/* Created manual tcache for tcache.create mallctl. */
tcache_t *
tcache_create_explicit(tsd_t *tsd) {
	/*
	 * We place the cache bin stacks, then the tcache_t, then a pointer to
	 * the beginning of the whole allocation (for freeing).  The makes sure
	 * the cache bins have the requested alignment.
	 */
	size_t size = tcache_bin_alloc_size + sizeof(tcache_t)
	    + sizeof(tcache_slow_t);
	/* Naturally align the pointer stacks. */
	size = PTR_CEILING(size);
	size = sz_sa2u(size, tcache_bin_alloc_alignment);

	void *mem = ipallocztm(tsd_tsdn(tsd), size, tcache_bin_alloc_alignment,
	    true, NULL, true, arena_get(TSDN_NULL, 0, true));
	if (mem == NULL) {
		return NULL;
	}
	tcache_t *tcache = (void *)((uintptr_t)mem + tcache_bin_alloc_size);
	tcache_slow_t *tcache_slow =
	    (void *)((uintptr_t)mem + tcache_bin_alloc_size + sizeof(tcache_t));
	tcache_init(tsd, tcache_slow, tcache, mem);

	tcache_arena_associate(tsd_tsdn(tsd), tcache_slow, tcache,
	    arena_ichoose(tsd, NULL));

	return tcache;
}

static void
tcache_flush_cache(tsd_t *tsd, tcache_t *tcache) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	assert(tcache_slow->arena != NULL);

	for (unsigned i = 0; i < nhbins; i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		if (i < SC_NBINS) {
			tcache_bin_flush_small(tsd, tcache, cache_bin, i, 0);
		} else {
			tcache_bin_flush_large(tsd, tcache, cache_bin, i, 0);
		}
		if (config_stats) {
			assert(cache_bin->tstats.nrequests == 0);
		}
	}
}

void
tcache_flush(tsd_t *tsd) {
	assert(tcache_available(tsd));
	tcache_flush_cache(tsd, tsd_tcachep_get(tsd));
}

static void
tcache_destroy(tsd_t *tsd, tcache_t *tcache, bool tsd_tcache) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	tcache_flush_cache(tsd, tcache);
	arena_t *arena = tcache_slow->arena;
	tcache_arena_dissociate(tsd_tsdn(tsd), tcache_slow, tcache);

	if (tsd_tcache) {
		cache_bin_t *cache_bin = &tcache->bins[0];
		cache_bin_assert_empty(cache_bin, &tcache_bin_info[0]);
	}
	idalloctm(tsd_tsdn(tsd), tcache_slow->dyn_alloc, NULL, NULL, true,
	    true);

	/*
	 * The deallocation and tcache flush above may not trigger decay since
	 * we are on the tcache shutdown path (potentially with non-nominal
	 * tsd).  Manually trigger decay to avoid pathological cases.  Also
	 * include arena 0 because the tcache array is allocated from it.
	 */
	arena_decay(tsd_tsdn(tsd), arena_get(tsd_tsdn(tsd), 0, false),
	    false, false);

	if (arena_nthreads_get(arena, false) == 0 &&
	    !background_thread_enabled()) {
		/* Force purging when no threads assigned to the arena anymore. */
		arena_decay(tsd_tsdn(tsd), arena,
		    /* is_background_thread */ false, /* all */ true);
	} else {
		arena_decay(tsd_tsdn(tsd), arena,
		    /* is_background_thread */ false, /* all */ false);
	}
}

/* For auto tcache (embedded in TSD) only. */
void
tcache_cleanup(tsd_t *tsd) {
	tcache_t *tcache = tsd_tcachep_get(tsd);
	if (!tcache_available(tsd)) {
		assert(tsd_tcache_enabled_get(tsd) == false);
		assert(cache_bin_still_zero_initialized(&tcache->bins[0]));
		return;
	}
	assert(tsd_tcache_enabled_get(tsd));
	assert(!cache_bin_still_zero_initialized(&tcache->bins[0]));

	tcache_destroy(tsd, tcache, true);
	if (config_debug) {
		/*
		 * For debug testing only, we want to pretend we're still in the
		 * zero-initialized state.
		 */
		memset(tcache->bins, 0, sizeof(cache_bin_t) * nhbins);
	}
}

void
tcache_stats_merge(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	cassert(config_stats);

	/* Merge and reset tcache stats. */
	for (unsigned i = 0; i < nhbins; i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		if (i < SC_NBINS) {
			bin_t *bin = arena_bin_choose(tsdn, arena, i, NULL);
			malloc_mutex_lock(tsdn, &bin->lock);
			bin->stats.nrequests += cache_bin->tstats.nrequests;
			malloc_mutex_unlock(tsdn, &bin->lock);
		} else {
			arena_stats_large_flush_nrequests_add(tsdn,
			    &arena->stats, i, cache_bin->tstats.nrequests);
		}
		cache_bin->tstats.nrequests = 0;
	}
}

static bool
tcaches_create_prep(tsd_t *tsd, base_t *base) {
	bool err;

	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tcaches_mtx);

	if (tcaches == NULL) {
		tcaches = base_alloc(tsd_tsdn(tsd), base,
		    sizeof(tcache_t *) * (MALLOCX_TCACHE_MAX+1), CACHELINE);
		if (tcaches == NULL) {
			err = true;
			goto label_return;
		}
	}

	if (tcaches_avail == NULL && tcaches_past > MALLOCX_TCACHE_MAX) {
		err = true;
		goto label_return;
	}

	err = false;
label_return:
	return err;
}

bool
tcaches_create(tsd_t *tsd, base_t *base, unsigned *r_ind) {
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);

	bool err;

	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);

	if (tcaches_create_prep(tsd, base)) {
		err = true;
		goto label_return;
	}

	tcache_t *tcache = tcache_create_explicit(tsd);
	if (tcache == NULL) {
		err = true;
		goto label_return;
	}

	tcaches_t *elm;
	if (tcaches_avail != NULL) {
		elm = tcaches_avail;
		tcaches_avail = tcaches_avail->next;
		elm->tcache = tcache;
		*r_ind = (unsigned)(elm - tcaches);
	} else {
		elm = &tcaches[tcaches_past];
		elm->tcache = tcache;
		*r_ind = tcaches_past;
		tcaches_past++;
	}

	err = false;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);
	return err;
}

static tcache_t *
tcaches_elm_remove(tsd_t *tsd, tcaches_t *elm, bool allow_reinit) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tcaches_mtx);

	if (elm->tcache == NULL) {
		return NULL;
	}
	tcache_t *tcache = elm->tcache;
	if (allow_reinit) {
		elm->tcache = TCACHES_ELM_NEED_REINIT;
	} else {
		elm->tcache = NULL;
	}

	if (tcache == TCACHES_ELM_NEED_REINIT) {
		return NULL;
	}
	return tcache;
}

void
tcaches_flush(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcache_t *tcache = tcaches_elm_remove(tsd, &tcaches[ind], true);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		/* Destroy the tcache; recreate in tcaches_get() if needed. */
		tcache_destroy(tsd, tcache, false);
	}
}

void
tcaches_destroy(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcaches_t *elm = &tcaches[ind];
	tcache_t *tcache = tcaches_elm_remove(tsd, elm, false);
	elm->next = tcaches_avail;
	tcaches_avail = elm;
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		tcache_destroy(tsd, tcache, false);
	}
}

static unsigned
tcache_ncached_max_compute(szind_t szind) {
	if (szind >= SC_NBINS) {
		assert(szind < nhbins);
		return opt_tcache_nslots_large;
	}
	unsigned slab_nregs = bin_infos[szind].nregs;

	/* We may modify these values; start with the opt versions. */
	unsigned nslots_small_min = opt_tcache_nslots_small_min;
	unsigned nslots_small_max = opt_tcache_nslots_small_max;

	/*
	 * Clamp values to meet our constraints -- even, nonzero, min < max, and
	 * suitable for a cache bin size.
	 */
	if (opt_tcache_nslots_small_max > CACHE_BIN_NCACHED_MAX) {
		nslots_small_max = CACHE_BIN_NCACHED_MAX;
	}
	if (nslots_small_min % 2 != 0) {
		nslots_small_min++;
	}
	if (nslots_small_max % 2 != 0) {
		nslots_small_max--;
	}
	if (nslots_small_min < 2) {
		nslots_small_min = 2;
	}
	if (nslots_small_max < 2) {
		nslots_small_max = 2;
	}
	if (nslots_small_min > nslots_small_max) {
		nslots_small_min = nslots_small_max;
	}

	unsigned candidate;
	if (opt_lg_tcache_nslots_mul < 0) {
		candidate = slab_nregs >> (-opt_lg_tcache_nslots_mul);
	} else {
		candidate = slab_nregs << opt_lg_tcache_nslots_mul;
	}
	if (candidate % 2 != 0) {
		/*
		 * We need the candidate size to be even -- we assume that we
		 * can divide by two and get a positive number (e.g. when
		 * flushing).
		 */
		++candidate;
	}
	if (candidate <= nslots_small_min) {
		return nslots_small_min;
	} else if (candidate <= nslots_small_max) {
		return candidate;
	} else {
		return nslots_small_max;
	}
}

bool
tcache_boot(tsdn_t *tsdn, base_t *base) {
	tcache_maxclass = sz_s2u(opt_tcache_max);
	assert(tcache_maxclass <= TCACHE_MAXCLASS_LIMIT);
	nhbins = sz_size2index(tcache_maxclass) + 1;

	if (malloc_mutex_init(&tcaches_mtx, "tcaches", WITNESS_RANK_TCACHES,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}

	/* Initialize tcache_bin_info.  See comments in tcache_init(). */
	unsigned n_reserved_bins = nhbins < SC_NBINS ? SC_NBINS : nhbins;
	size_t size = n_reserved_bins * sizeof(cache_bin_info_t);
	tcache_bin_info = (cache_bin_info_t *)base_alloc(tsdn, base, size,
	    CACHELINE);
	if (tcache_bin_info == NULL) {
		return true;
	}

	for (szind_t i = 0; i < nhbins; i++) {
		unsigned ncached_max = tcache_ncached_max_compute(i);
		cache_bin_info_init(&tcache_bin_info[i], ncached_max);
	}
	for (szind_t i = nhbins; i < SC_NBINS; i++) {
		/* Disabled small bins. */
		cache_bin_info_init(&tcache_bin_info[i], 0);
		assert(tcache_small_bin_disabled(i, NULL));
	}

	cache_bin_info_compute_alloc(tcache_bin_info, nhbins,
	    &tcache_bin_alloc_size, &tcache_bin_alloc_alignment);

	return false;
}

void
tcache_prefork(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &tcaches_mtx);
}

void
tcache_postfork_parent(tsdn_t *tsdn) {
	malloc_mutex_postfork_parent(tsdn, &tcaches_mtx);
}

void
tcache_postfork_child(tsdn_t *tsdn) {
	malloc_mutex_postfork_child(tsdn, &tcaches_mtx);
}

void tcache_assert_initialized(tcache_t *tcache) {
	assert(!cache_bin_still_zero_initialized(&tcache->bins[0]));
}
