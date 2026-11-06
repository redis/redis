#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

void
inspect_extent_util_stats_get(tsdn_t *tsdn, const void *ptr, size_t *nfree,
    size_t *nregs, size_t *size) {
	assert(ptr != NULL && nfree != NULL && nregs != NULL && size != NULL);

	const edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	if (unlikely(edata == NULL)) {
		*nfree = *nregs = *size = 0;
		return;
	}

	*size = edata_size_get(edata);
	if (!edata_slab_get(edata)) {
		*nfree = 0;
		*nregs = 1;
	} else {
		*nfree = edata_nfree_get(edata);
		*nregs = bin_infos[edata_szind_get(edata)].nregs;
		assert(*nfree <= *nregs);
		assert(*nfree * edata_usize_get(edata) <= *size);
	}
}

void
inspect_extent_util_stats_verbose_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size, size_t *bin_nfree,
    size_t *bin_nregs, void **slabcur_addr) {
	assert(ptr != NULL && nfree != NULL && nregs != NULL && size != NULL
	    && bin_nfree != NULL && bin_nregs != NULL && slabcur_addr != NULL);

	const edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	if (unlikely(edata == NULL)) {
		*nfree = *nregs = *size = *bin_nfree = *bin_nregs = 0;
		*slabcur_addr = NULL;
		return;
	}

	*size = edata_size_get(edata);
	if (!edata_slab_get(edata)) {
		*nfree = *bin_nfree = *bin_nregs = 0;
		*nregs = 1;
		*slabcur_addr = NULL;
		return;
	}

	*nfree = edata_nfree_get(edata);
	const szind_t szind = edata_szind_get(edata);
	*nregs = bin_infos[szind].nregs;
	assert(*nfree <= *nregs);
	assert(*nfree * edata_usize_get(edata) <= *size);

	arena_t *arena = (arena_t *)atomic_load_p(
	    &arenas[edata_arena_ind_get(edata)], ATOMIC_RELAXED);
	assert(arena != NULL);
	const unsigned binshard = edata_binshard_get(edata);
	bin_t *bin = arena_get_bin(arena, szind, binshard);

	malloc_mutex_lock(tsdn, &bin->lock);
	if (config_stats) {
		*bin_nregs = *nregs * bin->stats.curslabs;
		assert(*bin_nregs >= bin->stats.curregs);
		*bin_nfree = *bin_nregs - bin->stats.curregs;
	} else {
		*bin_nfree = *bin_nregs = 0;
	}
	edata_t *slab;
	if (bin->slabcur != NULL) {
		slab = bin->slabcur;
	} else {
		slab = edata_heap_first(&bin->slabs_nonfull);
	}
	*slabcur_addr = slab != NULL ? edata_addr_get(slab) : NULL;
	malloc_mutex_unlock(tsdn, &bin->lock);
}
