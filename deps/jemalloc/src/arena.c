#define	JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

ssize_t		opt_lg_dirty_mult = LG_DIRTY_MULT_DEFAULT;
static ssize_t	lg_dirty_mult_default;
arena_bin_info_t	arena_bin_info[NBINS];

size_t		map_bias;
size_t		map_misc_offset;
size_t		arena_maxrun; /* Max run size for arenas. */
size_t		arena_maxclass; /* Max size class for arenas. */
static size_t	small_maxrun; /* Max run size used for small size classes. */
static bool	*small_run_tab; /* Valid small run page multiples. */
unsigned	nlclasses; /* Number of large size classes. */
unsigned	nhclasses; /* Number of huge size classes. */

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void	arena_purge(arena_t *arena, bool all);
static void	arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty,
    bool cleaned, bool decommitted);
static void	arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);
static void	arena_bin_lower_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);

/******************************************************************************/

#define	CHUNK_MAP_KEY		((uintptr_t)0x1U)

JEMALLOC_INLINE_C arena_chunk_map_misc_t *
arena_miscelm_key_create(size_t size)
{

	return ((arena_chunk_map_misc_t *)((size << CHUNK_MAP_SIZE_SHIFT) |
	    CHUNK_MAP_KEY));
}

JEMALLOC_INLINE_C bool
arena_miscelm_is_key(const arena_chunk_map_misc_t *miscelm)
{

	return (((uintptr_t)miscelm & CHUNK_MAP_KEY) != 0);
}

#undef CHUNK_MAP_KEY

JEMALLOC_INLINE_C size_t
arena_miscelm_key_size_get(const arena_chunk_map_misc_t *miscelm)
{

	assert(arena_miscelm_is_key(miscelm));

	return (((uintptr_t)miscelm & CHUNK_MAP_SIZE_MASK) >>
	    CHUNK_MAP_SIZE_SHIFT);
}

JEMALLOC_INLINE_C size_t
arena_miscelm_size_get(arena_chunk_map_misc_t *miscelm)
{
	arena_chunk_t *chunk;
	size_t pageind, mapbits;

	assert(!arena_miscelm_is_key(miscelm));

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(miscelm);
	pageind = arena_miscelm_to_pageind(miscelm);
	mapbits = arena_mapbits_get(chunk, pageind);
	return ((mapbits & CHUNK_MAP_SIZE_MASK) >> CHUNK_MAP_SIZE_SHIFT);
}

JEMALLOC_INLINE_C int
arena_run_comp(arena_chunk_map_misc_t *a, arena_chunk_map_misc_t *b)
{
	uintptr_t a_miscelm = (uintptr_t)a;
	uintptr_t b_miscelm = (uintptr_t)b;

	assert(a != NULL);
	assert(b != NULL);

	return ((a_miscelm > b_miscelm) - (a_miscelm < b_miscelm));
}

/* Generate red-black tree functions. */
rb_gen(static UNUSED, arena_run_tree_, arena_run_tree_t, arena_chunk_map_misc_t,
    rb_link, arena_run_comp)

static size_t
run_quantize(size_t size)
{
	size_t qsize;

	assert(size != 0);
	assert(size == PAGE_CEILING(size));

	/* Don't change sizes that are valid small run sizes. */
	if (size <= small_maxrun && small_run_tab[size >> LG_PAGE])
		return (size);

	/*
	 * Round down to the nearest run size that can actually be requested
	 * during normal large allocation.  Add large_pad so that cache index
	 * randomization can offset the allocation from the page boundary.
	 */
	qsize = index2size(size2index(size - large_pad + 1) - 1) + large_pad;
	if (qsize <= SMALL_MAXCLASS + large_pad)
		return (run_quantize(size - large_pad));
	assert(qsize <= size);
	return (qsize);
}

static size_t
run_quantize_next(size_t size)
{
	size_t large_run_size_next;

	assert(size != 0);
	assert(size == PAGE_CEILING(size));

	/*
	 * Return the next quantized size greater than the input size.
	 * Quantized sizes comprise the union of run sizes that back small
	 * region runs, and run sizes that back large regions with no explicit
	 * alignment constraints.
	 */

	if (size > SMALL_MAXCLASS) {
		large_run_size_next = PAGE_CEILING(index2size(size2index(size -
		    large_pad) + 1) + large_pad);
	} else
		large_run_size_next = SIZE_T_MAX;
	if (size >= small_maxrun)
		return (large_run_size_next);

	while (true) {
		size += PAGE;
		assert(size <= small_maxrun);
		if (small_run_tab[size >> LG_PAGE]) {
			if (large_run_size_next < size)
				return (large_run_size_next);
			return (size);
		}
	}
}

static size_t
run_quantize_first(size_t size)
{
	size_t qsize = run_quantize(size);

	if (qsize < size) {
		/*
		 * Skip a quantization that may have an adequately large run,
		 * because under-sized runs may be mixed in.  This only happens
		 * when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		qsize = run_quantize_next(size);
	}
	return (qsize);
}

JEMALLOC_INLINE_C int
arena_avail_comp(arena_chunk_map_misc_t *a, arena_chunk_map_misc_t *b)
{
	int ret;
	uintptr_t a_miscelm = (uintptr_t)a;
	size_t a_qsize = run_quantize(arena_miscelm_is_key(a) ?
	    arena_miscelm_key_size_get(a) : arena_miscelm_size_get(a));
	size_t b_qsize = run_quantize(arena_miscelm_size_get(b));

	/*
	 * Compare based on quantized size rather than size, in order to sort
	 * equally useful runs only by address.
	 */
	ret = (a_qsize > b_qsize) - (a_qsize < b_qsize);
	if (ret == 0) {
		if (!arena_miscelm_is_key(a)) {
			uintptr_t b_miscelm = (uintptr_t)b;

			ret = (a_miscelm > b_miscelm) - (a_miscelm < b_miscelm);
		} else {
			/*
			 * Treat keys as if they are lower than anything else.
			 */
			ret = -1;
		}
	}

	return (ret);
}

/* Generate red-black tree functions. */
rb_gen(static UNUSED, arena_avail_tree_, arena_avail_tree_t,
    arena_chunk_map_misc_t, rb_link, arena_avail_comp)

static void
arena_avail_insert(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	arena_avail_tree_insert(&arena->runs_avail, arena_miscelm_get(chunk,
	    pageind));
}

static void
arena_avail_remove(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	arena_avail_tree_remove(&arena->runs_avail, arena_miscelm_get(chunk,
	    pageind));
}

static void
arena_run_dirty_insert(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	assert(arena_mapbits_dirty_get(chunk, pageind) == CHUNK_MAP_DIRTY);
	assert(arena_mapbits_dirty_get(chunk, pageind+npages-1) ==
	    CHUNK_MAP_DIRTY);

	qr_new(&miscelm->rd, rd_link);
	qr_meld(&arena->runs_dirty, &miscelm->rd, rd_link);
	arena->ndirty += npages;
}

static void
arena_run_dirty_remove(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	assert(arena_mapbits_dirty_get(chunk, pageind) == CHUNK_MAP_DIRTY);
	assert(arena_mapbits_dirty_get(chunk, pageind+npages-1) ==
	    CHUNK_MAP_DIRTY);

	qr_remove(&miscelm->rd, rd_link);
	assert(arena->ndirty >= npages);
	arena->ndirty -= npages;
}

static size_t
arena_chunk_dirty_npages(const extent_node_t *node)
{

	return (extent_node_size_get(node) >> LG_PAGE);
}

void
arena_chunk_cache_maybe_insert(arena_t *arena, extent_node_t *node, bool cache)
{

	if (cache) {
		extent_node_dirty_linkage_init(node);
		extent_node_dirty_insert(node, &arena->runs_dirty,
		    &arena->chunks_cache);
		arena->ndirty += arena_chunk_dirty_npages(node);
	}
}

void
arena_chunk_cache_maybe_remove(arena_t *arena, extent_node_t *node, bool dirty)
{

	if (dirty) {
		extent_node_dirty_remove(node);
		assert(arena->ndirty >= arena_chunk_dirty_npages(node));
		arena->ndirty -= arena_chunk_dirty_npages(node);
	}
}

JEMALLOC_INLINE_C void *
arena_run_reg_alloc(arena_run_t *run, arena_bin_info_t *bin_info)
{
	void *ret;
	unsigned regind;
	arena_chunk_map_misc_t *miscelm;
	void *rpages;

	assert(run->nfree > 0);
	assert(!bitmap_full(run->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(run->bitmap, &bin_info->bitmap_info);
	miscelm = arena_run_to_miscelm(run);
	rpages = arena_miscelm_to_rpages(miscelm);
	ret = (void *)((uintptr_t)rpages + (uintptr_t)bin_info->reg0_offset +
	    (uintptr_t)(bin_info->reg_interval * regind));
	run->nfree--;
	return (ret);
}

JEMALLOC_INLINE_C void
arena_run_reg_dalloc(arena_run_t *run, void *ptr)
{
	arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	size_t mapbits = arena_mapbits_get(chunk, pageind);
	index_t binind = arena_ptr_small_binind_get(ptr, mapbits);
	arena_bin_info_t *bin_info = &arena_bin_info[binind];
	unsigned regind = arena_run_regind(run, bin_info, ptr);

	assert(run->nfree < bin_info->nregs);
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr -
	    ((uintptr_t)arena_miscelm_to_rpages(arena_run_to_miscelm(run)) +
	    (uintptr_t)bin_info->reg0_offset)) %
	    (uintptr_t)bin_info->reg_interval == 0);
	assert((uintptr_t)ptr >=
	    (uintptr_t)arena_miscelm_to_rpages(arena_run_to_miscelm(run)) +
	    (uintptr_t)bin_info->reg0_offset);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(run->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(run->bitmap, &bin_info->bitmap_info, regind);
	run->nfree++;
}

JEMALLOC_INLINE_C void
arena_run_zero(arena_chunk_t *chunk, size_t run_ind, size_t npages)
{

	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED((void *)((uintptr_t)chunk +
	    (run_ind << LG_PAGE)), (npages << LG_PAGE));
	memset((void *)((uintptr_t)chunk + (run_ind << LG_PAGE)), 0,
	    (npages << LG_PAGE));
}

JEMALLOC_INLINE_C void
arena_run_page_mark_zeroed(arena_chunk_t *chunk, size_t run_ind)
{

	JEMALLOC_VALGRIND_MAKE_MEM_DEFINED((void *)((uintptr_t)chunk + (run_ind
	    << LG_PAGE)), PAGE);
}

JEMALLOC_INLINE_C void
arena_run_page_validate_zeroed(arena_chunk_t *chunk, size_t run_ind)
{
	size_t i;
	UNUSED size_t *p = (size_t *)((uintptr_t)chunk + (run_ind << LG_PAGE));

	arena_run_page_mark_zeroed(chunk, run_ind);
	for (i = 0; i < PAGE / sizeof(size_t); i++)
		assert(p[i] == 0);
}

static void
arena_cactive_update(arena_t *arena, size_t add_pages, size_t sub_pages)
{

	if (config_stats) {
		ssize_t cactive_diff = CHUNK_CEILING((arena->nactive + add_pages
		    - sub_pages) << LG_PAGE) - CHUNK_CEILING(arena->nactive <<
		    LG_PAGE);
		if (cactive_diff != 0)
			stats_cactive_add(cactive_diff);
	}
}

static void
arena_run_split_remove(arena_t *arena, arena_chunk_t *chunk, size_t run_ind,
    size_t flag_dirty, size_t flag_decommitted, size_t need_pages)
{
	size_t total_pages, rem_pages;

	assert(flag_dirty == 0 || flag_decommitted == 0);

	total_pages = arena_mapbits_unallocated_size_get(chunk, run_ind) >>
	    LG_PAGE;
	assert(arena_mapbits_dirty_get(chunk, run_ind+total_pages-1) ==
	    flag_dirty);
	assert(need_pages <= total_pages);
	rem_pages = total_pages - need_pages;

	arena_avail_remove(arena, chunk, run_ind, total_pages);
	if (flag_dirty != 0)
		arena_run_dirty_remove(arena, chunk, run_ind, total_pages);
	arena_cactive_update(arena, need_pages, 0);
	arena->nactive += need_pages;

	/* Keep track of trailing unused pages for later use. */
	if (rem_pages > 0) {
		size_t flags = flag_dirty | flag_decommitted;
		size_t flag_unzeroed_mask = (flags == 0) ?  CHUNK_MAP_UNZEROED :
		    0;

		arena_mapbits_unallocated_set(chunk, run_ind+need_pages,
		    (rem_pages << LG_PAGE), flags |
		    (arena_mapbits_unzeroed_get(chunk, run_ind+need_pages) &
		    flag_unzeroed_mask));
		arena_mapbits_unallocated_set(chunk, run_ind+total_pages-1,
		    (rem_pages << LG_PAGE), flags |
		    (arena_mapbits_unzeroed_get(chunk, run_ind+total_pages-1) &
		    flag_unzeroed_mask));
		if (flag_dirty != 0) {
			arena_run_dirty_insert(arena, chunk, run_ind+need_pages,
			    rem_pages);
		}
		arena_avail_insert(arena, chunk, run_ind+need_pages, rem_pages);
	}
}

static bool
arena_run_split_large_helper(arena_t *arena, arena_run_t *run, size_t size,
    bool remove, bool zero)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t flag_dirty, flag_decommitted, run_ind, need_pages, i;
	size_t flag_unzeroed_mask;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	run_ind = arena_miscelm_to_pageind(miscelm);
	flag_dirty = arena_mapbits_dirty_get(chunk, run_ind);
	flag_decommitted = arena_mapbits_decommitted_get(chunk, run_ind);
	need_pages = (size >> LG_PAGE);
	assert(need_pages > 0);

	if (flag_decommitted != 0 && arena->chunk_hooks.commit(chunk, chunksize,
	    run_ind << LG_PAGE, size, arena->ind))
		return (true);

	if (remove) {
		arena_run_split_remove(arena, chunk, run_ind, flag_dirty,
		    flag_decommitted, need_pages);
	}

	if (zero) {
		if (flag_decommitted != 0) {
			/* The run is untouched, and therefore zeroed. */
			JEMALLOC_VALGRIND_MAKE_MEM_DEFINED((void
			    *)((uintptr_t)chunk + (run_ind << LG_PAGE)),
			    (need_pages << LG_PAGE));
		} else if (flag_dirty != 0) {
			/* The run is dirty, so all pages must be zeroed. */
			arena_run_zero(chunk, run_ind, need_pages);
		} else {
			/*
			 * The run is clean, so some pages may be zeroed (i.e.
			 * never before touched).
			 */
			for (i = 0; i < need_pages; i++) {
				if (arena_mapbits_unzeroed_get(chunk, run_ind+i)
				    != 0)
					arena_run_zero(chunk, run_ind+i, 1);
				else if (config_debug) {
					arena_run_page_validate_zeroed(chunk,
					    run_ind+i);
				} else {
					arena_run_page_mark_zeroed(chunk,
					    run_ind+i);
				}
			}
		}
	} else {
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED((void *)((uintptr_t)chunk +
		    (run_ind << LG_PAGE)), (need_pages << LG_PAGE));
	}

	/*
	 * Set the last element first, in case the run only contains one page
	 * (i.e. both statements set the same element).
	 */
	flag_unzeroed_mask = (flag_dirty | flag_decommitted) == 0 ?
	    CHUNK_MAP_UNZEROED : 0;
	arena_mapbits_large_set(chunk, run_ind+need_pages-1, 0, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    run_ind+need_pages-1)));
	arena_mapbits_large_set(chunk, run_ind, size, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk, run_ind)));
	return (false);
}

