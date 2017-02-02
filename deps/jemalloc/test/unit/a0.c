#include "test/jemalloc_test.h"

TEST_BEGIN(test_a0)
{
	void *p;

	p = a0malloc(1);
	assert_ptr_not_null(p, "Unexpected a0malloc() error");
	a0dalloc(p);
}
TEST_END

int
main(void)
{

	return (test_no_malloc_init(
	    test_a0));
}
