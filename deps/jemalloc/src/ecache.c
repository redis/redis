#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/san.h"

bool
ecache_init(tsdn_t *tsdn, ecache_t *ecache, extent_state_t state, unsigned ind,
    bool delay_coalesce) {
	if (malloc_mutex_init(&ecache->mtx, "extents", WITNESS_RANK_EXTENTS,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	ecache->state = state;
	ecache->ind = ind;
	ecache->delay_coalesce = delay_coalesce;
	eset_init(&ecache->eset, state);
	eset_init(&ecache->guarded_eset, state);

	return false;
}

void
ecache_prefork(tsdn_t *tsdn, ecache_t *ecache) {
	malloc_mutex_prefork(tsdn, &ecache->mtx);
}

void
ecache_postfork_parent(tsdn_t *tsdn, ecache_t *ecache) {
	malloc_mutex_postfork_parent(tsdn, &ecache->mtx);
}

void
ecache_postfork_child(tsdn_t *tsdn, ecache_t *ecache) {
	malloc_mutex_postfork_child(tsdn, &ecache->mtx);
}
