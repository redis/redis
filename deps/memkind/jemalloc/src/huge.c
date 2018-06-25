#define	JEMALLOC_HUGE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

static extent_node_t *
huge_node_get(const void *ptr)
{
	extent_node_t *node;

	node = chunk_lookup(ptr, true);
	assert(!extent_node_achunk_get(node));

	return (node);
}

static bool
huge_node_set(tsdn_t *tsdn, const void *ptr, extent_node_t *node, bool *gdump)
{

	assert(extent_node_addr_get(node) == ptr);
	assert(!extent_node_achunk_get(node));
	return (chunk_register(ptr, node, gdump));
}

static void
huge_node_reset(tsdn_t *tsdn, const void *ptr, extent_node_t *node, bool *gdump)
{
	bool err;

	err = huge_node_set(tsdn, ptr, node, gdump);
	assert(!err);
}

static void
huge_node_unset(const void *ptr, const extent_node_t *node)
{

	chunk_deregister(ptr, node);
}

void *
huge_malloc(tsdn_t *tsdn, arena_t *arena, size_t usize, bool zero)
{

	assert(usize == s2u(usize));

	return (huge_palloc(tsdn, arena, usize, chunksize, zero));
}

void *
huge_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero)
{
	void *ret;
	size_t ausize;
	arena_t *iarena;
	extent_node_t *node;
	size_t sn;
	bool is_zeroed, gdump;

	/* Allocate one or more contiguous chunks for this request. */

	assert(!tsdn_null(tsdn) || arena != NULL);
	/* prof_gdump() requirement. */
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	ausize = sa2u(usize, alignment);
	if (unlikely(ausize == 0 || ausize > HUGE_MAXCLASS))
		return (NULL);
	assert(ausize >= chunksize);

	/* Allocate an extent node with which to track the chunk. */
	iarena = (!tsdn_null(tsdn)) ? arena_ichoose(tsdn_tsd(tsdn), NULL) :
	    a0get();
	node = ipallocztm(tsdn, CACHELINE_CEILING(sizeof(extent_node_t)),
	    CACHELINE, false, NULL, true, iarena);
	if (node == NULL)
		return (NULL);

	/*
	 * Copy zero into is_zeroed and pass the copy to chunk_alloc(), so that
	 * it is possible to make correct junk/zero fill decisions below.
	 */
	is_zeroed = zero;
	if (likely(!tsdn_null(tsdn)))
		arena = arena_choose(tsdn_tsd(tsdn), arena);
	if (unlikely(arena == NULL) || (ret = arena_chunk_alloc_huge(tsdn,
	    arena, usize, alignment, &sn, &is_zeroed)) == NULL) {
		idalloctm(tsdn, node, NULL, true, true);
		return (NULL);
	}

	extent_node_init(node, arena, ret, usize, sn, is_zeroed, true);

	if (huge_node_set(tsdn, ret, node, &gdump)) {
		arena_chunk_dalloc_huge(tsdn, arena, ret, usize, sn);
		idalloctm(tsdn, node, NULL, true, true);
		return (NULL);
	}
	if (config_prof && opt_prof && gdump)
		prof_gdump(tsdn);

	/* Insert node into huge. */
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	ql_elm_new(node, ql_link);
	ql_tail_insert(&arena->huge, node, ql_link);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	if (zero || (config_fill && unlikely(opt_zero))) {
		if (!is_zeroed)
			memset(ret, 0, usize);
	} else if (config_fill && unlikely(opt_junk_alloc))
		memset(ret, JEMALLOC_ALLOC_JUNK, usize);

	arena_decay_tick(tsdn, arena);
	return (ret);
}

#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk_impl)
#endif
static void
huge_dalloc_junk(void *ptr, size_t usize)
{

	if (config_fill && have_dss && unlikely(opt_junk_free)) {
		/*
		 * Only bother junk filling if the chunk isn't about to be
		 * unmapped.
		 */
		if (!config_munmap || (have_dss && chunk_in_dss(ptr)))
			memset(ptr, JEMALLOC_FREE_JUNK, usize);
	}
}
#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk)
huge_dalloc_junk_t *huge_dalloc_junk = JEMALLOC_N(huge_dalloc_junk_impl);
#endif

