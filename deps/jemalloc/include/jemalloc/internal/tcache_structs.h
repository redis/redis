#ifndef JEMALLOC_INTERNAL_TCACHE_STRUCTS_H
#define JEMALLOC_INTERNAL_TCACHE_STRUCTS_H

#include "jemalloc/internal/cache_bin.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/ticker.h"
#include "jemalloc/internal/tsd_types.h"

/*
 * The tcache state is split into the slow and hot path data.  Each has a
 * pointer to the other, and the data always comes in pairs.  The layout of each
 * of them varies in practice; tcache_slow lives in the TSD for the automatic
 * tcache, and as part of a dynamic allocation for manual allocations.  Keeping
 * a pointer to tcache_slow lets us treat these cases uniformly, rather than
 * splitting up the tcache [de]allocation code into those paths called with the
 * TSD tcache and those called with a manual tcache.
 */

struct tcache_slow_s {
	/* Lets us track all the tcaches in an arena. */
	ql_elm(tcache_slow_t) link;

	/*
	 * The descriptor lets the arena find our cache bins without seeing the
	 * tcache definition.  This enables arenas to aggregate stats across
	 * tcaches without having a tcache dependency.
	 */
	cache_bin_array_descriptor_t cache_bin_array_descriptor;

	/* The arena this tcache is associated with. */
	arena_t		*arena;
	/* Next bin to GC. */
	szind_t		next_gc_bin;
	/* For small bins, fill (ncached_max >> lg_fill_div). */
	uint8_t		lg_fill_div[SC_NBINS];
	/* For small bins, whether has been refilled since last GC. */
	bool		bin_refilled[SC_NBINS];
	/*
	 * For small bins, the number of items we can pretend to flush before
	 * actually flushing.
	 */
	uint8_t		bin_flush_delay_items[SC_NBINS];
	/*
	 * The start of the allocation containing the dynamic allocation for
	 * either the cache bins alone, or the cache bin memory as well as this
	 * tcache_slow_t and its associated tcache_t.
	 */
	void		*dyn_alloc;

	/* The associated bins. */
	tcache_t	*tcache;
};

struct tcache_s {
	tcache_slow_t	*tcache_slow;
	cache_bin_t	bins[TCACHE_NBINS_MAX];
};

/* Linkage for list of available (previously used) explicit tcache IDs. */
struct tcaches_s {
	union {
		tcache_t	*tcache;
		tcaches_t	*next;
	};
};

#endif /* JEMALLOC_INTERNAL_TCACHE_STRUCTS_H */
