#include <stdio.h>

#include "test/jemalloc_test.h"

/*
 * We can't test C++ in unit tests.  In order to intercept abort, use a secret
 * safety check abort hook in integration tests.
 */
typedef void (*abort_hook_t)(const char *message);
bool fake_abort_called;
void fake_abort(const char *message) {
	if (strcmp(message, "<jemalloc>: Allocation failed and "
	    "opt.experimental_infallible_new is true. Aborting.\n") != 0) {
		abort();
	}
	fake_abort_called = true;
}

static bool
own_operator_new(void) {
	uint64_t before, after;
	size_t sz = sizeof(before);

	/* thread.allocated is always available, even w/o config_stats. */
	expect_d_eq(mallctl("thread.allocated", (void *)&before, &sz, NULL, 0),
	    0, "Unexpected mallctl failure reading stats");
	void *volatile ptr = ::operator new((size_t)8);
	expect_ptr_not_null(ptr, "Unexpected allocation failure");
	expect_d_eq(mallctl("thread.allocated", (void *)&after, &sz, NULL, 0),
	    0, "Unexpected mallctl failure reading stats");

	return (after != before);
}

TEST_BEGIN(test_failing_alloc) {
	abort_hook_t abort_hook = &fake_abort;
	expect_d_eq(mallctl("experimental.hooks.safety_check_abort", NULL, NULL,
	    (void *)&abort_hook, sizeof(abort_hook)), 0,
	    "Unexpected mallctl failure setting abort hook");

	/*
	 * Not owning operator new is only expected to happen on MinGW which
	 * does not support operator new / delete replacement.
	 */
#ifdef _WIN32
	test_skip_if(!own_operator_new());
#else
	expect_true(own_operator_new(), "No operator new overload");
#endif
	void *volatile ptr = (void *)1;
	try {
		/* Too big of an allocation to succeed. */
		ptr = ::operator new((size_t)-1);
	} catch (...) {
		abort();
	}
	expect_ptr_null(ptr, "Allocation should have failed");
	expect_b_eq(fake_abort_called, true, "Abort hook not invoked");
}
TEST_END

int
main(void) {
	return test(
	    test_failing_alloc);
}