static void
huge_ralloc_no_move_similar(tsdn_t *tsdn, void *ptr, size_t oldsize,
    size_t usize_min, size_t usize_max, bool zero)
{
	size_t usize, usize_next;
	extent_node_t *node;
	arena_t *arena;
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;
	bool pre_zeroed, post_zeroed, gdump;

	/* prof_gdump() requirement. */
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	/* Increase usize to incorporate extra. */
	for (usize = usize_min; usize < usize_max && (usize_next = s2u(usize+1))
	    <= oldsize; usize = usize_next)
		; /* Do nothing. */

	if (oldsize == usize)
		return;

	node = huge_node_get(ptr);
	arena = extent_node_arena_get(node);
	pre_zeroed = extent_node_zeroed_get(node);

	/* Fill if necessary (shrinking). */
	if (oldsize > usize) {
		size_t sdiff = oldsize - usize;
		if (config_fill && unlikely(opt_junk_free)) {
			memset((void *)((uintptr_t)ptr + usize),
			    JEMALLOC_FREE_JUNK, sdiff);
			post_zeroed = false;
		} else {
			post_zeroed = !chunk_purge_wrapper(tsdn, arena,
			    &chunk_hooks, ptr, CHUNK_CEILING(oldsize), usize,
			    sdiff);
		}
	} else
		post_zeroed = pre_zeroed;

	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	/* Update the size of the huge allocation. */
	huge_node_unset(ptr, node);
	assert(extent_node_size_get(node) != usize);
	extent_node_size_set(node, usize);
	huge_node_reset(tsdn, ptr, node, &gdump);
	/* Update zeroed. */
	extent_node_zeroed_set(node, post_zeroed);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);
	/* gdump without any locks held. */
	if (config_prof && opt_prof && gdump)
		prof_gdump(tsdn);

	arena_chunk_ralloc_huge_similar(tsdn, arena, ptr, oldsize, usize);

	/* Fill if necessary (growing). */
	if (oldsize < usize) {
		if (zero || (config_fill && unlikely(opt_zero))) {
			if (!pre_zeroed) {
				memset((void *)((uintptr_t)ptr + oldsize), 0,
				    usize - oldsize);
			}
		} else if (config_fill && unlikely(opt_junk_alloc)) {
			memset((void *)((uintptr_t)ptr + oldsize),
			    JEMALLOC_ALLOC_JUNK, usize - oldsize);
		}
	}
}

static bool
huge_ralloc_no_move_shrink(tsdn_t *tsdn, void *ptr, size_t oldsize,
    size_t usize)
{
	extent_node_t *node;
	arena_t *arena;
	chunk_hooks_t chunk_hooks;
	size_t cdiff;
	bool pre_zeroed, post_zeroed, gdump;

	node = huge_node_get(ptr);
	arena = extent_node_arena_get(node);
	pre_zeroed = extent_node_zeroed_get(node);
	chunk_hooks = chunk_hooks_get(tsdn, arena);

	assert(oldsize > usize);
	/* prof_gdump() requirement. */
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	/* Split excess chunks. */
	cdiff = CHUNK_CEILING(oldsize) - CHUNK_CEILING(usize);
	if (cdiff != 0 && chunk_hooks.split(ptr, CHUNK_CEILING(oldsize),
	    CHUNK_CEILING(usize), cdiff, true, arena->ind))
		return (true);

	if (oldsize > usize) {
		size_t sdiff = oldsize - usize;
		if (config_fill && unlikely(opt_junk_free)) {
			huge_dalloc_junk((void *)((uintptr_t)ptr + usize),
			    sdiff);
			post_zeroed = false;
		} else {
			post_zeroed = !chunk_purge_wrapper(tsdn, arena,
			    &chunk_hooks, CHUNK_ADDR2BASE((uintptr_t)ptr +
			    usize), CHUNK_CEILING(oldsize),
			    CHUNK_ADDR2OFFSET((uintptr_t)ptr + usize), sdiff);
		}
	} else
		post_zeroed = pre_zeroed;

	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	/* Update the size of the huge allocation. */
	huge_node_unset(ptr, node);
	extent_node_size_set(node, usize);
	huge_node_reset(tsdn, ptr, node, &gdump);
	/* Update zeroed. */
	extent_node_zeroed_set(node, post_zeroed);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);
	/* gdump without any locks held. */
	if (config_prof && opt_prof && gdump)
		prof_gdump(tsdn);

	/* Zap the excess chunks. */
	arena_chunk_ralloc_huge_shrink(tsdn, arena, ptr, oldsize, usize,
	    extent_node_sn_get(node));

	return (false);
}

