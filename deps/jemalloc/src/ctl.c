#define	JEMALLOC_CTL_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

/*
 * ctl_mtx protects the following:
 * - ctl_stats.*
 * - opt_prof_active
 * - swap_enabled
 * - swap_prezeroed
 */
static malloc_mutex_t	ctl_mtx;
static bool		ctl_initialized;
static uint64_t		ctl_epoch;
static ctl_stats_t	ctl_stats;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

#define	CTL_PROTO(n)							\
static int	n##_ctl(const size_t *mib, size_t miblen, void *oldp,	\
    size_t *oldlenp, void *newp, size_t newlen);

#define	INDEX_PROTO(n)							\
const ctl_node_t	*n##_index(const size_t *mib, size_t miblen,	\
    size_t i);

#ifdef JEMALLOC_STATS
static bool	ctl_arena_init(ctl_arena_stats_t *astats);
#endif
static void	ctl_arena_clear(ctl_arena_stats_t *astats);
#ifdef JEMALLOC_STATS
static void	ctl_arena_stats_amerge(ctl_arena_stats_t *cstats,
    arena_t *arena);
static void	ctl_arena_stats_smerge(ctl_arena_stats_t *sstats,
    ctl_arena_stats_t *astats);
#endif
static void	ctl_arena_refresh(arena_t *arena, unsigned i);
static void	ctl_refresh(void);
static bool	ctl_init(void);
static int	ctl_lookup(const char *name, ctl_node_t const **nodesp,
    size_t *mibp, size_t *depthp);

CTL_PROTO(version)
CTL_PROTO(epoch)
#ifdef JEMALLOC_TCACHE
CTL_PROTO(tcache_flush)
#endif
CTL_PROTO(thread_arena)
#ifdef JEMALLOC_STATS
CTL_PROTO(thread_allocated)
CTL_PROTO(thread_allocatedp)
CTL_PROTO(thread_deallocated)
CTL_PROTO(thread_deallocatedp)
#endif
CTL_PROTO(config_debug)
CTL_PROTO(config_dss)
CTL_PROTO(config_dynamic_page_shift)
CTL_PROTO(config_fill)
CTL_PROTO(config_lazy_lock)
CTL_PROTO(config_prof)
CTL_PROTO(config_prof_libgcc)
CTL_PROTO(config_prof_libunwind)
CTL_PROTO(config_stats)
CTL_PROTO(config_swap)
CTL_PROTO(config_sysv)
CTL_PROTO(config_tcache)
CTL_PROTO(config_tiny)
CTL_PROTO(config_tls)
CTL_PROTO(config_xmalloc)
CTL_PROTO(opt_abort)
CTL_PROTO(opt_lg_qspace_max)
CTL_PROTO(opt_lg_cspace_max)
CTL_PROTO(opt_lg_chunk)
CTL_PROTO(opt_narenas)
CTL_PROTO(opt_lg_dirty_mult)
CTL_PROTO(opt_stats_print)
#ifdef JEMALLOC_FILL
CTL_PROTO(opt_junk)
CTL_PROTO(opt_zero)
#endif
#ifdef JEMALLOC_SYSV
CTL_PROTO(opt_sysv)
#endif
#ifdef JEMALLOC_XMALLOC
CTL_PROTO(opt_xmalloc)
#endif
#ifdef JEMALLOC_TCACHE
CTL_PROTO(opt_tcache)
CTL_PROTO(opt_lg_tcache_gc_sweep)
#endif
#ifdef JEMALLOC_PROF
CTL_PROTO(opt_prof)
CTL_PROTO(opt_prof_prefix)
CTL_PROTO(opt_prof_active)
CTL_PROTO(opt_lg_prof_bt_max)
CTL_PROTO(opt_lg_prof_sample)
CTL_PROTO(opt_lg_prof_interval)
CTL_PROTO(opt_prof_gdump)
CTL_PROTO(opt_prof_leak)
CTL_PROTO(opt_prof_accum)
CTL_PROTO(opt_lg_prof_tcmax)
#endif
#ifdef JEMALLOC_SWAP
CTL_PROTO(opt_overcommit)
#endif
CTL_PROTO(arenas_bin_i_size)
CTL_PROTO(arenas_bin_i_nregs)
CTL_PROTO(arenas_bin_i_run_size)
INDEX_PROTO(arenas_bin_i)
CTL_PROTO(arenas_lrun_i_size)
INDEX_PROTO(arenas_lrun_i)
CTL_PROTO(arenas_narenas)
CTL_PROTO(arenas_initialized)
CTL_PROTO(arenas_quantum)
CTL_PROTO(arenas_cacheline)
CTL_PROTO(arenas_subpage)
CTL_PROTO(arenas_pagesize)
CTL_PROTO(arenas_chunksize)
#ifdef JEMALLOC_TINY
CTL_PROTO(arenas_tspace_min)
CTL_PROTO(arenas_tspace_max)
#endif
CTL_PROTO(arenas_qspace_min)
CTL_PROTO(arenas_qspace_max)
CTL_PROTO(arenas_cspace_min)
CTL_PROTO(arenas_cspace_max)
CTL_PROTO(arenas_sspace_min)
CTL_PROTO(arenas_sspace_max)
#ifdef JEMALLOC_TCACHE
CTL_PROTO(arenas_tcache_max)
#endif
CTL_PROTO(arenas_ntbins)
CTL_PROTO(arenas_nqbins)
CTL_PROTO(arenas_ncbins)
CTL_PROTO(arenas_nsbins)
CTL_PROTO(arenas_nbins)
#ifdef JEMALLOC_TCACHE
CTL_PROTO(arenas_nhbins)
#endif
CTL_PROTO(arenas_nlruns)
CTL_PROTO(arenas_purge)
#ifdef JEMALLOC_PROF
CTL_PROTO(prof_active)
CTL_PROTO(prof_dump)
CTL_PROTO(prof_interval)
#endif
#ifdef JEMALLOC_STATS
CTL_PROTO(stats_chunks_current)
CTL_PROTO(stats_chunks_total)
CTL_PROTO(stats_chunks_high)
CTL_PROTO(stats_huge_allocated)
CTL_PROTO(stats_huge_nmalloc)
CTL_PROTO(stats_huge_ndalloc)
CTL_PROTO(stats_arenas_i_small_allocated)
CTL_PROTO(stats_arenas_i_small_nmalloc)
CTL_PROTO(stats_arenas_i_small_ndalloc)
CTL_PROTO(stats_arenas_i_small_nrequests)
CTL_PROTO(stats_arenas_i_large_allocated)
CTL_PROTO(stats_arenas_i_large_nmalloc)
CTL_PROTO(stats_arenas_i_large_ndalloc)
CTL_PROTO(stats_arenas_i_large_nrequests)
CTL_PROTO(stats_arenas_i_bins_j_allocated)
CTL_PROTO(stats_arenas_i_bins_j_nmalloc)
CTL_PROTO(stats_arenas_i_bins_j_ndalloc)
CTL_PROTO(stats_arenas_i_bins_j_nrequests)
#ifdef JEMALLOC_TCACHE
CTL_PROTO(stats_arenas_i_bins_j_nfills)
CTL_PROTO(stats_arenas_i_bins_j_nflushes)
#endif
CTL_PROTO(stats_arenas_i_bins_j_nruns)
CTL_PROTO(stats_arenas_i_bins_j_nreruns)
CTL_PROTO(stats_arenas_i_bins_j_highruns)
CTL_PROTO(stats_arenas_i_bins_j_curruns)
INDEX_PROTO(stats_arenas_i_bins_j)
CTL_PROTO(stats_arenas_i_lruns_j_nmalloc)
CTL_PROTO(stats_arenas_i_lruns_j_ndalloc)
CTL_PROTO(stats_arenas_i_lruns_j_nrequests)
CTL_PROTO(stats_arenas_i_lruns_j_highruns)
CTL_PROTO(stats_arenas_i_lruns_j_curruns)
INDEX_PROTO(stats_arenas_i_lruns_j)
#endif
CTL_PROTO(stats_arenas_i_nthreads)
CTL_PROTO(stats_arenas_i_pactive)
CTL_PROTO(stats_arenas_i_pdirty)
#ifdef JEMALLOC_STATS
CTL_PROTO(stats_arenas_i_mapped)
CTL_PROTO(stats_arenas_i_npurge)
CTL_PROTO(stats_arenas_i_nmadvise)
CTL_PROTO(stats_arenas_i_purged)
#endif
INDEX_PROTO(stats_arenas_i)
#ifdef JEMALLOC_STATS
CTL_PROTO(stats_cactive)
CTL_PROTO(stats_allocated)
CTL_PROTO(stats_active)
CTL_PROTO(stats_mapped)
#endif
#ifdef JEMALLOC_SWAP
#  ifdef JEMALLOC_STATS
CTL_PROTO(swap_avail)
#  endif
CTL_PROTO(swap_prezeroed)
CTL_PROTO(swap_nfds)
CTL_PROTO(swap_fds)
#endif