static bool
arena_run_split_large(arena_t *arena, arena_run_t *run, size_t size, bool zero)
{

	return (arena_run_split_large_helper(arena, run, size, true, zero));
}

static bool
arena_run_init_large(arena_t *arena, arena_run_t *run, size_t size, bool zero)
{

	return (arena_run_split_large_helper(arena, run, size, false, zero));
}

static bool
arena_run_split_small(arena_t *arena, arena_run_t *run, size_t size,
    index_t binind)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t flag_dirty, flag_decommitted, run_ind, need_pages, i;

	assert(binind != BININD_INVALID);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	run_ind = arena_miscelm_to_pageind(miscelm);
	flag_dirty = arena_mapbits_dirty_get(chunk, run_ind);
	flag_decommitted = arena_mapbits_decommitted_get(chunk, run_ind);
	need_pages = (size >> LG_PAGE);
	assert(need_pages > 0);

	if (flag_decommitted != 0 && arena->chunk_hooks.commit(chunk, chunksize,
	    run_ind << LG_PAGE, size, arena->ind))
		return (true);

	arena_run_split_remove(arena, chunk, run_ind, flag_dirty,
	    flag_decommitted, need_pages);

	for (i = 0; i < need_pages; i++) {
		size_t flag_unzeroed = arena_mapbits_unzeroed_get(chunk,
		    run_ind+i);
		arena_mapbits_small_set(chunk, run_ind+i, i, binind,
		    flag_unzeroed);
		if (config_debug && flag_dirty == 0 && flag_unzeroed == 0)
			arena_run_page_validate_zeroed(chunk, run_ind+i);
	}
	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED((void *)((uintptr_t)chunk +
	    (run_ind << LG_PAGE)), (need_pages << LG_PAGE));
	return (false);
}

static arena_chunk_t *
arena_chunk_init_spare(arena_t *arena)
{
	arena_chunk_t *chunk;

	assert(arena->spare != NULL);

	chunk = arena->spare;
	arena->spare = NULL;

	assert(arena_mapbits_allocated_get(chunk, map_bias) == 0);
	assert(arena_mapbits_allocated_get(chunk, chunk_npages-1) == 0);
	assert(arena_mapbits_unallocated_size_get(chunk, map_bias) ==
	    arena_maxrun);
	assert(arena_mapbits_unallocated_size_get(chunk, chunk_npages-1) ==
	    arena_maxrun);
	assert(arena_mapbits_dirty_get(chunk, map_bias) ==
	    arena_mapbits_dirty_get(chunk, chunk_npages-1));

	return (chunk);
}

static bool
arena_chunk_register(arena_t *arena, arena_chunk_t *chunk, bool zero)
{

	/*
	 * The extent node notion of "committed" doesn't directly apply to
	 * arena chunks.  Arbitrarily mark them as committed.  The commit state
	 * of runs is tracked individually, and upon chunk deallocation the
	 * entire chunk is in a consistent commit state.
	 */
	extent_node_init(&chunk->node, arena, chunk, chunksize, zero, true);
	extent_node_achunk_set(&chunk->node, true);
	return (chunk_register(chunk, &chunk->node));
}

static arena_chunk_t *
arena_chunk_alloc_internal_hard(arena_t *arena, chunk_hooks_t *chunk_hooks,
    bool *zero, bool *commit)
{
	arena_chunk_t *chunk;

	malloc_mutex_unlock(&arena->lock);

	chunk = (arena_chunk_t *)chunk_alloc_wrapper(arena, chunk_hooks, NULL,
	    chunksize, chunksize, zero, commit);
	if (chunk != NULL && !*commit) {
		/* Commit header. */
		if (chunk_hooks->commit(chunk, chunksize, 0, map_bias <<
		    LG_PAGE, arena->ind)) {
			chunk_dalloc_wrapper(arena, chunk_hooks,
			    (void *)chunk, chunksize, *commit);
			chunk = NULL;
		}
	}
	if (chunk != NULL && arena_chunk_register(arena, chunk, *zero)) {
		if (!*commit) {
			/* Undo commit of header. */
			chunk_hooks->decommit(chunk, chunksize, 0, map_bias <<
			    LG_PAGE, arena->ind);
		}
		chunk_dalloc_wrapper(arena, chunk_hooks, (void *)chunk,
		    chunksize, *commit);
		chunk = NULL;
	}

	malloc_mutex_lock(&arena->lock);
	return (chunk);
}

static arena_chunk_t *
arena_chunk_alloc_internal(arena_t *arena, bool *zero, bool *commit)
{
	arena_chunk_t *chunk;
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;

	chunk = chunk_alloc_cache(arena, &chunk_hooks, NULL, chunksize,
	    chunksize, zero, true);
	if (chunk != NULL) {
		if (arena_chunk_register(arena, chunk, *zero)) {
			chunk_dalloc_cache(arena, &chunk_hooks, chunk,
			    chunksize, true);
			return (NULL);
		}
		*commit = true;
	}
	if (chunk == NULL) {
		chunk = arena_chunk_alloc_internal_hard(arena, &chunk_hooks,
		    zero, commit);
	}

	if (config_stats && chunk != NULL) {
		arena->stats.mapped += chunksize;
		arena->stats.metadata_mapped += (map_bias << LG_PAGE);
	}

	return (chunk);
}

static arena_chunk_t *
arena_chunk_init_hard(arena_t *arena)
{
	arena_chunk_t *chunk;
	bool zero, commit;
	size_t flag_unzeroed, flag_decommitted, i;

	assert(arena->spare == NULL);

	zero = false;
	commit = false;
	chunk = arena_chunk_alloc_internal(arena, &zero, &commit);
	if (chunk == NULL)
		return (NULL);

	/*
	 * Initialize the map to contain one maximal free untouched run.  Mark
	 * the pages as zeroed if chunk_alloc() returned a zeroed or decommitted
	 * chunk.
	 */
	flag_unzeroed = (zero || !commit) ? 0 : CHUNK_MAP_UNZEROED;
	flag_decommitted = commit ? 0 : CHUNK_MAP_DECOMMITTED;
	arena_mapbits_unallocated_set(chunk, map_bias, arena_maxrun,
	    flag_unzeroed | flag_decommitted);
	/*
	 * There is no need to initialize the internal page map entries unless
	 * the chunk is not zeroed.
	 */
	if (!zero) {
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(
		    (void *)arena_bitselm_get(chunk, map_bias+1),
		    (size_t)((uintptr_t) arena_bitselm_get(chunk,
		    chunk_npages-1) - (uintptr_t)arena_bitselm_get(chunk,
		    map_bias+1)));
		for (i = map_bias+1; i < chunk_npages-1; i++)
			arena_mapbits_internal_set(chunk, i, flag_unzeroed);
	} else {
		JEMALLOC_VALGRIND_MAKE_MEM_DEFINED((void
		    *)arena_bitselm_get(chunk, map_bias+1), (size_t)((uintptr_t)
		    arena_bitselm_get(chunk, chunk_npages-1) -
		    (uintptr_t)arena_bitselm_get(chunk, map_bias+1)));
		if (config_debug) {
			for (i = map_bias+1; i < chunk_npages-1; i++) {
				assert(arena_mapbits_unzeroed_get(chunk, i) ==
				    flag_unzeroed);
			}
		}
	}
	arena_mapbits_unallocated_set(chunk, chunk_npages-1, arena_maxrun,
	    flag_unzeroed);

	return (chunk);
}

static arena_chunk_t *
arena_chunk_alloc(arena_t *arena)
{
	arena_chunk_t *chunk;

	if (arena->spare != NULL)
		chunk = arena_chunk_init_spare(arena);
	else {
		chunk = arena_chunk_init_hard(arena);
		if (chunk == NULL)
			return (NULL);
	}

	/* Insert the run into the runs_avail tree. */
	arena_avail_insert(arena, chunk, map_bias, chunk_npages-map_bias);

	return (chunk);
}

static void
arena_chunk_dalloc(arena_t *arena, arena_chunk_t *chunk)
{

	assert(arena_mapbits_allocated_get(chunk, map_bias) == 0);
	assert(arena_mapbits_allocated_get(chunk, chunk_npages-1) == 0);
	assert(arena_mapbits_unallocated_size_get(chunk, map_bias) ==
	    arena_maxrun);
	assert(arena_mapbits_unallocated_size_get(chunk, chunk_npages-1) ==
	    arena_maxrun);
	assert(arena_mapbits_dirty_get(chunk, map_bias) ==
	    arena_mapbits_dirty_get(chunk, chunk_npages-1));
	assert(arena_mapbits_decommitted_get(chunk, map_bias) ==
	    arena_mapbits_decommitted_get(chunk, chunk_npages-1));

	/*
	 * Remove run from the runs_avail tree, so that the arena does not use
	 * it.
	 */
	arena_avail_remove(arena, chunk, map_bias, chunk_npages-map_bias);

	if (arena->spare != NULL) {
		arena_chunk_t *spare = arena->spare;
		chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;
		bool committed;

		arena->spare = chunk;
		if (arena_mapbits_dirty_get(spare, map_bias) != 0) {
			arena_run_dirty_remove(arena, spare, map_bias,
			    chunk_npages-map_bias);
		}

		chunk_deregister(spare, &spare->node);

		committed = (arena_mapbits_decommitted_get(spare, map_bias) ==
		    0);
		if (!committed) {
			/*
			 * Decommit the header.  Mark the chunk as decommitted
			 * even if header decommit fails, since treating a
			 * partially committed chunk as committed has a high
			 * potential for causing later access of decommitted
			 * memory.
			 */
			chunk_hooks = chunk_hooks_get(arena);
			chunk_hooks.decommit(spare, chunksize, 0, map_bias <<
			    LG_PAGE, arena->ind);
		}

		chunk_dalloc_cache(arena, &chunk_hooks, (void *)spare,
		    chunksize, committed);

		if (config_stats) {
			arena->stats.mapped -= chunksize;
			arena->stats.metadata_mapped -= (map_bias << LG_PAGE);
		}
	} else
		arena->spare = chunk;
}

static void
arena_huge_malloc_stats_update(arena_t *arena, size_t usize)
{
	index_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.nmalloc_huge++;
	arena->stats.allocated_huge += usize;
	arena->stats.hstats[index].nmalloc++;
	arena->stats.hstats[index].curhchunks++;
}

static void
arena_huge_malloc_stats_update_undo(arena_t *arena, size_t usize)
{
	index_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.nmalloc_huge--;
	arena->stats.allocated_huge -= usize;
	arena->stats.hstats[index].nmalloc--;
	arena->stats.hstats[index].curhchunks--;
}

static void
arena_huge_dalloc_stats_update(arena_t *arena, size_t usize)
{
	index_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.ndalloc_huge++;
	arena->stats.allocated_huge -= usize;
	arena->stats.hstats[index].ndalloc++;
	arena->stats.hstats[index].curhchunks--;
}

static void
arena_huge_dalloc_stats_update_undo(arena_t *arena, size_t usize)
{
	index_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.ndalloc_huge--;
	arena->stats.allocated_huge += usize;
	arena->stats.hstats[index].ndalloc--;
	arena->stats.hstats[index].curhchunks++;
}

