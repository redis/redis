#ifndef JEMALLOC_INTERNAL_BASE_H
#define JEMALLOC_INTERNAL_BASE_H

#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/mutex.h"

enum metadata_thp_mode_e {
	metadata_thp_disabled   = 0,
	/*
	 * Lazily enable hugepage for metadata. To avoid high RSS caused by THP
	 * + low usage arena (i.e. THP becomes a significant percentage), the
	 * "auto" option only starts using THP after a base allocator used up
	 * the first THP region.  Starting from the second hugepage (in a single
	 * arena), "auto" behaves the same as "always", i.e. madvise hugepage
	 * right away.
	 */
	metadata_thp_auto       = 1,
	metadata_thp_always     = 2,
	metadata_thp_mode_limit = 3
};
typedef enum metadata_thp_mode_e metadata_thp_mode_t;

#define METADATA_THP_DEFAULT metadata_thp_disabled
extern metadata_thp_mode_t opt_metadata_thp;
extern const char *metadata_thp_mode_names[];


/* Embedded at the beginning of every block of base-managed virtual memory. */
typedef struct base_block_s base_block_t;
struct base_block_s {
	/* Total size of block's virtual memory mapping. */
	size_t size;

	/* Next block in list of base's blocks. */
	base_block_t *next;

	/* Tracks unused trailing space. */
	edata_t edata;
};

typedef struct base_s base_t;
struct base_s {
	/*
	 * User-configurable extent hook functions.
	 */
	ehooks_t ehooks;

	/*
	 * User-configurable extent hook functions for metadata allocations.
	 */
	ehooks_t ehooks_base;

	/* Protects base_alloc() and base_stats_get() operations. */
	malloc_mutex_t mtx;

	/* Using THP when true (metadata_thp auto mode). */
	bool auto_thp_switched;
	/*
	 * Most recent size class in the series of increasingly large base
	 * extents.  Logarithmic spacing between subsequent allocations ensures
	 * that the total number of distinct mappings remains small.
	 */
	pszind_t pind_last;

	/* Serial number generation state. */
	size_t extent_sn_next;

	/* Chain of all blocks associated with base. */
	base_block_t *blocks;

	/* Heap of extents that track unused trailing space within blocks. */
	edata_heap_t avail[SC_NSIZES];

	/* Stats, only maintained if config_stats. */
	size_t allocated;
	size_t resident;
	size_t mapped;
	/* Number of THP regions touched. */
	size_t n_thp;
};

static inline unsigned
base_ind_get(const base_t *base) {
	return ehooks_ind_get(&base->ehooks);
}

static inline bool
metadata_thp_enabled(void) {
	return (opt_metadata_thp != metadata_thp_disabled);
}

base_t *b0get(void);
base_t *base_new(tsdn_t *tsdn, unsigned ind,
    const extent_hooks_t *extent_hooks, bool metadata_use_hooks);
void base_delete(tsdn_t *tsdn, base_t *base);
ehooks_t *base_ehooks_get(base_t *base);
ehooks_t *base_ehooks_get_for_metadata(base_t *base);
extent_hooks_t *base_extent_hooks_set(base_t *base,
    extent_hooks_t *extent_hooks);
void *base_alloc(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment);
edata_t *base_alloc_edata(tsdn_t *tsdn, base_t *base);
void base_stats_get(tsdn_t *tsdn, base_t *base, size_t *allocated,
    size_t *resident, size_t *mapped, size_t *n_thp);
void base_prefork(tsdn_t *tsdn, base_t *base);
void base_postfork_parent(tsdn_t *tsdn, base_t *base);
void base_postfork_child(tsdn_t *tsdn, base_t *base);
bool base_boot(tsdn_t *tsdn);

#endif /* JEMALLOC_INTERNAL_BASE_H */
