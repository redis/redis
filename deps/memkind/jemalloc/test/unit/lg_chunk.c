#include "test/jemalloc_test.h"

TEST_BEGIN(test_lg_chunk_clamp)
{
	void *p;

	p = mallocx(1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	dallocx(p, 0);
}
TEST_END

int
main(void)
{

	return (test(
	    test_lg_chunk_clamp));
}
