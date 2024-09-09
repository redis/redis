#ifndef JEMALLOC_INTERNAL_EMAP_H
#define JEMALLOC_INTERNAL_EMAP_H

#include "jemalloc/internal/base.h"
#include "jemalloc/internal/rtree.h"

/*
 * Note: Ends without at semicolon, so that
 *     EMAP_DECLARE_RTREE_CTX;
 * in uses will avoid empty-statement warnings.
 */
#define EMAP_DECLARE_RTREE_CTX						\
    rtree_ctx_t rtree_ctx_fallback;					\
    rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback)

typedef struct emap_s emap_t;
struct emap_s {
	rtree_t rtree;
};

/* Used to pass rtree lookup context down the path. */
typedef struct emap_alloc_ctx_t emap_alloc_ctx_t;
struct emap_alloc_ctx_t {
	szind_t szind;
	bool slab;
};

typedef struct emap_full_alloc_ctx_s emap_full_alloc_ctx_t;
struct emap_full_alloc_ctx_s {
	szind_t szind;
	bool slab;
	edata_t *edata;
};

bool emap_init(emap_t *emap, base_t *base, bool zeroed);

void emap_remap(tsdn_t *tsdn, emap_t *emap, edata_t *edata, szind_t szind,
    bool slab);

void emap_update_edata_state(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    extent_state_t state);

/*
 * The two acquire functions below allow accessing neighbor edatas, if it's safe
 * and valid to do so (i.e. from the same arena, of the same state, etc.).  This
 * is necessary because the ecache locks are state based, and only protect
 * edatas with the same state.  Therefore the neighbor edata's state needs to be
 * verified first, before chasing the edata pointer.  The returned edata will be
 * in an acquired state, meaning other threads will be prevented from accessing
 * it, even if technically the edata can still be discovered from the rtree.
 *
 * This means, at any moment when holding pointers to edata, either one of the
 * state based locks is held (and the edatas are all of the protected state), or
 * the edatas are in an acquired state (e.g. in active or merging state).  The
 * acquire operation itself (changing the edata to an acquired state) is done
 * under the state locks.
 */
edata_t *emap_try_acquire_edata_neighbor(tsdn_t *tsdn, emap_t *emap,
    edata_t *edata, extent_pai_t pai, extent_state_t expected_state,
    bool forward);
edata_t *emap_try_acquire_edata_neighbor_expand(tsdn_t *tsdn, emap_t *emap,
    edata_t *edata, extent_pai_t pai, extent_state_t expected_state);
void emap_release_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    extent_state_t new_state);

/*
 * Associate the given edata with its beginning and end address, setting the
 * szind and slab info appropriately.
 * Returns true on error (i.e. resource exhaustion).
 */
bool emap_register_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind, bool slab);

/*
 * Does the same thing, but with the interior of the range, for slab
 * allocations.
 *
 * You might wonder why we don't just have a single emap_register function that
 * does both depending on the value of 'slab'.  The answer is twofold:
 * - As a practical matter, in places like the extract->split->commit pathway,
 *   we defer the interior operation until we're sure that the commit won't fail
 *   (but we have to register the split boundaries there).
 * - In general, we're trying to move to a world where the page-specific
 *   allocator doesn't know as much about how the pages it allocates will be
 *   used, and passing a 'slab' parameter everywhere makes that more
 *   complicated.
 *
 * Unlike the boundary version, this function can't fail; this is because slabs
 * can't get big enough to touch a new page that neither of the boundaries
 * touched, so no allocation is necessary to fill the interior once the boundary
 * has been touched.
 */
void emap_register_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind);

void emap_deregister_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
void emap_deregister_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata);

typedef struct emap_prepare_s emap_prepare_t;
struct emap_prepare_s {
	rtree_leaf_elm_t *lead_elm_a;
	rtree_leaf_elm_t *lead_elm_b;
	rtree_leaf_elm_t *trail_elm_a;
	rtree_leaf_elm_t *trail_elm_b;
};

/**
 * These functions the emap metadata management for merging, splitting, and
 * reusing extents.  In particular, they set the boundary mappings from
 * addresses to edatas.  If the result is going to be used as a slab, you
 * still need to call emap_register_interior on it, though.
 *
 * Remap simply changes the szind and slab status of an extent's boundary
 * mappings.  If the extent is not a slab, it doesn't bother with updating the
 * end mapping (since lookups only occur in the interior of an extent for
 * slabs).  Since the szind and slab status only make sense for active extents,
 * this should only be called while activating or deactivating an extent.
 *
 * Split and merge have a "prepare" and a "commit" portion.  The prepare portion
 * does the operations that can be done without exclusive access to the extent
 * in question, while the commit variant requires exclusive access to maintain
 * the emap invariants.  The only function that can fail is emap_split_prepare,
 * and it returns true on failure (at which point the caller shouldn't commit).
 *
 * In all cases, "lead" refers to the lower-addressed extent, and trail to the
 * higher-addressed one.  It's the caller's responsibility to set the edata
 * state appropriately.
 */
