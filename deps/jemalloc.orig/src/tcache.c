#define	JEMALLOC_TCACHE_C_
#include "jemalloc/internal/jemalloc_internal.h"
#ifdef JEMALLOC_TCACHE
/******************************************************************************/
/* Data. */

bool	opt_tcache = true;
ssize_t	opt_lg_tcache_max = LG_TCACHE_MAXCLASS_DEFAULT;
ssize_t	opt_lg_tcache_gc_sweep = LG_TCACHE_GC_SWEEP_DEFAULT;

tcache_bin_info_t	*tcache_bin_info;
static unsigned		stack_nelms; /* Total stack elms per tcache. */

/* Map of thread-specific caches. */
#ifndef NO_TLS
__thread tcache_t	*tcache_tls JEMALLOC_ATTR(tls_model("initial-exec"));
#endif

/*
 * Same contents as tcache, but initialized such that the TSD destructor is
 * called when a thread exits, so that the cache can be cleaned up.
 */
pthread_key_t		tcache_tsd;

size_t				nhbins;
size_t				tcache_maxclass;
unsigned			tcache_gc_incr;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	tcache_thread_cleanup(void *arg);

/******************************************************************************/

void *
tcache_alloc_small_hard(tcache_t *tcache, tcache_bin_t *tbin, size_t binind)
{
	void *ret;

	arena_tcache_fill_small(tcache->arena, tbin, binind
#ifdef JEMALLOC_PROF
	    , tcache->prof_accumbytes
#endif
	    );
#ifdef JEMALLOC_PROF
	tcache->prof_accumbytes = 0;
#endif
	ret = tcache_alloc_easy(tbin);

	return (ret);
}

void
tcache_bin_flush_small(tcache_bin_t *tbin, size_t binind, unsigned rem
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
    , tcache_t *tcache
#endif
    )
{
	void *ptr;
	unsigned i, nflush, ndeferred;
#ifdef JEMALLOC_STATS
	bool merged_stats = false;
#endif

	assert(binind < nbins);
	assert(rem <= tbin->ncached);

	for (nflush = tbin->ncached - rem; nflush > 0; nflush = ndeferred) {
		/* Lock the arena bin associated with the first object. */
		arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(
		    tbin->avail[0]);
		arena_t *arena = chunk->arena;
		arena_bin_t *bin = &arena->bins[binind];

#ifdef JEMALLOC_PROF
		if (arena == tcache->arena) {
			malloc_mutex_lock(&arena->lock);
			arena_prof_accum(arena, tcache->prof_accumbytes);
			malloc_mutex_unlock(&arena->lock);
			tcache->prof_accumbytes = 0;
		}
#endif

		malloc_mutex_lock(&bin->lock);
#ifdef JEMALLOC_STATS
		if (arena == tcache->arena) {
			assert(merged_stats == false);
			merged_stats = true;
			bin->stats.nflushes++;
			bin->stats.nrequests += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
		}
#endif
		ndeferred = 0;
		for (i = 0; i < nflush; i++) {
			ptr = tbin->avail[i];
			assert(ptr != NULL);
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
			if (chunk->arena == arena) {
				size_t pageind = ((uintptr_t)ptr -
				    (uintptr_t)chunk) >> PAGE_SHIFT;
				arena_chunk_map_t *mapelm =
				    &chunk->map[pageind-map_bias];
				arena_dalloc_bin(arena, chunk, ptr, mapelm);
			} else {
				/*
				 * This object was allocated via a different
				 * arena bin than the one that is currently
				 * locked.  Stash the object, so that it can be
				 * handled in a future pass.
				 */
				tbin->avail[ndeferred] = ptr;
				ndeferred++;
			}
		}
		malloc_mutex_unlock(&bin->lock);
	}
#ifdef JEMALLOC_STATS
	if (merged_stats == false) {
		/*
		 * The flush loop didn't happen to flush to this thread's
		 * arena, so the stats didn't get merged.  Manually do so now.
		 */
		arena_bin_t *bin = &tcache->arena->bins[binind];
		malloc_mutex_lock(&bin->lock);
		bin->stats.nflushes++;
		bin->stats.nrequests += tbin->tstats.nrequests;
		tbin->tstats.nrequests = 0;
		malloc_mutex_unlock(&bin->lock);
	}
#endif

	memmove(tbin->avail, &tbin->avail[tbin->ncached - rem],
	    rem * sizeof(void *));
	tbin->ncached = rem;
	if ((int)tbin->ncached < tbin->low_water)
		tbin->low_water = tbin->ncached;
}

