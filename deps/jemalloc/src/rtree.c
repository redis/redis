#define	JEMALLOC_RTREE_C_
#include "jemalloc/internal/jemalloc_internal.h"

static unsigned
hmin(unsigned ha, unsigned hb)
{

	return (ha < hb ? ha : hb);
}

/* Only the most significant bits of keys passed to rtree_[gs]et() are used. */
bool
rtree_new(rtree_t *rtree, unsigned bits, rtree_node_alloc_t *alloc,
    rtree_node_dalloc_t *dalloc)
{
	unsigned bits_in_leaf, height, i;

	assert(bits > 0 && bits <= (sizeof(uintptr_t) << 3));

	bits_in_leaf = (bits % RTREE_BITS_PER_LEVEL) == 0 ? RTREE_BITS_PER_LEVEL
	    : (bits % RTREE_BITS_PER_LEVEL);
	if (bits > bits_in_leaf) {
		height = 1 + (bits - bits_in_leaf) / RTREE_BITS_PER_LEVEL;
		if ((height-1) * RTREE_BITS_PER_LEVEL + bits_in_leaf != bits)
			height++;
	} else
		height = 1;
	assert((height-1) * RTREE_BITS_PER_LEVEL + bits_in_leaf == bits);

	rtree->alloc = alloc;
	rtree->dalloc = dalloc;
	rtree->height = height;

	/* Root level. */
	rtree->levels[0].subtree = NULL;
	rtree->levels[0].bits = (height > 1) ? RTREE_BITS_PER_LEVEL :
	    bits_in_leaf;
	rtree->levels[0].cumbits = rtree->levels[0].bits;
	/* Interior levels. */
	for (i = 1; i < height-1; i++) {
		rtree->levels[i].subtree = NULL;
		rtree->levels[i].bits = RTREE_BITS_PER_LEVEL;
		rtree->levels[i].cumbits = rtree->levels[i-1].cumbits +
		    RTREE_BITS_PER_LEVEL;
	}
	/* Leaf level. */
	if (height > 1) {
		rtree->levels[height-1].subtree = NULL;
		rtree->levels[height-1].bits = bits_in_leaf;
		rtree->levels[height-1].cumbits = bits;
	}

	/* Compute lookup table to be used by rtree_start_level(). */
	for (i = 0; i < RTREE_HEIGHT_MAX; i++) {
		rtree->start_level[i] = hmin(RTREE_HEIGHT_MAX - 1 - i, height -
		    1);
	}

	return (false);
}

static void
rtree_delete_subtree(rtree_t *rtree, rtree_node_elm_t *node, unsigned level)
{

	if (level + 1 < rtree->height) {
		size_t nchildren, i;

		nchildren = ZU(1) << rtree->levels[level].bits;
		for (i = 0; i < nchildren; i++) {
			rtree_node_elm_t *child = node[i].child;
			if (child != NULL)
				rtree_delete_subtree(rtree, child, level + 1);
		}
	}
	rtree->dalloc(node);
}

void
rtree_delete(rtree_t *rtree)
{
	unsigned i;

	for (i = 0; i < rtree->height; i++) {
		rtree_node_elm_t *subtree = rtree->levels[i].subtree;
		if (subtree != NULL)
			rtree_delete_subtree(rtree, subtree, i);
	}
}

static rtree_node_elm_t *
rtree_node_init(rtree_t *rtree, unsigned level, rtree_node_elm_t **elmp)
{
	rtree_node_elm_t *node;

	if (atomic_cas_p((void **)elmp, NULL, RTREE_NODE_INITIALIZING)) {
		/*
		 * Another thread is already in the process of initializing.
		 * Spin-wait until initialization is complete.
		 */
		do {
			CPU_SPINWAIT;
			node = atomic_read_p((void **)elmp);
		} while (node == RTREE_NODE_INITIALIZING);
	} else {
		node = rtree->alloc(ZU(1) << rtree->levels[level].bits);
		if (node == NULL)
			return (NULL);
		atomic_write_p((void **)elmp, node);
	}

	return (node);
}

rtree_node_elm_t *
rtree_subtree_read_hard(rtree_t *rtree, unsigned level)
{

	return (rtree_node_init(rtree, level, &rtree->levels[level].subtree));
}

rtree_node_elm_t *
rtree_child_read_hard(rtree_t *rtree, rtree_node_elm_t *elm, unsigned level)
{

	return (rtree_node_init(rtree, level, &elm->child));
}