bool emap_split_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *edata, size_t size_a, edata_t *trail, size_t size_b);
void emap_split_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, size_t size_a, edata_t *trail, size_t size_b);
void emap_merge_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail);
void emap_merge_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail);

/* Assert that the emap's view of the given edata matches the edata's view. */
void emap_do_assert_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
static inline void
emap_assert_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	if (config_debug) {
		emap_do_assert_mapped(tsdn, emap, edata);
	}
}

/* Assert that the given edata isn't in the map. */
void emap_do_assert_not_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
static inline void
emap_assert_not_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	if (config_debug) {
		emap_do_assert_not_mapped(tsdn, emap, edata);
	}
}

JEMALLOC_ALWAYS_INLINE bool
emap_edata_in_transition(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	assert(config_debug);
	emap_assert_mapped(tsdn, emap, edata);

	EMAP_DECLARE_RTREE_CTX;
	rtree_contents_t contents = rtree_read(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata));

	return edata_state_in_transition(contents.metadata.state);
}

JEMALLOC_ALWAYS_INLINE bool
emap_edata_is_acquired(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	if (!config_debug) {
		/* For assertions only. */
		return false;
	}

	/*
	 * The edata is considered acquired if no other threads will attempt to
	 * read / write any fields from it.  This includes a few cases:
	 *
	 * 1) edata not hooked into emap yet -- This implies the edata just got
	 * allocated or initialized.
	 *
	 * 2) in an active or transition state -- In both cases, the edata can
	 * be discovered from the emap, however the state tracked in the rtree
	 * will prevent other threads from accessing the actual edata.
	 */
	EMAP_DECLARE_RTREE_CTX;
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, &emap->rtree,
	    rtree_ctx, (uintptr_t)edata_base_get(edata), /* dependent */ true,
	    /* init_missing */ false);
	if (elm == NULL) {
		return true;
	}
	rtree_contents_t contents = rtree_leaf_elm_read(tsdn, &emap->rtree, elm,
	    /* dependent */ true);
	if (contents.edata == NULL ||
	    contents.metadata.state == extent_state_active ||
	    edata_state_in_transition(contents.metadata.state)) {
		return true;
	}

	return false;
}

JEMALLOC_ALWAYS_INLINE void
extent_assert_can_coalesce(const edata_t *inner, const edata_t *outer) {
	assert(edata_arena_ind_get(inner) == edata_arena_ind_get(outer));
	assert(edata_pai_get(inner) == edata_pai_get(outer));
	assert(edata_committed_get(inner) == edata_committed_get(outer));
	assert(edata_state_get(inner) == extent_state_active);
	assert(edata_state_get(outer) == extent_state_merging);
	assert(!edata_guarded_get(inner) && !edata_guarded_get(outer));
	assert(edata_base_get(inner) == edata_past_get(outer) ||
	    edata_base_get(outer) == edata_past_get(inner));
}

JEMALLOC_ALWAYS_INLINE void
extent_assert_can_expand(const edata_t *original, const edata_t *expand) {
	assert(edata_arena_ind_get(original) == edata_arena_ind_get(expand));
	assert(edata_pai_get(original) == edata_pai_get(expand));
	assert(edata_state_get(original) == extent_state_active);
	assert(edata_state_get(expand) == extent_state_merging);
	assert(edata_past_get(original) == edata_base_get(expand));
}

JEMALLOC_ALWAYS_INLINE edata_t *
emap_edata_lookup(tsdn_t *tsdn, emap_t *emap, const void *ptr) {
	EMAP_DECLARE_RTREE_CTX;

	return rtree_read(tsdn, &emap->rtree, rtree_ctx, (uintptr_t)ptr).edata;
}

/* Fills in alloc_ctx with the info in the map. */
JEMALLOC_ALWAYS_INLINE void
emap_alloc_ctx_lookup(tsdn_t *tsdn, emap_t *emap, const void *ptr,
    emap_alloc_ctx_t *alloc_ctx) {
	EMAP_DECLARE_RTREE_CTX;

	rtree_metadata_t metadata = rtree_metadata_read(tsdn, &emap->rtree,
	    rtree_ctx, (uintptr_t)ptr);
	alloc_ctx->szind = metadata.szind;
	alloc_ctx->slab = metadata.slab;
}