/******************************************************************************/
/* mallctl tree. */

/* Maximum tree depth. */
#define	CTL_MAX_DEPTH	6

#define	NAME(n)	true,	{.named = {n
#define	CHILD(c) sizeof(c##_node) / sizeof(ctl_node_t),	c##_node}},	NULL
#define	CTL(c)	0,				NULL}},		c##_ctl

/*
 * Only handles internal indexed nodes, since there are currently no external
 * ones.
 */
#define	INDEX(i)	false,	{.indexed = {i##_index}},		NULL

#ifdef JEMALLOC_TCACHE
static const ctl_node_t	tcache_node[] = {
	{NAME("flush"),		CTL(tcache_flush)}
};
#endif

static const ctl_node_t	thread_node[] = {
	{NAME("arena"),		CTL(thread_arena)}
#ifdef JEMALLOC_STATS
	,
	{NAME("allocated"),	CTL(thread_allocated)},
	{NAME("allocatedp"),	CTL(thread_allocatedp)},
	{NAME("deallocated"),	CTL(thread_deallocated)},
	{NAME("deallocatedp"),	CTL(thread_deallocatedp)}
#endif
};

static const ctl_node_t	config_node[] = {
	{NAME("debug"),			CTL(config_debug)},
	{NAME("dss"),			CTL(config_dss)},
	{NAME("dynamic_page_shift"),	CTL(config_dynamic_page_shift)},
	{NAME("fill"),			CTL(config_fill)},
	{NAME("lazy_lock"),		CTL(config_lazy_lock)},
	{NAME("prof"),			CTL(config_prof)},
	{NAME("prof_libgcc"),		CTL(config_prof_libgcc)},
	{NAME("prof_libunwind"),	CTL(config_prof_libunwind)},
	{NAME("stats"),			CTL(config_stats)},
	{NAME("swap"),			CTL(config_swap)},
	{NAME("sysv"),			CTL(config_sysv)},
	{NAME("tcache"),		CTL(config_tcache)},
	{NAME("tiny"),			CTL(config_tiny)},
	{NAME("tls"),			CTL(config_tls)},
	{NAME("xmalloc"),		CTL(config_xmalloc)}
};

static const ctl_node_t opt_node[] = {
	{NAME("abort"),			CTL(opt_abort)},
	{NAME("lg_qspace_max"),		CTL(opt_lg_qspace_max)},
	{NAME("lg_cspace_max"),		CTL(opt_lg_cspace_max)},
	{NAME("lg_chunk"),		CTL(opt_lg_chunk)},
	{NAME("narenas"),		CTL(opt_narenas)},
	{NAME("lg_dirty_mult"),		CTL(opt_lg_dirty_mult)},
	{NAME("stats_print"),		CTL(opt_stats_print)}
#ifdef JEMALLOC_FILL
	,
	{NAME("junk"),			CTL(opt_junk)},
	{NAME("zero"),			CTL(opt_zero)}
#endif
#ifdef JEMALLOC_SYSV
	,
	{NAME("sysv"),			CTL(opt_sysv)}
#endif
#ifdef JEMALLOC_XMALLOC
	,
	{NAME("xmalloc"),		CTL(opt_xmalloc)}
#endif
#ifdef JEMALLOC_TCACHE
	,
	{NAME("tcache"),		CTL(opt_tcache)},
	{NAME("lg_tcache_gc_sweep"),	CTL(opt_lg_tcache_gc_sweep)}
#endif
#ifdef JEMALLOC_PROF
	,
	{NAME("prof"),			CTL(opt_prof)},
	{NAME("prof_prefix"),		CTL(opt_prof_prefix)},
	{NAME("prof_active"),		CTL(opt_prof_active)},
	{NAME("lg_prof_bt_max"),	CTL(opt_lg_prof_bt_max)},
	{NAME("lg_prof_sample"),	CTL(opt_lg_prof_sample)},
	{NAME("lg_prof_interval"),	CTL(opt_lg_prof_interval)},
	{NAME("prof_gdump"),		CTL(opt_prof_gdump)},
	{NAME("prof_leak"),		CTL(opt_prof_leak)},
	{NAME("prof_accum"),		CTL(opt_prof_accum)},
	{NAME("lg_prof_tcmax"),		CTL(opt_lg_prof_tcmax)}
#endif
#ifdef JEMALLOC_SWAP
	,
	{NAME("overcommit"),		CTL(opt_overcommit)}
#endif
};

static const ctl_node_t arenas_bin_i_node[] = {
	{NAME("size"),			CTL(arenas_bin_i_size)},
	{NAME("nregs"),			CTL(arenas_bin_i_nregs)},
	{NAME("run_size"),		CTL(arenas_bin_i_run_size)}
};
static const ctl_node_t super_arenas_bin_i_node[] = {
	{NAME(""),			CHILD(arenas_bin_i)}
};

static const ctl_node_t arenas_bin_node[] = {
	{INDEX(arenas_bin_i)}
};

static const ctl_node_t arenas_lrun_i_node[] = {
	{NAME("size"),			CTL(arenas_lrun_i_size)}
};
static const ctl_node_t super_arenas_lrun_i_node[] = {
	{NAME(""),			CHILD(arenas_lrun_i)}
};

static const ctl_node_t arenas_lrun_node[] = {
	{INDEX(arenas_lrun_i)}
};

static const ctl_node_t arenas_node[] = {
	{NAME("narenas"),		CTL(arenas_narenas)},
	{NAME("initialized"),		CTL(arenas_initialized)},
	{NAME("quantum"),		CTL(arenas_quantum)},
	{NAME("cacheline"),		CTL(arenas_cacheline)},
	{NAME("subpage"),		CTL(arenas_subpage)},
	{NAME("pagesize"),		CTL(arenas_pagesize)},
	{NAME("chunksize"),		CTL(arenas_chunksize)},
#ifdef JEMALLOC_TINY
	{NAME("tspace_min"),		CTL(arenas_tspace_min)},
	{NAME("tspace_max"),		CTL(arenas_tspace_max)},
#endif
	{NAME("qspace_min"),		CTL(arenas_qspace_min)},
	{NAME("qspace_max"),		CTL(arenas_qspace_max)},
	{NAME("cspace_min"),		CTL(arenas_cspace_min)},
	{NAME("cspace_max"),		CTL(arenas_cspace_max)},
	{NAME("sspace_min"),		CTL(arenas_sspace_min)},
	{NAME("sspace_max"),		CTL(arenas_sspace_max)},
#ifdef JEMALLOC_TCACHE
	{NAME("tcache_max"),		CTL(arenas_tcache_max)},
#endif
	{NAME("ntbins"),		CTL(arenas_ntbins)},
	{NAME("nqbins"),		CTL(arenas_nqbins)},
	{NAME("ncbins"),		CTL(arenas_ncbins)},
	{NAME("nsbins"),		CTL(arenas_nsbins)},
	{NAME("nbins"),			CTL(arenas_nbins)},
#ifdef JEMALLOC_TCACHE
	{NAME("nhbins"),		CTL(arenas_nhbins)},
#endif
	{NAME("bin"),			CHILD(arenas_bin)},
	{NAME("nlruns"),		CTL(arenas_nlruns)},
	{NAME("lrun"),			CHILD(arenas_lrun)},
	{NAME("purge"),			CTL(arenas_purge)}
};

