#include "test/jemalloc_test.h"

const char *malloc_conf = "dirty_decay_ms:1000";
const char *malloc_conf_2_conf_harder = "dirty_decay_ms:1234";

TEST_BEGIN(test_malloc_conf_2) {
#ifdef _WIN32
	bool windows = true;
#else
	bool windows = false;
#endif
	/* Windows doesn't support weak symbol linker trickery. */
	test_skip_if(windows);

	ssize_t dirty_decay_ms;
	size_t sz = sizeof(dirty_decay_ms);

	int err = mallctl("opt.dirty_decay_ms", &dirty_decay_ms, &sz, NULL, 0);
	assert_d_eq(err, 0, "Unexpected mallctl failure");
	expect_zd_eq(dirty_decay_ms, 1234,
	    "malloc_conf_2 setting didn't take effect");
}
TEST_END

int
main(void) {
	return test(
	    test_malloc_conf_2);
}
