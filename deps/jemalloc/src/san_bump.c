#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/san_bump.h"
#include "jemalloc/internal/pac.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/edata_cache.h"

static bool
san_bump_grow_locked(tsdn_t *tsdn, san_bump_alloc_t *sba, pac_t *pac,
    ehooks_t *ehooks, size_t size);

edata_t *
san_bump_alloc(tsdn_t *tsdn, san_bump_alloc_t* sba, pac_t *pac,
    ehooks_t *ehooks, size_t size, bool zero) {
	assert(san_bump_enabled());

	edata_t* to_destroy;
	size_t guarded_size = san_one_side_guarded_sz(size);

	malloc_mutex_lock(tsdn, &sba->mtx);

	if (sba->curr_reg == NULL ||
	    edata_size_get(sba->curr_reg) < guarded_size) {
		/*
		 * If the current region can't accommodate the allocation,
		 * try replacing it with a larger one and destroy current if the
		 * replacement succeeds.
		 */
		to_destroy = sba->curr_reg;
		bool err = san_bump_grow_locked(tsdn, sba, pac, ehooks,
		    guarded_size);
		if (err) {
			goto label_err;
		}
	} else {
		to_destroy = NULL;
	}
	assert(guarded_size <= edata_size_get(sba->curr_reg));
	size_t trail_size = edata_size_get(sba->curr_reg) - guarded_size;

	edata_t* edata;
	if (trail_size != 0) {
		edata_t* curr_reg_trail = extent_split_wrapper(tsdn, pac,
		    ehooks, sba->curr_reg, guarded_size, trail_size,
		    /* holding_core_locks */ true);
		if (curr_reg_trail == NULL) {
			goto label_err;
		}
		edata = sba->curr_reg;
		sba->curr_reg = curr_reg_trail;
	} else {
		edata = sba->curr_reg;
		sba->curr_reg = NULL;
	}

	malloc_mutex_unlock(tsdn, &sba->mtx);

	assert(!edata_guarded_get(edata));
	assert(sba->curr_reg == NULL || !edata_guarded_get(sba->curr_reg));
	assert(to_destroy == NULL || !edata_guarded_get(to_destroy));

	if (to_destroy != NULL) {
		extent_destroy_wrapper(tsdn, pac, ehooks, to_destroy);
	}

	san_guard_pages(tsdn, ehooks, edata, pac->emap, /* left */ false,
	    /* right */ true, /* remap */ true);

	if (extent_commit_zero(tsdn, ehooks, edata, /* commit */ true, zero,
	    /* growing_retained */ false)) {
		extent_record(tsdn, pac, ehooks, &pac->ecache_retained,
		    edata);
		return NULL;
	}

	if (config_prof) {
		extent_gdump_add(tsdn, edata);
	}

	return edata;
label_err:
	malloc_mutex_unlock(tsdn, &sba->mtx);
	return NULL;
}

static bool
san_bump_grow_locked(tsdn_t *tsdn, san_bump_alloc_t *sba, pac_t *pac,
    ehooks_t *ehooks, size_t size) {
	malloc_mutex_assert_owner(tsdn, &sba->mtx);

	bool committed = false, zeroed = false;
	size_t alloc_size = size > SBA_RETAINED_ALLOC_SIZE ? size :
	    SBA_RETAINED_ALLOC_SIZE;
	assert((alloc_size & PAGE_MASK) == 0);
	sba->curr_reg = extent_alloc_wrapper(tsdn, pac, ehooks, NULL,
	    alloc_size, PAGE, zeroed, &committed,
	    /* growing_retained */ true);
	if (sba->curr_reg == NULL) {
		return true;
	}
	return false;
}
