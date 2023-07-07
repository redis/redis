#include "test/jemalloc_test.h"

TEST_BEGIN(test_zero_alloc) {
	void *res = malloc(0);
	assert(res);
	size_t usable = TEST_MALLOC_SIZE(res);
	assert(usable > 0);
	free(res);
}
TEST_END

int
main(void) {
	return test(
	    test_zero_alloc);
}