static void
arena_huge_ralloc_stats_update(arena_t *arena, size_t oldsize, size_t usize)
{

	arena_huge_dalloc_stats_update(arena, oldsize);
	arena_huge_malloc_stats_update(arena, usize);
}

static void
arena_huge_ralloc_stats_update_undo(arena_t *arena, size_t oldsize,
    size_t usize)
{

	arena_huge_dalloc_stats_update_undo(arena, oldsize);
	arena_huge_malloc_stats_update_undo(arena, usize);
}

extent_node_t *
arena_node_alloc(arena_t *arena)
{
	extent_node_t *node;

	malloc_mutex_lock(&arena->node_cache_mtx);
	node = ql_last(&arena->node_cache, ql_link);
	if (node == NULL) {
		malloc_mutex_unlock(&arena->node_cache_mtx);
		return (base_alloc(sizeof(extent_node_t)));
	}
	ql_tail_remove(&arena->node_cache, extent_node_t, ql_link);
	malloc_mutex_unlock(&arena->node_cache_mtx);
	return (node);
}

void
arena_node_dalloc(arena_t *arena, extent_node_t *node)
{

	malloc_mutex_lock(&arena->node_cache_mtx);
	ql_elm_new(node, ql_link);
	ql_tail_insert(&arena->node_cache, node, ql_link);
	malloc_mutex_unlock(&arena->node_cache_mtx);
}

static void *
arena_chunk_alloc_huge_hard(arena_t *arena, chunk_hooks_t *chunk_hooks,
    size_t usize, size_t alignment, bool *zero, size_t csize)
{
	void *ret;
	bool commit = true;

	ret = chunk_alloc_wrapper(arena, chunk_hooks, NULL, csize, alignment,
	    zero, &commit);
	if (ret == NULL) {
		/* Revert optimistic stats updates. */
		malloc_mutex_lock(&arena->lock);
		if (config_stats) {
			arena_huge_malloc_stats_update_undo(arena, usize);
			arena->stats.mapped -= usize;
		}
		arena->nactive -= (usize >> LG_PAGE);
		malloc_mutex_unlock(&arena->lock);
	}

	return (ret);
}

void *
arena_chunk_alloc_huge(arena_t *arena, size_t usize, size_t alignment,
    bool *zero)
{
	void *ret;
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;
	size_t csize = CHUNK_CEILING(usize);

	malloc_mutex_lock(&arena->lock);

	/* Optimistically update stats. */
	if (config_stats) {
		arena_huge_malloc_stats_update(arena, usize);
		arena->stats.mapped += usize;
	}
	arena->nactive += (usize >> LG_PAGE);

	ret = chunk_alloc_cache(arena, &chunk_hooks, NULL, csize, alignment,
	    zero, true);
	malloc_mutex_unlock(&arena->lock);
	if (ret == NULL) {
		ret = arena_chunk_alloc_huge_hard(arena, &chunk_hooks, usize,
		    alignment, zero, csize);
	}

	if (config_stats && ret != NULL)
		stats_cactive_add(usize);
	return (ret);
}

void
arena_chunk_dalloc_huge(arena_t *arena, void *chunk, size_t usize)
{
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;
	size_t csize;

	csize = CHUNK_CEILING(usize);
	malloc_mutex_lock(&arena->lock);
	if (config_stats) {
		arena_huge_dalloc_stats_update(arena, usize);
		arena->stats.mapped -= usize;
		stats_cactive_sub(usize);
	}
	arena->nactive -= (usize >> LG_PAGE);

	chunk_dalloc_cache(arena, &chunk_hooks, chunk, csize, true);
	malloc_mutex_unlock(&arena->lock);
}

void
arena_chunk_ralloc_huge_similar(arena_t *arena, void *chunk, size_t oldsize,
    size_t usize)
{

	assert(CHUNK_CEILING(oldsize) == CHUNK_CEILING(usize));
	assert(oldsize != usize);

	malloc_mutex_lock(&arena->lock);
	if (config_stats)
		arena_huge_ralloc_stats_update(arena, oldsize, usize);
	if (oldsize < usize) {
		size_t udiff = usize - oldsize;
		arena->nactive += udiff >> LG_PAGE;
		if (config_stats)
			stats_cactive_add(udiff);
	} else {
		size_t udiff = oldsize - usize;
		arena->nactive -= udiff >> LG_PAGE;
		if (config_stats)
			stats_cactive_sub(udiff);
	}
	malloc_mutex_unlock(&arena->lock);
}

void
arena_chunk_ralloc_huge_shrink(arena_t *arena, void *chunk, size_t oldsize,
    size_t usize)
{
	size_t udiff = oldsize - usize;
	size_t cdiff = CHUNK_CEILING(oldsize) - CHUNK_CEILING(usize);

	malloc_mutex_lock(&arena->lock);
	if (config_stats) {
		arena_huge_ralloc_stats_update(arena, oldsize, usize);
		if (cdiff != 0) {
			arena->stats.mapped -= cdiff;
			stats_cactive_sub(udiff);
		}
	}
	arena->nactive -= udiff >> LG_PAGE;

	if (cdiff != 0) {
		chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;
		void *nchunk = (void *)((uintptr_t)chunk +
		    CHUNK_CEILING(usize));

		chunk_dalloc_cache(arena, &chunk_hooks, nchunk, cdiff, true);
	}
	malloc_mutex_unlock(&arena->lock);
}

static bool
arena_chunk_ralloc_huge_expand_hard(arena_t *arena, chunk_hooks_t *chunk_hooks,
    void *chunk, size_t oldsize, size_t usize, bool *zero, void *nchunk,
    size_t udiff, size_t cdiff)
{
	bool err;
	bool commit = true;

	err = (chunk_alloc_wrapper(arena, chunk_hooks, nchunk, cdiff, chunksize,
	    zero, &commit) == NULL);
	if (err) {
		/* Revert optimistic stats updates. */
		malloc_mutex_lock(&arena->lock);
		if (config_stats) {
			arena_huge_ralloc_stats_update_undo(arena, oldsize,
			    usize);
			arena->stats.mapped -= cdiff;
		}
		arena->nactive -= (udiff >> LG_PAGE);
		malloc_mutex_unlock(&arena->lock);
	} else if (chunk_hooks->merge(chunk, CHUNK_CEILING(oldsize), nchunk,
	    cdiff, true, arena->ind)) {
		chunk_dalloc_arena(arena, chunk_hooks, nchunk, cdiff, *zero,
		    true);
		err = true;
	}
	return (err);
}

bool
arena_chunk_ralloc_huge_expand(arena_t *arena, void *chunk, size_t oldsize,
    size_t usize, bool *zero)
{
	bool err;
	chunk_hooks_t chunk_hooks = chunk_hooks_get(arena);
	void *nchunk = (void *)((uintptr_t)chunk + CHUNK_CEILING(oldsize));
	size_t udiff = usize - oldsize;
	size_t cdiff = CHUNK_CEILING(usize) - CHUNK_CEILING(oldsize);

	malloc_mutex_lock(&arena->lock);

	/* Optimistically update stats. */
	if (config_stats) {
		arena_huge_ralloc_stats_update(arena, oldsize, usize);
		arena->stats.mapped += cdiff;
	}
	arena->nactive += (udiff >> LG_PAGE);

	err = (chunk_alloc_cache(arena, &arena->chunk_hooks, nchunk, cdiff,
	    chunksize, zero, true) == NULL);
	malloc_mutex_unlock(&arena->lock);
	if (err) {
		err = arena_chunk_ralloc_huge_expand_hard(arena, &chunk_hooks,
		    chunk, oldsize, usize, zero, nchunk, udiff,
		    cdiff);
	} else if (chunk_hooks.merge(chunk, CHUNK_CEILING(oldsize), nchunk,
	    cdiff, true, arena->ind)) {
		chunk_dalloc_arena(arena, &chunk_hooks, nchunk, cdiff, *zero,
		    true);
		err = true;
	}

	if (config_stats && !err)
		stats_cactive_add(udiff);
	return (err);
}

/*
 * Do first-best-fit run selection, i.e. select the lowest run that best fits.
 * Run sizes are quantized, so not all candidate runs are necessarily exactly
 * the same size.
 */
static arena_run_t *
arena_run_first_best_fit(arena_t *arena, size_t size)
{
	size_t search_size = run_quantize_first(size);
	arena_chunk_map_misc_t *key = arena_miscelm_key_create(search_size);
	arena_chunk_map_misc_t *miscelm =
	    arena_avail_tree_nsearch(&arena->runs_avail, key);
	if (miscelm == NULL)
		return (NULL);
	return (&miscelm->run);
}

static arena_run_t *
arena_run_alloc_large_helper(arena_t *arena, size_t size, bool zero)
{
	arena_run_t *run = arena_run_first_best_fit(arena, s2u(size));
	if (run != NULL) {
		if (arena_run_split_large(arena, run, size, zero))
			run = NULL;
	}
	return (run);
}