#ifdef JEMALLOC_PROF
static const ctl_node_t	prof_node[] = {
	{NAME("active"),	CTL(prof_active)},
	{NAME("dump"),		CTL(prof_dump)},
	{NAME("interval"),	CTL(prof_interval)}
};
#endif

#ifdef JEMALLOC_STATS
static const ctl_node_t stats_chunks_node[] = {
	{NAME("current"),		CTL(stats_chunks_current)},
	{NAME("total"),			CTL(stats_chunks_total)},
	{NAME("high"),			CTL(stats_chunks_high)}
};

static const ctl_node_t stats_huge_node[] = {
	{NAME("allocated"),		CTL(stats_huge_allocated)},
	{NAME("nmalloc"),		CTL(stats_huge_nmalloc)},
	{NAME("ndalloc"),		CTL(stats_huge_ndalloc)}
};

static const ctl_node_t stats_arenas_i_small_node[] = {
	{NAME("allocated"),		CTL(stats_arenas_i_small_allocated)},
	{NAME("nmalloc"),		CTL(stats_arenas_i_small_nmalloc)},
	{NAME("ndalloc"),		CTL(stats_arenas_i_small_ndalloc)},
	{NAME("nrequests"),		CTL(stats_arenas_i_small_nrequests)}
};

static const ctl_node_t stats_arenas_i_large_node[] = {
	{NAME("allocated"),		CTL(stats_arenas_i_large_allocated)},
	{NAME("nmalloc"),		CTL(stats_arenas_i_large_nmalloc)},
	{NAME("ndalloc"),		CTL(stats_arenas_i_large_ndalloc)},
	{NAME("nrequests"),		CTL(stats_arenas_i_large_nrequests)}
};

static const ctl_node_t stats_arenas_i_bins_j_node[] = {
	{NAME("allocated"),		CTL(stats_arenas_i_bins_j_allocated)},
	{NAME("nmalloc"),		CTL(stats_arenas_i_bins_j_nmalloc)},
	{NAME("ndalloc"),		CTL(stats_arenas_i_bins_j_ndalloc)},
	{NAME("nrequests"),		CTL(stats_arenas_i_bins_j_nrequests)},
#ifdef JEMALLOC_TCACHE
	{NAME("nfills"),		CTL(stats_arenas_i_bins_j_nfills)},
	{NAME("nflushes"),		CTL(stats_arenas_i_bins_j_nflushes)},
#endif
	{NAME("nruns"),			CTL(stats_arenas_i_bins_j_nruns)},
	{NAME("nreruns"),		CTL(stats_arenas_i_bins_j_nreruns)},
	{NAME("highruns"),		CTL(stats_arenas_i_bins_j_highruns)},
	{NAME("curruns"),		CTL(stats_arenas_i_bins_j_curruns)}
};
static const ctl_node_t super_stats_arenas_i_bins_j_node[] = {
	{NAME(""),			CHILD(stats_arenas_i_bins_j)}
};

static const ctl_node_t stats_arenas_i_bins_node[] = {
	{INDEX(stats_arenas_i_bins_j)}
};

static const ctl_node_t stats_arenas_i_lruns_j_node[] = {
	{NAME("nmalloc"),		CTL(stats_arenas_i_lruns_j_nmalloc)},
	{NAME("ndalloc"),		CTL(stats_arenas_i_lruns_j_ndalloc)},
	{NAME("nrequests"),		CTL(stats_arenas_i_lruns_j_nrequests)},
	{NAME("highruns"),		CTL(stats_arenas_i_lruns_j_highruns)},
	{NAME("curruns"),		CTL(stats_arenas_i_lruns_j_curruns)}
};
static const ctl_node_t super_stats_arenas_i_lruns_j_node[] = {
	{NAME(""),			CHILD(stats_arenas_i_lruns_j)}
};

static const ctl_node_t stats_arenas_i_lruns_node[] = {
	{INDEX(stats_arenas_i_lruns_j)}
};
#endif

static const ctl_node_t stats_arenas_i_node[] = {
	{NAME("nthreads"),		CTL(stats_arenas_i_nthreads)},
	{NAME("pactive"),		CTL(stats_arenas_i_pactive)},
	{NAME("pdirty"),		CTL(stats_arenas_i_pdirty)}
#ifdef JEMALLOC_STATS
	,
	{NAME("mapped"),		CTL(stats_arenas_i_mapped)},
	{NAME("npurge"),		CTL(stats_arenas_i_npurge)},
	{NAME("nmadvise"),		CTL(stats_arenas_i_nmadvise)},
	{NAME("purged"),		CTL(stats_arenas_i_purged)},
	{NAME("small"),			CHILD(stats_arenas_i_small)},
	{NAME("large"),			CHILD(stats_arenas_i_large)},
	{NAME("bins"),			CHILD(stats_arenas_i_bins)},
	{NAME("lruns"),		CHILD(stats_arenas_i_lruns)}
#endif
};
static const ctl_node_t super_stats_arenas_i_node[] = {
	{NAME(""),			CHILD(stats_arenas_i)}
};

static const ctl_node_t stats_arenas_node[] = {
	{INDEX(stats_arenas_i)}
};

static const ctl_node_t stats_node[] = {
#ifdef JEMALLOC_STATS
	{NAME("cactive"),		CTL(stats_cactive)},
	{NAME("allocated"),		CTL(stats_allocated)},
	{NAME("active"),		CTL(stats_active)},
	{NAME("mapped"),		CTL(stats_mapped)},
	{NAME("chunks"),		CHILD(stats_chunks)},
	{NAME("huge"),			CHILD(stats_huge)},
#endif
	{NAME("arenas"),		CHILD(stats_arenas)}
};

#ifdef JEMALLOC_SWAP
static const ctl_node_t swap_node[] = {
#  ifdef JEMALLOC_STATS
	{NAME("avail"),			CTL(swap_avail)},
#  endif
	{NAME("prezeroed"),		CTL(swap_prezeroed)},
	{NAME("nfds"),			CTL(swap_nfds)},
	{NAME("fds"),			CTL(swap_fds)}
};
#endif

static const ctl_node_t	root_node[] = {
	{NAME("version"),	CTL(version)},
	{NAME("epoch"),		CTL(epoch)},
#ifdef JEMALLOC_TCACHE
	{NAME("tcache"),	CHILD(tcache)},
#endif
	{NAME("thread"),	CHILD(thread)},
	{NAME("config"),	CHILD(config)},
	{NAME("opt"),		CHILD(opt)},
	{NAME("arenas"),	CHILD(arenas)},
#ifdef JEMALLOC_PROF
	{NAME("prof"),		CHILD(prof)},
#endif
	{NAME("stats"),		CHILD(stats)}
#ifdef JEMALLOC_SWAP
	,
	{NAME("swap"),		CHILD(swap)}
#endif
};
static const ctl_node_t super_root_node[] = {
	{NAME(""),		CHILD(root)}
};

