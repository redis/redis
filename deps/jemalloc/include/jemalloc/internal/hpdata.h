#ifndef JEMALLOC_INTERNAL_HPDATA_H
#define JEMALLOC_INTERNAL_HPDATA_H

#include "jemalloc/internal/fb.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/typed_list.h"

/*
 * The metadata representation we use for extents in hugepages.  While the PAC
 * uses the edata_t to represent both active and inactive extents, the HP only
 * uses the edata_t for active ones; instead, inactive extent state is tracked
 * within hpdata associated with the enclosing hugepage-sized, hugepage-aligned
 * region of virtual address space.
 *
 * An hpdata need not be "truly" backed by a hugepage (which is not necessarily
 * an observable property of any given region of address space).  It's just
 * hugepage-sized and hugepage-aligned; it's *potentially* huge.
 */
typedef struct hpdata_s hpdata_t;
ph_structs(hpdata_age_heap, hpdata_t);
struct hpdata_s {
	/*
	 * We likewise follow the edata convention of mangling names and forcing
	 * the use of accessors -- this lets us add some consistency checks on
	 * access.
	 */

	/*
	 * The address of the hugepage in question.  This can't be named h_addr,
	 * since that conflicts with a macro defined in Windows headers.
	 */
	void *h_address;
	/* Its age (measured in psset operations). */
	uint64_t h_age;
	/* Whether or not we think the hugepage is mapped that way by the OS. */
	bool h_huge;

	/*
	 * For some properties, we keep parallel sets of bools; h_foo_allowed
	 * and h_in_psset_foo_container.  This is a decoupling mechanism to
	 * avoid bothering the hpa (which manages policies) from the psset
	 * (which is the mechanism used to enforce those policies).  This allows
	 * all the container management logic to live in one place, without the
	 * HPA needing to know or care how that happens.
	 */

	/*
	 * Whether or not the hpdata is allowed to be used to serve allocations,
	 * and whether or not the psset is currently tracking it as such.
	 */
	bool h_alloc_allowed;
	bool h_in_psset_alloc_container;

	/*
	 * The same, but with purging.  There's no corresponding
	 * h_in_psset_purge_container, because the psset (currently) always
	 * removes hpdatas from their containers during updates (to implement
	 * LRU for purging).
	 */
	bool h_purge_allowed;

	/* And with hugifying. */
	bool h_hugify_allowed;
	/* When we became a hugification candidate. */
	nstime_t h_time_hugify_allowed;
	bool h_in_psset_hugify_container;

	/* Whether or not a purge or hugify is currently happening. */
	bool h_mid_purge;
	bool h_mid_hugify;

	/*
	 * Whether or not the hpdata is being updated in the psset (i.e. if
	 * there has been a psset_update_begin call issued without a matching
	 * psset_update_end call).  Eventually this will expand to other types
	 * of updates.
	 */
	bool h_updating;

	/* Whether or not the hpdata is in a psset. */
	bool h_in_psset;

	union {
		/* When nonempty (and also nonfull), used by the psset bins. */
		hpdata_age_heap_link_t age_link;
		/*
		 * When empty (or not corresponding to any hugepage), list
		 * linkage.
		 */
		ql_elm(hpdata_t) ql_link_empty;
	};

	/*
	 * Linkage for the psset to track candidates for purging and hugifying.
	 */
	ql_elm(hpdata_t) ql_link_purge;
	ql_elm(hpdata_t) ql_link_hugify;

	/* The length of the largest contiguous sequence of inactive pages. */
	size_t h_longest_free_range;

	/* Number of active pages. */
	size_t h_nactive;

	/* A bitmap with bits set in the active pages. */
	fb_group_t active_pages[FB_NGROUPS(HUGEPAGE_PAGES)];

	/*
	 * Number of dirty or active pages, and a bitmap tracking them.  One
	 * way to think of this is as which pages are dirty from the OS's
	 * perspective.
	 */
	size_t h_ntouched;

	/* The touched pages (using the same definition as above). */
	fb_group_t touched_pages[FB_NGROUPS(HUGEPAGE_PAGES)];
};

TYPED_LIST(hpdata_empty_list, hpdata_t, ql_link_empty)
TYPED_LIST(hpdata_purge_list, hpdata_t, ql_link_purge)
TYPED_LIST(hpdata_hugify_list, hpdata_t, ql_link_hugify)

ph_proto(, hpdata_age_heap, hpdata_t);

static inline void *
hpdata_addr_get(const hpdata_t *hpdata) {
	return hpdata->h_address;
}

static inline void
hpdata_addr_set(hpdata_t *hpdata, void *addr) {
	assert(HUGEPAGE_ADDR2BASE(addr) == addr);
	hpdata->h_address = addr;
}