static arena_run_t *
arena_run_alloc_large(arena_t *arena, size_t size, bool zero)
{
	arena_chunk_t *chunk;
	arena_run_t *run;

	assert(size <= arena_maxrun);
	assert(size == PAGE_CEILING(size));

	/* Search the arena's chunks for the lowest best fit. */
	run = arena_run_alloc_large_helper(arena, size, zero);
	if (run != NULL)
		return (run);

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	chunk = arena_chunk_alloc(arena);
	if (chunk != NULL) {
		run = &arena_miscelm_get(chunk, map_bias)->run;
		if (arena_run_split_large(arena, run, size, zero))
			run = NULL;
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	return (arena_run_alloc_large_helper(arena, size, zero));
}

static arena_run_t *
arena_run_alloc_small_helper(arena_t *arena, size_t size, index_t binind)
{
	arena_run_t *run = arena_run_first_best_fit(arena, size);
	if (run != NULL) {
		if (arena_run_split_small(arena, run, size, binind))
			run = NULL;
	}
	return (run);
}

static arena_run_t *
arena_run_alloc_small(arena_t *arena, size_t size, index_t binind)
{
	arena_chunk_t *chunk;
	arena_run_t *run;

	assert(size <= arena_maxrun);
	assert(size == PAGE_CEILING(size));
	assert(binind != BININD_INVALID);

	/* Search the arena's chunks for the lowest best fit. */
	run = arena_run_alloc_small_helper(arena, size, binind);
	if (run != NULL)
		return (run);

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	chunk = arena_chunk_alloc(arena);
	if (chunk != NULL) {
		run = &arena_miscelm_get(chunk, map_bias)->run;
		if (arena_run_split_small(arena, run, size, binind))
			run = NULL;
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	return (arena_run_alloc_small_helper(arena, size, binind));
}

static bool
arena_lg_dirty_mult_valid(ssize_t lg_dirty_mult)
{

	return (lg_dirty_mult >= -1 && lg_dirty_mult < (ssize_t)(sizeof(size_t)
	    << 3));
}

ssize_t
arena_lg_dirty_mult_get(arena_t *arena)
{
	ssize_t lg_dirty_mult;

	malloc_mutex_lock(&arena->lock);
	lg_dirty_mult = arena->lg_dirty_mult;
	malloc_mutex_unlock(&arena->lock);

	return (lg_dirty_mult);
}

bool
arena_lg_dirty_mult_set(arena_t *arena, ssize_t lg_dirty_mult)
{

	if (!arena_lg_dirty_mult_valid(lg_dirty_mult))
		return (true);

	malloc_mutex_lock(&arena->lock);
	arena->lg_dirty_mult = lg_dirty_mult;
	arena_maybe_purge(arena);
	malloc_mutex_unlock(&arena->lock);

	return (false);
}

void
arena_maybe_purge(arena_t *arena)
{

	/* Don't purge if the option is disabled. */
	if (arena->lg_dirty_mult < 0)
		return;
	/* Don't recursively purge. */
	if (arena->purging)
		return;
	/*
	 * Iterate, since preventing recursive purging could otherwise leave too
	 * many dirty pages.
	 */
	while (true) {
		size_t threshold = (arena->nactive >> arena->lg_dirty_mult);
		if (threshold < chunk_npages)
			threshold = chunk_npages;
		/*
		 * Don't purge unless the number of purgeable pages exceeds the
		 * threshold.
		 */
		if (arena->ndirty <= threshold)
			return;
		arena_purge(arena, false);
	}
}

static size_t
arena_dirty_count(arena_t *arena)
{
	size_t ndirty = 0;
	arena_runs_dirty_link_t *rdelm;
	extent_node_t *chunkselm;

	for (rdelm = qr_next(&arena->runs_dirty, rd_link),
	    chunkselm = qr_next(&arena->chunks_cache, cc_link);
	    rdelm != &arena->runs_dirty; rdelm = qr_next(rdelm, rd_link)) {
		size_t npages;

		if (rdelm == &chunkselm->rd) {
			npages = extent_node_size_get(chunkselm) >> LG_PAGE;
			chunkselm = qr_next(chunkselm, cc_link);
		} else {
			arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(
			    rdelm);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(rdelm);
			size_t pageind = arena_miscelm_to_pageind(miscelm);
			assert(arena_mapbits_allocated_get(chunk, pageind) ==
			    0);
			assert(arena_mapbits_large_get(chunk, pageind) == 0);
			assert(arena_mapbits_dirty_get(chunk, pageind) != 0);
			npages = arena_mapbits_unallocated_size_get(chunk,
			    pageind) >> LG_PAGE;
		}
		ndirty += npages;
	}

	return (ndirty);
}

static size_t
arena_compute_npurge(arena_t *arena, bool all)
{
	size_t npurge;

	/*
	 * Compute the minimum number of pages that this thread should try to
	 * purge.
	 */
	if (!all) {
		size_t threshold = (arena->nactive >> arena->lg_dirty_mult);
		threshold = threshold < chunk_npages ? chunk_npages : threshold;

		npurge = arena->ndirty - threshold;
	} else
		npurge = arena->ndirty;

	return (npurge);
}

static size_t
arena_stash_dirty(arena_t *arena, chunk_hooks_t *chunk_hooks, bool all,
    size_t npurge, arena_runs_dirty_link_t *purge_runs_sentinel,
    extent_node_t *purge_chunks_sentinel)
{
	arena_runs_dirty_link_t *rdelm, *rdelm_next;
	extent_node_t *chunkselm;
	size_t nstashed = 0;

	/* Stash at least npurge pages. */
	for (rdelm = qr_next(&arena->runs_dirty, rd_link),
	    chunkselm = qr_next(&arena->chunks_cache, cc_link);
	    rdelm != &arena->runs_dirty; rdelm = rdelm_next) {
		size_t npages;
		rdelm_next = qr_next(rdelm, rd_link);

		if (rdelm == &chunkselm->rd) {
			extent_node_t *chunkselm_next;
			bool zero;
			UNUSED void *chunk;

			chunkselm_next = qr_next(chunkselm, cc_link);
			/*
			 * Allocate.  chunkselm remains valid due to the
			 * dalloc_node=false argument to chunk_alloc_cache().
			 */
			zero = false;
			chunk = chunk_alloc_cache(arena, chunk_hooks,
			    extent_node_addr_get(chunkselm),
			    extent_node_size_get(chunkselm), chunksize, &zero,
			    false);
			assert(chunk == extent_node_addr_get(chunkselm));
			assert(zero == extent_node_zeroed_get(chunkselm));
			extent_node_dirty_insert(chunkselm, purge_runs_sentinel,
			    purge_chunks_sentinel);
			npages = extent_node_size_get(chunkselm) >> LG_PAGE;
			chunkselm = chunkselm_next;
		} else {
			arena_chunk_t *chunk =
			    (arena_chunk_t *)CHUNK_ADDR2BASE(rdelm);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(rdelm);
			size_t pageind = arena_miscelm_to_pageind(miscelm);
			arena_run_t *run = &miscelm->run;
			size_t run_size =
			    arena_mapbits_unallocated_size_get(chunk, pageind);

			npages = run_size >> LG_PAGE;

			assert(pageind + npages <= chunk_npages);
			assert(arena_mapbits_dirty_get(chunk, pageind) ==
			    arena_mapbits_dirty_get(chunk, pageind+npages-1));

			/*
			 * If purging the spare chunk's run, make it available
			 * prior to allocation.
			 */
			if (chunk == arena->spare)
				arena_chunk_alloc(arena);

			/* Temporarily allocate the free dirty run. */
			arena_run_split_large(arena, run, run_size, false);
			/* Stash. */
			if (false)
				qr_new(rdelm, rd_link); /* Redundant. */
			else {
				assert(qr_next(rdelm, rd_link) == rdelm);
				assert(qr_prev(rdelm, rd_link) == rdelm);
			}
			qr_meld(purge_runs_sentinel, rdelm, rd_link);
		}

		nstashed += npages;
		if (!all && nstashed >= npurge)
			break;
	}

	return (nstashed);
}

static size_t
arena_purge_stashed(arena_t *arena, chunk_hooks_t *chunk_hooks,
    arena_runs_dirty_link_t *purge_runs_sentinel,
    extent_node_t *purge_chunks_sentinel)
{
	size_t npurged, nmadvise;
	arena_runs_dirty_link_t *rdelm;
	extent_node_t *chunkselm;

	if (config_stats)
		nmadvise = 0;
	npurged = 0;

	malloc_mutex_unlock(&arena->lock);
	for (rdelm = qr_next(purge_runs_sentinel, rd_link),
	    chunkselm = qr_next(purge_chunks_sentinel, cc_link);
	    rdelm != purge_runs_sentinel; rdelm = qr_next(rdelm, rd_link)) {
		size_t npages;

		if (rdelm == &chunkselm->rd) {
			/*
			 * Don't actually purge the chunk here because 1)
			 * chunkselm is embedded in the chunk and must remain
			 * valid, and 2) we deallocate the chunk in
			 * arena_unstash_purged(), where it is destroyed,
			 * decommitted, or purged, depending on chunk
			 * deallocation policy.
			 */
			size_t size = extent_node_size_get(chunkselm);
			npages = size >> LG_PAGE;
			chunkselm = qr_next(chunkselm, cc_link);
		} else {
			size_t pageind, run_size, flag_unzeroed, flags, i;
			bool decommitted;
			arena_chunk_t *chunk =
			    (arena_chunk_t *)CHUNK_ADDR2BASE(rdelm);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(rdelm);
			pageind = arena_miscelm_to_pageind(miscelm);
			run_size = arena_mapbits_large_size_get(chunk, pageind);
			npages = run_size >> LG_PAGE;

			assert(pageind + npages <= chunk_npages);
			assert(!arena_mapbits_decommitted_get(chunk, pageind));
			assert(!arena_mapbits_decommitted_get(chunk,
			    pageind+npages-1));
			decommitted = !chunk_hooks->decommit(chunk, chunksize,
			    pageind << LG_PAGE, npages << LG_PAGE, arena->ind);
			if (decommitted) {
				flag_unzeroed = 0;
				flags = CHUNK_MAP_DECOMMITTED;
			} else {
				flag_unzeroed = chunk_purge_wrapper(arena,
				    chunk_hooks, chunk, chunksize, pageind <<
				    LG_PAGE, run_size) ? CHUNK_MAP_UNZEROED : 0;
				flags = flag_unzeroed;
			}
			arena_mapbits_large_set(chunk, pageind+npages-1, 0,
			    flags);
			arena_mapbits_large_set(chunk, pageind, run_size,
			    flags);

			/*
			 * Set the unzeroed flag for internal pages, now that
			 * chunk_purge_wrapper() has returned whether the pages
			 * were zeroed as a side effect of purging.  This chunk
			 * map modification is safe even though the arena mutex
			 * isn't currently owned by this thread, because the run
			 * is marked as allocated, thus protecting it from being
			 * modified by any other thread.  As long as these
			 * writes don't perturb the first and last elements'
			 * CHUNK_MAP_ALLOCATED bits, behavior is well defined.
			 */
			for (i = 1; i < npages-1; i++) {
				arena_mapbits_internal_set(chunk, pageind+i,
				    flag_unzeroed);
			}
		}

		npurged += npages;
		if (config_stats)
			nmadvise++;
	}
	malloc_mutex_lock(&arena->lock);

	if (config_stats) {
		arena->stats.nmadvise += nmadvise;
		arena->stats.purged += npurged;
	}

	return (npurged);
}

static void
arena_unstash_purged(arena_t *arena, chunk_hooks_t *chunk_hooks,
    arena_runs_dirty_link_t *purge_runs_sentinel,
    extent_node_t *purge_chunks_sentinel)
{
	arena_runs_dirty_link_t *rdelm, *rdelm_next;
	extent_node_t *chunkselm;

	/* Deallocate chunks/runs. */
	for (rdelm = qr_next(purge_runs_sentinel, rd_link),
	    chunkselm = qr_next(purge_chunks_sentinel, cc_link);
	    rdelm != purge_runs_sentinel; rdelm = rdelm_next) {
		rdelm_next = qr_next(rdelm, rd_link);
		if (rdelm == &chunkselm->rd) {
			extent_node_t *chunkselm_next = qr_next(chunkselm,
			    cc_link);
			void *addr = extent_node_addr_get(chunkselm);
			size_t size = extent_node_size_get(chunkselm);
			bool zeroed = extent_node_zeroed_get(chunkselm);
			bool committed = extent_node_committed_get(chunkselm);
			extent_node_dirty_remove(chunkselm);
			arena_node_dalloc(arena, chunkselm);
			chunkselm = chunkselm_next;
			chunk_dalloc_arena(arena, chunk_hooks, addr, size,
			    zeroed, committed);
		} else {
			arena_chunk_t *chunk =
			    (arena_chunk_t *)CHUNK_ADDR2BASE(rdelm);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(rdelm);
			size_t pageind = arena_miscelm_to_pageind(miscelm);
			bool decommitted = (arena_mapbits_decommitted_get(chunk,
			    pageind) != 0);
			arena_run_t *run = &miscelm->run;
			qr_remove(rdelm, rd_link);
			arena_run_dalloc(arena, run, false, true, decommitted);
		}
	}
}

static void
arena_purge(arena_t *arena, bool all)
{
	chunk_hooks_t chunk_hooks = chunk_hooks_get(arena);
	size_t npurge, npurgeable, npurged;
	arena_runs_dirty_link_t purge_runs_sentinel;
	extent_node_t purge_chunks_sentinel;

	arena->purging = true;

	/*
	 * Calls to arena_dirty_count() are disabled even for debug builds
	 * because overhead grows nonlinearly as memory usage increases.
	 */
	if (false && config_debug) {
		size_t ndirty = arena_dirty_count(arena);
		assert(ndirty == arena->ndirty);
	}
	assert((arena->nactive >> arena->lg_dirty_mult) < arena->ndirty || all);

	if (config_stats)
		arena->stats.npurge++;

	npurge = arena_compute_npurge(arena, all);
	qr_new(&purge_runs_sentinel, rd_link);
	extent_node_dirty_linkage_init(&purge_chunks_sentinel);

	npurgeable = arena_stash_dirty(arena, &chunk_hooks, all, npurge,
	    &purge_runs_sentinel, &purge_chunks_sentinel);
	assert(npurgeable >= npurge);
	npurged = arena_purge_stashed(arena, &chunk_hooks, &purge_runs_sentinel,
	    &purge_chunks_sentinel);
	assert(npurged == npurgeable);
	arena_unstash_purged(arena, &chunk_hooks, &purge_runs_sentinel,
	    &purge_chunks_sentinel);

	arena->purging = false;
}

void
arena_purge_all(arena_t *arena)
{

	malloc_mutex_lock(&arena->lock);
	arena_purge(arena, true);
	malloc_mutex_unlock(&arena->lock);
}

static void
arena_run_coalesce(arena_t *arena, arena_chunk_t *chunk, size_t *p_size,
    size_t *p_run_ind, size_t *p_run_pages, size_t flag_dirty,
    size_t flag_decommitted)
{
	size_t size = *p_size;
	size_t run_ind = *p_run_ind;
	size_t run_pages = *p_run_pages;

	/* Try to coalesce forward. */
	if (run_ind + run_pages < chunk_npages &&
	    arena_mapbits_allocated_get(chunk, run_ind+run_pages) == 0 &&
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages) == flag_dirty &&
	    arena_mapbits_decommitted_get(chunk, run_ind+run_pages) ==
	    flag_decommitted) {
		size_t nrun_size = arena_mapbits_unallocated_size_get(chunk,
		    run_ind+run_pages);
		size_t nrun_pages = nrun_size >> LG_PAGE;

		/*
		 * Remove successor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		assert(arena_mapbits_unallocated_size_get(chunk,
		    run_ind+run_pages+nrun_pages-1) == nrun_size);
		assert(arena_mapbits_dirty_get(chunk,
		    run_ind+run_pages+nrun_pages-1) == flag_dirty);
		assert(arena_mapbits_decommitted_get(chunk,
		    run_ind+run_pages+nrun_pages-1) == flag_decommitted);
		arena_avail_remove(arena, chunk, run_ind+run_pages, nrun_pages);

		/*
		 * If the successor is dirty, remove it from the set of dirty
		 * pages.
		 */
		if (flag_dirty != 0) {
			arena_run_dirty_remove(arena, chunk, run_ind+run_pages,
			    nrun_pages);
		}

		size += nrun_size;
		run_pages += nrun_pages;

		arena_mapbits_unallocated_size_set(chunk, run_ind, size);
		arena_mapbits_unallocated_size_set(chunk, run_ind+run_pages-1,
		    size);
	}

	/* Try to coalesce backward. */
	if (run_ind > map_bias && arena_mapbits_allocated_get(chunk,
	    run_ind-1) == 0 && arena_mapbits_dirty_get(chunk, run_ind-1) ==
	    flag_dirty && arena_mapbits_decommitted_get(chunk, run_ind-1) ==
	    flag_decommitted) {
		size_t prun_size = arena_mapbits_unallocated_size_get(chunk,
		    run_ind-1);
		size_t prun_pages = prun_size >> LG_PAGE;

		run_ind -= prun_pages;

		/*
		 * Remove predecessor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		assert(arena_mapbits_unallocated_size_get(chunk, run_ind) ==
		    prun_size);
		assert(arena_mapbits_dirty_get(chunk, run_ind) == flag_dirty);
		assert(arena_mapbits_decommitted_get(chunk, run_ind) ==
		    flag_decommitted);
		arena_avail_remove(arena, chunk, run_ind, prun_pages);

		/*
		 * If the predecessor is dirty, remove it from the set of dirty
		 * pages.
		 */
		if (flag_dirty != 0) {
			arena_run_dirty_remove(arena, chunk, run_ind,
			    prun_pages);
		}

		size += prun_size;
		run_pages += prun_pages;

		arena_mapbits_unallocated_size_set(chunk, run_ind, size);
		arena_mapbits_unallocated_size_set(chunk, run_ind+run_pages-1,
		    size);
	}

	*p_size = size;
	*p_run_ind = run_ind;
	*p_run_pages = run_pages;
}

static size_t
arena_run_size_get(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t run_ind)
{
	size_t size;

	assert(run_ind >= map_bias);
	assert(run_ind < chunk_npages);

	if (arena_mapbits_large_get(chunk, run_ind) != 0) {
		size = arena_mapbits_large_size_get(chunk, run_ind);
		assert(size == PAGE || arena_mapbits_large_size_get(chunk,
		    run_ind+(size>>LG_PAGE)-1) == 0);
	} else {
		arena_bin_info_t *bin_info = &arena_bin_info[run->binind];
		size = bin_info->run_size;
	}

	return (size);
}

static bool
arena_run_decommit(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);
	size_t run_ind = arena_miscelm_to_pageind(miscelm);
	size_t offset = run_ind << LG_PAGE;
	size_t length = arena_run_size_get(arena, chunk, run, run_ind);

	return (arena->chunk_hooks.decommit(chunk, chunksize, offset, length,
	    arena->ind));
}

