#include "test/jemalloc_test.h"
#include "test/arena_util.h"

#include "jemalloc/internal/arena_structs.h"
#include "jemalloc/internal/san_bump.h"

TEST_BEGIN(test_san_bump_alloc) {
	test_skip_if(!maps_coalesce || !opt_retain);

	tsdn_t *tsdn = tsdn_fetch();

	san_bump_alloc_t sba;
	san_bump_alloc_init(&sba);

	unsigned arena_ind = do_arena_create(0, 0);
	assert_u_ne(arena_ind, UINT_MAX, "Failed to create an arena");

	arena_t *arena = arena_get(tsdn, arena_ind, false);
	pac_t *pac = &arena->pa_shard.pac;

	size_t alloc_size = PAGE * 16;
	size_t alloc_n = alloc_size / sizeof(unsigned);
	edata_t* edata = san_bump_alloc(tsdn, &sba, pac, pac_ehooks_get(pac),
	    alloc_size, /* zero */ false);

	expect_ptr_not_null(edata, "Failed to allocate edata");
	expect_u_eq(edata_arena_ind_get(edata), arena_ind,
	    "Edata was assigned an incorrect arena id");
	expect_zu_eq(edata_size_get(edata), alloc_size,
	    "Allocated edata of incorrect size");
	expect_false(edata_slab_get(edata),
	    "Bump allocator incorrectly assigned 'slab' to true");
	expect_true(edata_committed_get(edata), "Edata is not committed");

	void *ptr = edata_addr_get(edata);
	expect_ptr_not_null(ptr, "Edata was assigned an invalid address");
	/* Test that memory is allocated; no guard pages are misplaced */
	for (unsigned i = 0; i < alloc_n; ++i) {
		((unsigned *)ptr)[i] = 1;
	}

	size_t alloc_size2 = PAGE * 28;
	size_t alloc_n2 = alloc_size / sizeof(unsigned);
	edata_t *edata2 = san_bump_alloc(tsdn, &sba, pac, pac_ehooks_get(pac),
	    alloc_size2, /* zero */ true);

	expect_ptr_not_null(edata2, "Failed to allocate edata");
	expect_u_eq(edata_arena_ind_get(edata2), arena_ind,
	    "Edata was assigned an incorrect arena id");
	expect_zu_eq(edata_size_get(edata2), alloc_size2,
	    "Allocated edata of incorrect size");
	expect_false(edata_slab_get(edata2),
	    "Bump allocator incorrectly assigned 'slab' to true");
	expect_true(edata_committed_get(edata2), "Edata is not committed");

	void *ptr2 = edata_addr_get(edata2);
	expect_ptr_not_null(ptr, "Edata was assigned an invalid address");

	uintptr_t ptrdiff = ptr2 > ptr ? (uintptr_t)ptr2 - (uintptr_t)ptr
	    : (uintptr_t)ptr - (uintptr_t)ptr2;
	size_t between_allocs = (size_t)ptrdiff - alloc_size;

	expect_zu_ge(between_allocs, PAGE,
	    "Guard page between allocs is missing");

	for (unsigned i = 0; i < alloc_n2; ++i) {
		expect_u_eq(((unsigned *)ptr2)[i], 0, "Memory is not zeroed");
	}
}
TEST_END

TEST_BEGIN(test_large_alloc_size) {
	test_skip_if(!maps_coalesce || !opt_retain);

	tsdn_t *tsdn = tsdn_fetch();

	san_bump_alloc_t sba;
	san_bump_alloc_init(&sba);

	unsigned arena_ind = do_arena_create(0, 0);
	assert_u_ne(arena_ind, UINT_MAX, "Failed to create an arena");

	arena_t *arena = arena_get(tsdn, arena_ind, false);
	pac_t *pac = &arena->pa_shard.pac;

	size_t alloc_size = SBA_RETAINED_ALLOC_SIZE * 2;
	edata_t* edata = san_bump_alloc(tsdn, &sba, pac, pac_ehooks_get(pac),
	    alloc_size, /* zero */ false);
	expect_u_eq(edata_arena_ind_get(edata), arena_ind,
	    "Edata was assigned an incorrect arena id");
	expect_zu_eq(edata_size_get(edata), alloc_size,
	    "Allocated edata of incorrect size");
	expect_false(edata_slab_get(edata),
	    "Bump allocator incorrectly assigned 'slab' to true");
	expect_true(edata_committed_get(edata), "Edata is not committed");

	void *ptr = edata_addr_get(edata);
	expect_ptr_not_null(ptr, "Edata was assigned an invalid address");
	/* Test that memory is allocated; no guard pages are misplaced */
	for (unsigned i = 0; i < alloc_size / PAGE; ++i) {
		*((char *)ptr + PAGE * i) = 1;
	}
}
TEST_END

int
main(void) {
	return test(
	    test_san_bump_alloc,
	    test_large_alloc_size);
}
