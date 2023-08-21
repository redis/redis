#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/emap.h"

enum emap_lock_result_e {
	emap_lock_result_success,
	emap_lock_result_failure,
	emap_lock_result_no_extent
};
typedef enum emap_lock_result_e emap_lock_result_t;

bool
emap_init(emap_t *emap, base_t *base, bool zeroed) {
	return rtree_new(&emap->rtree, base, zeroed);
}

void
emap_update_edata_state(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    extent_state_t state) {
	witness_assert_positive_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE);

	edata_state_set(edata, state);

	EMAP_DECLARE_RTREE_CTX;
	rtree_leaf_elm_t *elm1 = rtree_leaf_elm_lookup(tsdn, &emap->rtree,
	    rtree_ctx, (uintptr_t)edata_base_get(edata), /* dependent */ true,
	    /* init_missing */ false);
	assert(elm1 != NULL);
	rtree_leaf_elm_t *elm2 = edata_size_get(edata) == PAGE ? NULL :
	    rtree_leaf_elm_lookup(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_last_get(edata), /* dependent */ true,
	    /* init_missing */ false);

	rtree_leaf_elm_state_update(tsdn, &emap->rtree, elm1, elm2, state);

	emap_assert_mapped(tsdn, emap, edata);
}

static inline edata_t *
emap_try_acquire_edata_neighbor_impl(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    extent_pai_t pai, extent_state_t expected_state, bool forward,
    bool expanding) {
	witness_assert_positive_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE);
	assert(!edata_guarded_get(edata));
	assert(!expanding || forward);
	assert(!edata_state_in_transition(expected_state));
	assert(expected_state == extent_state_dirty ||
	       expected_state == extent_state_muzzy ||
	       expected_state == extent_state_retained);

	void *neighbor_addr = forward ? edata_past_get(edata) :
	    edata_before_get(edata);
	/*
	 * This is subtle; the rtree code asserts that its input pointer is
	 * non-NULL, and this is a useful thing to check.  But it's possible
	 * that edata corresponds to an address of (void *)PAGE (in practice,
	 * this has only been observed on FreeBSD when address-space
	 * randomization is on, but it could in principle happen anywhere).  In
	 * this case, edata_before_get(edata) is NULL, triggering the assert.
	 */
	if (neighbor_addr == NULL) {
		return NULL;
	}

	EMAP_DECLARE_RTREE_CTX;
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, &emap->rtree,
	    rtree_ctx, (uintptr_t)neighbor_addr, /* dependent*/ false,
	    /* init_missing */ false);
	if (elm == NULL) {
		return NULL;
	}

	rtree_contents_t neighbor_contents = rtree_leaf_elm_read(tsdn,
	    &emap->rtree, elm, /* dependent */ true);
	if (!extent_can_acquire_neighbor(edata, neighbor_contents, pai,
	    expected_state, forward, expanding)) {
		return NULL;
	}

	/* From this point, the neighbor edata can be safely acquired. */
	edata_t *neighbor = neighbor_contents.edata;
	assert(edata_state_get(neighbor) == expected_state);
	emap_update_edata_state(tsdn, emap, neighbor, extent_state_merging);
	if (expanding) {
		extent_assert_can_expand(edata, neighbor);
	} else {
		extent_assert_can_coalesce(edata, neighbor);
	}

	return neighbor;
}

edata_t *
emap_try_acquire_edata_neighbor(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    extent_pai_t pai, extent_state_t expected_state, bool forward) {
	return emap_try_acquire_edata_neighbor_impl(tsdn, emap, edata, pai,
	    expected_state, forward, /* expand */ false);
}

edata_t *
emap_try_acquire_edata_neighbor_expand(tsdn_t *tsdn, emap_t *emap,
    edata_t *edata, extent_pai_t pai, extent_state_t expected_state) {
	/* Try expanding forward. */
	return emap_try_acquire_edata_neighbor_impl(tsdn, emap, edata, pai,
	    expected_state, /* forward */ true, /* expand */ true);
}

void
emap_release_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    extent_state_t new_state) {
	assert(emap_edata_in_transition(tsdn, emap, edata));
	assert(emap_edata_is_acquired(tsdn, emap, edata));

	emap_update_edata_state(tsdn, emap, edata, new_state);
}

static bool
emap_rtree_leaf_elms_lookup(tsdn_t *tsdn, emap_t *emap, rtree_ctx_t *rtree_ctx,
    const edata_t *edata, bool dependent, bool init_missing,
    rtree_leaf_elm_t **r_elm_a, rtree_leaf_elm_t **r_elm_b) {
	*r_elm_a = rtree_leaf_elm_lookup(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata), dependent, init_missing);
	if (!dependent && *r_elm_a == NULL) {
		return true;
	}
	assert(*r_elm_a != NULL);

	*r_elm_b = rtree_leaf_elm_lookup(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_last_get(edata), dependent, init_missing);
	if (!dependent && *r_elm_b == NULL) {
		return true;
	}
	assert(*r_elm_b != NULL);

	return false;
}

