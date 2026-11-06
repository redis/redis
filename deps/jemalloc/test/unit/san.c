#include "test/jemalloc_test.h"
#include "test/arena_util.h"
#include "test/san.h"

#include "jemalloc/internal/san.h"

static void
verify_extent_guarded(tsdn_t *tsdn, void *ptr) {
	expect_true(extent_is_guarded(tsdn, ptr),
	    "All extents should be guarded.");
}

#define MAX_SMALL_ALLOCATIONS 4096
void *small_alloc[MAX_SMALL_ALLOCATIONS];

/*
 * This test allocates page sized slabs and checks that every two slabs have
 * at least one page in between them. That page is supposed to be the guard
 * page.
 */
TEST_BEGIN(test_guarded_small) {
	test_skip_if(opt_prof);

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	unsigned npages = 16, pages_found = 0, ends_found = 0;
	VARIABLE_ARRAY(uintptr_t, pages, npages);

	/* Allocate to get sanitized pointers. */
	size_t slab_sz = PAGE;
	size_t sz = slab_sz / 8;
	unsigned n_alloc = 0;
	while (n_alloc < MAX_SMALL_ALLOCATIONS) {
		void *ptr = malloc(sz);
		expect_ptr_not_null(ptr, "Unexpected malloc() failure");
		small_alloc[n_alloc] = ptr;
		verify_extent_guarded(tsdn, ptr);
		if ((uintptr_t)ptr % PAGE == 0) {
			assert_u_lt(pages_found, npages,
			    "Unexpectedly large number of page aligned allocs");
			pages[pages_found++] = (uintptr_t)ptr;
		}
		if (((uintptr_t)ptr + (uintptr_t)sz) % PAGE == 0) {
			ends_found++;
		}
		n_alloc++;
		if (pages_found == npages && ends_found == npages) {
			break;
		}
	}
	/* Should found the ptrs being checked for overflow and underflow. */
	expect_u_eq(pages_found, npages, "Could not found the expected pages.");
	expect_u_eq(ends_found, npages, "Could not found the expected pages.");

	/* Verify the pages are not continuous, i.e. separated by guards. */
	for (unsigned i = 0; i < npages - 1; i++) {
		for (unsigned j = i + 1; j < npages; j++) {
			uintptr_t ptr_diff = pages[i] > pages[j] ?
			    pages[i] - pages[j] : pages[j] - pages[i];
			expect_zu_ge((size_t)ptr_diff, slab_sz + PAGE,
			    "There should be at least one pages between "
			    "guarded slabs");
		}
	}

	for (unsigned i = 0; i < n_alloc + 1; i++) {
		free(small_alloc[i]);
	}
}
TEST_END

TEST_BEGIN(test_guarded_large) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	unsigned nlarge = 32;
	VARIABLE_ARRAY(uintptr_t, large, nlarge);

	/* Allocate to get sanitized pointers. */
	size_t large_sz = SC_LARGE_MINCLASS;
	for (unsigned i = 0; i < nlarge; i++) {
		void *ptr = malloc(large_sz);
		verify_extent_guarded(tsdn, ptr);
		expect_ptr_not_null(ptr, "Unexpected malloc() failure");
		large[i] = (uintptr_t)ptr;
	}

	/* Verify the pages are not continuous, i.e. separated by guards. */
	for (unsigned i = 0; i < nlarge; i++) {
		for (unsigned j = i + 1; j < nlarge; j++) {
			uintptr_t ptr_diff = large[i] > large[j] ?
			    large[i] - large[j] : large[j] - large[i];
			expect_zu_ge((size_t)ptr_diff, large_sz + 2 * PAGE,
			    "There should be at least two pages between "
			    " guarded large allocations");
		}
	}

	for (unsigned i = 0; i < nlarge; i++) {
		free((void *)large[i]);
	}
}
TEST_END

static void
verify_pdirty(unsigned arena_ind, uint64_t expected) {
	uint64_t pdirty = get_arena_pdirty(arena_ind);
	expect_u64_eq(pdirty, expected / PAGE,
	    "Unexpected dirty page amount.");
}

