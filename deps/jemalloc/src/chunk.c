#define	JEMALLOC_CHUNK_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

size_t	opt_lg_chunk = LG_CHUNK_DEFAULT;
#ifdef JEMALLOC_SWAP
bool	opt_overcommit = true;
#endif

#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
malloc_mutex_t	chunks_mtx;
chunk_stats_t	stats_chunks;
#endif

#ifdef JEMALLOC_IVSALLOC
rtree_t		*chunks_rtree;
#endif

/* Various chunk-related settings. */
size_t		chunksize;
size_t		chunksize_mask; /* (chunksize - 1). */
size_t		chunk_npages;
size_t		map_bias;
size_t		arena_maxclass; /* Max size class for arenas. */

/******************************************************************************/

/*
 * If the caller specifies (*zero == false), it is still possible to receive
 * zeroed memory, in which case *zero is toggled to true.  arena_chunk_alloc()
 * takes advantage of this to avoid demanding zeroed chunks, but taking
 * advantage of them if they are returned.
 */
void *
chunk_alloc(size_t size, bool base, bool *zero)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);

#ifdef JEMALLOC_SWAP
	if (swap_enabled) {
		ret = chunk_alloc_swap(size, zero);
		if (ret != NULL)
			goto RETURN;
	}

	if (swap_enabled == false || opt_overcommit) {
#endif
#ifdef JEMALLOC_DSS
		ret = chunk_alloc_dss(size, zero);
		if (ret != NULL)
			goto RETURN;
#endif
		ret = chunk_alloc_mmap(size);
		if (ret != NULL) {
			*zero = true;
			goto RETURN;
		}
#ifdef JEMALLOC_SWAP
	}
#endif

	/* All strategies for allocation failed. */
	ret = NULL;
RETURN:
#ifdef JEMALLOC_IVSALLOC
	if (base == false && ret != NULL) {
		if (rtree_set(chunks_rtree, (uintptr_t)ret, ret)) {
			chunk_dealloc(ret, size);
			return (NULL);
		}
	}
#endif
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
	if (ret != NULL) {
#  ifdef JEMALLOC_PROF
		bool gdump;
#  endif
		malloc_mutex_lock(&chunks_mtx);
#  ifdef JEMALLOC_STATS
		stats_chunks.nchunks += (size / chunksize);
#  endif
		stats_chunks.curchunks += (size / chunksize);
		if (stats_chunks.curchunks > stats_chunks.highchunks) {
			stats_chunks.highchunks = stats_chunks.curchunks;
#  ifdef JEMALLOC_PROF
			gdump = true;
#  endif
		}
#  ifdef JEMALLOC_PROF
		else
			gdump = false;
#  endif
		malloc_mutex_unlock(&chunks_mtx);
#  ifdef JEMALLOC_PROF
		if (opt_prof && opt_prof_gdump && gdump)
			prof_gdump();
#  endif
	}
#endif

	assert(CHUNK_ADDR2BASE(ret) == ret);
	return (ret);
}

void
chunk_dealloc(void *chunk, size_t size)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

#ifdef JEMALLOC_IVSALLOC
	rtree_set(chunks_rtree, (uintptr_t)chunk, NULL);
#endif
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
	malloc_mutex_lock(&chunks_mtx);
	stats_chunks.curchunks -= (size / chunksize);
	malloc_mutex_unlock(&chunks_mtx);
#endif

#ifdef JEMALLOC_SWAP
	if (swap_enabled && chunk_dealloc_swap(chunk, size) == false)
		return;
#endif
#ifdef JEMALLOC_DSS
	if (chunk_dealloc_dss(chunk, size) == false)
		return;
#endif
	chunk_dealloc_mmap(chunk, size);
}

bool
chunk_boot(void)
{

	/* Set variables according to the value of opt_lg_chunk. */
	chunksize = (ZU(1) << opt_lg_chunk);
	assert(chunksize >= PAGE_SIZE);
	chunksize_mask = chunksize - 1;
	chunk_npages = (chunksize >> PAGE_SHIFT);

#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
	if (malloc_mutex_init(&chunks_mtx))
		return (true);
	memset(&stats_chunks, 0, sizeof(chunk_stats_t));
#endif
#ifdef JEMALLOC_SWAP
	if (chunk_swap_boot())
		return (true);
#endif
	if (chunk_mmap_boot())
		return (true);
#ifdef JEMALLOC_DSS
	if (chunk_dss_boot())
		return (true);
#endif
#ifdef JEMALLOC_IVSALLOC
	chunks_rtree = rtree_new((ZU(1) << (LG_SIZEOF_PTR+3)) - opt_lg_chunk);
	if (chunks_rtree == NULL)
		return (true);
#endif

	return (false);
}