static bool
huge_ralloc_no_move_expand(tsdn_t *tsdn, void *ptr, size_t oldsize,
    size_t usize, bool zero) {
	extent_node_t *node;
	arena_t *arena;
	bool is_zeroed_subchunk, is_zeroed_chunk, gdump;

	node = huge_node_get(ptr);
	arena = extent_node_arena_get(node);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	is_zeroed_subchunk = extent_node_zeroed_get(node);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	/* prof_gdump() requirement. */
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	/*
	 * Use is_zeroed_chunk to detect whether the trailing memory is zeroed,
	 * update extent's zeroed field, and zero as necessary.
	 */
	is_zeroed_chunk = false;
	if (arena_chunk_ralloc_huge_expand(tsdn, arena, ptr, oldsize, usize,
	     &is_zeroed_chunk))
		return (true);

	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	huge_node_unset(ptr, node);
	extent_node_size_set(node, usize);
	extent_node_zeroed_set(node, extent_node_zeroed_get(node) &&
	    is_zeroed_chunk);
	huge_node_reset(tsdn, ptr, node, &gdump);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);
	/* gdump without any locks held. */
	if (config_prof && opt_prof && gdump)
		prof_gdump(tsdn);

	if (zero || (config_fill && unlikely(opt_zero))) {
		if (!is_zeroed_subchunk) {
			memset((void *)((uintptr_t)ptr + oldsize), 0,
			    CHUNK_CEILING(oldsize) - oldsize);
		}
		if (!is_zeroed_chunk) {
			memset((void *)((uintptr_t)ptr +
			    CHUNK_CEILING(oldsize)), 0, usize -
			    CHUNK_CEILING(oldsize));
		}
	} else if (config_fill && unlikely(opt_junk_alloc)) {
		memset((void *)((uintptr_t)ptr + oldsize), JEMALLOC_ALLOC_JUNK,
		    usize - oldsize);
	}

	return (false);
}

bool
huge_ralloc_no_move(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t usize_min,
    size_t usize_max, bool zero)
{

	assert(s2u(oldsize) == oldsize);
	/* The following should have been caught by callers. */
	assert(usize_min > 0 && usize_max <= HUGE_MAXCLASS);

	/* Both allocations must be huge to avoid a move. */
	if (oldsize < chunksize || usize_max < chunksize)
		return (true);

	if (CHUNK_CEILING(usize_max) > CHUNK_CEILING(oldsize)) {
		/* Attempt to expand the allocation in-place. */
		if (!huge_ralloc_no_move_expand(tsdn, ptr, oldsize, usize_max,
		    zero)) {
			arena_decay_tick(tsdn, huge_aalloc(ptr));
			return (false);
		}
		/* Try again, this time with usize_min. */
		if (usize_min < usize_max && CHUNK_CEILING(usize_min) >
		    CHUNK_CEILING(oldsize) && huge_ralloc_no_move_expand(tsdn,
		    ptr, oldsize, usize_min, zero)) {
			arena_decay_tick(tsdn, huge_aalloc(ptr));
			return (false);
		}
	}

	/*
	 * Avoid moving the allocation if the existing chunk size accommodates
	 * the new size.
	 */
	if (CHUNK_CEILING(oldsize) >= CHUNK_CEILING(usize_min)
	    && CHUNK_CEILING(oldsize) <= CHUNK_CEILING(usize_max)) {
		huge_ralloc_no_move_similar(tsdn, ptr, oldsize, usize_min,
		    usize_max, zero);
		arena_decay_tick(tsdn, huge_aalloc(ptr));
		return (false);
	}

	/* Attempt to shrink the allocation in-place. */
	if (CHUNK_CEILING(oldsize) > CHUNK_CEILING(usize_max)) {
		if (!huge_ralloc_no_move_shrink(tsdn, ptr, oldsize,
		    usize_max)) {
			arena_decay_tick(tsdn, huge_aalloc(ptr));
			return (false);
		}
	}
	return (true);
}