static void
verify_pmuzzy(unsigned arena_ind, uint64_t expected) {
	uint64_t pmuzzy = get_arena_pmuzzy(arena_ind);
	expect_u64_eq(pmuzzy, expected / PAGE,
	    "Unexpected muzzy page amount.");
}

TEST_BEGIN(test_guarded_decay) {
	unsigned arena_ind = do_arena_create(-1, -1);
	do_decay(arena_ind);
	do_purge(arena_ind);

	verify_pdirty(arena_ind, 0);
	verify_pmuzzy(arena_ind, 0);

	/* Verify that guarded extents as dirty. */
	size_t sz1 = PAGE, sz2 = PAGE * 2;
	/* W/o maps_coalesce, guarded extents are unguarded eagerly. */
	size_t add_guard_size = maps_coalesce ? 0 : SAN_PAGE_GUARDS_SIZE;
	generate_dirty(arena_ind, sz1);
	verify_pdirty(arena_ind, sz1 + add_guard_size);
	verify_pmuzzy(arena_ind, 0);

	/* Should reuse the first extent. */
	generate_dirty(arena_ind, sz1);
	verify_pdirty(arena_ind, sz1 + add_guard_size);
	verify_pmuzzy(arena_ind, 0);

	/* Should not reuse; expect new dirty pages. */
	generate_dirty(arena_ind, sz2);
	verify_pdirty(arena_ind, sz1 + sz2 + 2 * add_guard_size);
	verify_pmuzzy(arena_ind, 0);

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/* Should reuse dirty extents for the two mallocx. */
	void *p1 = do_mallocx(sz1, flags);
	verify_extent_guarded(tsdn, p1);
	verify_pdirty(arena_ind, sz2 + add_guard_size);

	void *p2 = do_mallocx(sz2, flags);
	verify_extent_guarded(tsdn, p2);
	verify_pdirty(arena_ind, 0);
	verify_pmuzzy(arena_ind, 0);

	dallocx(p1, flags);
	verify_pdirty(arena_ind, sz1 + add_guard_size);
	dallocx(p2, flags);
	verify_pdirty(arena_ind, sz1 + sz2 + 2 * add_guard_size);
	verify_pmuzzy(arena_ind, 0);

	do_purge(arena_ind);
	verify_pdirty(arena_ind, 0);
	verify_pmuzzy(arena_ind, 0);

	if (config_stats) {
		expect_u64_eq(get_arena_npurge(arena_ind), 1,
		    "Expected purging to occur");
		expect_u64_eq(get_arena_dirty_npurge(arena_ind), 1,
		    "Expected purging to occur");
		expect_u64_eq(get_arena_dirty_purged(arena_ind),
		    (sz1 + sz2 + 2 * add_guard_size) / PAGE,
		    "Expected purging to occur");
		expect_u64_eq(get_arena_muzzy_npurge(arena_ind), 0,
		    "Expected purging to occur");
	}

	if (opt_retain) {
		/*
		 * With retain, guarded extents are not mergable and will be
		 * cached in ecache_retained.  They should be reused.
		 */
		void *new_p1 = do_mallocx(sz1, flags);
		verify_extent_guarded(tsdn, p1);
		expect_ptr_eq(p1, new_p1, "Expect to reuse p1");

		void *new_p2 = do_mallocx(sz2, flags);
		verify_extent_guarded(tsdn, p2);
		expect_ptr_eq(p2, new_p2, "Expect to reuse p2");

		dallocx(new_p1, flags);
		verify_pdirty(arena_ind, sz1 + add_guard_size);
		dallocx(new_p2, flags);
		verify_pdirty(arena_ind, sz1 + sz2 + 2 * add_guard_size);
		verify_pmuzzy(arena_ind, 0);
	}

	do_arena_destroy(arena_ind);
}
TEST_END

int
main(void) {
	return test(
	    test_guarded_small,
	    test_guarded_large,
	    test_guarded_decay);
}