static void
arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty, bool cleaned,
    bool decommitted)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t size, run_ind, run_pages, flag_dirty, flag_decommitted;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	run_ind = arena_miscelm_to_pageind(miscelm);
	assert(run_ind >= map_bias);
	assert(run_ind < chunk_npages);
	size = arena_run_size_get(arena, chunk, run, run_ind);
	run_pages = (size >> LG_PAGE);
	arena_cactive_update(arena, 0, run_pages);
	arena->nactive -= run_pages;

	/*
	 * The run is dirty if the caller claims to have dirtied it, as well as
	 * if it was already dirty before being allocated and the caller
	 * doesn't claim to have cleaned it.
	 */
	assert(arena_mapbits_dirty_get(chunk, run_ind) ==
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages-1));
	if (!cleaned && !decommitted && arena_mapbits_dirty_get(chunk, run_ind)
	    != 0)
		dirty = true;
	flag_dirty = dirty ? CHUNK_MAP_DIRTY : 0;
	flag_decommitted = decommitted ? CHUNK_MAP_DECOMMITTED : 0;

	/* Mark pages as unallocated in the chunk map. */
	if (dirty || decommitted) {
		size_t flags = flag_dirty | flag_decommitted;
		arena_mapbits_unallocated_set(chunk, run_ind, size, flags);
		arena_mapbits_unallocated_set(chunk, run_ind+run_pages-1, size,
		    flags);
	} else {
		arena_mapbits_unallocated_set(chunk, run_ind, size,
		    arena_mapbits_unzeroed_get(chunk, run_ind));
		arena_mapbits_unallocated_set(chunk, run_ind+run_pages-1, size,
		    arena_mapbits_unzeroed_get(chunk, run_ind+run_pages-1));
	}

	arena_run_coalesce(arena, chunk, &size, &run_ind, &run_pages,
	    flag_dirty, flag_decommitted);

	/* Insert into runs_avail, now that coalescing is complete. */
	assert(arena_mapbits_unallocated_size_get(chunk, run_ind) ==
	    arena_mapbits_unallocated_size_get(chunk, run_ind+run_pages-1));
	assert(arena_mapbits_dirty_get(chunk, run_ind) ==
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages-1));
	assert(arena_mapbits_decommitted_get(chunk, run_ind) ==
	    arena_mapbits_decommitted_get(chunk, run_ind+run_pages-1));
	arena_avail_insert(arena, chunk, run_ind, run_pages);

	if (dirty)
		arena_run_dirty_insert(arena, chunk, run_ind, run_pages);

	/* Deallocate chunk if it is now completely unused. */
	if (size == arena_maxrun) {
		assert(run_ind == map_bias);
		assert(run_pages == (arena_maxrun >> LG_PAGE));
		arena_chunk_dalloc(arena, chunk);
	}

	/*
	 * It is okay to do dirty page processing here even if the chunk was
	 * deallocated above, since in that case it is the spare.  Waiting
	 * until after possible chunk deallocation to do dirty processing
	 * allows for an old spare to be fully deallocated, thus decreasing the
	 * chances of spuriously crossing the dirty page purging threshold.
	 */
	if (dirty)
		arena_maybe_purge(arena);
}

static void
arena_run_dalloc_decommit(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run)
{
	bool committed = arena_run_decommit(arena, chunk, run);

	arena_run_dalloc(arena, run, committed, false, !committed);
}

static void
arena_run_trim_head(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);
	size_t pageind = arena_miscelm_to_pageind(miscelm);
	size_t head_npages = (oldsize - newsize) >> LG_PAGE;
	size_t flag_dirty = arena_mapbits_dirty_get(chunk, pageind);
	size_t flag_decommitted = arena_mapbits_decommitted_get(chunk, pageind);
	size_t flag_unzeroed_mask = (flag_dirty | flag_decommitted) == 0 ?
	    CHUNK_MAP_UNZEROED : 0;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * leading run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert(arena_mapbits_large_size_get(chunk, pageind) == oldsize);
	arena_mapbits_large_set(chunk, pageind+head_npages-1, 0, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages-1)));
	arena_mapbits_large_set(chunk, pageind, oldsize-newsize, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk, pageind)));

	if (config_debug) {
		UNUSED size_t tail_npages = newsize >> LG_PAGE;
		assert(arena_mapbits_large_size_get(chunk,
		    pageind+head_npages+tail_npages-1) == 0);
		assert(arena_mapbits_dirty_get(chunk,
		    pageind+head_npages+tail_npages-1) == flag_dirty);
	}
	arena_mapbits_large_set(chunk, pageind+head_npages, newsize,
	    flag_dirty | (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages)));

	arena_run_dalloc(arena, run, false, false, (flag_decommitted != 0));
}

static void
arena_run_trim_tail(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize, bool dirty)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);
	size_t pageind = arena_miscelm_to_pageind(miscelm);
	size_t head_npages = newsize >> LG_PAGE;
	size_t flag_dirty = arena_mapbits_dirty_get(chunk, pageind);
	size_t flag_decommitted = arena_mapbits_decommitted_get(chunk, pageind);
	size_t flag_unzeroed_mask = (flag_dirty | flag_decommitted) == 0 ?
	    CHUNK_MAP_UNZEROED : 0;
	arena_chunk_map_misc_t *tail_miscelm;
	arena_run_t *tail_run;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * trailing run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert(arena_mapbits_large_size_get(chunk, pageind) == oldsize);
	arena_mapbits_large_set(chunk, pageind+head_npages-1, 0, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages-1)));
	arena_mapbits_large_set(chunk, pageind, newsize, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk, pageind)));

	if (config_debug) {
		UNUSED size_t tail_npages = (oldsize - newsize) >> LG_PAGE;
		assert(arena_mapbits_large_size_get(chunk,
		    pageind+head_npages+tail_npages-1) == 0);
		assert(arena_mapbits_dirty_get(chunk,
		    pageind+head_npages+tail_npages-1) == flag_dirty);
	}
	arena_mapbits_large_set(chunk, pageind+head_npages, oldsize-newsize,
	    flag_dirty | (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages)));

	tail_miscelm = arena_miscelm_get(chunk, pageind + head_npages);
	tail_run = &tail_miscelm->run;
	arena_run_dalloc(arena, tail_run, dirty, false, (flag_decommitted !=
	    0));
}

static arena_run_t *
arena_bin_runs_first(arena_bin_t *bin)
{
	arena_chunk_map_misc_t *miscelm = arena_run_tree_first(&bin->runs);
	if (miscelm != NULL)
		return (&miscelm->run);

	return (NULL);
}

static void
arena_bin_runs_insert(arena_bin_t *bin, arena_run_t *run)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);

	assert(arena_run_tree_search(&bin->runs, miscelm) == NULL);

	arena_run_tree_insert(&bin->runs, miscelm);
}

static void
arena_bin_runs_remove(arena_bin_t *bin, arena_run_t *run)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);

	assert(arena_run_tree_search(&bin->runs, miscelm) != NULL);

	arena_run_tree_remove(&bin->runs, miscelm);
}

static arena_run_t *
arena_bin_nonfull_run_tryget(arena_bin_t *bin)
{
	arena_run_t *run = arena_bin_runs_first(bin);
	if (run != NULL) {
		arena_bin_runs_remove(bin, run);
		if (config_stats)
			bin->stats.reruns++;
	}
	return (run);
}

static arena_run_t *
arena_bin_nonfull_run_get(arena_t *arena, arena_bin_t *bin)
{
	arena_run_t *run;
	index_t binind;
	arena_bin_info_t *bin_info;

	/* Look for a usable run. */
	run = arena_bin_nonfull_run_tryget(bin);
	if (run != NULL)
		return (run);
	/* No existing runs have any space available. */

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];

	/* Allocate a new run. */
	malloc_mutex_unlock(&bin->lock);
	/******************************/
	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc_small(arena, bin_info->run_size, binind);
	if (run != NULL) {
		/* Initialize run internals. */
		run->binind = binind;
		run->nfree = bin_info->nregs;
		bitmap_init(run->bitmap, &bin_info->bitmap_info);
	}
	malloc_mutex_unlock(&arena->lock);
	/********************************/
	malloc_mutex_lock(&bin->lock);
	if (run != NULL) {
		if (config_stats) {
			bin->stats.nruns++;
			bin->stats.curruns++;
		}
		return (run);
	}

	/*
	 * arena_run_alloc_small() failed, but another thread may have made
	 * sufficient memory available while this one dropped bin->lock above,
	 * so search one more time.
	 */
	run = arena_bin_nonfull_run_tryget(bin);
	if (run != NULL)
		return (run);

	return (NULL);
}

/* Re-fill bin->runcur, then call arena_run_reg_alloc(). */
static void *
arena_bin_malloc_hard(arena_t *arena, arena_bin_t *bin)
{
	void *ret;
	index_t binind;
	arena_bin_info_t *bin_info;
	arena_run_t *run;

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];
	bin->runcur = NULL;
	run = arena_bin_nonfull_run_get(arena, bin);
	if (bin->runcur != NULL && bin->runcur->nfree > 0) {
		/*
		 * Another thread updated runcur while this one ran without the
		 * bin lock in arena_bin_nonfull_run_get().
		 */
		assert(bin->runcur->nfree > 0);
		ret = arena_run_reg_alloc(bin->runcur, bin_info);
		if (run != NULL) {
			arena_chunk_t *chunk;

			/*
			 * arena_run_alloc_small() may have allocated run, or
			 * it may have pulled run from the bin's run tree.
			 * Therefore it is unsafe to make any assumptions about
			 * how run has previously been used, and
			 * arena_bin_lower_run() must be called, as if a region
			 * were just deallocated from the run.
			 */
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
			if (run->nfree == bin_info->nregs)
				arena_dalloc_bin_run(arena, chunk, run, bin);
			else
				arena_bin_lower_run(arena, chunk, run, bin);
		}
		return (ret);
	}

	if (run == NULL)
		return (NULL);

	bin->runcur = run;

	assert(bin->runcur->nfree > 0);

	return (arena_run_reg_alloc(bin->runcur, bin_info));
}

void
arena_tcache_fill_small(arena_t *arena, tcache_bin_t *tbin, index_t binind,
    uint64_t prof_accumbytes)
{
	unsigned i, nfill;
	arena_bin_t *bin;
	arena_run_t *run;
	void *ptr;

	assert(tbin->ncached == 0);

	if (config_prof && arena_prof_accum(arena, prof_accumbytes))
		prof_idump();
	bin = &arena->bins[binind];
	malloc_mutex_lock(&bin->lock);
	for (i = 0, nfill = (tcache_bin_info[binind].ncached_max >>
	    tbin->lg_fill_div); i < nfill; i++) {
		if ((run = bin->runcur) != NULL && run->nfree > 0)
			ptr = arena_run_reg_alloc(run, &arena_bin_info[binind]);
		else
			ptr = arena_bin_malloc_hard(arena, bin);
		if (ptr == NULL) {
			/*
			 * OOM.  tbin->avail isn't yet filled down to its first
			 * element, so the successful allocations (if any) must
			 * be moved to the base of tbin->avail before bailing
			 * out.
			 */
			if (i > 0) {
				memmove(tbin->avail, &tbin->avail[nfill - i],
				    i * sizeof(void *));
			}
			break;
		}
		if (config_fill && unlikely(opt_junk_alloc)) {
			arena_alloc_junk_small(ptr, &arena_bin_info[binind],
			    true);
		}
		/* Insert such that low regions get used first. */
		tbin->avail[nfill - 1 - i] = ptr;
	}
	if (config_stats) {
		bin->stats.nmalloc += i;
		bin->stats.nrequests += tbin->tstats.nrequests;
		bin->stats.curregs += i;
		bin->stats.nfills++;
		tbin->tstats.nrequests = 0;
	}
	malloc_mutex_unlock(&bin->lock);
	tbin->ncached = i;
}

void
arena_alloc_junk_small(void *ptr, arena_bin_info_t *bin_info, bool zero)
{

	if (zero) {
		size_t redzone_size = bin_info->redzone_size;
		memset((void *)((uintptr_t)ptr - redzone_size), 0xa5,
		    redzone_size);
		memset((void *)((uintptr_t)ptr + bin_info->reg_size), 0xa5,
		    redzone_size);
	} else {
		memset((void *)((uintptr_t)ptr - bin_info->redzone_size), 0xa5,
		    bin_info->reg_interval);
	}
}