static void *
huge_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero)
{

	if (alignment <= chunksize)
		return (huge_malloc(tsdn, arena, usize, zero));
	return (huge_palloc(tsdn, arena, usize, alignment, zero));
}

void *
huge_ralloc(tsd_t *tsd, arena_t *arena, void *ptr, size_t oldsize,
    size_t usize, size_t alignment, bool zero, tcache_t *tcache)
{
	void *ret;
	size_t copysize;

	/* The following should have been caught by callers. */
	assert(usize > 0 && usize <= HUGE_MAXCLASS);

	/* Try to avoid moving the allocation. */
	if (!huge_ralloc_no_move(tsd_tsdn(tsd), ptr, oldsize, usize, usize,
	    zero))
		return (ptr);

	/*
	 * usize and oldsize are different enough that we need to use a
	 * different size class.  In that case, fall back to allocating new
	 * space and copying.
	 */
	ret = huge_ralloc_move_helper(tsd_tsdn(tsd), arena, usize, alignment,
	    zero);
	if (ret == NULL)
		return (NULL);

	copysize = (usize < oldsize) ? usize : oldsize;
	memcpy(ret, ptr, copysize);
	isqalloc(tsd, ptr, oldsize, tcache, true);
	return (ret);
}

void
huge_dalloc(tsdn_t *tsdn, void *ptr)
{
	extent_node_t *node;
	arena_t *arena;

	node = huge_node_get(ptr);
	arena = extent_node_arena_get(node);
	huge_node_unset(ptr, node);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	ql_remove(&arena->huge, node, ql_link);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	huge_dalloc_junk(extent_node_addr_get(node),
	    extent_node_size_get(node));
	arena_chunk_dalloc_huge(tsdn, extent_node_arena_get(node),
	    extent_node_addr_get(node), extent_node_size_get(node),
	    extent_node_sn_get(node));
	idalloctm(tsdn, node, NULL, true, true);

	arena_decay_tick(tsdn, arena);
}

arena_t *
huge_aalloc(const void *ptr)
{

	return (extent_node_arena_get(huge_node_get(ptr)));
}

size_t
huge_salloc(tsdn_t *tsdn, const void *ptr)
{
	size_t size;
	extent_node_t *node;
	arena_t *arena;

	node = huge_node_get(ptr);
	arena = extent_node_arena_get(node);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	size = extent_node_size_get(node);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	return (size);
}

prof_tctx_t *
huge_prof_tctx_get(tsdn_t *tsdn, const void *ptr)
{
	prof_tctx_t *tctx;
	extent_node_t *node;
	arena_t *arena;

	node = huge_node_get(ptr);
	arena = extent_node_arena_get(node);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	tctx = extent_node_prof_tctx_get(node);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	return (tctx);
}

void
huge_prof_tctx_set(tsdn_t *tsdn, const void *ptr, prof_tctx_t *tctx)
{
	extent_node_t *node;
	arena_t *arena;

	node = huge_node_get(ptr);
	arena = extent_node_arena_get(node);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	extent_node_prof_tctx_set(node, tctx);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);
}

void
huge_prof_tctx_reset(tsdn_t *tsdn, const void *ptr)
{

	huge_prof_tctx_set(tsdn, ptr, (prof_tctx_t *)(uintptr_t)1U);
}
