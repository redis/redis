#define	JEMALLOC_CHUNK_MMAP_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

static void *
chunk_alloc_mmap_slow(size_t size, size_t alignment, bool *zero, bool *commit)
{
	void *ret;
	size_t alloc_size;

	alloc_size = size + alignment - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size < size)
		return (NULL);
	do {
		void *pages;
		size_t leadsize;
		pages = pages_map(NULL, alloc_size);
		if (pages == NULL)
			return (NULL);
		leadsize = ALIGNMENT_CEILING((uintptr_t)pages, alignment) -
		    (uintptr_t)pages;
		ret = pages_trim(pages, alloc_size, leadsize, size);
	} while (ret == NULL);

	assert(ret != NULL);
	*zero = true;
	if (!*commit)
		*commit = pages_decommit(ret, size);
	return (ret);
}

void *
chunk_alloc_mmap(size_t size, size_t alignment, bool *zero, bool *commit)
{
	void *ret;
	size_t offset;

	/*
	 * Ideally, there would be a way to specify alignment to mmap() (like
	 * NetBSD has), but in the absence of such a feature, we have to work
	 * hard to efficiently create aligned mappings.  The reliable, but
	 * slow method is to create a mapping that is over-sized, then trim the
	 * excess.  However, that always results in one or two calls to
	 * pages_unmap().
	 *
	 * Optimistically try mapping precisely the right amount before falling
	 * back to the slow method, with the expectation that the optimistic
	 * approach works most of the time.
	 */

	assert(alignment != 0);
	assert((alignment & chunksize_mask) == 0);

	ret = pages_map(NULL, size);
	if (ret == NULL)
		return (NULL);
	offset = ALIGNMENT_ADDR2OFFSET(ret, alignment);
	if (offset != 0) {
		pages_unmap(ret, size);
		return (chunk_alloc_mmap_slow(size, alignment, zero, commit));
	}

	assert(ret != NULL);
	*zero = true;
	if (!*commit)
		*commit = pages_decommit(ret, size);
	return (ret);
}

bool
chunk_dalloc_mmap(void *chunk, size_t size)
{

	if (config_munmap)
		pages_unmap(chunk, size);

	return (!config_munmap);
}
