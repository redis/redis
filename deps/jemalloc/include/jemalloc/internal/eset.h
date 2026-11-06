#ifndef JEMALLOC_INTERNAL_ESET_H
#define JEMALLOC_INTERNAL_ESET_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/fb.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/mutex.h"

/*
 * An eset ("extent set") is a quantized collection of extents, with built-in
 * LRU queue.
 *
 * This class is not thread-safe; synchronization must be done externally if
 * there are mutating operations.  One exception is the stats counters, which
 * may be read without any locking.
 */

typedef struct eset_bin_s eset_bin_t;
struct eset_bin_s {
	edata_heap_t heap;
	/*
	 * We do first-fit across multiple size classes.  If we compared against
	 * the min element in each heap directly, we'd take a cache miss per
	 * extent we looked at.  If we co-locate the edata summaries, we only
	 * take a miss on the edata we're actually going to return (which is
	 * inevitable anyways).
	 */
	edata_cmp_summary_t heap_min;
};

typedef struct eset_bin_stats_s eset_bin_stats_t;
struct eset_bin_stats_s {
	atomic_zu_t nextents;
	atomic_zu_t nbytes;
};

typedef struct eset_s eset_t;
struct eset_s {
	/* Bitmap for which set bits correspond to non-empty heaps. */
	fb_group_t bitmap[FB_NGROUPS(SC_NPSIZES + 1)];

	/* Quantized per size class heaps of extents. */
	eset_bin_t bins[SC_NPSIZES + 1];

	eset_bin_stats_t bin_stats[SC_NPSIZES + 1];

	/* LRU of all extents in heaps. */
	edata_list_inactive_t lru;

	/* Page sum for all extents in heaps. */
	atomic_zu_t npages;

	/*
	 * A duplication of the data in the containing ecache.  We use this only
	 * for assertions on the states of the passed-in extents.
	 */
	extent_state_t state;
};

void eset_init(eset_t *eset, extent_state_t state);

size_t eset_npages_get(eset_t *eset);
/* Get the number of extents in the given page size index. */
size_t eset_nextents_get(eset_t *eset, pszind_t ind);
/* Get the sum total bytes of the extents in the given page size index. */
size_t eset_nbytes_get(eset_t *eset, pszind_t ind);

void eset_insert(eset_t *eset, edata_t *edata);
void eset_remove(eset_t *eset, edata_t *edata);
/*
 * Select an extent from this eset of the given size and alignment.  Returns
 * null if no such item could be found.
 */
edata_t *eset_fit(eset_t *eset, size_t esize, size_t alignment, bool exact_only,
    unsigned lg_max_fit);

#endif /* JEMALLOC_INTERNAL_ESET_H */
