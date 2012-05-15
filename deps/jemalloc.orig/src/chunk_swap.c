#define	JEMALLOC_CHUNK_SWAP_C_
#include "jemalloc/internal/jemalloc_internal.h"
#ifdef JEMALLOC_SWAP
/******************************************************************************/
/* Data. */

malloc_mutex_t	swap_mtx;
bool		swap_enabled;
bool		swap_prezeroed;
size_t		swap_nfds;
int		*swap_fds;
#ifdef JEMALLOC_STATS
size_t		swap_avail;
#endif

/* Base address of the mmap()ed file(s). */
static void	*swap_base;
/* Current end of the space in use (<= swap_max). */
static void	*swap_end;
/* Absolute upper limit on file-backed addresses. */
static void	*swap_max;

/*
 * Trees of chunks that were previously allocated (trees differ only in node
 * ordering).  These are used when allocating chunks, in an attempt to re-use
 * address space.  Depending on function, different tree orderings are needed,
 * which is why there are two trees with the same contents.
 */
static extent_tree_t	swap_chunks_szad;
static extent_tree_t	swap_chunks_ad;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	*chunk_recycle_swap(size_t size, bool *zero);
static extent_node_t *chunk_dealloc_swap_record(void *chunk, size_t size);

/******************************************************************************/

static void *
chunk_recycle_swap(size_t size, bool *zero)
{
	extent_node_t *node, key;

	key.addr = NULL;
	key.size = size;
	malloc_mutex_lock(&swap_mtx);
	node = extent_tree_szad_nsearch(&swap_chunks_szad, &key);
	if (node != NULL) {
		void *ret = node->addr;

		/* Remove node from the tree. */
		extent_tree_szad_remove(&swap_chunks_szad, node);
		if (node->size == size) {
			extent_tree_ad_remove(&swap_chunks_ad, node);
			base_node_dealloc(node);
		} else {
			/*
			 * Insert the remainder of node's address range as a
			 * smaller chunk.  Its position within swap_chunks_ad
			 * does not change.
			 */
			assert(node->size > size);
			node->addr = (void *)((uintptr_t)node->addr + size);
			node->size -= size;
			extent_tree_szad_insert(&swap_chunks_szad, node);
		}
#ifdef JEMALLOC_STATS
		swap_avail -= size;
#endif
		malloc_mutex_unlock(&swap_mtx);

		if (*zero)
			memset(ret, 0, size);
		return (ret);
	}
	malloc_mutex_unlock(&swap_mtx);

	return (NULL);
}

void *
chunk_alloc_swap(size_t size, bool *zero)
{
	void *ret;

	assert(swap_enabled);

	ret = chunk_recycle_swap(size, zero);
	if (ret != NULL)
		return (ret);

	malloc_mutex_lock(&swap_mtx);
	if ((uintptr_t)swap_end + size <= (uintptr_t)swap_max) {
		ret = swap_end;
		swap_end = (void *)((uintptr_t)swap_end + size);
#ifdef JEMALLOC_STATS
		swap_avail -= size;
#endif
		malloc_mutex_unlock(&swap_mtx);

		if (swap_prezeroed)
			*zero = true;
		else if (*zero)
			memset(ret, 0, size);
	} else {
		malloc_mutex_unlock(&swap_mtx);
		return (NULL);
	}

	return (ret);
}

