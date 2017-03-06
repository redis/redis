#define	JEMALLOC_BASE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

static malloc_mutex_t	base_mtx;
static extent_tree_t	base_avail_szad;
static extent_node_t	*base_nodes;
static size_t		base_allocated;
static size_t		base_resident;
static size_t		base_mapped;

/******************************************************************************/

/* base_mtx must be held. */
static extent_node_t *
base_node_try_alloc(void)
{
	extent_node_t *node;

	if (base_nodes == NULL)
		return (NULL);
	node = base_nodes;
	base_nodes = *(extent_node_t **)node;
	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(node, sizeof(extent_node_t));
	return (node);
}

/* base_mtx must be held. */
static void
base_node_dalloc(extent_node_t *node)
{

	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(node, sizeof(extent_node_t));
	*(extent_node_t **)node = base_nodes;
	base_nodes = node;
}

/* base_mtx must be held. */
static extent_node_t *
base_chunk_alloc(size_t minsize)
{
	extent_node_t *node;
	size_t csize, nsize;
	void *addr;

	assert(minsize != 0);
	node = base_node_try_alloc();
	/* Allocate enough space to also carve a node out if necessary. */
	nsize = (node == NULL) ? CACHELINE_CEILING(sizeof(extent_node_t)) : 0;
	csize = CHUNK_CEILING(minsize + nsize);
	addr = chunk_alloc_base(csize);
	if (addr == NULL) {
		if (node != NULL)
			base_node_dalloc(node);
		return (NULL);
	}
	base_mapped += csize;
	if (node == NULL) {
		node = (extent_node_t *)addr;
		addr = (void *)((uintptr_t)addr + nsize);
		csize -= nsize;
		if (config_stats) {
			base_allocated += nsize;
			base_resident += PAGE_CEILING(nsize);
		}
	}
	extent_node_init(node, NULL, addr, csize, true, true);
	return (node);
}

/*
 * base_alloc() guarantees demand-zeroed memory, in order to make multi-page
 * sparse data structures such as radix tree nodes efficient with respect to
 * physical memory usage.
 */
void *
base_alloc(size_t size)
{
	void *ret;
	size_t csize, usize;
	extent_node_t *node;
	extent_node_t key;

	/*
	 * Round size up to nearest multiple of the cacheline size, so that
	 * there is no chance of false cache line sharing.
	 */
	csize = CACHELINE_CEILING(size);

	usize = s2u(csize);
	extent_node_init(&key, NULL, NULL, usize, false, false);
	malloc_mutex_lock(&base_mtx);
	node = extent_tree_szad_nsearch(&base_avail_szad, &key);
	if (node != NULL) {
		/* Use existing space. */
		extent_tree_szad_remove(&base_avail_szad, node);
	} else {
		/* Try to allocate more space. */
		node = base_chunk_alloc(csize);
	}
	if (node == NULL) {
		ret = NULL;
		goto label_return;
	}

	ret = extent_node_addr_get(node);
	if (extent_node_size_get(node) > csize) {
		extent_node_addr_set(node, (void *)((uintptr_t)ret + csize));
		extent_node_size_set(node, extent_node_size_get(node) - csize);
		extent_tree_szad_insert(&base_avail_szad, node);
	} else
		base_node_dalloc(node);
	if (config_stats) {
		base_allocated += csize;
		/*
		 * Add one PAGE to base_resident for every page boundary that is
		 * crossed by the new allocation.
		 */
		base_resident += PAGE_CEILING((uintptr_t)ret + csize) -
		    PAGE_CEILING((uintptr_t)ret);
	}
	JEMALLOC_VALGRIND_MAKE_MEM_DEFINED(ret, csize);
label_return:
	malloc_mutex_unlock(&base_mtx);
	return (ret);
}

void
base_stats_get(size_t *allocated, size_t *resident, size_t *mapped)
{

	malloc_mutex_lock(&base_mtx);
	assert(base_allocated <= base_resident);
	assert(base_resident <= base_mapped);
	*allocated = base_allocated;
	*resident = base_resident;
	*mapped = base_mapped;
	malloc_mutex_unlock(&base_mtx);
}

bool
base_boot(void)
{

	if (malloc_mutex_init(&base_mtx))
		return (true);
	extent_tree_szad_new(&base_avail_szad);
	base_nodes = NULL;

	return (false);
}

void
base_prefork(void)
{

	malloc_mutex_prefork(&base_mtx);
}

void
base_postfork_parent(void)
{

	malloc_mutex_postfork_parent(&base_mtx);
}

void
base_postfork_child(void)
{

	malloc_mutex_postfork_child(&base_mtx);
}