#ifdef JEMALLOC_JET
#undef arena_redzone_corruption
#define	arena_redzone_corruption JEMALLOC_N(arena_redzone_corruption_impl)
#endif
static void
arena_redzone_corruption(void *ptr, size_t usize, bool after,
    size_t offset, uint8_t byte)
{

	malloc_printf("<jemalloc>: Corrupt redzone %zu byte%s %s %p "
	    "(size %zu), byte=%#x\n", offset, (offset == 1) ? "" : "s",
	    after ? "after" : "before", ptr, usize, byte);
}
#ifdef JEMALLOC_JET
#undef arena_redzone_corruption
#define	arena_redzone_corruption JEMALLOC_N(arena_redzone_corruption)
arena_redzone_corruption_t *arena_redzone_corruption =
    JEMALLOC_N(arena_redzone_corruption_impl);
#endif

static void
arena_redzones_validate(void *ptr, arena_bin_info_t *bin_info, bool reset)
{
	size_t size = bin_info->reg_size;
	size_t redzone_size = bin_info->redzone_size;
	size_t i;
	bool error = false;

	if (opt_junk_alloc) {
		for (i = 1; i <= redzone_size; i++) {
			uint8_t *byte = (uint8_t *)((uintptr_t)ptr - i);
			if (*byte != 0xa5) {
				error = true;
				arena_redzone_corruption(ptr, size, false, i,
				    *byte);
				if (reset)
					*byte = 0xa5;
			}
		}
		for (i = 0; i < redzone_size; i++) {
			uint8_t *byte = (uint8_t *)((uintptr_t)ptr + size + i);
			if (*byte != 0xa5) {
				error = true;
				arena_redzone_corruption(ptr, size, true, i,
				    *byte);
				if (reset)
					*byte = 0xa5;
			}
		}
	}

	if (opt_abort && error)
		abort();
}

#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define	arena_dalloc_junk_small JEMALLOC_N(arena_dalloc_junk_small_impl)
#endif
void
arena_dalloc_junk_small(void *ptr, arena_bin_info_t *bin_info)
{
	size_t redzone_size = bin_info->redzone_size;

	arena_redzones_validate(ptr, bin_info, false);
	memset((void *)((uintptr_t)ptr - redzone_size), 0x5a,
	    bin_info->reg_interval);
}
#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define	arena_dalloc_junk_small JEMALLOC_N(arena_dalloc_junk_small)
arena_dalloc_junk_small_t *arena_dalloc_junk_small =
    JEMALLOC_N(arena_dalloc_junk_small_impl);
#endif

void
arena_quarantine_junk_small(void *ptr, size_t usize)
{
	index_t binind;
	arena_bin_info_t *bin_info;
	cassert(config_fill);
	assert(opt_junk_free);
	assert(opt_quarantine);
	assert(usize <= SMALL_MAXCLASS);

	binind = size2index(usize);
	bin_info = &arena_bin_info[binind];
	arena_redzones_validate(ptr, bin_info, true);
}

void *
arena_malloc_small(arena_t *arena, size_t size, bool zero)
{
	void *ret;
	arena_bin_t *bin;
	arena_run_t *run;
	index_t binind;

	binind = size2index(size);
	assert(binind < NBINS);
	bin = &arena->bins[binind];
	size = index2size(binind);

	malloc_mutex_lock(&bin->lock);
	if ((run = bin->runcur) != NULL && run->nfree > 0)
		ret = arena_run_reg_alloc(run, &arena_bin_info[binind]);
	else
		ret = arena_bin_malloc_hard(arena, bin);

	if (ret == NULL) {
		malloc_mutex_unlock(&bin->lock);
		return (NULL);
	}

	if (config_stats) {
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
		bin->stats.curregs++;
	}
	malloc_mutex_unlock(&bin->lock);
	if (config_prof && !isthreaded && arena_prof_accum(arena, size))
		prof_idump();

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk_alloc)) {
				arena_alloc_junk_small(ret,
				    &arena_bin_info[binind], false);
			} else if (unlikely(opt_zero))
				memset(ret, 0, size);
		}
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, size);
	} else {
		if (config_fill && unlikely(opt_junk_alloc)) {
			arena_alloc_junk_small(ret, &arena_bin_info[binind],
			    true);
		}
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, size);
		memset(ret, 0, size);
	}

	return (ret);
}

void *
arena_malloc_large(arena_t *arena, size_t size, bool zero)
{
	void *ret;
	size_t usize;
	uintptr_t random_offset;
	arena_run_t *run;
	arena_chunk_map_misc_t *miscelm;
	UNUSED bool idump;

	/* Large allocation. */
	usize = s2u(size);
	malloc_mutex_lock(&arena->lock);
	if (config_cache_oblivious) {
		uint64_t r;

		/*
		 * Compute a uniformly distributed offset within the first page
		 * that is a multiple of the cacheline size, e.g. [0 .. 63) * 64
		 * for 4 KiB pages and 64-byte cachelines.
		 */
		prng64(r, LG_PAGE - LG_CACHELINE, arena->offset_state,
		    UINT64_C(6364136223846793009),
		    UINT64_C(1442695040888963409));
		random_offset = ((uintptr_t)r) << LG_CACHELINE;
	} else
		random_offset = 0;
	run = arena_run_alloc_large(arena, usize + large_pad, zero);
	if (run == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
	miscelm = arena_run_to_miscelm(run);
	ret = (void *)((uintptr_t)arena_miscelm_to_rpages(miscelm) +
	    random_offset);
	if (config_stats) {
		index_t index = size2index(usize) - NBINS;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += usize;
		arena->stats.lstats[index].nmalloc++;
		arena->stats.lstats[index].nrequests++;
		arena->stats.lstats[index].curruns++;
	}
	if (config_prof)
		idump = arena_prof_accum_locked(arena, usize);
	malloc_mutex_unlock(&arena->lock);
	if (config_prof && idump)
		prof_idump();

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk_alloc))
				memset(ret, 0xa5, usize);
			else if (unlikely(opt_zero))
				memset(ret, 0, usize);
		}
	}

	return (ret);
}

/* Only handles large allocations that require more than page alignment. */
static void *
arena_palloc_large(tsd_t *tsd, arena_t *arena, size_t usize, size_t alignment,
    bool zero)
{
	void *ret;
	size_t alloc_size, leadsize, trailsize;
	arena_run_t *run;
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	void *rpages;

	assert(usize == PAGE_CEILING(usize));

	arena = arena_choose(tsd, arena);
	if (unlikely(arena == NULL))
		return (NULL);

	alignment = PAGE_CEILING(alignment);
	alloc_size = usize + large_pad + alignment - PAGE;

	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc_large(arena, alloc_size, false);
	if (run == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	rpages = arena_miscelm_to_rpages(miscelm);

	leadsize = ALIGNMENT_CEILING((uintptr_t)rpages, alignment) -
	    (uintptr_t)rpages;
	assert(alloc_size >= leadsize + usize);
	trailsize = alloc_size - leadsize - usize - large_pad;
	if (leadsize != 0) {
		arena_chunk_map_misc_t *head_miscelm = miscelm;
		arena_run_t *head_run = run;

		miscelm = arena_miscelm_get(chunk,
		    arena_miscelm_to_pageind(head_miscelm) + (leadsize >>
		    LG_PAGE));
		run = &miscelm->run;

		arena_run_trim_head(arena, chunk, head_run, alloc_size,
		    alloc_size - leadsize);
	}
	if (trailsize != 0) {
		arena_run_trim_tail(arena, chunk, run, usize + large_pad +
		    trailsize, usize + large_pad, false);
	}
	if (arena_run_init_large(arena, run, usize + large_pad, zero)) {
		size_t run_ind =
		    arena_miscelm_to_pageind(arena_run_to_miscelm(run));
		bool dirty = (arena_mapbits_dirty_get(chunk, run_ind) != 0);
		bool decommitted = (arena_mapbits_decommitted_get(chunk,
		    run_ind) != 0);

		assert(decommitted); /* Cause of OOM. */
		arena_run_dalloc(arena, run, dirty, false, decommitted);
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
	ret = arena_miscelm_to_rpages(miscelm);

	if (config_stats) {
		index_t index = size2index(usize) - NBINS;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += usize;
		arena->stats.lstats[index].nmalloc++;
		arena->stats.lstats[index].nrequests++;
		arena->stats.lstats[index].curruns++;
	}
	malloc_mutex_unlock(&arena->lock);

	if (config_fill && !zero) {
		if (unlikely(opt_junk_alloc))
			memset(ret, 0xa5, usize);
		else if (unlikely(opt_zero))
			memset(ret, 0, usize);
	}
	return (ret);
}

void *
arena_palloc(tsd_t *tsd, arena_t *arena, size_t usize, size_t alignment,
    bool zero, tcache_t *tcache)
{
	void *ret;

	if (usize <= SMALL_MAXCLASS && (alignment < PAGE || (alignment == PAGE
	    && (usize & PAGE_MASK) == 0))) {
		/* Small; alignment doesn't require special run placement. */
		ret = arena_malloc(tsd, arena, usize, zero, tcache);
	} else if (usize <= arena_maxclass && alignment <= PAGE) {
		/*
		 * Large; alignment doesn't require special run placement.
		 * However, the cached pointer may be at a random offset from
		 * the base of the run, so do some bit manipulation to retrieve
		 * the base.
		 */
		ret = arena_malloc(tsd, arena, usize, zero, tcache);
		if (config_cache_oblivious)
			ret = (void *)((uintptr_t)ret & ~PAGE_MASK);
	} else {
		if (likely(usize <= arena_maxclass)) {
			ret = arena_palloc_large(tsd, arena, usize, alignment,
			    zero);
		} else if (likely(alignment <= chunksize))
			ret = huge_malloc(tsd, arena, usize, zero, tcache);
		else {
			ret = huge_palloc(tsd, arena, usize, alignment, zero,
			    tcache);
		}
	}
	return (ret);
}

void
arena_prof_promoted(const void *ptr, size_t size)
{
	arena_chunk_t *chunk;
	size_t pageind;
	index_t binind;

	cassert(config_prof);
	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);
	assert(isalloc(ptr, false) == LARGE_MINCLASS);
	assert(isalloc(ptr, true) == LARGE_MINCLASS);
	assert(size <= SMALL_MAXCLASS);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	binind = size2index(size);
	assert(binind < NBINS);
	arena_mapbits_large_binind_set(chunk, pageind, binind);

	assert(isalloc(ptr, false) == LARGE_MINCLASS);
	assert(isalloc(ptr, true) == size);
}

static void
arena_dissociate_bin_run(arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{

	/* Dissociate run from bin. */
	if (run == bin->runcur)
		bin->runcur = NULL;
	else {
		index_t binind = arena_bin_index(extent_node_arena_get(
		    &chunk->node), bin);
		arena_bin_info_t *bin_info = &arena_bin_info[binind];

		if (bin_info->nregs != 1) {
			/*
			 * This block's conditional is necessary because if the
			 * run only contains one region, then it never gets
			 * inserted into the non-full runs tree.
			 */
			arena_bin_runs_remove(bin, run);
		}
	}
}

static void
arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{

	assert(run != bin->runcur);
	assert(arena_run_tree_search(&bin->runs, arena_run_to_miscelm(run)) ==
	    NULL);

	malloc_mutex_unlock(&bin->lock);
	/******************************/
	malloc_mutex_lock(&arena->lock);
	arena_run_dalloc_decommit(arena, chunk, run);
	malloc_mutex_unlock(&arena->lock);
	/****************************/
	malloc_mutex_lock(&bin->lock);
	if (config_stats)
		bin->stats.curruns--;
}

static void
arena_bin_lower_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{

	/*
	 * Make sure that if bin->runcur is non-NULL, it refers to the lowest
	 * non-full run.  It is okay to NULL runcur out rather than proactively
	 * keeping it pointing at the lowest non-full run.
	 */
	if ((uintptr_t)run < (uintptr_t)bin->runcur) {
		/* Switch runcur. */
		if (bin->runcur->nfree > 0)
			arena_bin_runs_insert(bin, bin->runcur);
		bin->runcur = run;
		if (config_stats)
			bin->stats.reruns++;
	} else
		arena_bin_runs_insert(bin, run);
}

static void
arena_dalloc_bin_locked_impl(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_bits_t *bitselm, bool junked)
{
	size_t pageind, rpages_ind;
	arena_run_t *run;
	arena_bin_t *bin;
	arena_bin_info_t *bin_info;
	index_t binind;

	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	rpages_ind = pageind - arena_mapbits_small_runind_get(chunk, pageind);
	run = &arena_miscelm_get(chunk, rpages_ind)->run;
	binind = run->binind;
	bin = &arena->bins[binind];
	bin_info = &arena_bin_info[binind];

	if (!junked && config_fill && unlikely(opt_junk_free))
		arena_dalloc_junk_small(ptr, bin_info);

	arena_run_reg_dalloc(run, ptr);
	if (run->nfree == bin_info->nregs) {
		arena_dissociate_bin_run(chunk, run, bin);
		arena_dalloc_bin_run(arena, chunk, run, bin);
	} else if (run->nfree == 1 && run != bin->runcur)
		arena_bin_lower_run(arena, chunk, run, bin);

	if (config_stats) {
		bin->stats.ndalloc++;
		bin->stats.curregs--;
	}
}

void
arena_dalloc_bin_junked_locked(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_bits_t *bitselm)
{

	arena_dalloc_bin_locked_impl(arena, chunk, ptr, bitselm, true);
}

void
arena_dalloc_bin(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t pageind, arena_chunk_map_bits_t *bitselm)
{
	arena_run_t *run;
	arena_bin_t *bin;
	size_t rpages_ind;

	rpages_ind = pageind - arena_mapbits_small_runind_get(chunk, pageind);
	run = &arena_miscelm_get(chunk, rpages_ind)->run;
	bin = &arena->bins[run->binind];
	malloc_mutex_lock(&bin->lock);
	arena_dalloc_bin_locked_impl(arena, chunk, ptr, bitselm, false);
	malloc_mutex_unlock(&bin->lock);
}

void
arena_dalloc_small(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t pageind)
{
	arena_chunk_map_bits_t *bitselm;

	if (config_debug) {
		/* arena_ptr_small_binind_get() does extra sanity checking. */
		assert(arena_ptr_small_binind_get(ptr, arena_mapbits_get(chunk,
		    pageind)) != BININD_INVALID);
	}
	bitselm = arena_bitselm_get(chunk, pageind);
	arena_dalloc_bin(arena, chunk, ptr, pageind, bitselm);
}

#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_large
#define	arena_dalloc_junk_large JEMALLOC_N(arena_dalloc_junk_large_impl)
#endif
void
arena_dalloc_junk_large(void *ptr, size_t usize)
{

	if (config_fill && unlikely(opt_junk_free))
		memset(ptr, 0x5a, usize);
}
#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_large
#define	arena_dalloc_junk_large JEMALLOC_N(arena_dalloc_junk_large)
arena_dalloc_junk_large_t *arena_dalloc_junk_large =
    JEMALLOC_N(arena_dalloc_junk_large_impl);
#endif

void
arena_dalloc_large_locked_impl(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, bool junked)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);
	arena_run_t *run = &miscelm->run;

	if (config_fill || config_stats) {
		size_t usize = arena_mapbits_large_size_get(chunk, pageind) -
		    large_pad;

		if (!junked)
			arena_dalloc_junk_large(ptr, usize);
		if (config_stats) {
			index_t index = size2index(usize) - NBINS;

			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= usize;
			arena->stats.lstats[index].ndalloc++;
			arena->stats.lstats[index].curruns--;
		}
	}

	arena_run_dalloc_decommit(arena, chunk, run);
}

