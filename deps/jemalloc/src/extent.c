#define	JEMALLOC_EXTENT_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

JEMALLOC_INLINE_C size_t
extent_quantize(size_t size)
{

	/*
	 * Round down to the nearest chunk size that can actually be requested
	 * during normal huge allocation.
	 */
	return (index2size(size2index(size + 1) - 1));
}

JEMALLOC_INLINE_C int
extent_szad_comp(extent_node_t *a, extent_node_t *b)
{
	int ret;
	size_t a_qsize = extent_quantize(extent_node_size_get(a));
	size_t b_qsize = extent_quantize(extent_node_size_get(b));

	/*
	 * Compare based on quantized size rather than size, in order to sort
	 * equally useful extents only by address.
	 */
	ret = (a_qsize > b_qsize) - (a_qsize < b_qsize);
	if (ret == 0) {
		uintptr_t a_addr = (uintptr_t)extent_node_addr_get(a);
		uintptr_t b_addr = (uintptr_t)extent_node_addr_get(b);

		ret = (a_addr > b_addr) - (a_addr < b_addr);
	}

	return (ret);
}

/* Generate red-black tree functions. */
rb_gen(, extent_tree_szad_, extent_tree_t, extent_node_t, szad_link,
    extent_szad_comp)

JEMALLOC_INLINE_C int
extent_ad_comp(extent_node_t *a, extent_node_t *b)
{
	uintptr_t a_addr = (uintptr_t)extent_node_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_node_addr_get(b);

	return ((a_addr > b_addr) - (a_addr < b_addr));
}

/* Generate red-black tree functions. */
rb_gen(, extent_tree_ad_, extent_tree_t, extent_node_t, ad_link, extent_ad_comp)