static void
emap_rtree_write_acquired(tsdn_t *tsdn, emap_t *emap, rtree_leaf_elm_t *elm_a,
    rtree_leaf_elm_t *elm_b, edata_t *edata, szind_t szind, bool slab) {
	rtree_contents_t contents;
	contents.edata = edata;
	contents.metadata.szind = szind;
	contents.metadata.slab = slab;
	contents.metadata.is_head = (edata == NULL) ? false :
	    edata_is_head_get(edata);
	contents.metadata.state = (edata == NULL) ? 0 : edata_state_get(edata);
	rtree_leaf_elm_write(tsdn, &emap->rtree, elm_a, contents);
	if (elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &emap->rtree, elm_b, contents);
	}
}

bool
emap_register_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind, bool slab) {
	assert(edata_state_get(edata) == extent_state_active);
	EMAP_DECLARE_RTREE_CTX;

	rtree_leaf_elm_t *elm_a, *elm_b;
	bool err = emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, edata,
	    false, true, &elm_a, &elm_b);
	if (err) {
		return true;
	}
	assert(rtree_leaf_elm_read(tsdn, &emap->rtree, elm_a,
	    /* dependent */ false).edata == NULL);
	assert(rtree_leaf_elm_read(tsdn, &emap->rtree, elm_b,
	    /* dependent */ false).edata == NULL);
	emap_rtree_write_acquired(tsdn, emap, elm_a, elm_b, edata, szind, slab);
	return false;
}

/* Invoked *after* emap_register_boundary. */
void
emap_register_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind) {
	EMAP_DECLARE_RTREE_CTX;

	assert(edata_slab_get(edata));
	assert(edata_state_get(edata) == extent_state_active);

	if (config_debug) {
		/* Making sure the boundary is registered already. */
		rtree_leaf_elm_t *elm_a, *elm_b;
		bool err = emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx,
		    edata, /* dependent */ true, /* init_missing */ false,
		    &elm_a, &elm_b);
		assert(!err);
		rtree_contents_t contents_a, contents_b;
		contents_a = rtree_leaf_elm_read(tsdn, &emap->rtree, elm_a,
		    /* dependent */ true);
		contents_b = rtree_leaf_elm_read(tsdn, &emap->rtree, elm_b,
		    /* dependent */ true);
		assert(contents_a.edata == edata && contents_b.edata == edata);
		assert(contents_a.metadata.slab && contents_b.metadata.slab);
	}

	rtree_contents_t contents;
	contents.edata = edata;
	contents.metadata.szind = szind;
	contents.metadata.slab = true;
	contents.metadata.state = extent_state_active;
	contents.metadata.is_head = false; /* Not allowed to access. */

	assert(edata_size_get(edata) > (2 << LG_PAGE));
	rtree_write_range(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata) + PAGE,
	    (uintptr_t)edata_last_get(edata) - PAGE, contents);
}

void
emap_deregister_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	/*
	 * The edata must be either in an acquired state, or protected by state
	 * based locks.
	 */
	if (!emap_edata_is_acquired(tsdn, emap, edata)) {
		witness_assert_positive_depth_to_rank(
		    tsdn_witness_tsdp_get(tsdn), WITNESS_RANK_CORE);
	}

	EMAP_DECLARE_RTREE_CTX;
	rtree_leaf_elm_t *elm_a, *elm_b;

	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, edata,
	    true, false, &elm_a, &elm_b);
	emap_rtree_write_acquired(tsdn, emap, elm_a, elm_b, NULL, SC_NSIZES,
	    false);
}

void
emap_deregister_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	EMAP_DECLARE_RTREE_CTX;

	assert(edata_slab_get(edata));
	if (edata_size_get(edata) > (2 << LG_PAGE)) {
		rtree_clear_range(tsdn, &emap->rtree, rtree_ctx,
		    (uintptr_t)edata_base_get(edata) + PAGE,
		    (uintptr_t)edata_last_get(edata) - PAGE);
	}
}