void
arena_dalloc_large_junked_locked(arena_t *arena, arena_chunk_t *chunk,
    void *ptr)
{

	arena_dalloc_large_locked_impl(arena, chunk, ptr, true);
}

void
arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk, void *ptr)
{

	malloc_mutex_lock(&arena->lock);
	arena_dalloc_large_locked_impl(arena, chunk, ptr, false);
	malloc_mutex_unlock(&arena->lock);
}

static void
arena_ralloc_large_shrink(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t oldsize, size_t size)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);
	arena_run_t *run = &miscelm->run;

	assert(size < oldsize);

	/*
	 * Shrink the run, and make trailing pages available for other
	 * allocations.
	 */
	malloc_mutex_lock(&arena->lock);
	arena_run_trim_tail(arena, chunk, run, oldsize + large_pad, size +
	    large_pad, true);
	if (config_stats) {
		index_t oldindex = size2index(oldsize) - NBINS;
		index_t index = size2index(size) - NBINS;

		arena->stats.ndalloc_large++;
		arena->stats.allocated_large -= oldsize;
		arena->stats.lstats[oldindex].ndalloc++;
		arena->stats.lstats[oldindex].curruns--;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[index].nmalloc++;
		arena->stats.lstats[index].nrequests++;
		arena->stats.lstats[index].curruns++;
	}
	malloc_mutex_unlock(&arena->lock);
}

static bool
arena_ralloc_large_grow(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t oldsize, size_t size, size_t extra, bool zero)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	size_t npages = (oldsize + large_pad) >> LG_PAGE;
	size_t followsize;
	size_t usize_min = s2u(size);

	assert(oldsize == arena_mapbits_large_size_get(chunk, pageind) -
	    large_pad);

	/* Try to extend the run. */
	assert(usize_min > oldsize);
	malloc_mutex_lock(&arena->lock);
	if (pageind+npages < chunk_npages &&
	    arena_mapbits_allocated_get(chunk, pageind+npages) == 0 &&
	    (followsize = arena_mapbits_unallocated_size_get(chunk,
	    pageind+npages)) >= usize_min - oldsize) {
		/*
		 * The next run is available and sufficiently large.  Split the
		 * following run, then merge the first part with the existing
		 * allocation.
		 */
		arena_run_t *run;
		size_t flag_dirty, flag_unzeroed_mask, splitsize, usize;

		usize = s2u(size + extra);
		while (oldsize + followsize < usize)
			usize = index2size(size2index(usize)-1);
		assert(usize >= usize_min);
		splitsize = usize - oldsize;

		run = &arena_miscelm_get(chunk, pageind+npages)->run;
		if (arena_run_split_large(arena, run, splitsize, zero)) {
			malloc_mutex_unlock(&arena->lock);
			return (true);
		}

		size = oldsize + splitsize;
		npages = (size + large_pad) >> LG_PAGE;

		/*
		 * Mark the extended run as dirty if either portion of the run
		 * was dirty before allocation.  This is rather pedantic,
		 * because there's not actually any sequence of events that
		 * could cause the resulting run to be passed to
		 * arena_run_dalloc() with the dirty argument set to false
		 * (which is when dirty flag consistency would really matter).
		 */
		flag_dirty = arena_mapbits_dirty_get(chunk, pageind) |
		    arena_mapbits_dirty_get(chunk, pageind+npages-1);
		flag_unzeroed_mask = flag_dirty == 0 ? CHUNK_MAP_UNZEROED : 0;
		arena_mapbits_large_set(chunk, pageind, size + large_pad,
		    flag_dirty | (flag_unzeroed_mask &
		    arena_mapbits_unzeroed_get(chunk, pageind)));
		arena_mapbits_large_set(chunk, pageind+npages-1, 0, flag_dirty |
		    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
		    pageind+npages-1)));

		if (config_stats) {
			index_t oldindex = size2index(oldsize) - NBINS;
			index_t index = size2index(size) - NBINS;

			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= oldsize;
			arena->stats.lstats[oldindex].ndalloc++;
			arena->stats.lstats[oldindex].curruns--;

			arena->stats.nmalloc_large++;
			arena->stats.nrequests_large++;
			arena->stats.allocated_large += size;
			arena->stats.lstats[index].nmalloc++;
			arena->stats.lstats[index].nrequests++;
			arena->stats.lstats[index].curruns++;
		}
		malloc_mutex_unlock(&arena->lock);
		return (false);
	}
	malloc_mutex_unlock(&arena->lock);

	return (true);
}

#ifdef JEMALLOC_JET
#undef arena_ralloc_junk_large
#define	arena_ralloc_junk_large JEMALLOC_N(arena_ralloc_junk_large_impl)
#endif
static void
arena_ralloc_junk_large(void *ptr, size_t old_usize, size_t usize)
{

	if (config_fill && unlikely(opt_junk_free)) {
		memset((void *)((uintptr_t)ptr + usize), 0x5a,
		    old_usize - usize);
	}
}
#ifdef JEMALLOC_JET
#undef arena_ralloc_junk_large
#define	arena_ralloc_junk_large JEMALLOC_N(arena_ralloc_junk_large)
arena_ralloc_junk_large_t *arena_ralloc_junk_large =
    JEMALLOC_N(arena_ralloc_junk_large_impl);
#endif

/*
 * Try to resize a large allocation, in order to avoid copying.  This will
 * always fail if growing an object, and the following run is already in use.
 */
static bool
arena_ralloc_large(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{
	size_t usize;

	/* Make sure extra can't cause size_t overflow. */
	if (unlikely(extra >= arena_maxclass))
		return (true);

	usize = s2u(size + extra);
	if (usize == oldsize) {
		/* Same size class. */
		return (false);
	} else {
		arena_chunk_t *chunk;
		arena_t *arena;

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
		arena = extent_node_arena_get(&chunk->node);

		if (usize < oldsize) {
			/* Fill before shrinking in order avoid a race. */
			arena_ralloc_junk_large(ptr, oldsize, usize);
			arena_ralloc_large_shrink(arena, chunk, ptr, oldsize,
			    usize);
			return (false);
		} else {
			bool ret = arena_ralloc_large_grow(arena, chunk, ptr,
			    oldsize, size, extra, zero);
			if (config_fill && !ret && !zero) {
				if (unlikely(opt_junk_alloc)) {
					memset((void *)((uintptr_t)ptr +
					    oldsize), 0xa5, isalloc(ptr,
					    config_prof) - oldsize);
				} else if (unlikely(opt_zero)) {
					memset((void *)((uintptr_t)ptr +
					    oldsize), 0, isalloc(ptr,
					    config_prof) - oldsize);
				}
			}
			return (ret);
		}
	}
}

bool
arena_ralloc_no_move(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{

	if (likely(size <= arena_maxclass)) {
		/*
		 * Avoid moving the allocation if the size class can be left the
		 * same.
		 */
		if (likely(oldsize <= arena_maxclass)) {
			if (oldsize <= SMALL_MAXCLASS) {
				assert(
				    arena_bin_info[size2index(oldsize)].reg_size
				    == oldsize);
				if ((size + extra <= SMALL_MAXCLASS &&
				    size2index(size + extra) ==
				    size2index(oldsize)) || (size <= oldsize &&
				    size + extra >= oldsize))
					return (false);
			} else {
				assert(size <= arena_maxclass);
				if (size + extra > SMALL_MAXCLASS) {
					if (!arena_ralloc_large(ptr, oldsize,
					    size, extra, zero))
						return (false);
				}
			}
		}

		/* Reallocation would require a move. */
		return (true);
	} else
		return (huge_ralloc_no_move(ptr, oldsize, size, extra, zero));
}

void *
arena_ralloc(tsd_t *tsd, arena_t *arena, void *ptr, size_t oldsize, size_t size,
    size_t extra, size_t alignment, bool zero, tcache_t *tcache)
{
	void *ret;

	if (likely(size <= arena_maxclass)) {
		size_t copysize;

		/* Try to avoid moving the allocation. */
		if (!arena_ralloc_no_move(ptr, oldsize, size, extra, zero))
			return (ptr);

		/*
		 * size and oldsize are different enough that we need to move
		 * the object.  In that case, fall back to allocating new space
		 * and copying.
		 */
		if (alignment != 0) {
			size_t usize = sa2u(size + extra, alignment);
			if (usize == 0)
				return (NULL);
			ret = ipalloct(tsd, usize, alignment, zero, tcache,
			    arena);
		} else {
			ret = arena_malloc(tsd, arena, size + extra, zero,
			    tcache);
		}

		if (ret == NULL) {
			if (extra == 0)
				return (NULL);
			/* Try again, this time without extra. */
			if (alignment != 0) {
				size_t usize = sa2u(size, alignment);
				if (usize == 0)
					return (NULL);
				ret = ipalloct(tsd, usize, alignment, zero,
				    tcache, arena);
			} else {
				ret = arena_malloc(tsd, arena, size, zero,
				    tcache);
			}

			if (ret == NULL)
				return (NULL);
		}

		/*
		 * Junk/zero-filling were already done by
		 * ipalloc()/arena_malloc().
		 */

		/*
		 * Copy at most size bytes (not size+extra), since the caller
		 * has no expectation that the extra bytes will be reliably
		 * preserved.
		 */
		copysize = (size < oldsize) ? size : oldsize;
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, copysize);
		memcpy(ret, ptr, copysize);
		isqalloc(tsd, ptr, oldsize, tcache);
	} else {
		ret = huge_ralloc(tsd, arena, ptr, oldsize, size, extra,
		    alignment, zero, tcache);
	}
	return (ret);
}

dss_prec_t
arena_dss_prec_get(arena_t *arena)
{
	dss_prec_t ret;

	malloc_mutex_lock(&arena->lock);
	ret = arena->dss_prec;
	malloc_mutex_unlock(&arena->lock);
	return (ret);
}

bool
arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec)
{

	if (!have_dss)
		return (dss_prec != dss_prec_disabled);
	malloc_mutex_lock(&arena->lock);
	arena->dss_prec = dss_prec;
	malloc_mutex_unlock(&arena->lock);
	return (false);
}

ssize_t
arena_lg_dirty_mult_default_get(void)
{

	return ((ssize_t)atomic_read_z((size_t *)&lg_dirty_mult_default));
}

bool
arena_lg_dirty_mult_default_set(ssize_t lg_dirty_mult)
{

	if (!arena_lg_dirty_mult_valid(lg_dirty_mult))
		return (true);
	atomic_write_z((size_t *)&lg_dirty_mult_default, (size_t)lg_dirty_mult);
	return (false);
}