static inline uint64_t
hpdata_age_get(const hpdata_t *hpdata) {
	return hpdata->h_age;
}

static inline void
hpdata_age_set(hpdata_t *hpdata, uint64_t age) {
	hpdata->h_age = age;
}

static inline bool
hpdata_huge_get(const hpdata_t *hpdata) {
	return hpdata->h_huge;
}

static inline bool
hpdata_alloc_allowed_get(const hpdata_t *hpdata) {
	return hpdata->h_alloc_allowed;
}

static inline void
hpdata_alloc_allowed_set(hpdata_t *hpdata, bool alloc_allowed) {
	hpdata->h_alloc_allowed = alloc_allowed;
}

static inline bool
hpdata_in_psset_alloc_container_get(const hpdata_t *hpdata) {
	return hpdata->h_in_psset_alloc_container;
}

static inline void
hpdata_in_psset_alloc_container_set(hpdata_t *hpdata, bool in_container) {
	assert(in_container != hpdata->h_in_psset_alloc_container);
	hpdata->h_in_psset_alloc_container = in_container;
}

static inline bool
hpdata_purge_allowed_get(const hpdata_t *hpdata) {
	return hpdata->h_purge_allowed;
}

static inline void
hpdata_purge_allowed_set(hpdata_t *hpdata, bool purge_allowed) {
       assert(purge_allowed == false || !hpdata->h_mid_purge);
       hpdata->h_purge_allowed = purge_allowed;
}

static inline bool
hpdata_hugify_allowed_get(const hpdata_t *hpdata) {
	return hpdata->h_hugify_allowed;
}

static inline void
hpdata_allow_hugify(hpdata_t *hpdata, nstime_t now) {
	assert(!hpdata->h_mid_hugify);
	hpdata->h_hugify_allowed = true;
	hpdata->h_time_hugify_allowed = now;
}

static inline nstime_t
hpdata_time_hugify_allowed(hpdata_t *hpdata) {
	return hpdata->h_time_hugify_allowed;
}

static inline void
hpdata_disallow_hugify(hpdata_t *hpdata) {
	hpdata->h_hugify_allowed = false;
}

static inline bool
hpdata_in_psset_hugify_container_get(const hpdata_t *hpdata) {
	return hpdata->h_in_psset_hugify_container;
}

static inline void
hpdata_in_psset_hugify_container_set(hpdata_t *hpdata, bool in_container) {
	assert(in_container != hpdata->h_in_psset_hugify_container);
	hpdata->h_in_psset_hugify_container = in_container;
}

static inline bool
hpdata_mid_purge_get(const hpdata_t *hpdata) {
	return hpdata->h_mid_purge;
}

static inline void
hpdata_mid_purge_set(hpdata_t *hpdata, bool mid_purge) {
	assert(mid_purge != hpdata->h_mid_purge);
	hpdata->h_mid_purge = mid_purge;
}

static inline bool
hpdata_mid_hugify_get(const hpdata_t *hpdata) {
	return hpdata->h_mid_hugify;
}

static inline void
hpdata_mid_hugify_set(hpdata_t *hpdata, bool mid_hugify) {
	assert(mid_hugify != hpdata->h_mid_hugify);
	hpdata->h_mid_hugify = mid_hugify;
}

static inline bool
hpdata_changing_state_get(const hpdata_t *hpdata) {
	return hpdata->h_mid_purge || hpdata->h_mid_hugify;
}


static inline bool
hpdata_updating_get(const hpdata_t *hpdata) {
	return hpdata->h_updating;
}

static inline void
hpdata_updating_set(hpdata_t *hpdata, bool updating) {
	assert(updating != hpdata->h_updating);
	hpdata->h_updating = updating;
}

static inline bool
hpdata_in_psset_get(const hpdata_t *hpdata) {
	return hpdata->h_in_psset;
}

static inline void
hpdata_in_psset_set(hpdata_t *hpdata, bool in_psset) {
	assert(in_psset != hpdata->h_in_psset);
	hpdata->h_in_psset = in_psset;
}

static inline size_t
hpdata_longest_free_range_get(const hpdata_t *hpdata) {
	return hpdata->h_longest_free_range;
}

static inline void
hpdata_longest_free_range_set(hpdata_t *hpdata, size_t longest_free_range) {
	assert(longest_free_range <= HUGEPAGE_PAGES);
	hpdata->h_longest_free_range = longest_free_range;
}

static inline size_t
hpdata_nactive_get(hpdata_t *hpdata) {
	return hpdata->h_nactive;
}

static inline size_t
hpdata_ntouched_get(hpdata_t *hpdata) {
	return hpdata->h_ntouched;
}

