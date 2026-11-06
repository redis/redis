#ifndef JEMALLOC_INTERNAL_PAC_H
#define JEMALLOC_INTERNAL_PAC_H

#include "jemalloc/internal/exp_grow.h"
#include "jemalloc/internal/pai.h"
#include "san_bump.h"


/*
 * Page allocator classic; an implementation of the PAI interface that:
 * - Can be used for arenas with custom extent hooks.
 * - Can always satisfy any allocation request (including highly-fragmentary
 *   ones).
 * - Can use efficient OS-level zeroing primitives for demand-filled pages.
 */

/* How "eager" decay/purging should be. */
enum pac_purge_eagerness_e {
	PAC_PURGE_ALWAYS,
	PAC_PURGE_NEVER,
	PAC_PURGE_ON_EPOCH_ADVANCE
};
typedef enum pac_purge_eagerness_e pac_purge_eagerness_t;

typedef struct pac_decay_stats_s pac_decay_stats_t;
struct pac_decay_stats_s {
	/* Total number of purge sweeps. */
	locked_u64_t npurge;
	/* Total number of madvise calls made. */
	locked_u64_t nmadvise;
	/* Total number of pages purged. */
	locked_u64_t purged;
};

typedef struct pac_estats_s pac_estats_t;
struct pac_estats_s {
	/*
	 * Stats for a given index in the range [0, SC_NPSIZES] in the various
	 * ecache_ts.
	 * We track both bytes and # of extents: two extents in the same bucket
	 * may have different sizes if adjacent size classes differ by more than
	 * a page, so bytes cannot always be derived from # of extents.
	 */
	size_t ndirty;
	size_t dirty_bytes;
	size_t nmuzzy;
	size_t muzzy_bytes;
	size_t nretained;
	size_t retained_bytes;
};

typedef struct pac_stats_s pac_stats_t;
struct pac_stats_s {
	pac_decay_stats_t decay_dirty;
	pac_decay_stats_t decay_muzzy;

	/*
	 * Number of unused virtual memory bytes currently retained.  Retained
	 * bytes are technically mapped (though always decommitted or purged),
	 * but they are excluded from the mapped statistic (above).
	 */
	size_t retained; /* Derived. */

	/*
	 * Number of bytes currently mapped, excluding retained memory (and any
	 * base-allocated memory, which is tracked by the arena stats).
	 *
	 * We name this "pac_mapped" to avoid confusion with the arena_stats
	 * "mapped".
	 */
	atomic_zu_t pac_mapped;

	/* VM space had to be leaked (undocumented).  Normally 0. */
	atomic_zu_t abandoned_vm;
};

typedef struct pac_s pac_t;
struct pac_s {
	/*
	 * Must be the first member (we convert it to a PAC given only a
	 * pointer).  The handle to the allocation interface.
	 */
	pai_t pai;
	/*
	 * Collections of extents that were previously allocated.  These are
	 * used when allocating extents, in an attempt to re-use address space.
	 *
	 * Synchronization: internal.
	 */
	ecache_t ecache_dirty;
	ecache_t ecache_muzzy;
	ecache_t ecache_retained;

	base_t *base;
	emap_t *emap;
	edata_cache_t *edata_cache;

	/* The grow info for the retained ecache. */
	exp_grow_t exp_grow;
	malloc_mutex_t grow_mtx;

	/* Special allocator for guarded frequently reused extents. */
	san_bump_alloc_t sba;

	/* How large extents should be before getting auto-purged. */
	atomic_zu_t oversize_threshold;

	/*
	 * Decay-based purging state, responsible for scheduling extent state
	 * transitions.
	 *
	 * Synchronization: via the internal mutex.
	 */
	decay_t decay_dirty; /* dirty --> muzzy */
	decay_t decay_muzzy; /* muzzy --> retained */

	malloc_mutex_t *stats_mtx;
	pac_stats_t *stats;

	/* Extent serial number generator state. */
	atomic_zu_t extent_sn_next;
};

bool pac_init(tsdn_t *tsdn, pac_t *pac, base_t *base, emap_t *emap,
    edata_cache_t *edata_cache, nstime_t *cur_time, size_t oversize_threshold,
    ssize_t dirty_decay_ms, ssize_t muzzy_decay_ms, pac_stats_t *pac_stats,
    malloc_mutex_t *stats_mtx);

static inline size_t
pac_mapped(pac_t *pac) {
	return atomic_load_zu(&pac->stats->pac_mapped, ATOMIC_RELAXED);
}

static inline ehooks_t *
pac_ehooks_get(pac_t *pac) {
	return base_ehooks_get(pac->base);
}

/*
 * All purging functions require holding decay->mtx.  This is one of the few
 * places external modules are allowed to peek inside pa_shard_t internals.
 */

/*
 * Decays the number of pages currently in the ecache.  This might not leave the
 * ecache empty if other threads are inserting dirty objects into it
 * concurrently with the call.
 */
void pac_decay_all(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay);
/*
 * Updates decay settings for the current time, and conditionally purges in
 * response (depending on decay_purge_setting).  Returns whether or not the
 * epoch advanced.
 */
bool pac_maybe_decay_purge(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache,
    pac_purge_eagerness_t eagerness);

/*
 * Gets / sets the maximum amount that we'll grow an arena down the
 * grow-retained pathways (unless forced to by an allocaction request).
 *
 * Set new_limit to NULL if it's just a query, or old_limit to NULL if you don't
 * care about the previous value.
 *
 * Returns true on error (if the new limit is not valid).
 */
bool pac_retain_grow_limit_get_set(tsdn_t *tsdn, pac_t *pac, size_t *old_limit,
    size_t *new_limit);

bool pac_decay_ms_set(tsdn_t *tsdn, pac_t *pac, extent_state_t state,
    ssize_t decay_ms, pac_purge_eagerness_t eagerness);
ssize_t pac_decay_ms_get(pac_t *pac, extent_state_t state);

void pac_reset(tsdn_t *tsdn, pac_t *pac);
void pac_destroy(tsdn_t *tsdn, pac_t *pac);

#endif /* JEMALLOC_INTERNAL_PAC_H */
