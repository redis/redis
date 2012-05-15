#define	JEMALLOC_CHUNK_DSS_C_
#include "jemalloc/internal/jemalloc_internal.h"
#ifdef JEMALLOC_DSS
/******************************************************************************/
/* Data. */

malloc_mutex_t	dss_mtx;

/* Base address of the DSS. */
static void	*dss_base;
/* Current end of the DSS, or ((void *)-1) if the DSS is exhausted. */
static void	*dss_prev;
/* Current upper limit on DSS addresses. */
static void	*dss_max;

/*
 * Trees of chunks that were previously allocated (trees differ only in node
 * ordering).  These are used when allocating chunks, in an attempt to re-use
 * address space.  Depending on function, different tree orderings are needed,
 * which is why there are two trees with the same contents.
 */
static extent_tree_t	dss_chunks_szad;
static extent_tree_t	dss_chunks_ad;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	*chunk_recycle_dss(size_t size, bool *zero);
static extent_node_t *chunk_dealloc_dss_record(void *chunk, size_t size);

/******************************************************************************/

static void *
chunk_recycle_dss(size_t size, bool *zero)
{
	extent_node_t *node, key;

	key.addr = NULL;
	key.size = size;
	malloc_mutex_lock(&dss_mtx);
	node = extent_tree_szad_nsearch(&dss_chunks_szad, &key);
	if (node != NULL) {
		void *ret = node->addr;

		/* Remove node from the tree. */
		extent_tree_szad_remove(&dss_chunks_szad, node);
		if (node->size == size) {
			extent_tree_ad_remove(&dss_chunks_ad, node);
			base_node_dealloc(node);
		} else {
			/*
			 * Insert the remainder of node's address range as a
			 * smaller chunk.  Its position within dss_chunks_ad
			 * does not change.
			 */
			assert(node->size > size);
			node->addr = (void *)((uintptr_t)node->addr + size);
			node->size -= size;
			extent_tree_szad_insert(&dss_chunks_szad, node);
		}
		malloc_mutex_unlock(&dss_mtx);

		if (*zero)
			memset(ret, 0, size);
		return (ret);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}

void *
chunk_alloc_dss(size_t size, bool *zero)
{
	void *ret;

	ret = chunk_recycle_dss(size, zero);
	if (ret != NULL)
		return (ret);

	/*
	 * sbrk() uses a signed increment argument, so take care not to
	 * interpret a huge allocation request as a negative increment.
	 */
	if ((intptr_t)size < 0)
		return (NULL);

	malloc_mutex_lock(&dss_mtx);
	if (dss_prev != (void *)-1) {
		intptr_t incr;

		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		do {
			/* Get the current end of the DSS. */
			dss_max = sbrk(0);

			/*
			 * Calculate how much padding is necessary to
			 * chunk-align the end of the DSS.
			 */
			incr = (intptr_t)size
			    - (intptr_t)CHUNK_ADDR2OFFSET(dss_max);
			if (incr == (intptr_t)size)
				ret = dss_max;
			else {
				ret = (void *)((intptr_t)dss_max + incr);
				incr += size;
			}

			dss_prev = sbrk(incr);
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = (void *)((intptr_t)dss_prev + incr);
				malloc_mutex_unlock(&dss_mtx);
				*zero = true;
				return (ret);
			}
		} while (dss_prev != (void *)-1);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}

static extent_node_t *
chunk_dealloc_dss_record(void *chunk, size_t size)
{
	extent_node_t *xnode, *node, *prev, key;

	xnode = NULL;
	while (true) {
		key.addr = (void *)((uintptr_t)chunk + size);
		node = extent_tree_ad_nsearch(&dss_chunks_ad, &key);
		/* Try to coalesce forward. */
		if (node != NULL && node->addr == key.addr) {
			/*
			 * Coalesce chunk with the following address range.
			 * This does not change the position within
			 * dss_chunks_ad, so only remove/insert from/into
			 * dss_chunks_szad.
			 */
			extent_tree_szad_remove(&dss_chunks_szad, node);
			node->addr = chunk;
			node->size += size;
			extent_tree_szad_insert(&dss_chunks_szad, node);
			break;
		} else if (xnode == NULL) {
			/*
			 * It is possible that base_node_alloc() will cause a
			 * new base chunk to be allocated, so take care not to
			 * deadlock on dss_mtx, and recover if another thread
			 * deallocates an adjacent chunk while this one is busy
			 * allocating xnode.
			 */
			malloc_mutex_unlock(&dss_mtx);
			xnode = base_node_alloc();
			malloc_mutex_lock(&dss_mtx);
			if (xnode == NULL)
				return (NULL);
		} else {
			/* Coalescing forward failed, so insert a new node. */
			node = xnode;
			xnode = NULL;
			node->addr = chunk;
			node->size = size;
			extent_tree_ad_insert(&dss_chunks_ad, node);
			extent_tree_szad_insert(&dss_chunks_szad, node);
			break;
		}
	}
	/* Discard xnode if it ended up unused do to a race. */
	if (xnode != NULL)
		base_node_dealloc(xnode);

	/* Try to coalesce backward. */
	prev = extent_tree_ad_prev(&dss_chunks_ad, node);
	if (prev != NULL && (void *)((uintptr_t)prev->addr + prev->size) ==
	    chunk) {
		/*
		 * Coalesce chunk with the previous address range.  This does
		 * not change the position within dss_chunks_ad, so only
		 * remove/insert node from/into dss_chunks_szad.
		 */
		extent_tree_szad_remove(&dss_chunks_szad, prev);
		extent_tree_ad_remove(&dss_chunks_ad, prev);

		extent_tree_szad_remove(&dss_chunks_szad, node);
		node->addr = prev->addr;
		node->size += prev->size;
		extent_tree_szad_insert(&dss_chunks_szad, node);

		base_node_dealloc(prev);
	}

	return (node);
}

bool
chunk_in_dss(void *chunk)
{
	bool ret;

	malloc_mutex_lock(&dss_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)dss_base
	    && (uintptr_t)chunk < (uintptr_t)dss_max)
		ret = true;
	else
		ret = false;
	malloc_mutex_unlock(&dss_mtx);

	return (ret);
}