void
emap_remap(tsdn_t *tsdn, emap_t *emap, edata_t *edata, szind_t szind,
    bool slab) {
	EMAP_DECLARE_RTREE_CTX;

	if (szind != SC_NSIZES) {
		rtree_contents_t contents;
		contents.edata = edata;
		contents.metadata.szind = szind;
		contents.metadata.slab = slab;
		contents.metadata.is_head = edata_is_head_get(edata);
		contents.metadata.state = edata_state_get(edata);

		rtree_write(tsdn, &emap->rtree, rtree_ctx,
		    (uintptr_t)edata_addr_get(edata), contents);
		/*
		 * Recall that this is called only for active->inactive and
		 * inactive->active transitions (since only active extents have
		 * meaningful values for szind and slab).  Active, non-slab
		 * extents only need to handle lookups at their head (on
		 * deallocation), so we don't bother filling in the end
		 * boundary.
		 *
		 * For slab extents, we do the end-mapping change.  This still
		 * leaves the interior unmodified; an emap_register_interior
		 * call is coming in those cases, though.
		 */
		if (slab && edata_size_get(edata) > PAGE) {
			uintptr_t key = (uintptr_t)edata_past_get(edata)
			    - (uintptr_t)PAGE;
			rtree_write(tsdn, &emap->rtree, rtree_ctx, key,
			    contents);
		}
	}
}

bool
emap_split_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *edata, size_t size_a, edata_t *trail, size_t size_b) {
	EMAP_DECLARE_RTREE_CTX;

	/*
	 * We use incorrect constants for things like arena ind, zero, ranged,
	 * and commit state, and head status.  This is a fake edata_t, used to
	 * facilitate a lookup.
	 */
	edata_t lead = {0};
	edata_init(&lead, 0U, edata_addr_get(edata), size_a, false, 0, 0,
	    extent_state_active, false, false, EXTENT_PAI_PAC, EXTENT_NOT_HEAD);

	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, &lead, false, true,
	    &prepare->lead_elm_a, &prepare->lead_elm_b);
	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, trail, false, true,
	    &prepare->trail_elm_a, &prepare->trail_elm_b);

	if (prepare->lead_elm_a == NULL || prepare->lead_elm_b == NULL
	    || prepare->trail_elm_a == NULL || prepare->trail_elm_b == NULL) {
		return true;
	}
	return false;
}

void
emap_split_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, size_t size_a, edata_t *trail, size_t size_b) {
	/*
	 * We should think about not writing to the lead leaf element.  We can
	 * get into situations where a racing realloc-like call can disagree
	 * with a size lookup request.  I think it's fine to declare that these
	 * situations are race bugs, but there's an argument to be made that for
	 * things like xallocx, a size lookup call should return either the old
	 * size or the new size, but not anything else.
	 */
	emap_rtree_write_acquired(tsdn, emap, prepare->lead_elm_a,
	    prepare->lead_elm_b, lead, SC_NSIZES, /* slab */ false);
	emap_rtree_write_acquired(tsdn, emap, prepare->trail_elm_a,
	    prepare->trail_elm_b, trail, SC_NSIZES, /* slab */ false);
}

void
emap_merge_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail) {
	EMAP_DECLARE_RTREE_CTX;
	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, lead, true, false,
	    &prepare->lead_elm_a, &prepare->lead_elm_b);
	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, trail, true, false,
	    &prepare->trail_elm_a, &prepare->trail_elm_b);
}

void
emap_merge_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail) {
	rtree_contents_t clear_contents;
	clear_contents.edata = NULL;
	clear_contents.metadata.szind = SC_NSIZES;
	clear_contents.metadata.slab = false;
	clear_contents.metadata.is_head = false;
	clear_contents.metadata.state = (extent_state_t)0;

	if (prepare->lead_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &emap->rtree,
		    prepare->lead_elm_b, clear_contents);
	}

	rtree_leaf_elm_t *merged_b;
	if (prepare->trail_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &emap->rtree,
		    prepare->trail_elm_a, clear_contents);
		merged_b = prepare->trail_elm_b;
	} else {
		merged_b = prepare->trail_elm_a;
	}

	emap_rtree_write_acquired(tsdn, emap, prepare->lead_elm_a, merged_b,
	    lead, SC_NSIZES, false);
}

void
emap_do_assert_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	EMAP_DECLARE_RTREE_CTX;

	rtree_contents_t contents = rtree_read(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata));
	assert(contents.edata == edata);
	assert(contents.metadata.is_head == edata_is_head_get(edata));
	assert(contents.metadata.state == edata_state_get(edata));
}

void
emap_do_assert_not_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	emap_full_alloc_ctx_t context1 = {0};
	emap_full_alloc_ctx_try_lookup(tsdn, emap, edata_base_get(edata),
	    &context1);
	assert(context1.edata == NULL);

	emap_full_alloc_ctx_t context2 = {0};
	emap_full_alloc_ctx_try_lookup(tsdn, emap, edata_last_get(edata),
	    &context2);
	assert(context2.edata == NULL);
}
