#define	JEMALLOC_RTREE_C_
#include "jemalloc/internal/jemalloc_internal.h"

rtree_t *
rtree_new(unsigned bits, rtree_alloc_t *alloc, rtree_dalloc_t *dalloc)
{
	rtree_t *ret;
	unsigned bits_per_level, bits_in_leaf, height, i;

	assert(bits > 0 && bits <= (sizeof(uintptr_t) << 3));

	bits_per_level = ffs(pow2_ceil((RTREE_NODESIZE / sizeof(void *)))) - 1;
	bits_in_leaf = ffs(pow2_ceil((RTREE_NODESIZE / sizeof(uint8_t)))) - 1;
	if (bits > bits_in_leaf) {
		height = 1 + (bits - bits_in_leaf) / bits_per_level;
		if ((height-1) * bits_per_level + bits_in_leaf != bits)
			height++;
	} else {
		height = 1;
	}
	assert((height-1) * bits_per_level + bits_in_leaf >= bits);

	ret = (rtree_t*)alloc(offsetof(rtree_t, level2bits) +
	    (sizeof(unsigned) * height));
	if (ret == NULL)
		return (NULL);
	memset(ret, 0, offsetof(rtree_t, level2bits) + (sizeof(unsigned) *
	    height));

	ret->alloc = alloc;
	ret->dalloc = dalloc;
	if (malloc_mutex_init(&ret->mutex)) {
		if (dalloc != NULL)
			dalloc(ret);
		return (NULL);
	}
	ret->height = height;
	if (height > 1) {
		if ((height-1) * bits_per_level + bits_in_leaf > bits) {
			ret->level2bits[0] = (bits - bits_in_leaf) %
			    bits_per_level;
		} else
			ret->level2bits[0] = bits_per_level;
		for (i = 1; i < height-1; i++)
			ret->level2bits[i] = bits_per_level;
		ret->level2bits[height-1] = bits_in_leaf;
	} else
		ret->level2bits[0] = bits;

	ret->root = (void**)alloc(sizeof(void *) << ret->level2bits[0]);
	if (ret->root == NULL) {
		if (dalloc != NULL)
			dalloc(ret);
		return (NULL);
	}
	memset(ret->root, 0, sizeof(void *) << ret->level2bits[0]);

	return (ret);
}

static void
rtree_delete_subtree(rtree_t *rtree, void **node, unsigned level)
{

	if (level < rtree->height - 1) {
		size_t nchildren, i;

		nchildren = ZU(1) << rtree->level2bits[level];
		for (i = 0; i < nchildren; i++) {
			void **child = (void **)node[i];
			if (child != NULL)
				rtree_delete_subtree(rtree, child, level + 1);
		}
	}
	rtree->dalloc(node);
}

void
rtree_delete(rtree_t *rtree)
{

	rtree_delete_subtree(rtree, rtree->root, 0);
	rtree->dalloc(rtree);
}

void
rtree_prefork(rtree_t *rtree)
{

	malloc_mutex_prefork(&rtree->mutex);
}

void
rtree_postfork_parent(rtree_t *rtree)
{

	malloc_mutex_postfork_parent(&rtree->mutex);
}

void
rtree_postfork_child(rtree_t *rtree)
{

	malloc_mutex_postfork_child(&rtree->mutex);
}