static inline size_t
hpdata_ndirty_get(hpdata_t *hpdata) {
	return hpdata->h_ntouched - hpdata->h_nactive;
}

static inline size_t
hpdata_nretained_get(hpdata_t *hpdata) {
	return HUGEPAGE_PAGES - hpdata->h_ntouched;
}

static inline void
hpdata_assert_empty(hpdata_t *hpdata) {
	assert(fb_empty(hpdata->active_pages, HUGEPAGE_PAGES));
	assert(hpdata->h_nactive == 0);
}

/*
 * Only used in tests, and in hpdata_assert_consistent, below.  Verifies some
 * consistency properties of the hpdata (e.g. that cached counts of page stats
 * match computed ones).
 */
static inline bool
hpdata_consistent(hpdata_t *hpdata) {
	if(fb_urange_longest(hpdata->active_pages, HUGEPAGE_PAGES)
	    != hpdata_longest_free_range_get(hpdata)) {
		return false;
	}
	if (fb_scount(hpdata->active_pages, HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES)
	    != hpdata->h_nactive) {
		return false;
	}
	if (fb_scount(hpdata->touched_pages, HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES)
	    != hpdata->h_ntouched) {
		return false;
	}
	if (hpdata->h_ntouched < hpdata->h_nactive) {
		return false;
	}
	if (hpdata->h_huge && hpdata->h_ntouched != HUGEPAGE_PAGES) {
		return false;
	}
	if (hpdata_changing_state_get(hpdata)
	    && ((hpdata->h_purge_allowed) || hpdata->h_hugify_allowed)) {
		return false;
	}
	if (hpdata_hugify_allowed_get(hpdata)
	    != hpdata_in_psset_hugify_container_get(hpdata)) {
		return false;
	}
	return true;
}

static inline void
hpdata_assert_consistent(hpdata_t *hpdata) {
	assert(hpdata_consistent(hpdata));
}

static inline bool
hpdata_empty(hpdata_t *hpdata) {
	return hpdata->h_nactive == 0;
}

static inline bool
hpdata_full(hpdata_t *hpdata) {
	return hpdata->h_nactive == HUGEPAGE_PAGES;
}

void hpdata_init(hpdata_t *hpdata, void *addr, uint64_t age);

/*
 * Given an hpdata which can serve an allocation request, pick and reserve an
 * offset within that allocation.
 */
void *hpdata_reserve_alloc(hpdata_t *hpdata, size_t sz);
void hpdata_unreserve(hpdata_t *hpdata, void *begin, size_t sz);

/*
 * The hpdata_purge_prepare_t allows grabbing the metadata required to purge
 * subranges of a hugepage while holding a lock, drop the lock during the actual
 * purging of them, and reacquire it to update the metadata again.
 */
typedef struct hpdata_purge_state_s hpdata_purge_state_t;
struct hpdata_purge_state_s {
	size_t npurged;
	size_t ndirty_to_purge;
	fb_group_t to_purge[FB_NGROUPS(HUGEPAGE_PAGES)];
	size_t next_purge_search_begin;
};

/*
 * Initializes purge state.  The access to hpdata must be externally
 * synchronized with other hpdata_* calls.
 *
 * You can tell whether or not a thread is purging or hugifying a given hpdata
 * via hpdata_changing_state_get(hpdata).  Racing hugification or purging
 * operations aren't allowed.
 *
 * Once you begin purging, you have to follow through and call hpdata_purge_next
 * until you're done, and then end.  Allocating out of an hpdata undergoing
 * purging is not allowed.
 *
 * Returns the number of dirty pages that will be purged.
 */
size_t hpdata_purge_begin(hpdata_t *hpdata, hpdata_purge_state_t *purge_state);

/*
 * If there are more extents to purge, sets *r_purge_addr and *r_purge_size to
 * true, and returns true.  Otherwise, returns false to indicate that we're
 * done.
 *
 * This requires exclusive access to the purge state, but *not* to the hpdata.
 * In particular, unreserve calls are allowed while purging (i.e. you can dalloc
 * into one part of the hpdata while purging a different part).
 */
bool hpdata_purge_next(hpdata_t *hpdata, hpdata_purge_state_t *purge_state,
    void **r_purge_addr, size_t *r_purge_size);
/*
 * Updates the hpdata metadata after all purging is done.  Needs external
 * synchronization.
 */
void hpdata_purge_end(hpdata_t *hpdata, hpdata_purge_state_t *purge_state);

void hpdata_hugify(hpdata_t *hpdata);
void hpdata_dehugify(hpdata_t *hpdata);

#endif /* JEMALLOC_INTERNAL_HPDATA_H */
