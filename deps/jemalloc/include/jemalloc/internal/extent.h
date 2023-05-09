#ifndef JEMALLOC_INTERNAL_EXTENT_H
#define JEMALLOC_INTERNAL_EXTENT_H

#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/rtree.h"

/*
 * This module contains the page-level allocator.  It chooses the addresses that
 * allocations requested by other modules will inhabit, and updates the global
 * metadata to reflect allocation/deallocation/purging decisions.
 */

/*
 * When reuse (and split) an active extent, (1U << opt_lg_extent_max_active_fit)
 * is the max ratio between the size of the active extent and the new extent.
 */
#define LG_EXTENT_MAX_ACTIVE_FIT_DEFAULT 6
extern size_t opt_lg_extent_max_active_fit;

edata_t *ecache_alloc(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *expand_edata, size_t size, size_t alignment,
    bool zero, bool guarded);
edata_t *ecache_alloc_grow(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *expand_edata, size_t size, size_t alignment,
    bool zero, bool guarded);
void ecache_dalloc(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata);
edata_t *ecache_evict(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    ecache_t *ecache, size_t npages_min);

void extent_gdump_add(tsdn_t *tsdn, const edata_t *edata);
void extent_record(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks, ecache_t *ecache,
    edata_t *edata);
void extent_dalloc_gap(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    edata_t *edata);
edata_t *extent_alloc_wrapper(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t alignment, bool zero, bool *commit,
    bool growing_retained);
void extent_dalloc_wrapper(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    edata_t *edata);
void extent_destroy_wrapper(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    edata_t *edata);
bool extent_commit_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length);
bool extent_decommit_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length);
bool extent_purge_lazy_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length);
bool extent_purge_forced_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length);
edata_t *extent_split_wrapper(tsdn_t *tsdn, pac_t *pac,
    ehooks_t *ehooks, edata_t *edata, size_t size_a, size_t size_b,
    bool holding_core_locks);
bool extent_merge_wrapper(tsdn_t *tsdn, pac_t *pac, ehooks_t *ehooks,
    edata_t *a, edata_t *b);
bool extent_commit_zero(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    bool commit, bool zero, bool growing_retained);
size_t extent_sn_next(pac_t *pac);
bool extent_boot(void);

JEMALLOC_ALWAYS_INLINE bool
extent_neighbor_head_state_mergeable(bool edata_is_head,
    bool neighbor_is_head, bool forward) {
	/*
	 * Head states checking: disallow merging if the higher addr extent is a
	 * head extent.  This helps preserve first-fit, and more importantly
	 * makes sure no merge across arenas.
	 */
	if (forward) {
		if (neighbor_is_head) {
			return false;
		}
	} else {
		if (edata_is_head) {
			return false;
		}
	}
	return true;
}

JEMALLOC_ALWAYS_INLINE bool
extent_can_acquire_neighbor(edata_t *edata, rtree_contents_t contents,
    extent_pai_t pai, extent_state_t expected_state, bool forward,
    bool expanding) {
	edata_t *neighbor = contents.edata;
	if (neighbor == NULL) {
		return false;
	}
	/* It's not safe to access *neighbor yet; must verify states first. */
	bool neighbor_is_head = contents.metadata.is_head;
	if (!extent_neighbor_head_state_mergeable(edata_is_head_get(edata),
	    neighbor_is_head, forward)) {
		return false;
	}
	extent_state_t neighbor_state = contents.metadata.state;
	if (pai == EXTENT_PAI_PAC) {
		if (neighbor_state != expected_state) {
			return false;
		}
		/* From this point, it's safe to access *neighbor. */
		if (!expanding && (edata_committed_get(edata) !=
		    edata_committed_get(neighbor))) {
			/*
			 * Some platforms (e.g. Windows) require an explicit
			 * commit step (and writing to uncommitted memory is not
			 * allowed).
			 */
			return false;
		}
	} else {
		if (neighbor_state == extent_state_active) {
			return false;
		}
		/* From this point, it's safe to access *neighbor. */
	}

	assert(edata_pai_get(edata) == pai);
	if (edata_pai_get(neighbor) != pai) {
		return false;
	}
	if (opt_retain) {
		assert(edata_arena_ind_get(edata) ==
		    edata_arena_ind_get(neighbor));
	} else {
		if (edata_arena_ind_get(edata) !=
		    edata_arena_ind_get(neighbor)) {
			return false;
		}
	}
	assert(!edata_guarded_get(edata) && !edata_guarded_get(neighbor));

	return true;
}

#endif /* JEMALLOC_INTERNAL_EXTENT_H */
