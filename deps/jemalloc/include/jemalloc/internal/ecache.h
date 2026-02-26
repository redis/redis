#ifndef JEMALLOC_INTERNAL_ECACHE_H
#define JEMALLOC_INTERNAL_ECACHE_H

#include "jemalloc/internal/eset.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/mutex.h"

typedef struct ecache_s ecache_t;
struct ecache_s {
	malloc_mutex_t mtx;
	eset_t eset;
	eset_t guarded_eset;
	/* All stored extents must be in the same state. */
	extent_state_t state;
	/* The index of the ehooks the ecache is associated with. */
	unsigned ind;
	/*
	 * If true, delay coalescing until eviction; otherwise coalesce during
	 * deallocation.
	 */
	bool delay_coalesce;
};

static inline size_t
ecache_npages_get(ecache_t *ecache) {
	return eset_npages_get(&ecache->eset) +
	    eset_npages_get(&ecache->guarded_eset);
}

/* Get the number of extents in the given page size index. */
static inline size_t
ecache_nextents_get(ecache_t *ecache, pszind_t ind) {
	return eset_nextents_get(&ecache->eset, ind) +
	    eset_nextents_get(&ecache->guarded_eset, ind);
}

/* Get the sum total bytes of the extents in the given page size index. */
static inline size_t
ecache_nbytes_get(ecache_t *ecache, pszind_t ind) {
	return eset_nbytes_get(&ecache->eset, ind) +
	    eset_nbytes_get(&ecache->guarded_eset, ind);
}

static inline unsigned
ecache_ind_get(ecache_t *ecache) {
	return ecache->ind;
}

bool ecache_init(tsdn_t *tsdn, ecache_t *ecache, extent_state_t state,
    unsigned ind, bool delay_coalesce);
void ecache_prefork(tsdn_t *tsdn, ecache_t *ecache);
void ecache_postfork_parent(tsdn_t *tsdn, ecache_t *ecache);
void ecache_postfork_child(tsdn_t *tsdn, ecache_t *ecache);

#endif /* JEMALLOC_INTERNAL_ECACHE_H */
