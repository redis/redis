#include "test/jemalloc_test.h"
#include "test/bench.h"

#define SMALL_ALLOC_SIZE 128
#define LARGE_ALLOC_SIZE SC_LARGE_MINCLASS
#define NALLOCS 1000

/*
 * We make this volatile so the 1-at-a-time variants can't leave the allocation
 * in a register, just to try to get the cache behavior closer.
 */
void *volatile allocs[NALLOCS];

static void
array_alloc_dalloc_small(void) {
	for (int i = 0; i < NALLOCS; i++) {
		void *p = mallocx(SMALL_ALLOC_SIZE, 0);
		assert_ptr_not_null(p, "mallocx shouldn't fail");
		allocs[i] = p;
	}
	for (int i = 0; i < NALLOCS; i++) {
		sdallocx(allocs[i], SMALL_ALLOC_SIZE, 0);
	}
}

static void
item_alloc_dalloc_small(void) {
	for (int i = 0; i < NALLOCS; i++) {
		void *p = mallocx(SMALL_ALLOC_SIZE, 0);
		assert_ptr_not_null(p, "mallocx shouldn't fail");
		allocs[i] = p;
		sdallocx(allocs[i], SMALL_ALLOC_SIZE, 0);
	}
}

TEST_BEGIN(test_array_vs_item_small) {
	compare_funcs(1 * 1000, 10 * 1000,
	    "array of small allocations", array_alloc_dalloc_small,
	    "small item allocation", item_alloc_dalloc_small);
}
TEST_END

static void
array_alloc_dalloc_large(void) {
	for (int i = 0; i < NALLOCS; i++) {
		void *p = mallocx(LARGE_ALLOC_SIZE, 0);
		assert_ptr_not_null(p, "mallocx shouldn't fail");
		allocs[i] = p;
	}
	for (int i = 0; i < NALLOCS; i++) {
		sdallocx(allocs[i], LARGE_ALLOC_SIZE, 0);
	}
}

static void
item_alloc_dalloc_large(void) {
	for (int i = 0; i < NALLOCS; i++) {
		void *p = mallocx(LARGE_ALLOC_SIZE, 0);
		assert_ptr_not_null(p, "mallocx shouldn't fail");
		allocs[i] = p;
		sdallocx(allocs[i], LARGE_ALLOC_SIZE, 0);
	}
}

TEST_BEGIN(test_array_vs_item_large) {
	compare_funcs(100, 1000,
	    "array of large allocations", array_alloc_dalloc_large,
	    "large item allocation", item_alloc_dalloc_large);
}
TEST_END

int main(void) {
	return test_no_reentrancy(
	    test_array_vs_item_small,
	    test_array_vs_item_large);
}
