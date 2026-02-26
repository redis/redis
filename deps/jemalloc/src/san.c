#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/tsd.h"

/* The sanitizer options. */
size_t opt_san_guard_large = SAN_GUARD_LARGE_EVERY_N_EXTENTS_DEFAULT;
size_t opt_san_guard_small = SAN_GUARD_SMALL_EVERY_N_EXTENTS_DEFAULT;

/* Aligned (-1 is off) ptrs will be junked & stashed on dealloc. */
ssize_t opt_lg_san_uaf_align = SAN_LG_UAF_ALIGN_DEFAULT;

/*
 *  Initialized in san_init().  When disabled, the mask is set to (uintptr_t)-1
 *  to always fail the nonfast_align check.
 */
uintptr_t san_cache_bin_nonfast_mask = SAN_CACHE_BIN_NONFAST_MASK_DEFAULT;

static inline void
san_find_guarded_addr(edata_t *edata, uintptr_t *guard1, uintptr_t *guard2,
    uintptr_t *addr, size_t size, bool left, bool right) {
	assert(!edata_guarded_get(edata));
	assert(size % PAGE == 0);
	*addr = (uintptr_t)edata_base_get(edata);
	if (left) {
		*guard1 = *addr;
		*addr += SAN_PAGE_GUARD;
	} else {
		*guard1 = 0;
	}

	if (right) {
		*guard2 = *addr + size;
	} else {
		*guard2 = 0;
	}
}

static inline void
san_find_unguarded_addr(edata_t *edata, uintptr_t *guard1, uintptr_t *guard2,
    uintptr_t *addr, size_t size, bool left, bool right) {
	assert(edata_guarded_get(edata));
	assert(size % PAGE == 0);
	*addr = (uintptr_t)edata_base_get(edata);
	if (right) {
		*guard2 = *addr + size;
	} else {
		*guard2 = 0;
	}

	if (left) {
		*guard1 = *addr - SAN_PAGE_GUARD;
		assert(*guard1 != 0);
		*addr = *guard1;
	} else {
		*guard1 = 0;
	}
}

void
san_guard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata, emap_t *emap,
    bool left, bool right, bool remap) {
	assert(left || right);
	if (remap) {
		emap_deregister_boundary(tsdn, emap, edata);
	}

	size_t size_with_guards = edata_size_get(edata);
	size_t usize = (left && right)
	    ? san_two_side_unguarded_sz(size_with_guards)
	    : san_one_side_unguarded_sz(size_with_guards);

	uintptr_t guard1, guard2, addr;
	san_find_guarded_addr(edata, &guard1, &guard2, &addr, usize, left,
	    right);

	assert(edata_state_get(edata) == extent_state_active);
	ehooks_guard(tsdn, ehooks, (void *)guard1, (void *)guard2);

	/* Update the guarded addr and usable size of the edata. */
	edata_size_set(edata, usize);
	edata_addr_set(edata, (void *)addr);
	edata_guarded_set(edata, true);

	if (remap) {
		emap_register_boundary(tsdn, emap, edata, SC_NSIZES,
		    /* slab */ false);
	}
}

static void
san_unguard_pages_impl(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool left, bool right, bool remap) {
	assert(left || right);
	/* Remove the inner boundary which no longer exists. */
	if (remap) {
		assert(edata_state_get(edata) == extent_state_active);
		emap_deregister_boundary(tsdn, emap, edata);
	} else {
		assert(edata_state_get(edata) == extent_state_retained);
	}

	size_t size = edata_size_get(edata);
	size_t size_with_guards = (left && right)
	    ? san_two_side_guarded_sz(size)
	    : san_one_side_guarded_sz(size);

	uintptr_t guard1, guard2, addr;
	san_find_unguarded_addr(edata, &guard1, &guard2, &addr, size, left,
	    right);

	ehooks_unguard(tsdn, ehooks, (void *)guard1, (void *)guard2);

	/* Update the true addr and usable size of the edata. */
	edata_size_set(edata, size_with_guards);
	edata_addr_set(edata, (void *)addr);
	edata_guarded_set(edata, false);

	/*
	 * Then re-register the outer boundary including the guards, if
	 * requested.
	 */
	if (remap) {
		emap_register_boundary(tsdn, emap, edata, SC_NSIZES,
		    /* slab */ false);
	}
}

void
san_unguard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool left, bool right) {
	san_unguard_pages_impl(tsdn, ehooks, edata, emap, left, right,
	    /* remap */ true);
}

void
san_unguard_pages_pre_destroy(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap) {
	emap_assert_not_mapped(tsdn, emap, edata);
	/*
	 * We don't want to touch the emap of about to be destroyed extents, as
	 * they have been unmapped upon eviction from the retained ecache. Also,
	 * we unguard the extents to the right, because retained extents only
	 * own their right guard page per san_bump_alloc's logic.
	 */
	 san_unguard_pages_impl(tsdn, ehooks, edata, emap, /* left */ false,
	    /* right */ true, /* remap */ false);
}

static bool
san_stashed_corrupted(void *ptr, size_t size) {
	if (san_junk_ptr_should_slow()) {
		for (size_t i = 0; i < size; i++) {
			if (((char *)ptr)[i] != (char)uaf_detect_junk) {
				return true;
			}
		}
		return false;
	}

	void *first, *mid, *last;
	san_junk_ptr_locations(ptr, size, &first, &mid, &last);
	if (*(uintptr_t *)first != uaf_detect_junk ||
	    *(uintptr_t *)mid != uaf_detect_junk ||
	    *(uintptr_t *)last != uaf_detect_junk) {
		return true;
	}

	return false;
}

void
san_check_stashed_ptrs(void **ptrs, size_t nstashed, size_t usize) {
	/*
	 * Verify that the junked-filled & stashed pointers remain unchanged, to
	 * detect write-after-free.
	 */
	for (size_t n = 0; n < nstashed; n++) {
		void *stashed = ptrs[n];
		assert(stashed != NULL);
		assert(cache_bin_nonfast_aligned(stashed));
		if (unlikely(san_stashed_corrupted(stashed, usize))) {
			safety_check_fail("<jemalloc>: Write-after-free "
			    "detected on deallocated pointer %p (size %zu).\n",
			    stashed, usize);
		}
	}
}

void
tsd_san_init(tsd_t *tsd) {
	*tsd_san_extents_until_guard_smallp_get(tsd) = opt_san_guard_small;
	*tsd_san_extents_until_guard_largep_get(tsd) = opt_san_guard_large;
}

void
san_init(ssize_t lg_san_uaf_align) {
	assert(lg_san_uaf_align == -1 || lg_san_uaf_align >= LG_PAGE);
	if (lg_san_uaf_align == -1) {
		san_cache_bin_nonfast_mask = (uintptr_t)-1;
		return;
	}

	san_cache_bin_nonfast_mask = ((uintptr_t)1 << lg_san_uaf_align) - 1;
}