static extent_node_t *
chunk_dealloc_swap_record(void *chunk, size_t size)
{
	extent_node_t *xnode, *node, *prev, key;

	xnode = NULL;
	while (true) {
		key.addr = (void *)((uintptr_t)chunk + size);
		node = extent_tree_ad_nsearch(&swap_chunks_ad, &key);
		/* Try to coalesce forward. */
		if (node != NULL && node->addr == key.addr) {
			/*
			 * Coalesce chunk with the following address range.
			 * This does not change the position within
			 * swap_chunks_ad, so only remove/insert from/into
			 * swap_chunks_szad.
			 */
			extent_tree_szad_remove(&swap_chunks_szad, node);
			node->addr = chunk;
			node->size += size;
			extent_tree_szad_insert(&swap_chunks_szad, node);
			break;
		} else if (xnode == NULL) {
			/*
			 * It is possible that base_node_alloc() will cause a
			 * new base chunk to be allocated, so take care not to
			 * deadlock on swap_mtx, and recover if another thread
			 * deallocates an adjacent chunk while this one is busy
			 * allocating xnode.
			 */
			malloc_mutex_unlock(&swap_mtx);
			xnode = base_node_alloc();
			malloc_mutex_lock(&swap_mtx);
			if (xnode == NULL)
				return (NULL);
		} else {
			/* Coalescing forward failed, so insert a new node. */
			node = xnode;
			xnode = NULL;
			node->addr = chunk;
			node->size = size;
			extent_tree_ad_insert(&swap_chunks_ad, node);
			extent_tree_szad_insert(&swap_chunks_szad, node);
			break;
		}
	}
	/* Discard xnode if it ended up unused do to a race. */
	if (xnode != NULL)
		base_node_dealloc(xnode);

	/* Try to coalesce backward. */
	prev = extent_tree_ad_prev(&swap_chunks_ad, node);
	if (prev != NULL && (void *)((uintptr_t)prev->addr + prev->size) ==
	    chunk) {
		/*
		 * Coalesce chunk with the previous address range.  This does
		 * not change the position within swap_chunks_ad, so only
		 * remove/insert node from/into swap_chunks_szad.
		 */
		extent_tree_szad_remove(&swap_chunks_szad, prev);
		extent_tree_ad_remove(&swap_chunks_ad, prev);

		extent_tree_szad_remove(&swap_chunks_szad, node);
		node->addr = prev->addr;
		node->size += prev->size;
		extent_tree_szad_insert(&swap_chunks_szad, node);

		base_node_dealloc(prev);
	}

	return (node);
}

bool
chunk_in_swap(void *chunk)
{
	bool ret;

	assert(swap_enabled);

	malloc_mutex_lock(&swap_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)swap_base
	    && (uintptr_t)chunk < (uintptr_t)swap_max)
		ret = true;
	else
		ret = false;
	malloc_mutex_unlock(&swap_mtx);

	return (ret);
}

bool
chunk_dealloc_swap(void *chunk, size_t size)
{
	bool ret;

	assert(swap_enabled);

	malloc_mutex_lock(&swap_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)swap_base
	    && (uintptr_t)chunk < (uintptr_t)swap_max) {
		extent_node_t *node;

		/* Try to coalesce with other unused chunks. */
		node = chunk_dealloc_swap_record(chunk, size);
		if (node != NULL) {
			chunk = node->addr;
			size = node->size;
		}

		/*
		 * Try to shrink the in-use memory if this chunk is at the end
		 * of the in-use memory.
		 */
		if ((void *)((uintptr_t)chunk + size) == swap_end) {
			swap_end = (void *)((uintptr_t)swap_end - size);

			if (node != NULL) {
				extent_tree_szad_remove(&swap_chunks_szad,
				    node);
				extent_tree_ad_remove(&swap_chunks_ad, node);
				base_node_dealloc(node);
			}
		} else
			madvise(chunk, size, MADV_DONTNEED);

#ifdef JEMALLOC_STATS
		swap_avail += size;
#endif
		ret = false;
		goto RETURN;
	}

	ret = true;
RETURN:
	malloc_mutex_unlock(&swap_mtx);
	return (ret);
}

