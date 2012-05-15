/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct ctl_node_s ctl_node_t;
typedef struct ctl_arena_stats_s ctl_arena_stats_t;
typedef struct ctl_stats_s ctl_stats_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct ctl_node_s {
	bool			named;
	union {
		struct {
			const char	*name;
			/* If (nchildren == 0), this is a terminal node. */
			unsigned	nchildren;
			const	ctl_node_t *children;
		} named;
		struct {
			const ctl_node_t *(*index)(const size_t *, size_t,
			    size_t);
		} indexed;
	} u;
	int	(*ctl)(const size_t *, size_t, void *, size_t *, void *,
	    size_t);
};

struct ctl_arena_stats_s {
	bool			initialized;
	unsigned		nthreads;
	size_t			pactive;
	size_t			pdirty;
#ifdef JEMALLOC_STATS
	arena_stats_t		astats;

	/* Aggregate stats for small size classes, based on bin stats. */
	size_t			allocated_small;
	uint64_t		nmalloc_small;
	uint64_t		ndalloc_small;
	uint64_t		nrequests_small;

	malloc_bin_stats_t	*bstats;	/* nbins elements. */
	malloc_large_stats_t	*lstats;	/* nlclasses elements. */
#endif
};

struct ctl_stats_s {
#ifdef JEMALLOC_STATS
	size_t			allocated;
	size_t			active;
	size_t			mapped;
	struct {
		size_t		current;	/* stats_chunks.curchunks */
		uint64_t	total;		/* stats_chunks.nchunks */
		size_t		high;		/* stats_chunks.highchunks */
	} chunks;
	struct {
		size_t		allocated;	/* huge_allocated */
		uint64_t	nmalloc;	/* huge_nmalloc */
		uint64_t	ndalloc;	/* huge_ndalloc */
	} huge;
#endif
	ctl_arena_stats_t	*arenas;	/* (narenas + 1) elements. */
#ifdef JEMALLOC_SWAP
	size_t			swap_avail;
#endif
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

int	ctl_byname(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen);
int	ctl_nametomib(const char *name, size_t *mibp, size_t *miblenp);

int	ctl_bymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);
bool	ctl_boot(void);

#define	xmallctl(name, oldp, oldlenp, newp, newlen) do {		\
	if (JEMALLOC_P(mallctl)(name, oldp, oldlenp, newp, newlen)	\
	    != 0) {							\
		malloc_write("<jemalloc>: Failure in xmallctl(\"");	\
		malloc_write(name);					\
		malloc_write("\", ...)\n");				\
		abort();						\
	}								\
} while (0)

#define	xmallctlnametomib(name, mibp, miblenp) do {			\
	if (JEMALLOC_P(mallctlnametomib)(name, mibp, miblenp) != 0) {	\
		malloc_write(						\
		    "<jemalloc>: Failure in xmallctlnametomib(\"");	\
		malloc_write(name);					\
		malloc_write("\", ...)\n");				\
		abort();						\
	}								\
} while (0)

#define	xmallctlbymib(mib, miblen, oldp, oldlenp, newp, newlen) do {	\
	if (JEMALLOC_P(mallctlbymib)(mib, miblen, oldp, oldlenp, newp,	\
	    newlen) != 0) {						\
		malloc_write(						\
		    "<jemalloc>: Failure in xmallctlbymib()\n");	\
		abort();						\
	}								\
} while (0)

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

