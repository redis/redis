#include "test/jemalloc_test.h"

#include "jemalloc/internal/psset.h"

#define PAGESLAB_ADDR ((void *)(1234 * HUGEPAGE))
#define PAGESLAB_AGE 5678

#define ALLOC_ARENA_IND 111
#define ALLOC_ESN 222

static void
edata_init_test(edata_t *edata) {
	memset(edata, 0, sizeof(*edata));
	edata_arena_ind_set(edata, ALLOC_ARENA_IND);
	edata_esn_set(edata, ALLOC_ESN);
}

static void
test_psset_fake_purge(hpdata_t *ps) {
	hpdata_purge_state_t purge_state;
	hpdata_alloc_allowed_set(ps, false);
	hpdata_purge_begin(ps, &purge_state);
	void *addr;
	size_t size;
	while (hpdata_purge_next(ps, &purge_state, &addr, &size)) {
	}
	hpdata_purge_end(ps, &purge_state);
	hpdata_alloc_allowed_set(ps, true);
}

static void
test_psset_alloc_new(psset_t *psset, hpdata_t *ps, edata_t *r_edata,
    size_t size) {
	hpdata_assert_empty(ps);

	test_psset_fake_purge(ps);

	psset_insert(psset, ps);
	psset_update_begin(psset, ps);

        void *addr = hpdata_reserve_alloc(ps, size);
        edata_init(r_edata, edata_arena_ind_get(r_edata), addr, size,
	    /* slab */ false, SC_NSIZES, /* sn */ 0, extent_state_active,
            /* zeroed */ false, /* committed */ true, EXTENT_PAI_HPA,
            EXTENT_NOT_HEAD);
        edata_ps_set(r_edata, ps);
	psset_update_end(psset, ps);
}

static bool
test_psset_alloc_reuse(psset_t *psset, edata_t *r_edata, size_t size) {
	hpdata_t *ps = psset_pick_alloc(psset, size);
	if (ps == NULL) {
		return true;
	}
	psset_update_begin(psset, ps);
	void *addr = hpdata_reserve_alloc(ps, size);
	edata_init(r_edata, edata_arena_ind_get(r_edata), addr, size,
	    /* slab */ false, SC_NSIZES, /* sn */ 0, extent_state_active,
	    /* zeroed */ false, /* committed */ true, EXTENT_PAI_HPA,
	    EXTENT_NOT_HEAD);
	edata_ps_set(r_edata, ps);
	psset_update_end(psset, ps);
	return false;
}

static hpdata_t *
test_psset_dalloc(psset_t *psset, edata_t *edata) {
	hpdata_t *ps = edata_ps_get(edata);
	psset_update_begin(psset, ps);
	hpdata_unreserve(ps, edata_addr_get(edata), edata_size_get(edata));
	psset_update_end(psset, ps);
	if (hpdata_empty(ps)) {
		psset_remove(psset, ps);
		return ps;
	} else {
		return NULL;
	}
}

static void
edata_expect(edata_t *edata, size_t page_offset, size_t page_cnt) {
	/*
	 * Note that allocations should get the arena ind of their home
	 * arena, *not* the arena ind of the pageslab allocator.
	 */
	expect_u_eq(ALLOC_ARENA_IND, edata_arena_ind_get(edata),
	    "Arena ind changed");
	expect_ptr_eq(
	    (void *)((uintptr_t)PAGESLAB_ADDR + (page_offset << LG_PAGE)),
	    edata_addr_get(edata), "Didn't allocate in order");
	expect_zu_eq(page_cnt << LG_PAGE, edata_size_get(edata), "");
	expect_false(edata_slab_get(edata), "");
	expect_u_eq(SC_NSIZES, edata_szind_get_maybe_invalid(edata),
	    "");
	expect_u64_eq(0, edata_sn_get(edata), "");
	expect_d_eq(edata_state_get(edata), extent_state_active, "");
	expect_false(edata_zeroed_get(edata), "");
	expect_true(edata_committed_get(edata), "");
	expect_d_eq(EXTENT_PAI_HPA, edata_pai_get(edata), "");
	expect_false(edata_is_head_get(edata), "");
}

