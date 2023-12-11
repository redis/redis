#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

bool
edata_cache_init(edata_cache_t *edata_cache, base_t *base) {
	edata_avail_new(&edata_cache->avail);
	/*
	 * This is not strictly necessary, since the edata_cache_t is only
	 * created inside an arena, which is zeroed on creation.  But this is
	 * handy as a safety measure.
	 */
	atomic_store_zu(&edata_cache->count, 0, ATOMIC_RELAXED);
	if (malloc_mutex_init(&edata_cache->mtx, "edata_cache",
	    WITNESS_RANK_EDATA_CACHE, malloc_mutex_rank_exclusive)) {
		return true;
	}
	edata_cache->base = base;
	return false;
}

edata_t *
edata_cache_get(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_lock(tsdn, &edata_cache->mtx);
	edata_t *edata = edata_avail_first(&edata_cache->avail);
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &edata_cache->mtx);
		return base_alloc_edata(tsdn, edata_cache->base);
	}
	edata_avail_remove(&edata_cache->avail, edata);
	atomic_load_sub_store_zu(&edata_cache->count, 1);
	malloc_mutex_unlock(tsdn, &edata_cache->mtx);
	return edata;
}

void
edata_cache_put(tsdn_t *tsdn, edata_cache_t *edata_cache, edata_t *edata) {
	malloc_mutex_lock(tsdn, &edata_cache->mtx);
	edata_avail_insert(&edata_cache->avail, edata);
	atomic_load_add_store_zu(&edata_cache->count, 1);
	malloc_mutex_unlock(tsdn, &edata_cache->mtx);
}

void
edata_cache_prefork(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_prefork(tsdn, &edata_cache->mtx);
}

void
edata_cache_postfork_parent(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_parent(tsdn, &edata_cache->mtx);
}

void
edata_cache_postfork_child(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_child(tsdn, &edata_cache->mtx);
}

void
edata_cache_fast_init(edata_cache_fast_t *ecs, edata_cache_t *fallback) {
	edata_list_inactive_init(&ecs->list);
	ecs->fallback = fallback;
	ecs->disabled = false;
}

static void
edata_cache_fast_try_fill_from_fallback(tsdn_t *tsdn,
    edata_cache_fast_t *ecs) {
	edata_t *edata;
	malloc_mutex_lock(tsdn, &ecs->fallback->mtx);
	for (int i = 0; i < EDATA_CACHE_FAST_FILL; i++) {
		edata = edata_avail_remove_first(&ecs->fallback->avail);
		if (edata == NULL) {
			break;
		}
		edata_list_inactive_append(&ecs->list, edata);
		atomic_load_sub_store_zu(&ecs->fallback->count, 1);
	}
	malloc_mutex_unlock(tsdn, &ecs->fallback->mtx);
}

edata_t *
edata_cache_fast_get(tsdn_t *tsdn, edata_cache_fast_t *ecs) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_EDATA_CACHE, 0);

	if (ecs->disabled) {
		assert(edata_list_inactive_first(&ecs->list) == NULL);
		return edata_cache_get(tsdn, ecs->fallback);
	}

	edata_t *edata = edata_list_inactive_first(&ecs->list);
	if (edata != NULL) {
		edata_list_inactive_remove(&ecs->list, edata);
		return edata;
	}
	/* Slow path; requires synchronization. */
	edata_cache_fast_try_fill_from_fallback(tsdn, ecs);
	edata = edata_list_inactive_first(&ecs->list);
	if (edata != NULL) {
		edata_list_inactive_remove(&ecs->list, edata);
	} else {
		/*
		 * Slowest path (fallback was also empty); allocate something
		 * new.
		 */
		edata = base_alloc_edata(tsdn, ecs->fallback->base);
	}
	return edata;
}

static void
edata_cache_fast_flush_all(tsdn_t *tsdn, edata_cache_fast_t *ecs) {
	/*
	 * You could imagine smarter cache management policies (like
	 * only flushing down to some threshold in anticipation of
	 * future get requests).  But just flushing everything provides
	 * a good opportunity to defrag too, and lets us share code between the
	 * flush and disable pathways.
	 */
	edata_t *edata;
	size_t nflushed = 0;
	malloc_mutex_lock(tsdn, &ecs->fallback->mtx);
	while ((edata = edata_list_inactive_first(&ecs->list)) != NULL) {
		edata_list_inactive_remove(&ecs->list, edata);
		edata_avail_insert(&ecs->fallback->avail, edata);
		nflushed++;
	}
	atomic_load_add_store_zu(&ecs->fallback->count, nflushed);
	malloc_mutex_unlock(tsdn, &ecs->fallback->mtx);
}

void
edata_cache_fast_put(tsdn_t *tsdn, edata_cache_fast_t *ecs, edata_t *edata) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_EDATA_CACHE, 0);

	if (ecs->disabled) {
		assert(edata_list_inactive_first(&ecs->list) == NULL);
		edata_cache_put(tsdn, ecs->fallback, edata);
		return;
	}

	/*
	 * Prepend rather than append, to do LIFO ordering in the hopes of some
	 * cache locality.
	 */
	edata_list_inactive_prepend(&ecs->list, edata);
}

void
edata_cache_fast_disable(tsdn_t *tsdn, edata_cache_fast_t *ecs) {
	edata_cache_fast_flush_all(tsdn, ecs);
	ecs->disabled = true;
}
