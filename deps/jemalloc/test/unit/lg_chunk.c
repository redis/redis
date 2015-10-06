#include "test/jemalloc_test.h"

/*
 * Make sure that opt.lg_chunk clamping is sufficient.  In practice, this test
 * program will fail a debug assertion during initialization and abort (rather
 * than the test soft-failing) if clamping is insufficient.
 */
const char *malloc_conf = "lg_chunk:0";

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