void
tcache_bin_flush_large(tcache_bin_t *tbin, size_t binind, unsigned rem
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
    , tcache_t *tcache
#endif
    )
{
	void *ptr;
	unsigned i, nflush, ndeferred;
#ifdef JEMALLOC_STATS
	bool merged_stats = false;
#endif

	assert(binind < nhbins);
	assert(rem <= tbin->ncached);

	for (nflush = tbin->ncached - rem; nflush > 0; nflush = ndeferred) {
		/* Lock the arena associated with the first object. */
		arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(
		    tbin->avail[0]);
		arena_t *arena = chunk->arena;

		malloc_mutex_lock(&arena->lock);
#if (defined(JEMALLOC_PROF) || defined(JEMALLOC_STATS))
		if (arena == tcache->arena) {
#endif
#ifdef JEMALLOC_PROF
			arena_prof_accum(arena, tcache->prof_accumbytes);
			tcache->prof_accumbytes = 0;
#endif
#ifdef JEMALLOC_STATS
			merged_stats = true;
			arena->stats.nrequests_large += tbin->tstats.nrequests;
			arena->stats.lstats[binind - nbins].nrequests +=
			    tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
#endif
#if (defined(JEMALLOC_PROF) || defined(JEMALLOC_STATS))
		}
#endif
		ndeferred = 0;
		for (i = 0; i < nflush; i++) {
			ptr = tbin->avail[i];
			assert(ptr != NULL);
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
			if (chunk->arena == arena)
				arena_dalloc_large(arena, chunk, ptr);
			else {
				/*
				 * This object was allocated via a different
				 * arena than the one that is currently locked.
				 * Stash the object, so that it can be handled
				 * in a future pass.
				 */
				tbin->avail[ndeferred] = ptr;
				ndeferred++;
			}
		}
		malloc_mutex_unlock(&arena->lock);
	}
#ifdef JEMALLOC_STATS
	if (merged_stats == false) {
		/*
		 * The flush loop didn't happen to flush to this thread's
		 * arena, so the stats didn't get merged.  Manually do so now.
		 */
		arena_t *arena = tcache->arena;
		malloc_mutex_lock(&arena->lock);
		arena->stats.nrequests_large += tbin->tstats.nrequests;
		arena->stats.lstats[binind - nbins].nrequests +=
		    tbin->tstats.nrequests;
		tbin->tstats.nrequests = 0;
		malloc_mutex_unlock(&arena->lock);
	}
#endif

	memmove(tbin->avail, &tbin->avail[tbin->ncached - rem],
	    rem * sizeof(void *));
	tbin->ncached = rem;
	if ((int)tbin->ncached < tbin->low_water)
		tbin->low_water = tbin->ncached;
}

tcache_t *
tcache_create(arena_t *arena)
{
	tcache_t *tcache;
	size_t size, stack_offset;
	unsigned i;

	size = offsetof(tcache_t, tbins) + (sizeof(tcache_bin_t) * nhbins);
	/* Naturally align the pointer stacks. */
	size = PTR_CEILING(size);
	stack_offset = size;
	size += stack_nelms * sizeof(void *);
	/*
	 * Round up to the nearest multiple of the cacheline size, in order to
	 * avoid the possibility of false cacheline sharing.
	 *
	 * That this works relies on the same logic as in ipalloc(), but we
	 * cannot directly call ipalloc() here due to tcache bootstrapping
	 * issues.
	 */
	size = (size + CACHELINE_MASK) & (-CACHELINE);

	if (size <= small_maxclass)
		tcache = (tcache_t *)arena_malloc_small(arena, size, true);
	else if (size <= tcache_maxclass)
		tcache = (tcache_t *)arena_malloc_large(arena, size, true);
	else
		tcache = (tcache_t *)icalloc(size);

	if (tcache == NULL)
		return (NULL);

#ifdef JEMALLOC_STATS
	/* Link into list of extant tcaches. */
	malloc_mutex_lock(&arena->lock);
	ql_elm_new(tcache, link);
	ql_tail_insert(&arena->tcache_ql, tcache, link);
	malloc_mutex_unlock(&arena->lock);
#endif

	tcache->arena = arena;
	assert((TCACHE_NSLOTS_SMALL_MAX & 1U) == 0);
	for (i = 0; i < nhbins; i++) {
		tcache->tbins[i].lg_fill_div = 1;
		tcache->tbins[i].avail = (void **)((uintptr_t)tcache +
		    (uintptr_t)stack_offset);
		stack_offset += tcache_bin_info[i].ncached_max * sizeof(void *);
	}

	TCACHE_SET(tcache);

	return (tcache);
}