#undef NAME
#undef CHILD
#undef CTL
#undef INDEX

/******************************************************************************/

#ifdef JEMALLOC_STATS
static bool
ctl_arena_init(ctl_arena_stats_t *astats)
{

	if (astats->bstats == NULL) {
		astats->bstats = (malloc_bin_stats_t *)base_alloc(nbins *
		    sizeof(malloc_bin_stats_t));
		if (astats->bstats == NULL)
			return (true);
	}
	if (astats->lstats == NULL) {
		astats->lstats = (malloc_large_stats_t *)base_alloc(nlclasses *
		    sizeof(malloc_large_stats_t));
		if (astats->lstats == NULL)
			return (true);
	}

	return (false);
}
#endif

static void
ctl_arena_clear(ctl_arena_stats_t *astats)
{

	astats->pactive = 0;
	astats->pdirty = 0;
#ifdef JEMALLOC_STATS
	memset(&astats->astats, 0, sizeof(arena_stats_t));
	astats->allocated_small = 0;
	astats->nmalloc_small = 0;
	astats->ndalloc_small = 0;
	astats->nrequests_small = 0;
	memset(astats->bstats, 0, nbins * sizeof(malloc_bin_stats_t));
	memset(astats->lstats, 0, nlclasses * sizeof(malloc_large_stats_t));
#endif
}

#ifdef JEMALLOC_STATS
static void
ctl_arena_stats_amerge(ctl_arena_stats_t *cstats, arena_t *arena)
{
	unsigned i;

	arena_stats_merge(arena, &cstats->pactive, &cstats->pdirty,
	    &cstats->astats, cstats->bstats, cstats->lstats);

	for (i = 0; i < nbins; i++) {
		cstats->allocated_small += cstats->bstats[i].allocated;
		cstats->nmalloc_small += cstats->bstats[i].nmalloc;
		cstats->ndalloc_small += cstats->bstats[i].ndalloc;
		cstats->nrequests_small += cstats->bstats[i].nrequests;
	}
}

static void
ctl_arena_stats_smerge(ctl_arena_stats_t *sstats, ctl_arena_stats_t *astats)
{
	unsigned i;

	sstats->pactive += astats->pactive;
	sstats->pdirty += astats->pdirty;

	sstats->astats.mapped += astats->astats.mapped;
	sstats->astats.npurge += astats->astats.npurge;
	sstats->astats.nmadvise += astats->astats.nmadvise;
	sstats->astats.purged += astats->astats.purged;

	sstats->allocated_small += astats->allocated_small;
	sstats->nmalloc_small += astats->nmalloc_small;
	sstats->ndalloc_small += astats->ndalloc_small;
	sstats->nrequests_small += astats->nrequests_small;

	sstats->astats.allocated_large += astats->astats.allocated_large;
	sstats->astats.nmalloc_large += astats->astats.nmalloc_large;
	sstats->astats.ndalloc_large += astats->astats.ndalloc_large;
	sstats->astats.nrequests_large += astats->astats.nrequests_large;

	for (i = 0; i < nlclasses; i++) {
		sstats->lstats[i].nmalloc += astats->lstats[i].nmalloc;
		sstats->lstats[i].ndalloc += astats->lstats[i].ndalloc;
		sstats->lstats[i].nrequests += astats->lstats[i].nrequests;
		sstats->lstats[i].highruns += astats->lstats[i].highruns;
		sstats->lstats[i].curruns += astats->lstats[i].curruns;
	}

	for (i = 0; i < nbins; i++) {
		sstats->bstats[i].allocated += astats->bstats[i].allocated;
		sstats->bstats[i].nmalloc += astats->bstats[i].nmalloc;
		sstats->bstats[i].ndalloc += astats->bstats[i].ndalloc;
		sstats->bstats[i].nrequests += astats->bstats[i].nrequests;
#ifdef JEMALLOC_TCACHE
		sstats->bstats[i].nfills += astats->bstats[i].nfills;
		sstats->bstats[i].nflushes += astats->bstats[i].nflushes;
#endif
		sstats->bstats[i].nruns += astats->bstats[i].nruns;
		sstats->bstats[i].reruns += astats->bstats[i].reruns;
		sstats->bstats[i].highruns += astats->bstats[i].highruns;
		sstats->bstats[i].curruns += astats->bstats[i].curruns;
	}
}
#endif

static void
ctl_arena_refresh(arena_t *arena, unsigned i)
{
	ctl_arena_stats_t *astats = &ctl_stats.arenas[i];
	ctl_arena_stats_t *sstats = &ctl_stats.arenas[narenas];

	ctl_arena_clear(astats);

	sstats->nthreads += astats->nthreads;
#ifdef JEMALLOC_STATS
	ctl_arena_stats_amerge(astats, arena);
	/* Merge into sum stats as well. */
	ctl_arena_stats_smerge(sstats, astats);
#else
	astats->pactive += arena->nactive;
	astats->pdirty += arena->ndirty;
	/* Merge into sum stats as well. */
	sstats->pactive += arena->nactive;
	sstats->pdirty += arena->ndirty;
#endif
}

static void
ctl_refresh(void)
{
	unsigned i;
	arena_t *tarenas[narenas];

#ifdef JEMALLOC_STATS
	malloc_mutex_lock(&chunks_mtx);
	ctl_stats.chunks.current = stats_chunks.curchunks;
	ctl_stats.chunks.total = stats_chunks.nchunks;
	ctl_stats.chunks.high = stats_chunks.highchunks;
	malloc_mutex_unlock(&chunks_mtx);

	malloc_mutex_lock(&huge_mtx);
	ctl_stats.huge.allocated = huge_allocated;
	ctl_stats.huge.nmalloc = huge_nmalloc;
	ctl_stats.huge.ndalloc = huge_ndalloc;
	malloc_mutex_unlock(&huge_mtx);
#endif

	/*
	 * Clear sum stats, since they will be merged into by
	 * ctl_arena_refresh().
	 */
	ctl_stats.arenas[narenas].nthreads = 0;
	ctl_arena_clear(&ctl_stats.arenas[narenas]);

	malloc_mutex_lock(&arenas_lock);
	memcpy(tarenas, arenas, sizeof(arena_t *) * narenas);
	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			ctl_stats.arenas[i].nthreads = arenas[i]->nthreads;
		else
			ctl_stats.arenas[i].nthreads = 0;
	}
	malloc_mutex_unlock(&arenas_lock);
	for (i = 0; i < narenas; i++) {
		bool initialized = (tarenas[i] != NULL);

		ctl_stats.arenas[i].initialized = initialized;
		if (initialized)
			ctl_arena_refresh(tarenas[i], i);
	}

#ifdef JEMALLOC_STATS
	ctl_stats.allocated = ctl_stats.arenas[narenas].allocated_small
	    + ctl_stats.arenas[narenas].astats.allocated_large
	    + ctl_stats.huge.allocated;
	ctl_stats.active = (ctl_stats.arenas[narenas].pactive << PAGE_SHIFT)
	    + ctl_stats.huge.allocated;
	ctl_stats.mapped = (ctl_stats.chunks.current << opt_lg_chunk);

#  ifdef JEMALLOC_SWAP
	malloc_mutex_lock(&swap_mtx);
	ctl_stats.swap_avail = swap_avail;
	malloc_mutex_unlock(&swap_mtx);
#  endif
#endif

	ctl_epoch++;
}

