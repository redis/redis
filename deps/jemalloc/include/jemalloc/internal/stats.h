/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#define	UMAX2S_BUFSIZE	65

#ifdef JEMALLOC_STATS
typedef struct tcache_bin_stats_s tcache_bin_stats_t;
typedef struct malloc_bin_stats_s malloc_bin_stats_t;
typedef struct malloc_large_stats_s malloc_large_stats_t;
typedef struct arena_stats_s arena_stats_t;
#endif
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
typedef struct chunk_stats_s chunk_stats_t;
#endif

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#ifdef JEMALLOC_STATS

#ifdef JEMALLOC_TCACHE
struct tcache_bin_stats_s {
	/*
	 * Number of allocation requests that corresponded to the size of this
	 * bin.
	 */
	uint64_t	nrequests;
};
#endif

struct malloc_bin_stats_s {
	/*
	 * Current number of bytes allocated, including objects currently
	 * cached by tcache.
	 */
	size_t		allocated;

	/*
	 * Total number of allocation/deallocation requests served directly by
	 * the bin.  Note that tcache may allocate an object, then recycle it
	 * many times, resulting many increments to nrequests, but only one
	 * each to nmalloc and ndalloc.
	 */
	uint64_t	nmalloc;
	uint64_t	ndalloc;

	/*
	 * Number of allocation requests that correspond to the size of this
	 * bin.  This includes requests served by tcache, though tcache only
	 * periodically merges into this counter.
	 */
	uint64_t	nrequests;

#ifdef JEMALLOC_TCACHE
	/* Number of tcache fills from this bin. */
	uint64_t	nfills;

	/* Number of tcache flushes to this bin. */
	uint64_t	nflushes;
#endif

	/* Total number of runs created for this bin's size class. */
	uint64_t	nruns;

	/*
	 * Total number of runs reused by extracting them from the runs tree for
	 * this bin's size class.
	 */
	uint64_t	reruns;

	/* High-water mark for this bin. */
	size_t		highruns;

	/* Current number of runs in this bin. */
	size_t		curruns;
};

struct malloc_large_stats_s {
	/*
	 * Total number of allocation/deallocation requests served directly by
	 * the arena.  Note that tcache may allocate an object, then recycle it
	 * many times, resulting many increments to nrequests, but only one
	 * each to nmalloc and ndalloc.
	 */
	uint64_t	nmalloc;
	uint64_t	ndalloc;

	/*
	 * Number of allocation requests that correspond to this size class.
	 * This includes requests served by tcache, though tcache only
	 * periodically merges into this counter.
	 */
	uint64_t	nrequests;

	/* High-water mark for this size class. */
	size_t		highruns;

	/* Current number of runs of this size class. */
	size_t		curruns;
};

struct arena_stats_s {
	/* Number of bytes currently mapped. */
	size_t		mapped;

	/*
	 * Total number of purge sweeps, total number of madvise calls made,
	 * and total pages purged in order to keep dirty unused memory under
	 * control.
	 */
	uint64_t	npurge;
	uint64_t	nmadvise;
	uint64_t	purged;

	/* Per-size-category statistics. */
	size_t		allocated_large;
	uint64_t	nmalloc_large;
	uint64_t	ndalloc_large;
	uint64_t	nrequests_large;

	/*
	 * One element for each possible size class, including sizes that
	 * overlap with bin size classes.  This is necessary because ipalloc()
	 * sometimes has to use such large objects in order to assure proper
	 * alignment.
	 */
	malloc_large_stats_t	*lstats;
};
#endif /* JEMALLOC_STATS */

#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
struct chunk_stats_s {
#  ifdef JEMALLOC_STATS
	/* Number of chunks that were allocated. */
	uint64_t	nchunks;
#  endif

	/* High-water mark for number of chunks allocated. */
	size_t		highchunks;

	/*
	 * Current number of chunks allocated.  This value isn't maintained for
	 * any other purpose, so keep track of it in order to be able to set
	 * highchunks.
	 */
	size_t		curchunks;
};
#endif /* JEMALLOC_STATS */

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern bool	opt_stats_print;

#ifdef JEMALLOC_STATS
extern size_t	stats_cactive;
#endif

char	*u2s(uint64_t x, unsigned base, char *s);
#ifdef JEMALLOC_STATS
void malloc_cprintf(void (*write)(void *, const char *), void *cbopaque,
    const char *format, ...) JEMALLOC_ATTR(format(printf, 3, 4));
void	malloc_printf(const char *format, ...)
    JEMALLOC_ATTR(format(printf, 1, 2));
#endif
void	stats_print(void (*write)(void *, const char *), void *cbopaque,
    const char *opts);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES
#ifdef JEMALLOC_STATS

#ifndef JEMALLOC_ENABLE_INLINE
size_t	stats_cactive_get(void);
void	stats_cactive_add(size_t size);
void	stats_cactive_sub(size_t size);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_STATS_C_))
JEMALLOC_INLINE size_t
stats_cactive_get(void)
{

	return (atomic_read_z(&stats_cactive));
}

JEMALLOC_INLINE void
stats_cactive_add(size_t size)
{

	atomic_add_z(&stats_cactive, size);
}

JEMALLOC_INLINE void
stats_cactive_sub(size_t size)
{

	atomic_sub_z(&stats_cactive, size);
}
#endif

#endif /* JEMALLOC_STATS */
#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
