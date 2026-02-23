#ifndef JEMALLOC_INTERNAL_PROF_INLINES_H
#define JEMALLOC_INTERNAL_PROF_INLINES_H

#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/thread_event.h"

JEMALLOC_ALWAYS_INLINE void
prof_active_assert() {
	cassert(config_prof);
	/*
	 * If opt_prof is off, then prof_active must always be off, regardless
	 * of whether prof_active_mtx is in effect or not.
	 */
	assert(opt_prof || !prof_active_state);
}

JEMALLOC_ALWAYS_INLINE bool
prof_active_get_unlocked(void) {
	prof_active_assert();
	/*
	 * Even if opt_prof is true, sampling can be temporarily disabled by
	 * setting prof_active to false.  No locking is used when reading
	 * prof_active in the fast path, so there are no guarantees regarding
	 * how long it will take for all threads to notice state changes.
	 */
	return prof_active_state;
}

JEMALLOC_ALWAYS_INLINE bool
prof_gdump_get_unlocked(void) {
	/*
	 * No locking is used when reading prof_gdump_val in the fast path, so
	 * there are no guarantees regarding how long it will take for all
	 * threads to notice state changes.
	 */
	return prof_gdump_val;
}

JEMALLOC_ALWAYS_INLINE prof_tdata_t *
prof_tdata_get(tsd_t *tsd, bool create) {
	prof_tdata_t *tdata;

	cassert(config_prof);

	tdata = tsd_prof_tdata_get(tsd);
	if (create) {
		assert(tsd_reentrancy_level_get(tsd) == 0);
		if (unlikely(tdata == NULL)) {
			if (tsd_nominal(tsd)) {
				tdata = prof_tdata_init(tsd);
				tsd_prof_tdata_set(tsd, tdata);
			}
		} else if (unlikely(tdata->expired)) {
			tdata = prof_tdata_reinit(tsd, tdata);
			tsd_prof_tdata_set(tsd, tdata);
		}
		assert(tdata == NULL || tdata->attached);
	}

	return tdata;
}

JEMALLOC_ALWAYS_INLINE void
prof_info_get(tsd_t *tsd, const void *ptr, emap_alloc_ctx_t *alloc_ctx,
    prof_info_t *prof_info) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(prof_info != NULL);

	arena_prof_info_get(tsd, ptr, alloc_ctx, prof_info, false);
}

JEMALLOC_ALWAYS_INLINE void
prof_info_get_and_reset_recent(tsd_t *tsd, const void *ptr,
    emap_alloc_ctx_t *alloc_ctx, prof_info_t *prof_info) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(prof_info != NULL);

	arena_prof_info_get(tsd, ptr, alloc_ctx, prof_info, true);
}

JEMALLOC_ALWAYS_INLINE void
prof_tctx_reset(tsd_t *tsd, const void *ptr, emap_alloc_ctx_t *alloc_ctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	arena_prof_tctx_reset(tsd, ptr, alloc_ctx);
}

JEMALLOC_ALWAYS_INLINE void
prof_tctx_reset_sampled(tsd_t *tsd, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	arena_prof_tctx_reset_sampled(tsd, ptr);
}

JEMALLOC_ALWAYS_INLINE void
prof_info_set(tsd_t *tsd, edata_t *edata, prof_tctx_t *tctx, size_t size) {
	cassert(config_prof);
	assert(edata != NULL);
	assert((uintptr_t)tctx > (uintptr_t)1U);

	arena_prof_info_set(tsd, edata, tctx, size);
}

JEMALLOC_ALWAYS_INLINE bool
prof_sample_should_skip(tsd_t *tsd, bool sample_event) {
	cassert(config_prof);

	/* Fastpath: no need to load tdata */
	if (likely(!sample_event)) {
		return true;
	}

	/*
	 * sample_event is always obtained from the thread event module, and
	 * whenever it's true, it means that the thread event module has
	 * already checked the reentrancy level.
	 */
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata = prof_tdata_get(tsd, true);
	if (unlikely(tdata == NULL)) {
		return true;
	}

	return !tdata->active;
}

JEMALLOC_ALWAYS_INLINE prof_tctx_t *
prof_alloc_prep(tsd_t *tsd, bool prof_active, bool sample_event) {
	prof_tctx_t *ret;

	if (!prof_active ||
	    likely(prof_sample_should_skip(tsd, sample_event))) {
		ret = (prof_tctx_t *)(uintptr_t)1U;
	} else {
		ret = prof_tctx_create(tsd);
	}

	return ret;
}