TEST_BEGIN(test_empty) {
	bool err;
	hpdata_t pageslab;
	hpdata_init(&pageslab, PAGESLAB_ADDR, PAGESLAB_AGE);

	edata_t alloc;
	edata_init_test(&alloc);

	psset_t psset;
	psset_init(&psset);

	/* Empty psset should return fail allocations. */
	err = test_psset_alloc_reuse(&psset, &alloc, PAGE);
	expect_true(err, "Empty psset succeeded in an allocation.");
}
TEST_END

TEST_BEGIN(test_fill) {
	bool err;

	hpdata_t pageslab;
	hpdata_init(&pageslab, PAGESLAB_ADDR, PAGESLAB_AGE);

	edata_t alloc[HUGEPAGE_PAGES];

	psset_t psset;
	psset_init(&psset);

	edata_init_test(&alloc[0]);
	test_psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < HUGEPAGE_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = test_psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
	}

	for (size_t i = 0; i < HUGEPAGE_PAGES; i++) {
		edata_t *edata = &alloc[i];
		edata_expect(edata, i, 1);
	}

	/* The pageslab, and thus psset, should now have no allocations. */
	edata_t extra_alloc;
	edata_init_test(&extra_alloc);
	err = test_psset_alloc_reuse(&psset, &extra_alloc, PAGE);
	expect_true(err, "Alloc succeeded even though psset should be empty");
}
TEST_END