static bool
ctl_init(void)
{
	bool ret;

	malloc_mutex_lock(&ctl_mtx);
	if (ctl_initialized == false) {
#ifdef JEMALLOC_STATS
		unsigned i;
#endif

		/*
		 * Allocate space for one extra arena stats element, which
		 * contains summed stats across all arenas.
		 */
		ctl_stats.arenas = (ctl_arena_stats_t *)base_alloc(
		    (narenas + 1) * sizeof(ctl_arena_stats_t));
		if (ctl_stats.arenas == NULL) {
			ret = true;
			goto RETURN;
		}
		memset(ctl_stats.arenas, 0, (narenas + 1) *
		    sizeof(ctl_arena_stats_t));

		/*
		 * Initialize all stats structures, regardless of whether they
		 * ever get used.  Lazy initialization would allow errors to
		 * cause inconsistent state to be viewable by the application.
		 */
#ifdef JEMALLOC_STATS
		for (i = 0; i <= narenas; i++) {
			if (ctl_arena_init(&ctl_stats.arenas[i])) {
				ret = true;
				goto RETURN;
			}
		}
#endif
		ctl_stats.arenas[narenas].initialized = true;

		ctl_epoch = 0;
		ctl_refresh();
		ctl_initialized = true;
	}

	ret = false;
RETURN:
	malloc_mutex_unlock(&ctl_mtx);
	return (ret);
}

static int
ctl_lookup(const char *name, ctl_node_t const **nodesp, size_t *mibp,
    size_t *depthp)
{
	int ret;
	const char *elm, *tdot, *dot;
	size_t elen, i, j;
	const ctl_node_t *node;

	elm = name;
	/* Equivalent to strchrnul(). */
	dot = ((tdot = strchr(elm, '.')) != NULL) ? tdot : strchr(elm, '\0');
	elen = (size_t)((uintptr_t)dot - (uintptr_t)elm);
	if (elen == 0) {
		ret = ENOENT;
		goto RETURN;
	}
	node = super_root_node;
	for (i = 0; i < *depthp; i++) {
		assert(node->named);
		assert(node->u.named.nchildren > 0);
		if (node->u.named.children[0].named) {
			const ctl_node_t *pnode = node;

			/* Children are named. */
			for (j = 0; j < node->u.named.nchildren; j++) {
				const ctl_node_t *child =
				    &node->u.named.children[j];
				if (strlen(child->u.named.name) == elen
				    && strncmp(elm, child->u.named.name,
				    elen) == 0) {
					node = child;
					if (nodesp != NULL)
						nodesp[i] = node;
					mibp[i] = j;
					break;
				}
			}
			if (node == pnode) {
				ret = ENOENT;
				goto RETURN;
			}
		} else {
			unsigned long index;
			const ctl_node_t *inode;

			/* Children are indexed. */
			index = strtoul(elm, NULL, 10);
			if (index == ULONG_MAX) {
				ret = ENOENT;
				goto RETURN;
			}

			inode = &node->u.named.children[0];
			node = inode->u.indexed.index(mibp, *depthp,
			    index);
			if (node == NULL) {
				ret = ENOENT;
				goto RETURN;
			}

			if (nodesp != NULL)
				nodesp[i] = node;
			mibp[i] = (size_t)index;
		}

		if (node->ctl != NULL) {
			/* Terminal node. */
			if (*dot != '\0') {
				/*
				 * The name contains more elements than are
				 * in this path through the tree.
				 */
				ret = ENOENT;
				goto RETURN;
			}
			/* Complete lookup successful. */
			*depthp = i + 1;
			break;
		}

		/* Update elm. */
		if (*dot == '\0') {
			/* No more elements. */
			ret = ENOENT;
			goto RETURN;
		}
		elm = &dot[1];
		dot = ((tdot = strchr(elm, '.')) != NULL) ? tdot :
		    strchr(elm, '\0');
		elen = (size_t)((uintptr_t)dot - (uintptr_t)elm);
	}

	ret = 0;
RETURN:
	return (ret);
}

int
ctl_byname(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen)
{
	int ret;
	size_t depth;
	ctl_node_t const *nodes[CTL_MAX_DEPTH];
	size_t mib[CTL_MAX_DEPTH];

	if (ctl_initialized == false && ctl_init()) {
		ret = EAGAIN;
		goto RETURN;
	}

	depth = CTL_MAX_DEPTH;
	ret = ctl_lookup(name, nodes, mib, &depth);
	if (ret != 0)
		goto RETURN;

	if (nodes[depth-1]->ctl == NULL) {
		/* The name refers to a partial path through the ctl tree. */
		ret = ENOENT;
		goto RETURN;
	}

	ret = nodes[depth-1]->ctl(mib, depth, oldp, oldlenp, newp, newlen);
RETURN:
	return(ret);
}

int
ctl_nametomib(const char *name, size_t *mibp, size_t *miblenp)
{
	int ret;

	if (ctl_initialized == false && ctl_init()) {
		ret = EAGAIN;
		goto RETURN;
	}

	ret = ctl_lookup(name, NULL, mibp, miblenp);
RETURN:
	return(ret);
}

int
ctl_bymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;
	const ctl_node_t *node;
	size_t i;

	if (ctl_initialized == false && ctl_init()) {
		ret = EAGAIN;
		goto RETURN;
	}

	/* Iterate down the tree. */
	node = super_root_node;
	for (i = 0; i < miblen; i++) {
		if (node->u.named.children[0].named) {
			/* Children are named. */
			if (node->u.named.nchildren <= mib[i]) {
				ret = ENOENT;
				goto RETURN;
			}
			node = &node->u.named.children[mib[i]];
		} else {
			const ctl_node_t *inode;

			/* Indexed element. */
			inode = &node->u.named.children[0];
			node = inode->u.indexed.index(mib, miblen, mib[i]);
			if (node == NULL) {
				ret = ENOENT;
				goto RETURN;
			}
		}
	}

	/* Call the ctl function. */
	if (node->ctl == NULL) {
		/* Partial MIB. */
		ret = ENOENT;
		goto RETURN;
	}
	ret = node->ctl(mib, miblen, oldp, oldlenp, newp, newlen);

RETURN:
	return(ret);
}

bool
ctl_boot(void)
{

	if (malloc_mutex_init(&ctl_mtx))
		return (true);

	ctl_initialized = false;

	return (false);
}

/******************************************************************************/
/* *_ctl() functions. */

#define	READONLY()	do {						\
	if (newp != NULL || newlen != 0) {				\
		ret = EPERM;						\
		goto RETURN;						\
	}								\
} while (0)

#define	WRITEONLY()	do {						\
	if (oldp != NULL || oldlenp != NULL) {				\
		ret = EPERM;						\
		goto RETURN;						\
	}								\
} while (0)

#define	VOID()	do {							\
	READONLY();							\
	WRITEONLY();							\
} while (0)

#define	READ(v, t)	do {						\
	if (oldp != NULL && oldlenp != NULL) {				\
		if (*oldlenp != sizeof(t)) {				\
			size_t	copylen = (sizeof(t) <= *oldlenp)	\
			    ? sizeof(t) : *oldlenp;			\
			memcpy(oldp, (void *)&v, copylen);		\
			ret = EINVAL;					\
			goto RETURN;					\
		} else							\
			*(t *)oldp = v;					\
	}								\
} while (0)

#define	WRITE(v, t)	do {						\
	if (newp != NULL) {						\
		if (newlen != sizeof(t)) {				\
			ret = EINVAL;					\
			goto RETURN;					\
		}							\
		v = *(t *)newp;						\
	}								\
} while (0)

#define	CTL_RO_GEN(n, v, t)						\
static int								\
n##_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,	\
    void *newp, size_t newlen)						\
{									\
	int ret;							\
	t oldval;							\
									\
	malloc_mutex_lock(&ctl_mtx);					\
	READONLY();							\
	oldval = v;							\
	READ(oldval, t);						\
									\
	ret = 0;							\
RETURN:									\
	malloc_mutex_unlock(&ctl_mtx);					\
	return (ret);							\
}