JEMALLOC_ALWAYS_INLINE void
prof_malloc(tsd_t *tsd, const void *ptr, size_t size, size_t usize,
    emap_alloc_ctx_t *alloc_ctx, prof_tctx_t *tctx) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(usize == isalloc(tsd_tsdn(tsd), ptr));

	if (unlikely((uintptr_t)tctx > (uintptr_t)1U)) {
		prof_malloc_sample_object(tsd, ptr, size, usize, tctx);
	} else {
		prof_tctx_reset(tsd, ptr, alloc_ctx);
	}
}

JEMALLOC_ALWAYS_INLINE void
prof_realloc(tsd_t *tsd, const void *ptr, size_t size, size_t usize,
    prof_tctx_t *tctx, bool prof_active, const void *old_ptr, size_t old_usize,
    prof_info_t *old_prof_info, bool sample_event) {
	bool sampled, old_sampled, moved;

	cassert(config_prof);
	assert(ptr != NULL || (uintptr_t)tctx <= (uintptr_t)1U);

	if (prof_active && ptr != NULL) {
		assert(usize == isalloc(tsd_tsdn(tsd), ptr));
		if (prof_sample_should_skip(tsd, sample_event)) {
			/*
			 * Don't sample.  The usize passed to prof_alloc_prep()
			 * was larger than what actually got allocated, so a
			 * backtrace was captured for this allocation, even
			 * though its actual usize was insufficient to cross the
			 * sample threshold.
			 */
			prof_alloc_rollback(tsd, tctx);
			tctx = (prof_tctx_t *)(uintptr_t)1U;
		}
	}

	sampled = ((uintptr_t)tctx > (uintptr_t)1U);
	old_sampled = ((uintptr_t)old_prof_info->alloc_tctx > (uintptr_t)1U);
	moved = (ptr != old_ptr);

	if (unlikely(sampled)) {
		prof_malloc_sample_object(tsd, ptr, size, usize, tctx);
	} else if (moved) {
		prof_tctx_reset(tsd, ptr, NULL);
	} else if (unlikely(old_sampled)) {
		/*
		 * prof_tctx_reset() would work for the !moved case as well,
		 * but prof_tctx_reset_sampled() is slightly cheaper, and the
		 * proper thing to do here in the presence of explicit
		 * knowledge re: moved state.
		 */
		prof_tctx_reset_sampled(tsd, ptr);
	} else {
		prof_info_t prof_info;
		prof_info_get(tsd, ptr, NULL, &prof_info);
		assert((uintptr_t)prof_info.alloc_tctx == (uintptr_t)1U);
	}

	/*
	 * The prof_free_sampled_object() call must come after the
	 * prof_malloc_sample_object() call, because tctx and old_tctx may be
	 * the same, in which case reversing the call order could cause the tctx
	 * to be prematurely destroyed as a side effect of momentarily zeroed
	 * counters.
	 */
	if (unlikely(old_sampled)) {
		prof_free_sampled_object(tsd, old_usize, old_prof_info);
	}
}

JEMALLOC_ALWAYS_INLINE size_t
prof_sample_align(size_t orig_align) {
	/*
	 * Enforce page alignment, so that sampled allocations can be identified
	 * w/o metadata lookup.
	 */
	assert(opt_prof);
	return (opt_cache_oblivious && orig_align < PAGE) ? PAGE :
	    orig_align;
}

JEMALLOC_ALWAYS_INLINE bool
prof_sample_aligned(const void *ptr) {
	return ((uintptr_t)ptr & PAGE_MASK) == 0;
}

JEMALLOC_ALWAYS_INLINE bool
prof_sampled(tsd_t *tsd, const void *ptr) {
	prof_info_t prof_info;
	prof_info_get(tsd, ptr, NULL, &prof_info);
	bool sampled = (uintptr_t)prof_info.alloc_tctx > (uintptr_t)1U;
	if (sampled) {
		assert(prof_sample_aligned(ptr));
	}
	return sampled;
}

JEMALLOC_ALWAYS_INLINE void
prof_free(tsd_t *tsd, const void *ptr, size_t usize,
    emap_alloc_ctx_t *alloc_ctx) {
	prof_info_t prof_info;
	prof_info_get_and_reset_recent(tsd, ptr, alloc_ctx, &prof_info);

	cassert(config_prof);
	assert(usize == isalloc(tsd_tsdn(tsd), ptr));

	if (unlikely((uintptr_t)prof_info.alloc_tctx > (uintptr_t)1U)) {
		assert(prof_sample_aligned(ptr));
		prof_free_sampled_object(tsd, usize, &prof_info);
	}
}

#endif /* JEMALLOC_INTERNAL_PROF_INLINES_H */
