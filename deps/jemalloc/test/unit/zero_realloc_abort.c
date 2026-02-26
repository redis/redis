#include "test/jemalloc_test.h"

#include <signal.h>

static bool abort_called = false;

void set_abort_called() {
	abort_called = true;
};

TEST_BEGIN(test_realloc_abort) {
	abort_called = false;
	safety_check_set_abort(&set_abort_called);
	void *ptr = mallocx(42, 0);
	expect_ptr_not_null(ptr, "Unexpected mallocx error");
	ptr = realloc(ptr, 0);
	expect_true(abort_called, "Realloc with zero size didn't abort");
}
TEST_END

int
main(void) {
	return test(
	    test_realloc_abort);
}

