#define	JEMALLOC_EXTENT_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

#ifndef JEMALLOC_JET
static
#endif
size_t
extent_size_quantize_floor(size_t size) {
	size_t ret;
	szind_t ind;

	assert(size > 0);

	ind = size2index(size + 1);
	if (ind == 0) {
		/* Avoid underflow. */
		return (index2size(0));
	}
	ret = index2size(ind - 1);
	assert(ret <= size);
	return (ret);
}

size_t
extent_size_quantize_ceil(size_t size) {
	size_t ret;

	assert(size > 0);

	ret = extent_size_quantize_floor(size);
	if (ret < size) {
		/*
		 * Skip a quantization that may have an adequately large extent,
		 * because under-sized extents may be mixed in.  This only
		 * happens when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		ret = index2size(size2index(ret  + 1));
	}
	return ret;
}

JEMALLOC_INLINE_C int
extent_sz_comp(const extent_node_t *a, const extent_node_t *b)
{
	size_t a_qsize = extent_size_quantize_floor(extent_node_size_get(a));
	size_t b_qsize = extent_size_quantize_floor(extent_node_size_get(b));

	return ((a_qsize > b_qsize) - (a_qsize < b_qsize));
}

JEMALLOC_INLINE_C int
extent_sn_comp(const extent_node_t *a, const extent_node_t *b)
{
	size_t a_sn = extent_node_sn_get(a);
	size_t b_sn = extent_node_sn_get(b);

	return ((a_sn > b_sn) - (a_sn < b_sn));
}

JEMALLOC_INLINE_C int
extent_ad_comp(const extent_node_t *a, const extent_node_t *b)
{
	uintptr_t a_addr = (uintptr_t)extent_node_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_node_addr_get(b);

	return ((a_addr > b_addr) - (a_addr < b_addr));
}

JEMALLOC_INLINE_C int
extent_szsnad_comp(const extent_node_t *a, const extent_node_t *b)
{
	int ret;

	ret = extent_sz_comp(a, b);
	if (ret != 0)
		return (ret);

	ret = extent_sn_comp(a, b);
	if (ret != 0)
		return (ret);

	ret = extent_ad_comp(a, b);
	return (ret);
}

/* Generate red-black tree functions. */
rb_gen(, extent_tree_szsnad_, extent_tree_t, extent_node_t, szsnad_link,
    extent_szsnad_comp)

/* Generate red-black tree functions. */
rb_gen(, extent_tree_ad_, extent_tree_t, extent_node_t, ad_link, extent_ad_comp)
