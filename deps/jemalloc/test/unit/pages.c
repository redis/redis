#include "test/jemalloc_test.h"

TEST_BEGIN(test_pages_huge) {
	size_t alloc_size;
	bool commit;
	void *pages, *hugepage;

	alloc_size = HUGEPAGE * 2 - PAGE;
	commit = true;
	pages = pages_map(NULL, alloc_size, PAGE, &commit);
	expect_ptr_not_null(pages, "Unexpected pages_map() error");

	if (init_system_thp_mode == thp_mode_default) {
	    hugepage = (void *)(ALIGNMENT_CEILING((uintptr_t)pages, HUGEPAGE));
	    expect_b_ne(pages_huge(hugepage, HUGEPAGE), have_madvise_huge,
	        "Unexpected pages_huge() result");
	    expect_false(pages_nohuge(hugepage, HUGEPAGE),
	        "Unexpected pages_nohuge() result");
	}

	pages_unmap(pages, alloc_size);
}
TEST_END

int
main(void) {
	return test(
	    test_pages_huge);
}