/* The pointer must be mapped. */
JEMALLOC_ALWAYS_INLINE void
emap_full_alloc_ctx_lookup(tsdn_t *tsdn, emap_t *emap, const void *ptr,
    emap_full_alloc_ctx_t *full_alloc_ctx) {
	EMAP_DECLARE_RTREE_CTX;

	rtree_contents_t contents = rtree_read(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)ptr);
	full_alloc_ctx->edata = contents.edata;
	full_alloc_ctx->szind = contents.metadata.szind;
	full_alloc_ctx->slab = contents.metadata.slab;
}

/*
 * The pointer is allowed to not be mapped.
 *
 * Returns true when the pointer is not present.
 */
JEMALLOC_ALWAYS_INLINE bool
emap_full_alloc_ctx_try_lookup(tsdn_t *tsdn, emap_t *emap, const void *ptr,
    emap_full_alloc_ctx_t *full_alloc_ctx) {
	EMAP_DECLARE_RTREE_CTX;

	rtree_contents_t contents;
	bool err = rtree_read_independent(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)ptr, &contents);
	if (err) {
		return true;
	}
	full_alloc_ctx->edata = contents.edata;
	full_alloc_ctx->szind = contents.metadata.szind;
	full_alloc_ctx->slab = contents.metadata.slab;
	return false;
}

/*
 * Only used on the fastpath of free.  Returns true when cannot be fulfilled by
 * fast path, e.g. when the metadata key is not cached.
 */
JEMALLOC_ALWAYS_INLINE bool
emap_alloc_ctx_try_lookup_fast(tsd_t *tsd, emap_t *emap, const void *ptr,
    emap_alloc_ctx_t *alloc_ctx) {
	/* Use the unsafe getter since this may gets called during exit. */
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctxp_get_unsafe(tsd);

	rtree_metadata_t metadata;
	bool err = rtree_metadata_try_read_fast(tsd_tsdn(tsd), &emap->rtree,
	    rtree_ctx, (uintptr_t)ptr, &metadata);
	if (err) {
		return true;
	}
	alloc_ctx->szind = metadata.szind;
	alloc_ctx->slab = metadata.slab;
	return false;
}

/*
 * We want to do batch lookups out of the cache bins, which use
 * cache_bin_ptr_array_get to access the i'th element of the bin (since they
 * invert usual ordering in deciding what to flush).  This lets the emap avoid
 * caring about its caller's ordering.
 */
typedef const void *(*emap_ptr_getter)(void *ctx, size_t ind);
/*
 * This allows size-checking assertions, which we can only do while we're in the
 * process of edata lookups.
 */
typedef void (*emap_metadata_visitor)(void *ctx, emap_full_alloc_ctx_t *alloc_ctx);

typedef union emap_batch_lookup_result_u emap_batch_lookup_result_t;
union emap_batch_lookup_result_u {
	edata_t *edata;
	rtree_leaf_elm_t *rtree_leaf;
};

JEMALLOC_ALWAYS_INLINE void
emap_edata_lookup_batch(tsd_t *tsd, emap_t *emap, size_t nptrs,
    emap_ptr_getter ptr_getter, void *ptr_getter_ctx,
    emap_metadata_visitor metadata_visitor, void *metadata_visitor_ctx,
    emap_batch_lookup_result_t *result) {
	/* Avoids null-checking tsdn in the loop below. */
	util_assume(tsd != NULL);
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctxp_get(tsd);

	for (size_t i = 0; i < nptrs; i++) {
		const void *ptr = ptr_getter(ptr_getter_ctx, i);
		/*
		 * Reuse the edatas array as a temp buffer, lying a little about
		 * the types.
		 */
		result[i].rtree_leaf = rtree_leaf_elm_lookup(tsd_tsdn(tsd),
		    &emap->rtree, rtree_ctx, (uintptr_t)ptr,
		    /* dependent */ true, /* init_missing */ false);
	}

	for (size_t i = 0; i < nptrs; i++) {
		rtree_leaf_elm_t *elm = result[i].rtree_leaf;
		rtree_contents_t contents = rtree_leaf_elm_read(tsd_tsdn(tsd),
		    &emap->rtree, elm, /* dependent */ true);
		result[i].edata = contents.edata;
		emap_full_alloc_ctx_t alloc_ctx;
		/*
		 * Not all these fields are read in practice by the metadata
		 * visitor.  But the compiler can easily optimize away the ones
		 * that aren't, so no sense in being incomplete.
		 */
		alloc_ctx.szind = contents.metadata.szind;
		alloc_ctx.slab = contents.metadata.slab;
		alloc_ctx.edata = contents.edata;
		metadata_visitor(metadata_visitor_ctx, &alloc_ctx);
	}
}

#endif /* JEMALLOC_INTERNAL_EMAP_H */
