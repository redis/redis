#include "test/jemalloc_test.h"

TEST_BEGIN(test_pages_huge)
{
	bool commit;
	void *pages;

	commit = true;
	pages = pages_map(NULL, PAGE, &commit);
	assert_ptr_not_null(pages, "Unexpected pages_map() error");

	assert_false(pages_huge(pages, PAGE),
	    "Unexpected pages_huge() result");
	assert_false(pages_nohuge(pages, PAGE),
	    "Unexpected pages_nohuge() result");

	pages_unmap(pages, PAGE);
}
TEST_END

int
main(void)
{

	return (test(
	    test_pages_huge));
}