void
tcache_destroy(tcache_t *tcache)
{
	unsigned i;
	size_t tcache_size;

#ifdef JEMALLOC_STATS
	/* Unlink from list of extant tcaches. */
	malloc_mutex_lock(&tcache->arena->lock);
	ql_remove(&tcache->arena->tcache_ql, tcache, link);
	malloc_mutex_unlock(&tcache->arena->lock);
	tcache_stats_merge(tcache, tcache->arena);
#endif

	for (i = 0; i < nbins; i++) {
		tcache_bin_t *tbin = &tcache->tbins[i];
		tcache_bin_flush_small(tbin, i, 0
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
		    , tcache
#endif
		    );

#ifdef JEMALLOC_STATS
		if (tbin->tstats.nrequests != 0) {
			arena_t *arena = tcache->arena;
			arena_bin_t *bin = &arena->bins[i];
			malloc_mutex_lock(&bin->lock);
			bin->stats.nrequests += tbin->tstats.nrequests;
			malloc_mutex_unlock(&bin->lock);
		}
#endif
	}

	for (; i < nhbins; i++) {
		tcache_bin_t *tbin = &tcache->tbins[i];
		tcache_bin_flush_large(tbin, i, 0
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
		    , tcache
#endif
		    );

#ifdef JEMALLOC_STATS
		if (tbin->tstats.nrequests != 0) {
			arena_t *arena = tcache->arena;
			malloc_mutex_lock(&arena->lock);
			arena->stats.nrequests_large += tbin->tstats.nrequests;
			arena->stats.lstats[i - nbins].nrequests +=
			    tbin->tstats.nrequests;
			malloc_mutex_unlock(&arena->lock);
		}
#endif
	}

#ifdef JEMALLOC_PROF
	if (tcache->prof_accumbytes > 0) {
		malloc_mutex_lock(&tcache->arena->lock);
		arena_prof_accum(tcache->arena, tcache->prof_accumbytes);
		malloc_mutex_unlock(&tcache->arena->lock);
	}
#endif

	tcache_size = arena_salloc(tcache);
	if (tcache_size <= small_maxclass) {
		arena_chunk_t *chunk = CHUNK_ADDR2BASE(tcache);
		arena_t *arena = chunk->arena;
		size_t pageind = ((uintptr_t)tcache - (uintptr_t)chunk) >>
		    PAGE_SHIFT;
		arena_chunk_map_t *mapelm = &chunk->map[pageind-map_bias];
		arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
		    (uintptr_t)((pageind - (mapelm->bits >> PAGE_SHIFT)) <<
		    PAGE_SHIFT));
		arena_bin_t *bin = run->bin;

		malloc_mutex_lock(&bin->lock);
		arena_dalloc_bin(arena, chunk, tcache, mapelm);
		malloc_mutex_unlock(&bin->lock);
	} else if (tcache_size <= tcache_maxclass) {
		arena_chunk_t *chunk = CHUNK_ADDR2BASE(tcache);
		arena_t *arena = chunk->arena;

		malloc_mutex_lock(&arena->lock);
		arena_dalloc_large(arena, chunk, tcache);
		malloc_mutex_unlock(&arena->lock);
	} else
		idalloc(tcache);
}

