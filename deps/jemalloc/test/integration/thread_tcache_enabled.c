#include "test/jemalloc_test.h"

void *
thd_start(void *arg) {
	bool e0, e1;
	size_t sz = sizeof(bool);
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz, NULL,
	    0), 0, "Unexpected mallctl failure");

	if (e0) {
		e1 = false;
		assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
		    (void *)&e1, sz), 0, "Unexpected mallctl() error");
		assert_true(e0, "tcache should be enabled");
	}

	e1 = true;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_false(e0, "tcache should be disabled");

	e1 = true;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_true(e0, "tcache should be enabled");

	e1 = false;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_true(e0, "tcache should be enabled");

	e1 = false;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_false(e0, "tcache should be disabled");

	free(malloc(1));
	e1 = true;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_false(e0, "tcache should be disabled");

	free(malloc(1));
	e1 = true;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_true(e0, "tcache should be enabled");

	free(malloc(1));
	e1 = false;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_true(e0, "tcache should be enabled");

	free(malloc(1));
	e1 = false;
	assert_d_eq(mallctl("thread.tcache.enabled", (void *)&e0, &sz,
	    (void *)&e1, sz), 0, "Unexpected mallctl() error");
	assert_false(e0, "tcache should be disabled");

	free(malloc(1));
	return NULL;
}

TEST_BEGIN(test_main_thread) {
	thd_start(NULL);
}
TEST_END

TEST_BEGIN(test_subthread) {
	thd_t thd;

	thd_create(&thd, thd_start, NULL);
	thd_join(thd, NULL);
}
TEST_END

int
main(void) {
	/* Run tests multiple times to check for bad interactions. */
	return test(
	    test_main_thread,
	    test_subthread,
	    test_main_thread,
	    test_subthread,
	    test_main_thread);
}