bool
chunk_swap_enable(const int *fds, unsigned nfds, bool prezeroed)
{
	bool ret;
	unsigned i;
	off_t off;
	void *vaddr;
	size_t cumsize, voff;
	size_t sizes[nfds];

	malloc_mutex_lock(&swap_mtx);

	/* Get file sizes. */
	for (i = 0, cumsize = 0; i < nfds; i++) {
		off = lseek(fds[i], 0, SEEK_END);
		if (off == ((off_t)-1)) {
			ret = true;
			goto RETURN;
		}
		if (PAGE_CEILING(off) != off) {
			/* Truncate to a multiple of the page size. */
			off &= ~PAGE_MASK;
			if (ftruncate(fds[i], off) != 0) {
				ret = true;
				goto RETURN;
			}
		}
		sizes[i] = off;
		if (cumsize + off < cumsize) {
			/*
			 * Cumulative file size is greater than the total
			 * address space.  Bail out while it's still obvious
			 * what the problem is.
			 */
			ret = true;
			goto RETURN;
		}
		cumsize += off;
	}

	/* Round down to a multiple of the chunk size. */
	cumsize &= ~chunksize_mask;
	if (cumsize == 0) {
		ret = true;
		goto RETURN;
	}

	/*
	 * Allocate a chunk-aligned region of anonymous memory, which will
	 * be the final location for the memory-mapped files.
	 */
	vaddr = chunk_alloc_mmap_noreserve(cumsize);
	if (vaddr == NULL) {
		ret = true;
		goto RETURN;
	}

	/* Overlay the files onto the anonymous mapping. */
	for (i = 0, voff = 0; i < nfds; i++) {
		void *addr = mmap((void *)((uintptr_t)vaddr + voff), sizes[i],
		    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[i], 0);
		if (addr == MAP_FAILED) {
			char buf[BUFERROR_BUF];


			buferror(errno, buf, sizeof(buf));
			malloc_write(
			    "<jemalloc>: Error in mmap(..., MAP_FIXED, ...): ");
			malloc_write(buf);
			malloc_write("\n");
			if (opt_abort)
				abort();
			if (munmap(vaddr, voff) == -1) {
				buferror(errno, buf, sizeof(buf));
				malloc_write("<jemalloc>: Error in munmap(): ");
				malloc_write(buf);
				malloc_write("\n");
			}
			ret = true;
			goto RETURN;
		}
		assert(addr == (void *)((uintptr_t)vaddr + voff));

		/*
		 * Tell the kernel that the mapping will be accessed randomly,
		 * and that it should not gratuitously sync pages to the
		 * filesystem.
		 */
#ifdef MADV_RANDOM
		madvise(addr, sizes[i], MADV_RANDOM);
#endif
#ifdef MADV_NOSYNC
		madvise(addr, sizes[i], MADV_NOSYNC);
#endif

		voff += sizes[i];
	}

	swap_prezeroed = prezeroed;
	swap_base = vaddr;
	swap_end = swap_base;
	swap_max = (void *)((uintptr_t)vaddr + cumsize);

	/* Copy the fds array for mallctl purposes. */
	swap_fds = (int *)base_alloc(nfds * sizeof(int));
	if (swap_fds == NULL) {
		ret = true;
		goto RETURN;
	}
	memcpy(swap_fds, fds, nfds * sizeof(int));
	swap_nfds = nfds;

#ifdef JEMALLOC_STATS
	swap_avail = cumsize;
#endif

	swap_enabled = true;

	ret = false;
RETURN:
	malloc_mutex_unlock(&swap_mtx);
	return (ret);
}

bool
chunk_swap_boot(void)
{

	if (malloc_mutex_init(&swap_mtx))
		return (true);

	swap_enabled = false;
	swap_prezeroed = false; /* swap.* mallctl's depend on this. */
	swap_nfds = 0;
	swap_fds = NULL;
#ifdef JEMALLOC_STATS
	swap_avail = 0;
#endif
	swap_base = NULL;
	swap_end = NULL;
	swap_max = NULL;

	extent_tree_szad_new(&swap_chunks_szad);
	extent_tree_ad_new(&swap_chunks_ad);

	return (false);
}

/******************************************************************************/
#endif /* JEMALLOC_SWAP */
