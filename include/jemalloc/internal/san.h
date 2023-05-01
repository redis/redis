#ifndef JEMALLOC_INTERNAL_GUARD_H
#define JEMALLOC_INTERNAL_GUARD_H

#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/emap.h"

#define SAN_PAGE_GUARD PAGE
#define SAN_PAGE_GUARDS_SIZE (SAN_PAGE_GUARD * 2)

#define SAN_GUARD_LARGE_EVERY_N_EXTENTS_DEFAULT 0
#define SAN_GUARD_SMALL_EVERY_N_EXTENTS_DEFAULT 0

#define SAN_LG_UAF_ALIGN_DEFAULT (-1)
#define SAN_CACHE_BIN_NONFAST_MASK_DEFAULT (uintptr_t)(-1)

static const uintptr_t uaf_detect_junk = (uintptr_t)0x5b5b5b5b5b5b5b5bULL;

/* 0 means disabled, i.e. never guarded. */
extern size_t opt_san_guard_large;
extern size_t opt_san_guard_small;
/* -1 means disabled, i.e. never check for use-after-free. */
extern ssize_t opt_lg_san_uaf_align;

void san_guard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool left, bool right, bool remap);
void san_unguard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool left, bool right);
/*
 * Unguard the extent, but don't modify emap boundaries. Must be called on an
 * extent that has been erased from emap and shouldn't be placed back.
 */
void san_unguard_pages_pre_destroy(tsdn_t *tsdn, ehooks_t *ehooks,
    edata_t *edata, emap_t *emap);
void san_check_stashed_ptrs(void **ptrs, size_t nstashed, size_t usize);

void tsd_san_init(tsd_t *tsd);
void san_init(ssize_t lg_san_uaf_align);

static inline void
san_guard_pages_two_sided(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool remap) {
	san_guard_pages(tsdn, ehooks, edata, emap, true, true, remap);
}

static inline void
san_unguard_pages_two_sided(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap) {
	san_unguard_pages(tsdn, ehooks, edata, emap, true, true);
}

static inline size_t
san_two_side_unguarded_sz(size_t size) {
	assert(size % PAGE == 0);
	assert(size >= SAN_PAGE_GUARDS_SIZE);
	return size - SAN_PAGE_GUARDS_SIZE;
}

static inline size_t
san_two_side_guarded_sz(size_t size) {
	assert(size % PAGE == 0);
	return size + SAN_PAGE_GUARDS_SIZE;
}

static inline size_t
san_one_side_unguarded_sz(size_t size) {
	assert(size % PAGE == 0);
	assert(size >= SAN_PAGE_GUARD);
	return size - SAN_PAGE_GUARD;
}

static inline size_t
san_one_side_guarded_sz(size_t size) {
	assert(size % PAGE == 0);
	return size + SAN_PAGE_GUARD;
}

static inline bool
san_guard_enabled(void) {
	return (opt_san_guard_large != 0 || opt_san_guard_small != 0);
}

static inline bool
san_large_extent_decide_guard(tsdn_t *tsdn, ehooks_t *ehooks, size_t size,
    size_t alignment) {
	if (opt_san_guard_large == 0 || ehooks_guard_will_fail(ehooks) ||
	    tsdn_null(tsdn)) {
		return false;
	}

	tsd_t *tsd = tsdn_tsd(tsdn);
	uint64_t n = tsd_san_extents_until_guard_large_get(tsd);
	assert(n >= 1);
	if (n > 1) {
		/*
		 * Subtract conditionally because the guard may not happen due
		 * to alignment or size restriction below.
		 */
		*tsd_san_extents_until_guard_largep_get(tsd) = n - 1;
	}

	if (n == 1 && (alignment <= PAGE) &&
	    (san_two_side_guarded_sz(size) <= SC_LARGE_MAXCLASS)) {
		*tsd_san_extents_until_guard_largep_get(tsd) =
		    opt_san_guard_large;
		return true;
	} else {
		assert(tsd_san_extents_until_guard_large_get(tsd) >= 1);
		return false;
	}
}

static inline bool
san_slab_extent_decide_guard(tsdn_t *tsdn, ehooks_t *ehooks) {
	if (opt_san_guard_small == 0 || ehooks_guard_will_fail(ehooks) ||
	    tsdn_null(tsdn)) {
		return false;
	}

	tsd_t *tsd = tsdn_tsd(tsdn);
	uint64_t n = tsd_san_extents_until_guard_small_get(tsd);
	assert(n >= 1);
	if (n == 1) {
		*tsd_san_extents_until_guard_smallp_get(tsd) =
		    opt_san_guard_small;
		return true;
	} else {
		*tsd_san_extents_until_guard_smallp_get(tsd) = n - 1;
		assert(tsd_san_extents_until_guard_small_get(tsd) >= 1);
		return false;
	}
}

static inline void
san_junk_ptr_locations(void *ptr, size_t usize, void **first, void **mid,
    void **last) {
	size_t ptr_sz = sizeof(void *);

	*first = ptr;

	*mid = (void *)((uintptr_t)ptr + ((usize >> 1) & ~(ptr_sz - 1)));
	assert(*first != *mid || usize == ptr_sz);
	assert((uintptr_t)*first <= (uintptr_t)*mid);

	/*
	 * When usize > 32K, the gap between requested_size and usize might be
	 * greater than 4K -- this means the last write may access an
	 * likely-untouched page (default settings w/ 4K pages).  However by
	 * default the tcache only goes up to the 32K size class, and is usually
	 * tuned lower instead of higher, which makes it less of a concern.
	 */
	*last = (void *)((uintptr_t)ptr + usize - sizeof(uaf_detect_junk));
	assert(*first != *last || usize == ptr_sz);
	assert(*mid != *last || usize <= ptr_sz * 2);
	assert((uintptr_t)*mid <= (uintptr_t)*last);
}

static inline bool
san_junk_ptr_should_slow(void) {
	/*
	 * The latter condition (pointer size greater than the min size class)
	 * is not expected -- fall back to the slow path for simplicity.
	 */
	return config_debug || (LG_SIZEOF_PTR > SC_LG_TINY_MIN);
}

static inline void
san_junk_ptr(void *ptr, size_t usize) {
	if (san_junk_ptr_should_slow()) {
		memset(ptr, (char)uaf_detect_junk, usize);
		return;
	}

	void *first, *mid, *last;
	san_junk_ptr_locations(ptr, usize, &first, &mid, &last);
	*(uintptr_t *)first = uaf_detect_junk;
	*(uintptr_t *)mid = uaf_detect_junk;
	*(uintptr_t *)last = uaf_detect_junk;
}

static inline bool
san_uaf_detection_enabled(void) {
	bool ret = config_uaf_detection && (opt_lg_san_uaf_align != -1);
	if (config_uaf_detection && ret) {
		assert(san_cache_bin_nonfast_mask == ((uintptr_t)1 <<
		    opt_lg_san_uaf_align) - 1);
	}

	return ret;
}

#endif /* JEMALLOC_INTERNAL_GUARD_H */
