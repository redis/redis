#ifndef JEMALLOC_INTERNAL_TCACHE_INLINES_H
#define JEMALLOC_INTERNAL_TCACHE_INLINES_H

#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/util.h"

static inline bool
tcache_enabled_get(tsd_t *tsd) {
	return tsd_tcache_enabled_get(tsd);
}

static inline void
tcache_enabled_set(tsd_t *tsd, bool enabled) {
	bool was_enabled = tsd_tcache_enabled_get(tsd);

	if (!was_enabled && enabled) {
		tsd_tcache_data_init(tsd);
	} else if (was_enabled && !enabled) {
		tcache_cleanup(tsd);
	}
	/* Commit the state last.  Above calls check current state. */
	tsd_tcache_enabled_set(tsd, enabled);
	tsd_slow_update(tsd);
}

JEMALLOC_ALWAYS_INLINE bool
tcache_small_bin_disabled(szind_t ind, cache_bin_t *bin) {
	assert(ind < SC_NBINS);
	bool ret = (cache_bin_info_ncached_max(&tcache_bin_info[ind]) == 0);
	if (ret && bin != NULL) {
		/* small size class but cache bin disabled. */
		assert(ind >= nhbins);
		assert((uintptr_t)(*bin->stack_head) ==
		    cache_bin_preceding_junk);
	}

	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
tcache_alloc_small(tsd_t *tsd, arena_t *arena, tcache_t *tcache,
    size_t size, szind_t binind, bool zero, bool slow_path) {
	void *ret;
	bool tcache_success;

	assert(binind < SC_NBINS);
	cache_bin_t *bin = &tcache->bins[binind];
	ret = cache_bin_alloc(bin, &tcache_success);
	assert(tcache_success == (ret != NULL));
	if (unlikely(!tcache_success)) {
		bool tcache_hard_success;
		arena = arena_choose(tsd, arena);
		if (unlikely(arena == NULL)) {
			return NULL;
		}
		if (unlikely(tcache_small_bin_disabled(binind, bin))) {
			/* stats and zero are handled directly by the arena. */
			return arena_malloc_hard(tsd_tsdn(tsd), arena, size,
			    binind, zero);
		}
		tcache_bin_flush_stashed(tsd, tcache, bin, binind,
		    /* is_small */ true);

		ret = tcache_alloc_small_hard(tsd_tsdn(tsd), arena, tcache,
		    bin, binind, &tcache_hard_success);
		if (tcache_hard_success == false) {
			return NULL;
		}
	}

	assert(ret);
	if (unlikely(zero)) {
		size_t usize = sz_index2size(binind);
		assert(tcache_salloc(tsd_tsdn(tsd), ret) == usize);
		memset(ret, 0, usize);
	}
	if (config_stats) {
		bin->tstats.nrequests++;
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
tcache_alloc_large(tsd_t *tsd, arena_t *arena, tcache_t *tcache, size_t size,
    szind_t binind, bool zero, bool slow_path) {
	void *ret;
	bool tcache_success;

	assert(binind >= SC_NBINS && binind < nhbins);
	cache_bin_t *bin = &tcache->bins[binind];
	ret = cache_bin_alloc(bin, &tcache_success);
	assert(tcache_success == (ret != NULL));
	if (unlikely(!tcache_success)) {
		/*
		 * Only allocate one large object at a time, because it's quite
		 * expensive to create one and not use it.
		 */
		arena = arena_choose(tsd, arena);
		if (unlikely(arena == NULL)) {
			return NULL;
		}
		tcache_bin_flush_stashed(tsd, tcache, bin, binind,
		    /* is_small */ false);

		ret = large_malloc(tsd_tsdn(tsd), arena, sz_s2u(size), zero);
		if (ret == NULL) {
			return NULL;
		}
	} else {
		if (unlikely(zero)) {
			size_t usize = sz_index2size(binind);
			assert(usize <= tcache_maxclass);
			memset(ret, 0, usize);
		}

		if (config_stats) {
			bin->tstats.nrequests++;
		}
	}

	return ret;
}

JEMALLOC_ALWAYS_INLINE void
tcache_dalloc_small(tsd_t *tsd, tcache_t *tcache, void *ptr, szind_t binind,
    bool slow_path) {
	assert(tcache_salloc(tsd_tsdn(tsd), ptr) <= SC_SMALL_MAXCLASS);

	cache_bin_t *bin = &tcache->bins[binind];
	/*
	 * Not marking the branch unlikely because this is past free_fastpath()
	 * (which handles the most common cases), i.e. at this point it's often
	 * uncommon cases.
	 */
	if (cache_bin_nonfast_aligned(ptr)) {
		/* Junk unconditionally, even if bin is full. */
		san_junk_ptr(ptr, sz_index2size(binind));
		if (cache_bin_stash(bin, ptr)) {
			return;
		}
		assert(cache_bin_full(bin));
		/* Bin full; fall through into the flush branch. */
	}

	if (unlikely(!cache_bin_dalloc_easy(bin, ptr))) {
		if (unlikely(tcache_small_bin_disabled(binind, bin))) {
			arena_dalloc_small(tsd_tsdn(tsd), ptr);
			return;
		}
		cache_bin_sz_t max = cache_bin_info_ncached_max(
		    &tcache_bin_info[binind]);
		unsigned remain = max >> opt_lg_tcache_flush_small_div;
		tcache_bin_flush_small(tsd, tcache, bin, binind, remain);
		bool ret = cache_bin_dalloc_easy(bin, ptr);
		assert(ret);
	}
}

JEMALLOC_ALWAYS_INLINE void
tcache_dalloc_large(tsd_t *tsd, tcache_t *tcache, void *ptr, szind_t binind,
    bool slow_path) {

	assert(tcache_salloc(tsd_tsdn(tsd), ptr)
	    > SC_SMALL_MAXCLASS);
	assert(tcache_salloc(tsd_tsdn(tsd), ptr) <= tcache_maxclass);

	cache_bin_t *bin = &tcache->bins[binind];
	if (unlikely(!cache_bin_dalloc_easy(bin, ptr))) {
		unsigned remain = cache_bin_info_ncached_max(
		    &tcache_bin_info[binind]) >> opt_lg_tcache_flush_large_div;
		tcache_bin_flush_large(tsd, tcache, bin, binind, remain);
		bool ret = cache_bin_dalloc_easy(bin, ptr);
		assert(ret);
	}
}

JEMALLOC_ALWAYS_INLINE tcache_t *
tcaches_get(tsd_t *tsd, unsigned ind) {
	tcaches_t *elm = &tcaches[ind];
	if (unlikely(elm->tcache == NULL)) {
		malloc_printf("<jemalloc>: invalid tcache id (%u).\n", ind);
		abort();
	} else if (unlikely(elm->tcache == TCACHES_ELM_NEED_REINIT)) {
		elm->tcache = tcache_create_explicit(tsd);
	}
	return elm->tcache;
}

#endif /* JEMALLOC_INTERNAL_TCACHE_INLINES_H */
