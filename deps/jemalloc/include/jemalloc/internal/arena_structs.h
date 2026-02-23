#ifndef JEMALLOC_INTERNAL_ARENA_STRUCTS_H
#define JEMALLOC_INTERNAL_ARENA_STRUCTS_H

#include "jemalloc/internal/arena_stats.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/counter.h"
#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/pa.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/ticker.h"

struct arena_s {
	/*
	 * Number of threads currently assigned to this arena.  Each thread has
	 * two distinct assignments, one for application-serving allocation, and
	 * the other for internal metadata allocation.  Internal metadata must
	 * not be allocated from arenas explicitly created via the arenas.create
	 * mallctl, because the arena.<i>.reset mallctl indiscriminately
	 * discards all allocations for the affected arena.
	 *
	 *   0: Application allocation.
	 *   1: Internal metadata allocation.
	 *
	 * Synchronization: atomic.
	 */
	atomic_u_t		nthreads[2];

	/* Next bin shard for binding new threads. Synchronization: atomic. */
	atomic_u_t		binshard_next;

	/*
	 * When percpu_arena is enabled, to amortize the cost of reading /
	 * updating the current CPU id, track the most recent thread accessing
	 * this arena, and only read CPU if there is a mismatch.
	 */
	tsdn_t		*last_thd;

	/* Synchronization: internal. */
	arena_stats_t		stats;

	/*
	 * Lists of tcaches and cache_bin_array_descriptors for extant threads
	 * associated with this arena.  Stats from these are merged
	 * incrementally, and at exit if opt_stats_print is enabled.
	 *
	 * Synchronization: tcache_ql_mtx.
	 */
	ql_head(tcache_slow_t)			tcache_ql;
	ql_head(cache_bin_array_descriptor_t)	cache_bin_array_descriptor_ql;
	malloc_mutex_t				tcache_ql_mtx;

	/*
	 * Represents a dss_prec_t, but atomically.
	 *
	 * Synchronization: atomic.
	 */
	atomic_u_t		dss_prec;

	/*
	 * Extant large allocations.
	 *
	 * Synchronization: large_mtx.
	 */
	edata_list_active_t	large;
	/* Synchronizes all large allocation/update/deallocation. */
	malloc_mutex_t		large_mtx;

	/* The page-level allocator shard this arena uses. */
	pa_shard_t		pa_shard;

	/*
	 * A cached copy of base->ind.  This can get accessed on hot paths;
	 * looking it up in base requires an extra pointer hop / cache miss.
	 */
	unsigned ind;

	/*
	 * Base allocator, from which arena metadata are allocated.
	 *
	 * Synchronization: internal.
	 */
	base_t			*base;
	/* Used to determine uptime.  Read-only after initialization. */
	nstime_t		create_time;

	/*
	 * The arena is allocated alongside its bins; really this is a
	 * dynamically sized array determined by the binshard settings.
	 */
	bin_t			bins[0];
};

#endif /* JEMALLOC_INTERNAL_ARENA_STRUCTS_H */