/*
 * ctl_mtx is not acquired, under the assumption that no pertinent data will
 * mutate during the call.
 */
#define	CTL_RO_NL_GEN(n, v, t)					\
static int								\
n##_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,	\
    void *newp, size_t newlen)						\
{									\
	int ret;							\
	t oldval;							\
									\
	READONLY();							\
	oldval = v;							\
	READ(oldval, t);						\
									\
	ret = 0;							\
RETURN:									\
	return (ret);							\
}

#define	CTL_RO_TRUE_GEN(n)						\
static int								\
n##_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,	\
    void *newp, size_t newlen)						\
{									\
	int ret;							\
	bool oldval;							\
									\
	READONLY();							\
	oldval = true;							\
	READ(oldval, bool);						\
									\
	ret = 0;							\
RETURN:									\
	return (ret);							\
}

#define	CTL_RO_FALSE_GEN(n)						\
static int								\
n##_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,	\
    void *newp, size_t newlen)						\
{									\
	int ret;							\
	bool oldval;							\
									\
	READONLY();							\
	oldval = false;							\
	READ(oldval, bool);						\
									\
	ret = 0;							\
RETURN:									\
	return (ret);							\
}

CTL_RO_NL_GEN(version, JEMALLOC_VERSION, const char *)

static int
epoch_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;
	uint64_t newval;

	malloc_mutex_lock(&ctl_mtx);
	newval = 0;
	WRITE(newval, uint64_t);
	if (newval != 0)
		ctl_refresh();
	READ(ctl_epoch, uint64_t);

	ret = 0;
RETURN:
	malloc_mutex_unlock(&ctl_mtx);
	return (ret);
}

#ifdef JEMALLOC_TCACHE
static int
tcache_flush_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;
	tcache_t *tcache;

	VOID();

	tcache = TCACHE_GET();
	if (tcache == NULL) {
		ret = 0;
		goto RETURN;
	}
	tcache_destroy(tcache);
	TCACHE_SET(NULL);

	ret = 0;
RETURN:
	return (ret);
}
#endif

static int
thread_arena_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;
	unsigned newind, oldind;

	newind = oldind = choose_arena()->ind;
	WRITE(newind, unsigned);
	READ(oldind, unsigned);
	if (newind != oldind) {
		arena_t *arena;

		if (newind >= narenas) {
			/* New arena index is out of range. */
			ret = EFAULT;
			goto RETURN;
		}

		/* Initialize arena if necessary. */
		malloc_mutex_lock(&arenas_lock);
		if ((arena = arenas[newind]) == NULL)
			arena = arenas_extend(newind);
		arenas[oldind]->nthreads--;
		arenas[newind]->nthreads++;
		malloc_mutex_unlock(&arenas_lock);
		if (arena == NULL) {
			ret = EAGAIN;
			goto RETURN;
		}

		/* Set new arena association. */
		ARENA_SET(arena);
		{
			tcache_t *tcache = TCACHE_GET();
			if (tcache != NULL)
				tcache->arena = arena;
		}
	}

	ret = 0;
RETURN:
	return (ret);
}

#ifdef JEMALLOC_STATS
CTL_RO_NL_GEN(thread_allocated, ALLOCATED_GET(), uint64_t);
CTL_RO_NL_GEN(thread_allocatedp, ALLOCATEDP_GET(), uint64_t *);
CTL_RO_NL_GEN(thread_deallocated, DEALLOCATED_GET(), uint64_t);
CTL_RO_NL_GEN(thread_deallocatedp, DEALLOCATEDP_GET(), uint64_t *);
#endif

/******************************************************************************/

#ifdef JEMALLOC_DEBUG
CTL_RO_TRUE_GEN(config_debug)
#else
CTL_RO_FALSE_GEN(config_debug)
#endif

#ifdef JEMALLOC_DSS
CTL_RO_TRUE_GEN(config_dss)
#else
CTL_RO_FALSE_GEN(config_dss)
#endif

#ifdef JEMALLOC_DYNAMIC_PAGE_SHIFT
CTL_RO_TRUE_GEN(config_dynamic_page_shift)
#else
CTL_RO_FALSE_GEN(config_dynamic_page_shift)
#endif

#ifdef JEMALLOC_FILL
CTL_RO_TRUE_GEN(config_fill)
#else
CTL_RO_FALSE_GEN(config_fill)
#endif

#ifdef JEMALLOC_LAZY_LOCK
CTL_RO_TRUE_GEN(config_lazy_lock)
#else
CTL_RO_FALSE_GEN(config_lazy_lock)
#endif

#ifdef JEMALLOC_PROF
CTL_RO_TRUE_GEN(config_prof)
#else
CTL_RO_FALSE_GEN(config_prof)
#endif

#ifdef JEMALLOC_PROF_LIBGCC
CTL_RO_TRUE_GEN(config_prof_libgcc)
#else
CTL_RO_FALSE_GEN(config_prof_libgcc)
#endif

#ifdef JEMALLOC_PROF_LIBUNWIND
CTL_RO_TRUE_GEN(config_prof_libunwind)
#else
CTL_RO_FALSE_GEN(config_prof_libunwind)
#endif

#ifdef JEMALLOC_STATS
CTL_RO_TRUE_GEN(config_stats)
#else
CTL_RO_FALSE_GEN(config_stats)
#endif

#ifdef JEMALLOC_SWAP
CTL_RO_TRUE_GEN(config_swap)
#else
CTL_RO_FALSE_GEN(config_swap)
#endif

#ifdef JEMALLOC_SYSV
CTL_RO_TRUE_GEN(config_sysv)
#else
CTL_RO_FALSE_GEN(config_sysv)
#endif

#ifdef JEMALLOC_TCACHE
CTL_RO_TRUE_GEN(config_tcache)
#else
CTL_RO_FALSE_GEN(config_tcache)
#endif

#ifdef JEMALLOC_TINY
CTL_RO_TRUE_GEN(config_tiny)
#else
CTL_RO_FALSE_GEN(config_tiny)
#endif

#ifdef JEMALLOC_TLS
CTL_RO_TRUE_GEN(config_tls)
#else
CTL_RO_FALSE_GEN(config_tls)
#endif

#ifdef JEMALLOC_XMALLOC
CTL_RO_TRUE_GEN(config_xmalloc)
#else
CTL_RO_FALSE_GEN(config_xmalloc)
#endif

/******************************************************************************/