bool
chunk_dealloc_dss(void *chunk, size_t size)
{
	bool ret;

	malloc_mutex_lock(&dss_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)dss_base
	    && (uintptr_t)chunk < (uintptr_t)dss_max) {
		extent_node_t *node;

		/* Try to coalesce with other unused chunks. */
		node = chunk_dealloc_dss_record(chunk, size);
		if (node != NULL) {
			chunk = node->addr;
			size = node->size;
		}

		/* Get the current end of the DSS. */
		dss_max = sbrk(0);

		/*
		 * Try to shrink the DSS if this chunk is at the end of the
		 * DSS.  The sbrk() call here is subject to a race condition
		 * with threads that use brk(2) or sbrk(2) directly, but the
		 * alternative would be to leak memory for the sake of poorly
		 * designed multi-threaded programs.
		 */
		if ((void *)((uintptr_t)chunk + size) == dss_max
		    && (dss_prev = sbrk(-(intptr_t)size)) == dss_max) {
			/* Success. */
			dss_max = (void *)((intptr_t)dss_prev - (intptr_t)size);

			if (node != NULL) {
				extent_tree_szad_remove(&dss_chunks_szad, node);
				extent_tree_ad_remove(&dss_chunks_ad, node);
				base_node_dealloc(node);
			}
		} else
			madvise(chunk, size, MADV_DONTNEED);

		ret = false;
		goto RETURN;
	}

	ret = true;
RETURN:
	malloc_mutex_unlock(&dss_mtx);
	return (ret);
}

bool
chunk_dss_boot(void)
{

	if (malloc_mutex_init(&dss_mtx))
		return (true);
	dss_base = sbrk(0);
	dss_prev = dss_base;
	dss_max = dss_base;
	extent_tree_szad_new(&dss_chunks_szad);
	extent_tree_ad_new(&dss_chunks_ad);

	return (false);
}

/******************************************************************************/
#endif /* JEMALLOC_DSS */
