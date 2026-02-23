#include <memory>

#include "test/jemalloc_test.h"

TEST_BEGIN(test_failing_alloc) {
	bool saw_exception = false;
	try {
		/* Too big of an allocation to succeed. */
		void *volatile ptr = ::operator new((size_t)-1);
		(void)ptr;
	} catch (...) {
		saw_exception = true;
	}
	expect_true(saw_exception, "Didn't get a failure");
}
TEST_END

int
main(void) {
	return test(
	    test_failing_alloc);
}

