/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct ctl_node_s ctl_node_t;
typedef struct ctl_named_node_s ctl_named_node_t;
typedef struct ctl_indexed_node_s ctl_indexed_node_t;
typedef struct ctl_arena_stats_s ctl_arena_stats_t;
typedef struct ctl_stats_s ctl_stats_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct ctl_node_s {
	bool			named;
};

struct ctl_named_node_s {
	struct ctl_node_s	node;
	const char		*name;
	/* If (nchildren == 0), this is a terminal node. */
	unsigned		nchildren;
	const			ctl_node_t *children;
	int			(*ctl)(tsd_t *, const size_t *, size_t, void *,
	    size_t *, void *, size_t);
};

struct ctl_indexed_node_s {
	struct ctl_node_s	node;
	const ctl_named_node_t	*(*index)(tsdn_t *, const size_t *, size_t,
	    size_t);
};

struct ctl_arena_stats_s {
	bool			initialized;
	unsigned		nthreads;
	const char		*dss;
	ssize_t			lg_dirty_mult;
	ssize_t			decay_time;
	size_t			pactive;
	size_t			pdirty;

	/* The remainder are only populated if config_stats is true. */

	arena_stats_t		astats;

	/* Aggregate stats for small size classes, based on bin stats. */
	size_t			allocated_small;
	uint64_t		nmalloc_small;
	uint64_t		ndalloc_small;
	uint64_t		nrequests_small;

	malloc_bin_stats_t	bstats[NBINS];
	malloc_large_stats_t	*lstats;	/* nlclasses elements. */
	malloc_huge_stats_t	*hstats;	/* nhclasses elements. */
};

struct ctl_stats_s {
	size_t			allocated;
	size_t			active;
	size_t			metadata;
	size_t			resident;
	size_t			mapped;
	size_t			retained;
	unsigned		narenas;
	ctl_arena_stats_t	*arenas;	/* (narenas + 1) elements. */
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

int	ctl_byname(tsd_t *tsd, const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);
int	ctl_nametomib(tsdn_t *tsdn, const char *name, size_t *mibp,
    size_t *miblenp);

int	ctl_bymib(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen);
bool	ctl_boot(void);
void	ctl_prefork(tsdn_t *tsdn);
void	ctl_postfork_parent(tsdn_t *tsdn);
void	ctl_postfork_child(tsdn_t *tsdn);

#define	xmallctl(name, oldp, oldlenp, newp, newlen) do {		\
	if (je_mallctl(name, oldp, oldlenp, newp, newlen)		\
	    != 0) {							\
		malloc_printf(						\
		    "<jemalloc>: Failure in xmallctl(\"%s\", ...)\n",	\
		    name);						\
		abort();						\
	}								\
} while (0)

#define	xmallctlnametomib(name, mibp, miblenp) do {			\
	if (je_mallctlnametomib(name, mibp, miblenp) != 0) {		\
		malloc_printf("<jemalloc>: Failure in "			\
		    "xmallctlnametomib(\"%s\", ...)\n", name);		\
		abort();						\
	}								\
} while (0)

#define	xmallctlbymib(mib, miblen, oldp, oldlenp, newp, newlen) do {	\
	if (je_mallctlbymib(mib, miblen, oldp, oldlenp, newp,		\
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