CTL_RO_NL_GEN(opt_abort, opt_abort, bool)
CTL_RO_NL_GEN(opt_lg_qspace_max, opt_lg_qspace_max, size_t)
CTL_RO_NL_GEN(opt_lg_cspace_max, opt_lg_cspace_max, size_t)
CTL_RO_NL_GEN(opt_lg_chunk, opt_lg_chunk, size_t)
CTL_RO_NL_GEN(opt_narenas, opt_narenas, size_t)
CTL_RO_NL_GEN(opt_lg_dirty_mult, opt_lg_dirty_mult, ssize_t)
CTL_RO_NL_GEN(opt_stats_print, opt_stats_print, bool)
#ifdef JEMALLOC_FILL
CTL_RO_NL_GEN(opt_junk, opt_junk, bool)
CTL_RO_NL_GEN(opt_zero, opt_zero, bool)
#endif
#ifdef JEMALLOC_SYSV
CTL_RO_NL_GEN(opt_sysv, opt_sysv, bool)
#endif
#ifdef JEMALLOC_XMALLOC
CTL_RO_NL_GEN(opt_xmalloc, opt_xmalloc, bool)
#endif
#ifdef JEMALLOC_TCACHE
CTL_RO_NL_GEN(opt_tcache, opt_tcache, bool)
CTL_RO_NL_GEN(opt_lg_tcache_gc_sweep, opt_lg_tcache_gc_sweep, ssize_t)
#endif
#ifdef JEMALLOC_PROF
CTL_RO_NL_GEN(opt_prof, opt_prof, bool)
CTL_RO_NL_GEN(opt_prof_prefix, opt_prof_prefix, const char *)
CTL_RO_GEN(opt_prof_active, opt_prof_active, bool) /* Mutable. */
CTL_RO_NL_GEN(opt_lg_prof_bt_max, opt_lg_prof_bt_max, size_t)
CTL_RO_NL_GEN(opt_lg_prof_sample, opt_lg_prof_sample, size_t)
CTL_RO_NL_GEN(opt_lg_prof_interval, opt_lg_prof_interval, ssize_t)
CTL_RO_NL_GEN(opt_prof_gdump, opt_prof_gdump, bool)
CTL_RO_NL_GEN(opt_prof_leak, opt_prof_leak, bool)
CTL_RO_NL_GEN(opt_prof_accum, opt_prof_accum, bool)
CTL_RO_NL_GEN(opt_lg_prof_tcmax, opt_lg_prof_tcmax, ssize_t)
#endif
#ifdef JEMALLOC_SWAP
CTL_RO_NL_GEN(opt_overcommit, opt_overcommit, bool)
#endif

/******************************************************************************/

CTL_RO_NL_GEN(arenas_bin_i_size, arena_bin_info[mib[2]].reg_size, size_t)
CTL_RO_NL_GEN(arenas_bin_i_nregs, arena_bin_info[mib[2]].nregs, uint32_t)
CTL_RO_NL_GEN(arenas_bin_i_run_size, arena_bin_info[mib[2]].run_size, size_t)
const ctl_node_t *
arenas_bin_i_index(const size_t *mib, size_t miblen, size_t i)
{

	if (i > nbins)
		return (NULL);
	return (super_arenas_bin_i_node);
}

CTL_RO_NL_GEN(arenas_lrun_i_size, ((mib[2]+1) << PAGE_SHIFT), size_t)
const ctl_node_t *
arenas_lrun_i_index(const size_t *mib, size_t miblen, size_t i)
{

	if (i > nlclasses)
		return (NULL);
	return (super_arenas_lrun_i_node);
}

CTL_RO_NL_GEN(arenas_narenas, narenas, unsigned)

static int
arenas_initialized_ctl(const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen)
{
	int ret;
	unsigned nread, i;

	malloc_mutex_lock(&ctl_mtx);
	READONLY();
	if (*oldlenp != narenas * sizeof(bool)) {
		ret = EINVAL;
		nread = (*oldlenp < narenas * sizeof(bool))
		    ? (*oldlenp / sizeof(bool)) : narenas;
	} else {
		ret = 0;
		nread = narenas;
	}

	for (i = 0; i < nread; i++)
		((bool *)oldp)[i] = ctl_stats.arenas[i].initialized;

RETURN:
	malloc_mutex_unlock(&ctl_mtx);
	return (ret);
}

CTL_RO_NL_GEN(arenas_quantum, QUANTUM, size_t)
CTL_RO_NL_GEN(arenas_cacheline, CACHELINE, size_t)
CTL_RO_NL_GEN(arenas_subpage, SUBPAGE, size_t)
CTL_RO_NL_GEN(arenas_pagesize, PAGE_SIZE, size_t)
CTL_RO_NL_GEN(arenas_chunksize, chunksize, size_t)
#ifdef JEMALLOC_TINY
CTL_RO_NL_GEN(arenas_tspace_min, (1U << LG_TINY_MIN), size_t)
CTL_RO_NL_GEN(arenas_tspace_max, (qspace_min >> 1), size_t)
#endif
CTL_RO_NL_GEN(arenas_qspace_min, qspace_min, size_t)
CTL_RO_NL_GEN(arenas_qspace_max, qspace_max, size_t)
CTL_RO_NL_GEN(arenas_cspace_min, cspace_min, size_t)
CTL_RO_NL_GEN(arenas_cspace_max, cspace_max, size_t)
CTL_RO_NL_GEN(arenas_sspace_min, sspace_min, size_t)
CTL_RO_NL_GEN(arenas_sspace_max, sspace_max, size_t)
#ifdef JEMALLOC_TCACHE
CTL_RO_NL_GEN(arenas_tcache_max, tcache_maxclass, size_t)
#endif
CTL_RO_NL_GEN(arenas_ntbins, ntbins, unsigned)
CTL_RO_NL_GEN(arenas_nqbins, nqbins, unsigned)
CTL_RO_NL_GEN(arenas_ncbins, ncbins, unsigned)
CTL_RO_NL_GEN(arenas_nsbins, nsbins, unsigned)
CTL_RO_NL_GEN(arenas_nbins, nbins, unsigned)
#ifdef JEMALLOC_TCACHE
CTL_RO_NL_GEN(arenas_nhbins, nhbins, unsigned)
#endif
CTL_RO_NL_GEN(arenas_nlruns, nlclasses, size_t)

static int
arenas_purge_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;
	unsigned arena;

	WRITEONLY();
	arena = UINT_MAX;
	WRITE(arena, unsigned);
	if (newp != NULL && arena >= narenas) {
		ret = EFAULT;
		goto RETURN;
	} else {
		arena_t *tarenas[narenas];

		malloc_mutex_lock(&arenas_lock);
		memcpy(tarenas, arenas, sizeof(arena_t *) * narenas);
		malloc_mutex_unlock(&arenas_lock);

		if (arena == UINT_MAX) {
			unsigned i;
			for (i = 0; i < narenas; i++) {
				if (tarenas[i] != NULL)
					arena_purge_all(tarenas[i]);
			}
		} else {
			assert(arena < narenas);
			if (tarenas[arena] != NULL)
				arena_purge_all(tarenas[arena]);
		}
	}

	ret = 0;
RETURN:
	return (ret);
}

/******************************************************************************/

#ifdef JEMALLOC_PROF
static int
prof_active_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;
	bool oldval;

	malloc_mutex_lock(&ctl_mtx); /* Protect opt_prof_active. */
	oldval = opt_prof_active;
	if (newp != NULL) {
		/*
		 * The memory barriers will tend to make opt_prof_active
		 * propagate faster on systems with weak memory ordering.
		 */
		mb_write();
		WRITE(opt_prof_active, bool);
		mb_write();
	}
	READ(oldval, bool);

	ret = 0;
RETURN:
	malloc_mutex_unlock(&ctl_mtx);
	return (ret);
}

static int
prof_dump_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;
	const char *filename = NULL;

	WRITEONLY();
	WRITE(filename, const char *);

	if (prof_mdump(filename)) {
		ret = EFAULT;
		goto RETURN;
	}

	ret = 0;
RETURN:
	return (ret);
}

CTL_RO_NL_GEN(prof_interval, prof_interval, uint64_t)
#endif

/******************************************************************************/

#ifdef JEMALLOC_STATS
CTL_RO_GEN(stats_chunks_current, ctl_stats.chunks.current, size_t)
CTL_RO_GEN(stats_chunks_total, ctl_stats.chunks.total, uint64_t)
CTL_RO_GEN(stats_chunks_high, ctl_stats.chunks.high, size_t)
CTL_RO_GEN(stats_huge_allocated, huge_allocated, size_t)
CTL_RO_GEN(stats_huge_nmalloc, huge_nmalloc, uint64_t)
CTL_RO_GEN(stats_huge_ndalloc, huge_ndalloc, uint64_t)
CTL_RO_GEN(stats_arenas_i_small_allocated,
    ctl_stats.arenas[mib[2]].allocated_small, size_t)