void
arena_stats_merge(arena_t *arena, const char **dss, ssize_t *lg_dirty_mult,
    size_t *nactive, size_t *ndirty, arena_stats_t *astats,
    malloc_bin_stats_t *bstats, malloc_large_stats_t *lstats,
    malloc_huge_stats_t *hstats)
{
	unsigned i;

	malloc_mutex_lock(&arena->lock);
	*dss = dss_prec_names[arena->dss_prec];
	*lg_dirty_mult = arena->lg_dirty_mult;
	*nactive += arena->nactive;
	*ndirty += arena->ndirty;

	astats->mapped += arena->stats.mapped;
	astats->npurge += arena->stats.npurge;
	astats->nmadvise += arena->stats.nmadvise;
	astats->purged += arena->stats.purged;
	astats->metadata_mapped += arena->stats.metadata_mapped;
	astats->metadata_allocated += arena_metadata_allocated_get(arena);
	astats->allocated_large += arena->stats.allocated_large;
	astats->nmalloc_large += arena->stats.nmalloc_large;
	astats->ndalloc_large += arena->stats.ndalloc_large;
	astats->nrequests_large += arena->stats.nrequests_large;
	astats->allocated_huge += arena->stats.allocated_huge;
	astats->nmalloc_huge += arena->stats.nmalloc_huge;
	astats->ndalloc_huge += arena->stats.ndalloc_huge;

	for (i = 0; i < nlclasses; i++) {
		lstats[i].nmalloc += arena->stats.lstats[i].nmalloc;
		lstats[i].ndalloc += arena->stats.lstats[i].ndalloc;
		lstats[i].nrequests += arena->stats.lstats[i].nrequests;
		lstats[i].curruns += arena->stats.lstats[i].curruns;
	}

	for (i = 0; i < nhclasses; i++) {
		hstats[i].nmalloc += arena->stats.hstats[i].nmalloc;
		hstats[i].ndalloc += arena->stats.hstats[i].ndalloc;
		hstats[i].curhchunks += arena->stats.hstats[i].curhchunks;
	}
	malloc_mutex_unlock(&arena->lock);

	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];

		malloc_mutex_lock(&bin->lock);
		bstats[i].nmalloc += bin->stats.nmalloc;
		bstats[i].ndalloc += bin->stats.ndalloc;
		bstats[i].nrequests += bin->stats.nrequests;
		bstats[i].curregs += bin->stats.curregs;
		if (config_tcache) {
			bstats[i].nfills += bin->stats.nfills;
			bstats[i].nflushes += bin->stats.nflushes;
		}
		bstats[i].nruns += bin->stats.nruns;
		bstats[i].reruns += bin->stats.reruns;
		bstats[i].curruns += bin->stats.curruns;
		malloc_mutex_unlock(&bin->lock);
	}
}

arena_t *
arena_new(unsigned ind)
{
	arena_t *arena;
	unsigned i;
	arena_bin_t *bin;

	/*
	 * Allocate arena, arena->lstats, and arena->hstats contiguously, mainly
	 * because there is no way to clean up if base_alloc() OOMs.
	 */
	if (config_stats) {
		arena = (arena_t *)base_alloc(CACHELINE_CEILING(sizeof(arena_t))
		    + QUANTUM_CEILING(nlclasses * sizeof(malloc_large_stats_t) +
		    nhclasses) * sizeof(malloc_huge_stats_t));
	} else
		arena = (arena_t *)base_alloc(sizeof(arena_t));
	if (arena == NULL)
		return (NULL);

	arena->ind = ind;
	arena->nthreads = 0;
	if (malloc_mutex_init(&arena->lock))
		return (NULL);

	if (config_stats) {
		memset(&arena->stats, 0, sizeof(arena_stats_t));
		arena->stats.lstats = (malloc_large_stats_t *)((uintptr_t)arena
		    + CACHELINE_CEILING(sizeof(arena_t)));
		memset(arena->stats.lstats, 0, nlclasses *
		    sizeof(malloc_large_stats_t));
		arena->stats.hstats = (malloc_huge_stats_t *)((uintptr_t)arena
		    + CACHELINE_CEILING(sizeof(arena_t)) +
		    QUANTUM_CEILING(nlclasses * sizeof(malloc_large_stats_t)));
		memset(arena->stats.hstats, 0, nhclasses *
		    sizeof(malloc_huge_stats_t));
		if (config_tcache)
			ql_new(&arena->tcache_ql);
	}

	if (config_prof)
		arena->prof_accumbytes = 0;

	if (config_cache_oblivious) {
		/*
		 * A nondeterministic seed based on the address of arena reduces
		 * the likelihood of lockstep non-uniform cache index
		 * utilization among identical concurrent processes, but at the
		 * cost of test repeatability.  For debug builds, instead use a
		 * deterministic seed.
		 */
		arena->offset_state = config_debug ? ind :
		    (uint64_t)(uintptr_t)arena;
	}

	arena->dss_prec = chunk_dss_prec_get();

	arena->spare = NULL;

	arena->lg_dirty_mult = arena_lg_dirty_mult_default_get();
	arena->purging = false;
	arena->nactive = 0;
	arena->ndirty = 0;

	arena_avail_tree_new(&arena->runs_avail);
	qr_new(&arena->runs_dirty, rd_link);
	qr_new(&arena->chunks_cache, cc_link);

	ql_new(&arena->huge);
	if (malloc_mutex_init(&arena->huge_mtx))
		return (NULL);

	extent_tree_szad_new(&arena->chunks_szad_cached);
	extent_tree_ad_new(&arena->chunks_ad_cached);
	extent_tree_szad_new(&arena->chunks_szad_retained);
	extent_tree_ad_new(&arena->chunks_ad_retained);
	if (malloc_mutex_init(&arena->chunks_mtx))
		return (NULL);
	ql_new(&arena->node_cache);
	if (malloc_mutex_init(&arena->node_cache_mtx))
		return (NULL);

	arena->chunk_hooks = chunk_hooks_default;

	/* Initialize bins. */
	for (i = 0; i < NBINS; i++) {
		bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock))
			return (NULL);
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);
		if (config_stats)
			memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
	}

	return (arena);
}

/*
 * Calculate bin_info->run_size such that it meets the following constraints:
 *
 *   *) bin_info->run_size <= arena_maxrun
 *   *) bin_info->nregs <= RUN_MAXREGS
 *
 * bin_info->nregs and bin_info->reg0_offset are also calculated here, since
 * these settings are all interdependent.
 */
static void
bin_info_run_size_calc(arena_bin_info_t *bin_info)
{
	size_t pad_size;
	size_t try_run_size, perfect_run_size, actual_run_size;
	uint32_t try_nregs, perfect_nregs, actual_nregs;

	/*
	 * Determine redzone size based on minimum alignment and minimum
	 * redzone size.  Add padding to the end of the run if it is needed to
	 * align the regions.  The padding allows each redzone to be half the
	 * minimum alignment; without the padding, each redzone would have to
	 * be twice as large in order to maintain alignment.
	 */
	if (config_fill && unlikely(opt_redzone)) {
		size_t align_min = ZU(1) << (jemalloc_ffs(bin_info->reg_size) -
		    1);
		if (align_min <= REDZONE_MINSIZE) {
			bin_info->redzone_size = REDZONE_MINSIZE;
			pad_size = 0;
		} else {
			bin_info->redzone_size = align_min >> 1;
			pad_size = bin_info->redzone_size;
		}
	} else {
		bin_info->redzone_size = 0;
		pad_size = 0;
	}
	bin_info->reg_interval = bin_info->reg_size +
	    (bin_info->redzone_size << 1);

	/*
	 * Compute run size under ideal conditions (no redzones, no limit on run
	 * size).
	 */
	try_run_size = PAGE;
	try_nregs = try_run_size / bin_info->reg_size;
	do {
		perfect_run_size = try_run_size;
		perfect_nregs = try_nregs;

		try_run_size += PAGE;
		try_nregs = try_run_size / bin_info->reg_size;
	} while (perfect_run_size != perfect_nregs * bin_info->reg_size);
	assert(perfect_nregs <= RUN_MAXREGS);

	actual_run_size = perfect_run_size;
	actual_nregs = (actual_run_size - pad_size) / bin_info->reg_interval;

	/*
	 * Redzones can require enough padding that not even a single region can
	 * fit within the number of pages that would normally be dedicated to a
	 * run for this size class.  Increase the run size until at least one
	 * region fits.
	 */
	while (actual_nregs == 0) {
		assert(config_fill && unlikely(opt_redzone));

		actual_run_size += PAGE;
		actual_nregs = (actual_run_size - pad_size) /
		    bin_info->reg_interval;
	}

	/*
	 * Make sure that the run will fit within an arena chunk.
	 */
	while (actual_run_size > arena_maxrun) {
		actual_run_size -= PAGE;
		actual_nregs = (actual_run_size - pad_size) /
		    bin_info->reg_interval;
	}
	assert(actual_nregs > 0);
	assert(actual_run_size == s2u(actual_run_size));

	/* Copy final settings. */
	bin_info->run_size = actual_run_size;
	bin_info->nregs = actual_nregs;
	bin_info->reg0_offset = actual_run_size - (actual_nregs *
	    bin_info->reg_interval) - pad_size + bin_info->redzone_size;

	if (actual_run_size > small_maxrun)
		small_maxrun = actual_run_size;

	assert(bin_info->reg0_offset - bin_info->redzone_size + (bin_info->nregs
	    * bin_info->reg_interval) + pad_size == bin_info->run_size);
}

static void
bin_info_init(void)
{
	arena_bin_info_t *bin_info;

#define	BIN_INFO_INIT_bin_yes(index, size)				\
	bin_info = &arena_bin_info[index];				\
	bin_info->reg_size = size;					\
	bin_info_run_size_calc(bin_info);				\
	bitmap_info_init(&bin_info->bitmap_info, bin_info->nregs);
#define	BIN_INFO_INIT_bin_no(index, size)
#define	SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup)	\
	BIN_INFO_INIT_bin_##bin(index, (ZU(1)<<lg_grp) + (ZU(ndelta)<<lg_delta))
	SIZE_CLASSES
#undef BIN_INFO_INIT_bin_yes
#undef BIN_INFO_INIT_bin_no
#undef SC
}

static bool
small_run_size_init(void)
{

	assert(small_maxrun != 0);

	small_run_tab = (bool *)base_alloc(sizeof(bool) * (small_maxrun >>
	    LG_PAGE));
	if (small_run_tab == NULL)
		return (true);

#define	TAB_INIT_bin_yes(index, size) {					\
		arena_bin_info_t *bin_info = &arena_bin_info[index];	\
		small_run_tab[bin_info->run_size >> LG_PAGE] = true;	\
	}
#define	TAB_INIT_bin_no(index, size)
#define	SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup)	\
	TAB_INIT_bin_##bin(index, (ZU(1)<<lg_grp) + (ZU(ndelta)<<lg_delta))
	SIZE_CLASSES
#undef TAB_INIT_bin_yes
#undef TAB_INIT_bin_no
#undef SC

	return (false);
}

bool
arena_boot(void)
{
	size_t header_size;
	unsigned i;

	arena_lg_dirty_mult_default_set(opt_lg_dirty_mult);

	/*
	 * Compute the header size such that it is large enough to contain the
	 * page map.  The page map is biased to omit entries for the header
	 * itself, so some iteration is necessary to compute the map bias.
	 *
	 * 1) Compute safe header_size and map_bias values that include enough
	 *    space for an unbiased page map.
	 * 2) Refine map_bias based on (1) to omit the header pages in the page
	 *    map.  The resulting map_bias may be one too small.
	 * 3) Refine map_bias based on (2).  The result will be >= the result
	 *    from (2), and will always be correct.
	 */
	map_bias = 0;
	for (i = 0; i < 3; i++) {
		header_size = offsetof(arena_chunk_t, map_bits) +
		    ((sizeof(arena_chunk_map_bits_t) +
		    sizeof(arena_chunk_map_misc_t)) * (chunk_npages-map_bias));
		map_bias = (header_size + PAGE_MASK) >> LG_PAGE;
	}
	assert(map_bias > 0);

	map_misc_offset = offsetof(arena_chunk_t, map_bits) +
	    sizeof(arena_chunk_map_bits_t) * (chunk_npages-map_bias);

	arena_maxrun = chunksize - (map_bias << LG_PAGE);
	assert(arena_maxrun > 0);
	arena_maxclass = index2size(size2index(chunksize)-1);
	if (arena_maxclass > arena_maxrun) {
		/*
		 * For small chunk sizes it's possible for there to be fewer
		 * non-header pages available than are necessary to serve the
		 * size classes just below chunksize.
		 */
		arena_maxclass = arena_maxrun;
	}
	assert(arena_maxclass > 0);
	nlclasses = size2index(arena_maxclass) - size2index(SMALL_MAXCLASS);
	nhclasses = NSIZES - nlclasses - NBINS;

	bin_info_init();
	return (small_run_size_init());
}

void
arena_prefork(arena_t *arena)
{
	unsigned i;

	malloc_mutex_prefork(&arena->lock);
	malloc_mutex_prefork(&arena->huge_mtx);
	malloc_mutex_prefork(&arena->chunks_mtx);
	malloc_mutex_prefork(&arena->node_cache_mtx);
	for (i = 0; i < NBINS; i++)
		malloc_mutex_prefork(&arena->bins[i].lock);
}

void
arena_postfork_parent(arena_t *arena)
{
	unsigned i;

	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_parent(&arena->bins[i].lock);
	malloc_mutex_postfork_parent(&arena->node_cache_mtx);
	malloc_mutex_postfork_parent(&arena->chunks_mtx);
	malloc_mutex_postfork_parent(&arena->huge_mtx);
	malloc_mutex_postfork_parent(&arena->lock);
}

void
arena_postfork_child(arena_t *arena)
{
	unsigned i;

	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_child(&arena->bins[i].lock);
	malloc_mutex_postfork_child(&arena->node_cache_mtx);
	malloc_mutex_postfork_child(&arena->chunks_mtx);
	malloc_mutex_postfork_child(&arena->huge_mtx);
	malloc_mutex_postfork_child(&arena->lock);
}