static void
tcache_thread_cleanup(void *arg)
{
	tcache_t *tcache = (tcache_t *)arg;

	if (tcache == (void *)(uintptr_t)1) {
		/*
		 * The previous time this destructor was called, we set the key
		 * to 1 so that other destructors wouldn't cause re-creation of
		 * the tcache.  This time, do nothing, so that the destructor
		 * will not be called again.
		 */
	} else if (tcache == (void *)(uintptr_t)2) {
		/*
		 * Another destructor called an allocator function after this
		 * destructor was called.  Reset tcache to 1 in order to
		 * receive another callback.
		 */
		TCACHE_SET((uintptr_t)1);
	} else if (tcache != NULL) {
		assert(tcache != (void *)(uintptr_t)1);
		tcache_destroy(tcache);
		TCACHE_SET((uintptr_t)1);
	}
}

#ifdef JEMALLOC_STATS
void
tcache_stats_merge(tcache_t *tcache, arena_t *arena)
{
	unsigned i;

	/* Merge and reset tcache stats. */
	for (i = 0; i < nbins; i++) {
		arena_bin_t *bin = &arena->bins[i];
		tcache_bin_t *tbin = &tcache->tbins[i];
		malloc_mutex_lock(&bin->lock);
		bin->stats.nrequests += tbin->tstats.nrequests;
		malloc_mutex_unlock(&bin->lock);
		tbin->tstats.nrequests = 0;
	}

	for (; i < nhbins; i++) {
		malloc_large_stats_t *lstats = &arena->stats.lstats[i - nbins];
		tcache_bin_t *tbin = &tcache->tbins[i];
		arena->stats.nrequests_large += tbin->tstats.nrequests;
		lstats->nrequests += tbin->tstats.nrequests;
		tbin->tstats.nrequests = 0;
	}
}
#endif

bool
tcache_boot(void)
{

	if (opt_tcache) {
		unsigned i;

		/*
		 * If necessary, clamp opt_lg_tcache_max, now that
		 * small_maxclass and arena_maxclass are known.
		 */
		if (opt_lg_tcache_max < 0 || (1U <<
		    opt_lg_tcache_max) < small_maxclass)
			tcache_maxclass = small_maxclass;
		else if ((1U << opt_lg_tcache_max) > arena_maxclass)
			tcache_maxclass = arena_maxclass;
		else
			tcache_maxclass = (1U << opt_lg_tcache_max);

		nhbins = nbins + (tcache_maxclass >> PAGE_SHIFT);

		/* Initialize tcache_bin_info. */
		tcache_bin_info = (tcache_bin_info_t *)base_alloc(nhbins *
		    sizeof(tcache_bin_info_t));
		if (tcache_bin_info == NULL)
			return (true);
		stack_nelms = 0;
		for (i = 0; i < nbins; i++) {
			if ((arena_bin_info[i].nregs << 1) <=
			    TCACHE_NSLOTS_SMALL_MAX) {
				tcache_bin_info[i].ncached_max =
				    (arena_bin_info[i].nregs << 1);
			} else {
				tcache_bin_info[i].ncached_max =
				    TCACHE_NSLOTS_SMALL_MAX;
			}
			stack_nelms += tcache_bin_info[i].ncached_max;
		}
		for (; i < nhbins; i++) {
			tcache_bin_info[i].ncached_max = TCACHE_NSLOTS_LARGE;
			stack_nelms += tcache_bin_info[i].ncached_max;
		}

		/* Compute incremental GC event threshold. */
		if (opt_lg_tcache_gc_sweep >= 0) {
			tcache_gc_incr = ((1U << opt_lg_tcache_gc_sweep) /
			    nbins) + (((1U << opt_lg_tcache_gc_sweep) % nbins ==
			    0) ? 0 : 1);
		} else
			tcache_gc_incr = 0;

		if (pthread_key_create(&tcache_tsd, tcache_thread_cleanup) !=
		    0) {
			malloc_write(
			    "<jemalloc>: Error in pthread_key_create()\n");
			abort();
		}
	}

	return (false);
}
/******************************************************************************/
#endif /* JEMALLOC_TCACHE */