CTL_RO_GEN(stats_arenas_i_small_nmalloc,
    ctl_stats.arenas[mib[2]].nmalloc_small, uint64_t)
CTL_RO_GEN(stats_arenas_i_small_ndalloc,
    ctl_stats.arenas[mib[2]].ndalloc_small, uint64_t)
CTL_RO_GEN(stats_arenas_i_small_nrequests,
    ctl_stats.arenas[mib[2]].nrequests_small, uint64_t)
CTL_RO_GEN(stats_arenas_i_large_allocated,
    ctl_stats.arenas[mib[2]].astats.allocated_large, size_t)
CTL_RO_GEN(stats_arenas_i_large_nmalloc,
    ctl_stats.arenas[mib[2]].astats.nmalloc_large, uint64_t)
CTL_RO_GEN(stats_arenas_i_large_ndalloc,
    ctl_stats.arenas[mib[2]].astats.ndalloc_large, uint64_t)
CTL_RO_GEN(stats_arenas_i_large_nrequests,
    ctl_stats.arenas[mib[2]].astats.nrequests_large, uint64_t)

CTL_RO_GEN(stats_arenas_i_bins_j_allocated,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].allocated, size_t)
CTL_RO_GEN(stats_arenas_i_bins_j_nmalloc,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].nmalloc, uint64_t)
CTL_RO_GEN(stats_arenas_i_bins_j_ndalloc,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].ndalloc, uint64_t)
CTL_RO_GEN(stats_arenas_i_bins_j_nrequests,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].nrequests, uint64_t)
#ifdef JEMALLOC_TCACHE
CTL_RO_GEN(stats_arenas_i_bins_j_nfills,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].nfills, uint64_t)
CTL_RO_GEN(stats_arenas_i_bins_j_nflushes,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].nflushes, uint64_t)
#endif
CTL_RO_GEN(stats_arenas_i_bins_j_nruns,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].nruns, uint64_t)
CTL_RO_GEN(stats_arenas_i_bins_j_nreruns,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].reruns, uint64_t)
CTL_RO_GEN(stats_arenas_i_bins_j_highruns,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].highruns, size_t)
CTL_RO_GEN(stats_arenas_i_bins_j_curruns,
    ctl_stats.arenas[mib[2]].bstats[mib[4]].curruns, size_t)

const ctl_node_t *
stats_arenas_i_bins_j_index(const size_t *mib, size_t miblen, size_t j)
{

	if (j > nbins)
		return (NULL);
	return (super_stats_arenas_i_bins_j_node);
}

CTL_RO_GEN(stats_arenas_i_lruns_j_nmalloc,
    ctl_stats.arenas[mib[2]].lstats[mib[4]].nmalloc, uint64_t)
CTL_RO_GEN(stats_arenas_i_lruns_j_ndalloc,
    ctl_stats.arenas[mib[2]].lstats[mib[4]].ndalloc, uint64_t)
CTL_RO_GEN(stats_arenas_i_lruns_j_nrequests,
    ctl_stats.arenas[mib[2]].lstats[mib[4]].nrequests, uint64_t)
CTL_RO_GEN(stats_arenas_i_lruns_j_curruns,
    ctl_stats.arenas[mib[2]].lstats[mib[4]].curruns, size_t)
CTL_RO_GEN(stats_arenas_i_lruns_j_highruns,
    ctl_stats.arenas[mib[2]].lstats[mib[4]].highruns, size_t)

const ctl_node_t *
stats_arenas_i_lruns_j_index(const size_t *mib, size_t miblen, size_t j)
{

	if (j > nlclasses)
		return (NULL);
	return (super_stats_arenas_i_lruns_j_node);
}

#endif
CTL_RO_GEN(stats_arenas_i_nthreads, ctl_stats.arenas[mib[2]].nthreads, unsigned)
CTL_RO_GEN(stats_arenas_i_pactive, ctl_stats.arenas[mib[2]].pactive, size_t)
CTL_RO_GEN(stats_arenas_i_pdirty, ctl_stats.arenas[mib[2]].pdirty, size_t)
#ifdef JEMALLOC_STATS
CTL_RO_GEN(stats_arenas_i_mapped, ctl_stats.arenas[mib[2]].astats.mapped,
    size_t)
CTL_RO_GEN(stats_arenas_i_npurge, ctl_stats.arenas[mib[2]].astats.npurge,
    uint64_t)
CTL_RO_GEN(stats_arenas_i_nmadvise, ctl_stats.arenas[mib[2]].astats.nmadvise,
    uint64_t)
CTL_RO_GEN(stats_arenas_i_purged, ctl_stats.arenas[mib[2]].astats.purged,
    uint64_t)
#endif

const ctl_node_t *
stats_arenas_i_index(const size_t *mib, size_t miblen, size_t i)
{
	const ctl_node_t * ret;

	malloc_mutex_lock(&ctl_mtx);
	if (ctl_stats.arenas[i].initialized == false) {
		ret = NULL;
		goto RETURN;
	}

	ret = super_stats_arenas_i_node;
RETURN:
	malloc_mutex_unlock(&ctl_mtx);
	return (ret);
}

#ifdef JEMALLOC_STATS
CTL_RO_GEN(stats_cactive, &stats_cactive, size_t *)
CTL_RO_GEN(stats_allocated, ctl_stats.allocated, size_t)
CTL_RO_GEN(stats_active, ctl_stats.active, size_t)
CTL_RO_GEN(stats_mapped, ctl_stats.mapped, size_t)
#endif

/******************************************************************************/

#ifdef JEMALLOC_SWAP
#  ifdef JEMALLOC_STATS
CTL_RO_GEN(swap_avail, ctl_stats.swap_avail, size_t)
#  endif

static int
swap_prezeroed_ctl(const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen)
{
	int ret;

	malloc_mutex_lock(&ctl_mtx);
	if (swap_enabled) {
		READONLY();
	} else {
		/*
		 * swap_prezeroed isn't actually used by the swap code until it
		 * is set during a successful chunk_swap_enabled() call.  We
		 * use it here to store the value that we'll pass to
		 * chunk_swap_enable() in a swap.fds mallctl().  This is not
		 * very clean, but the obvious alternatives are even worse.
		 */
		WRITE(swap_prezeroed, bool);
	}

	READ(swap_prezeroed, bool);

	ret = 0;
RETURN:
	malloc_mutex_unlock(&ctl_mtx);
	return (ret);
}

CTL_RO_GEN(swap_nfds, swap_nfds, size_t)

static int
swap_fds_ctl(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen)
{
	int ret;

	malloc_mutex_lock(&ctl_mtx);
	if (swap_enabled) {
		READONLY();
	} else if (newp != NULL) {
		size_t nfds = newlen / sizeof(int);

		{
			int fds[nfds];

			memcpy(fds, newp, nfds * sizeof(int));
			if (chunk_swap_enable(fds, nfds, swap_prezeroed)) {
				ret = EFAULT;
				goto RETURN;
			}
		}
	}

	if (oldp != NULL && oldlenp != NULL) {
		if (*oldlenp != swap_nfds * sizeof(int)) {
			size_t copylen = (swap_nfds * sizeof(int) <= *oldlenp)
			    ? swap_nfds * sizeof(int) : *oldlenp;

			memcpy(oldp, swap_fds, copylen);
			ret = EINVAL;
			goto RETURN;
		} else
			memcpy(oldp, swap_fds, *oldlenp);
	}

	ret = 0;
RETURN:
	malloc_mutex_unlock(&ctl_mtx);
	return (ret);
}
#endif