TEST_BEGIN(test_reuse) {
	bool err;
	hpdata_t *ps;

	hpdata_t pageslab;
	hpdata_init(&pageslab, PAGESLAB_ADDR, PAGESLAB_AGE);

	edata_t alloc[HUGEPAGE_PAGES];

	psset_t psset;
	psset_init(&psset);

	edata_init_test(&alloc[0]);
	test_psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < HUGEPAGE_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = test_psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
	}

	/* Free odd indices. */
	for (size_t i = 0; i < HUGEPAGE_PAGES; i ++) {
		if (i % 2 == 0) {
			continue;
		}
		ps = test_psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	/* Realloc into them. */
	for (size_t i = 0; i < HUGEPAGE_PAGES; i++) {
		if (i % 2 == 0) {
			continue;
		}
		err = test_psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		edata_expect(&alloc[i], i, 1);
	}
	/* Now, free the pages at indices 0 or 1 mod 2. */
	for (size_t i = 0; i < HUGEPAGE_PAGES; i++) {
		if (i % 4 > 1) {
			continue;
		}
		ps = test_psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	/* And realloc 2-page allocations into them. */
	for (size_t i = 0; i < HUGEPAGE_PAGES; i++) {
		if (i % 4 != 0) {
			continue;
		}
		err = test_psset_alloc_reuse(&psset, &alloc[i], 2 * PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		edata_expect(&alloc[i], i, 2);
	}
	/* Free all the 2-page allocations. */
	for (size_t i = 0; i < HUGEPAGE_PAGES; i++) {
		if (i % 4 != 0) {
			continue;
		}
		ps = test_psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	/*
	 * Free up a 1-page hole next to a 2-page hole, but somewhere in the
	 * middle of the pageslab.  Index 11 should be right before such a hole
	 * (since 12 % 4 == 0).
	 */
	size_t index_of_3 = 11;
	ps = test_psset_dalloc(&psset, &alloc[index_of_3]);
	expect_ptr_null(ps, "Nonempty pageslab evicted");
	err = test_psset_alloc_reuse(&psset, &alloc[index_of_3], 3 * PAGE);
	expect_false(err, "Should have been able to find alloc.");
	edata_expect(&alloc[index_of_3], index_of_3, 3);

	/*
	 * Free up a 4-page hole at the end.  Recall that the pages at offsets 0
	 * and 1 mod 4 were freed above, so we just have to free the last
	 * allocations.
	 */
	ps = test_psset_dalloc(&psset, &alloc[HUGEPAGE_PAGES - 1]);
	expect_ptr_null(ps, "Nonempty pageslab evicted");
	ps = test_psset_dalloc(&psset, &alloc[HUGEPAGE_PAGES - 2]);
	expect_ptr_null(ps, "Nonempty pageslab evicted");

	/* Make sure we can satisfy an allocation at the very end of a slab. */
	size_t index_of_4 = HUGEPAGE_PAGES - 4;
	err = test_psset_alloc_reuse(&psset, &alloc[index_of_4], 4 * PAGE);
	expect_false(err, "Should have been able to find alloc.");
	edata_expect(&alloc[index_of_4], index_of_4, 4);
}
TEST_END

TEST_BEGIN(test_evict) {
	bool err;
	hpdata_t *ps;

	hpdata_t pageslab;
	hpdata_init(&pageslab, PAGESLAB_ADDR, PAGESLAB_AGE);

	edata_t alloc[HUGEPAGE_PAGES];

	psset_t psset;
	psset_init(&psset);

	/* Alloc the whole slab. */
	edata_init_test(&alloc[0]);
	test_psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < HUGEPAGE_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = test_psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Unxpected allocation failure");
	}

	/* Dealloc the whole slab, going forwards. */
	for (size_t i = 0; i < HUGEPAGE_PAGES - 1; i++) {
		ps = test_psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	ps = test_psset_dalloc(&psset, &alloc[HUGEPAGE_PAGES - 1]);
	expect_ptr_eq(&pageslab, ps, "Empty pageslab not evicted.");

	err = test_psset_alloc_reuse(&psset, &alloc[0], PAGE);
	expect_true(err, "psset should be empty.");
}
TEST_END

TEST_BEGIN(test_multi_pageslab) {
	bool err;
	hpdata_t *ps;

	hpdata_t pageslab[2];
	hpdata_init(&pageslab[0], PAGESLAB_ADDR, PAGESLAB_AGE);
	hpdata_init(&pageslab[1],
	    (void *)((uintptr_t)PAGESLAB_ADDR + HUGEPAGE),
	    PAGESLAB_AGE + 1);

	edata_t alloc[2][HUGEPAGE_PAGES];

	psset_t psset;
	psset_init(&psset);

	/* Insert both slabs. */
	edata_init_test(&alloc[0][0]);
	test_psset_alloc_new(&psset, &pageslab[0], &alloc[0][0], PAGE);
	edata_init_test(&alloc[1][0]);
	test_psset_alloc_new(&psset, &pageslab[1], &alloc[1][0], PAGE);

	/* Fill them both up; make sure we do so in first-fit order. */
	for (size_t i = 0; i < 2; i++) {
		for (size_t j = 1; j < HUGEPAGE_PAGES; j++) {
			edata_init_test(&alloc[i][j]);
			err = test_psset_alloc_reuse(&psset, &alloc[i][j], PAGE);
			expect_false(err,
			    "Nonempty psset failed page allocation.");
			assert_ptr_eq(&pageslab[i], edata_ps_get(&alloc[i][j]),
			    "Didn't pick pageslabs in first-fit");
		}
	}

	/*
	 * Free up a 2-page hole in the earlier slab, and a 1-page one in the
	 * later one.  We should still pick the later one.
	 */
	ps = test_psset_dalloc(&psset, &alloc[0][0]);
	expect_ptr_null(ps, "Unexpected eviction");
	ps = test_psset_dalloc(&psset, &alloc[0][1]);
	expect_ptr_null(ps, "Unexpected eviction");
	ps = test_psset_dalloc(&psset, &alloc[1][0]);
	expect_ptr_null(ps, "Unexpected eviction");
	err = test_psset_alloc_reuse(&psset, &alloc[0][0], PAGE);
	expect_ptr_eq(&pageslab[1], edata_ps_get(&alloc[0][0]),
	    "Should have picked the fuller pageslab");

	/*
	 * Now both slabs have 1-page holes. Free up a second one in the later
	 * slab.
	 */
	ps = test_psset_dalloc(&psset, &alloc[1][1]);
	expect_ptr_null(ps, "Unexpected eviction");

	/*
	 * We should be able to allocate a 2-page object, even though an earlier
	 * size class is nonempty.
	 */
	err = test_psset_alloc_reuse(&psset, &alloc[1][0], 2 * PAGE);
	expect_false(err, "Allocation should have succeeded");
}
TEST_END

static void
stats_expect_empty(psset_bin_stats_t *stats) {
	assert_zu_eq(0, stats->npageslabs,
	    "Supposedly empty bin had positive npageslabs");
	expect_zu_eq(0, stats->nactive, "Unexpected nonempty bin"
	    "Supposedly empty bin had positive nactive");
}

static void
stats_expect(psset_t *psset, size_t nactive) {
	if (nactive == HUGEPAGE_PAGES) {
		expect_zu_eq(1, psset->stats.full_slabs[0].npageslabs,
		    "Expected a full slab");
		expect_zu_eq(HUGEPAGE_PAGES,
		    psset->stats.full_slabs[0].nactive,
		    "Should have exactly filled the bin");
	} else {
		stats_expect_empty(&psset->stats.full_slabs[0]);
	}
	size_t ninactive = HUGEPAGE_PAGES - nactive;
	pszind_t nonempty_pind = PSSET_NPSIZES;
	if (ninactive != 0 && ninactive < HUGEPAGE_PAGES) {
		nonempty_pind = sz_psz2ind(sz_psz_quantize_floor(
		    ninactive << LG_PAGE));
	}
	for (pszind_t i = 0; i < PSSET_NPSIZES; i++) {
		if (i == nonempty_pind) {
			assert_zu_eq(1,
			    psset->stats.nonfull_slabs[i][0].npageslabs,
			    "Should have found a slab");
			expect_zu_eq(nactive,
			    psset->stats.nonfull_slabs[i][0].nactive,
			    "Mismatch in active pages");
		} else {
			stats_expect_empty(&psset->stats.nonfull_slabs[i][0]);
		}
	}
	expect_zu_eq(nactive, psset_nactive(psset), "");
}

TEST_BEGIN(test_stats) {
	bool err;

	hpdata_t pageslab;
	hpdata_init(&pageslab, PAGESLAB_ADDR, PAGESLAB_AGE);

	edata_t alloc[HUGEPAGE_PAGES];

	psset_t psset;
	psset_init(&psset);
	stats_expect(&psset, 0);

	edata_init_test(&alloc[0]);
	test_psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < HUGEPAGE_PAGES; i++) {
		stats_expect(&psset, i);
		edata_init_test(&alloc[i]);
		err = test_psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
	}
	stats_expect(&psset, HUGEPAGE_PAGES);
	hpdata_t *ps;
	for (ssize_t i = HUGEPAGE_PAGES - 1; i >= 0; i--) {
		ps = test_psset_dalloc(&psset, &alloc[i]);
		expect_true((ps == NULL) == (i != 0),
		    "test_psset_dalloc should only evict a slab on the last "
		    "free");
		stats_expect(&psset, i);
	}

	test_psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	stats_expect(&psset, 1);
	psset_update_begin(&psset, &pageslab);
	stats_expect(&psset, 0);
	psset_update_end(&psset, &pageslab);
	stats_expect(&psset, 1);
}
TEST_END

/*
 * Fills in and inserts two pageslabs, with the first better than the second,
 * and each fully allocated (into the allocations in allocs and worse_allocs,
 * each of which should be HUGEPAGE_PAGES long), except for a single free page
 * at the end.
 *
 * (There's nothing magic about these numbers; it's just useful to share the
 * setup between the oldest fit and the insert/remove test).
 */
static void
init_test_pageslabs(psset_t *psset, hpdata_t *pageslab,
    hpdata_t *worse_pageslab, edata_t *alloc, edata_t *worse_alloc) {
	bool err;

	hpdata_init(pageslab, (void *)(10 * HUGEPAGE), PAGESLAB_AGE);
	/*
	 * This pageslab would be better from an address-first-fit POV, but
	 * worse from an age POV.
	 */
	hpdata_init(worse_pageslab, (void *)(9 * HUGEPAGE), PAGESLAB_AGE + 1);

	psset_init(psset);

	edata_init_test(&alloc[0]);
	test_psset_alloc_new(psset, pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < HUGEPAGE_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = test_psset_alloc_reuse(psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		expect_ptr_eq(pageslab, edata_ps_get(&alloc[i]),
		    "Allocated from the wrong pageslab");
	}

	edata_init_test(&worse_alloc[0]);
	test_psset_alloc_new(psset, worse_pageslab, &worse_alloc[0], PAGE);
	expect_ptr_eq(worse_pageslab, edata_ps_get(&worse_alloc[0]),
	    "Allocated from the wrong pageslab");
	/*
	 * Make the two pssets otherwise indistinguishable; all full except for
	 * a single page.
	 */
	for (size_t i = 1; i < HUGEPAGE_PAGES - 1; i++) {
		edata_init_test(&worse_alloc[i]);
		err = test_psset_alloc_reuse(psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		expect_ptr_eq(worse_pageslab, edata_ps_get(&alloc[i]),
		    "Allocated from the wrong pageslab");
	}

	/* Deallocate the last page from the older pageslab. */
	hpdata_t *evicted = test_psset_dalloc(psset,
	    &alloc[HUGEPAGE_PAGES - 1]);
	expect_ptr_null(evicted, "Unexpected eviction");
}

TEST_BEGIN(test_oldest_fit) {
	bool err;
	edata_t alloc[HUGEPAGE_PAGES];
	edata_t worse_alloc[HUGEPAGE_PAGES];

	hpdata_t pageslab;
	hpdata_t worse_pageslab;

	psset_t psset;

	init_test_pageslabs(&psset, &pageslab, &worse_pageslab, alloc,
	    worse_alloc);

	/* The edata should come from the better pageslab. */
	edata_t test_edata;
	edata_init_test(&test_edata);
	err = test_psset_alloc_reuse(&psset, &test_edata, PAGE);
	expect_false(err, "Nonempty psset failed page allocation");
	expect_ptr_eq(&pageslab, edata_ps_get(&test_edata),
	    "Allocated from the wrong pageslab");
}
TEST_END

TEST_BEGIN(test_insert_remove) {
	bool err;
	hpdata_t *ps;
	edata_t alloc[HUGEPAGE_PAGES];
	edata_t worse_alloc[HUGEPAGE_PAGES];

	hpdata_t pageslab;
	hpdata_t worse_pageslab;

	psset_t psset;

	init_test_pageslabs(&psset, &pageslab, &worse_pageslab, alloc,
	    worse_alloc);

	/* Remove better; should still be able to alloc from worse. */
	psset_update_begin(&psset, &pageslab);
	err = test_psset_alloc_reuse(&psset, &worse_alloc[HUGEPAGE_PAGES - 1],
	    PAGE);
	expect_false(err, "Removal should still leave an empty page");
	expect_ptr_eq(&worse_pageslab,
	    edata_ps_get(&worse_alloc[HUGEPAGE_PAGES - 1]),
	    "Allocated out of wrong ps");

	/*
	 * After deallocating the previous alloc and reinserting better, it
	 * should be preferred for future allocations.
	 */
	ps = test_psset_dalloc(&psset, &worse_alloc[HUGEPAGE_PAGES - 1]);
	expect_ptr_null(ps, "Incorrect eviction of nonempty pageslab");
	psset_update_end(&psset, &pageslab);
	err = test_psset_alloc_reuse(&psset, &alloc[HUGEPAGE_PAGES - 1], PAGE);
	expect_false(err, "psset should be nonempty");
	expect_ptr_eq(&pageslab, edata_ps_get(&alloc[HUGEPAGE_PAGES - 1]),
	    "Removal/reinsertion shouldn't change ordering");
	/*
	 * After deallocating and removing both, allocations should fail.
	 */
	ps = test_psset_dalloc(&psset, &alloc[HUGEPAGE_PAGES - 1]);
	expect_ptr_null(ps, "Incorrect eviction");
	psset_update_begin(&psset, &pageslab);
	psset_update_begin(&psset, &worse_pageslab);
	err = test_psset_alloc_reuse(&psset, &alloc[HUGEPAGE_PAGES - 1], PAGE);
	expect_true(err, "psset should be empty, but an alloc succeeded");
}
TEST_END

TEST_BEGIN(test_purge_prefers_nonhuge) {
	/*
	 * All else being equal, we should prefer purging non-huge pages over
	 * huge ones for non-empty extents.
	 */

	/* Nothing magic about this constant. */
	enum {
		NHP = 23,
	};
	hpdata_t *hpdata;

	psset_t psset;
	psset_init(&psset);

	hpdata_t hpdata_huge[NHP];
	uintptr_t huge_begin = (uintptr_t)&hpdata_huge[0];
	uintptr_t huge_end = (uintptr_t)&hpdata_huge[NHP];
	hpdata_t hpdata_nonhuge[NHP];
	uintptr_t nonhuge_begin = (uintptr_t)&hpdata_nonhuge[0];
	uintptr_t nonhuge_end = (uintptr_t)&hpdata_nonhuge[NHP];

	for (size_t i = 0; i < NHP; i++) {
		hpdata_init(&hpdata_huge[i], (void *)((10 + i) * HUGEPAGE),
		    123 + i);
		psset_insert(&psset, &hpdata_huge[i]);

		hpdata_init(&hpdata_nonhuge[i],
		    (void *)((10 + NHP + i) * HUGEPAGE),
		    456 + i);
		psset_insert(&psset, &hpdata_nonhuge[i]);

	}
	for (int i = 0; i < 2 * NHP; i++) {
		hpdata = psset_pick_alloc(&psset, HUGEPAGE * 3 / 4);
		psset_update_begin(&psset, hpdata);
		void *ptr;
		ptr = hpdata_reserve_alloc(hpdata, HUGEPAGE * 3 / 4);
		/* Ignore the first alloc, which will stick around. */
		(void)ptr;
		/*
		 * The second alloc is to dirty the pages; free it immediately
		 * after allocating.
		 */
		ptr = hpdata_reserve_alloc(hpdata, HUGEPAGE / 4);
		hpdata_unreserve(hpdata, ptr, HUGEPAGE / 4);

		if (huge_begin <= (uintptr_t)hpdata
		    && (uintptr_t)hpdata < huge_end) {
			hpdata_hugify(hpdata);
		}

		hpdata_purge_allowed_set(hpdata, true);
		psset_update_end(&psset, hpdata);
	}

	/*
	 * We've got a bunch of 1/8th dirty hpdatas.  It should give us all the
	 * non-huge ones to purge, then all the huge ones, then refuse to purge
	 * further.
	 */
	for (int i = 0; i < NHP; i++) {
		hpdata = psset_pick_purge(&psset);
		assert_true(nonhuge_begin <= (uintptr_t)hpdata
		    && (uintptr_t)hpdata < nonhuge_end, "");
		psset_update_begin(&psset, hpdata);
		test_psset_fake_purge(hpdata);
		hpdata_purge_allowed_set(hpdata, false);
		psset_update_end(&psset, hpdata);
	}
	for (int i = 0; i < NHP; i++) {
		hpdata = psset_pick_purge(&psset);
		expect_true(huge_begin <= (uintptr_t)hpdata
		    && (uintptr_t)hpdata < huge_end, "");
		psset_update_begin(&psset, hpdata);
		hpdata_dehugify(hpdata);
		test_psset_fake_purge(hpdata);
		hpdata_purge_allowed_set(hpdata, false);
		psset_update_end(&psset, hpdata);
	}
}
TEST_END

TEST_BEGIN(test_purge_prefers_empty) {
	void *ptr;

	psset_t psset;
	psset_init(&psset);

	hpdata_t hpdata_empty;
	hpdata_t hpdata_nonempty;
	hpdata_init(&hpdata_empty, (void *)(10 * HUGEPAGE), 123);
	psset_insert(&psset, &hpdata_empty);
	hpdata_init(&hpdata_nonempty, (void *)(11 * HUGEPAGE), 456);
	psset_insert(&psset, &hpdata_nonempty);

	psset_update_begin(&psset, &hpdata_empty);
	ptr = hpdata_reserve_alloc(&hpdata_empty, PAGE);
	expect_ptr_eq(hpdata_addr_get(&hpdata_empty), ptr, "");
	hpdata_unreserve(&hpdata_empty, ptr, PAGE);
	hpdata_purge_allowed_set(&hpdata_empty, true);
	psset_update_end(&psset, &hpdata_empty);

	psset_update_begin(&psset, &hpdata_nonempty);
	ptr = hpdata_reserve_alloc(&hpdata_nonempty, 10 * PAGE);
	expect_ptr_eq(hpdata_addr_get(&hpdata_nonempty), ptr, "");
	hpdata_unreserve(&hpdata_nonempty, ptr, 9 * PAGE);
	hpdata_purge_allowed_set(&hpdata_nonempty, true);
	psset_update_end(&psset, &hpdata_nonempty);

	/*
	 * The nonempty slab has 9 dirty pages, while the empty one has only 1.
	 * We should still pick the empty one for purging.
	 */
	hpdata_t *to_purge = psset_pick_purge(&psset);
	expect_ptr_eq(&hpdata_empty, to_purge, "");
}
TEST_END

TEST_BEGIN(test_purge_prefers_empty_huge) {
	void *ptr;

	psset_t psset;
	psset_init(&psset);

	enum {NHP = 10 };

	hpdata_t hpdata_huge[NHP];
	hpdata_t hpdata_nonhuge[NHP];

	uintptr_t cur_addr = 100 * HUGEPAGE;
	uint64_t cur_age = 123;
	for (int i = 0; i < NHP; i++) {
		hpdata_init(&hpdata_huge[i], (void *)cur_addr, cur_age);
		cur_addr += HUGEPAGE;
		cur_age++;
		psset_insert(&psset, &hpdata_huge[i]);

		hpdata_init(&hpdata_nonhuge[i], (void *)cur_addr, cur_age);
		cur_addr += HUGEPAGE;
		cur_age++;
		psset_insert(&psset, &hpdata_nonhuge[i]);

		/*
		 * Make the hpdata_huge[i] fully dirty, empty, purgable, and
		 * huge.
		 */
		psset_update_begin(&psset, &hpdata_huge[i]);
		ptr = hpdata_reserve_alloc(&hpdata_huge[i], HUGEPAGE);
		expect_ptr_eq(hpdata_addr_get(&hpdata_huge[i]), ptr, "");
		hpdata_hugify(&hpdata_huge[i]);
		hpdata_unreserve(&hpdata_huge[i], ptr, HUGEPAGE);
		hpdata_purge_allowed_set(&hpdata_huge[i], true);
		psset_update_end(&psset, &hpdata_huge[i]);

		/*
		 * Make hpdata_nonhuge[i] fully dirty, empty, purgable, and
		 * non-huge.
		 */
		psset_update_begin(&psset, &hpdata_nonhuge[i]);
		ptr = hpdata_reserve_alloc(&hpdata_nonhuge[i], HUGEPAGE);
		expect_ptr_eq(hpdata_addr_get(&hpdata_nonhuge[i]), ptr, "");
		hpdata_unreserve(&hpdata_nonhuge[i], ptr, HUGEPAGE);
		hpdata_purge_allowed_set(&hpdata_nonhuge[i], true);
		psset_update_end(&psset, &hpdata_nonhuge[i]);
	}

	/*
	 * We have a bunch of empty slabs, half huge, half nonhuge, inserted in
	 * alternating order.  We should pop all the huge ones before popping
	 * any of the non-huge ones for purging.
	 */
	for (int i = 0; i < NHP; i++) {
		hpdata_t *to_purge = psset_pick_purge(&psset);
		expect_ptr_eq(&hpdata_huge[i], to_purge, "");
		psset_update_begin(&psset, to_purge);
		hpdata_purge_allowed_set(to_purge, false);
		psset_update_end(&psset, to_purge);
	}
	for (int i = 0; i < NHP; i++) {
		hpdata_t *to_purge = psset_pick_purge(&psset);
		expect_ptr_eq(&hpdata_nonhuge[i], to_purge, "");
		psset_update_begin(&psset, to_purge);
		hpdata_purge_allowed_set(to_purge, false);
		psset_update_end(&psset, to_purge);
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_empty,
	    test_fill,
	    test_reuse,
	    test_evict,
	    test_multi_pageslab,
	    test_stats,
	    test_oldest_fit,
	    test_insert_remove,
	    test_purge_prefers_nonhuge,
	    test_purge_prefers_empty,
	    test_purge_prefers_empty_huge);
}
