#include "test/jemalloc_test.h"

#include "jemalloc/internal/safety_check.h"

bool fake_abort_called;
void fake_abort(const char *message) {
	(void)message;
	fake_abort_called = true;
}

#define SMALL_SIZE1 SC_SMALL_MAXCLASS
#define SMALL_SIZE2 (SC_SMALL_MAXCLASS / 2)

#define LARGE_SIZE1 SC_LARGE_MINCLASS
#define LARGE_SIZE2 (LARGE_SIZE1 * 2)

void *
test_invalid_size_pre(size_t sz) {
	safety_check_set_abort(&fake_abort);

	fake_abort_called = false;
	void *ptr = malloc(sz);
	assert_ptr_not_null(ptr, "Unexpected failure");

	return ptr;
}

void
test_invalid_size_post(void) {
	expect_true(fake_abort_called, "Safety check didn't fire");
	safety_check_set_abort(NULL);
}

TEST_BEGIN(test_invalid_size_sdallocx) {
	test_skip_if(!config_opt_size_checks);

	void *ptr = test_invalid_size_pre(SMALL_SIZE1);
	sdallocx(ptr, SMALL_SIZE2, 0);
	test_invalid_size_post();

	ptr = test_invalid_size_pre(LARGE_SIZE1);
	sdallocx(ptr, LARGE_SIZE2, 0);
	test_invalid_size_post();
}
TEST_END

TEST_BEGIN(test_invalid_size_sdallocx_nonzero_flag) {
	test_skip_if(!config_opt_size_checks);

	void *ptr = test_invalid_size_pre(SMALL_SIZE1);
	sdallocx(ptr, SMALL_SIZE2, MALLOCX_TCACHE_NONE);
	test_invalid_size_post();

	ptr = test_invalid_size_pre(LARGE_SIZE1);
	sdallocx(ptr, LARGE_SIZE2, MALLOCX_TCACHE_NONE);
	test_invalid_size_post();
}
TEST_END

TEST_BEGIN(test_invalid_size_sdallocx_noflags) {
	test_skip_if(!config_opt_size_checks);

	void *ptr = test_invalid_size_pre(SMALL_SIZE1);
	je_sdallocx_noflags(ptr, SMALL_SIZE2);
	test_invalid_size_post();

	ptr = test_invalid_size_pre(LARGE_SIZE1);
	je_sdallocx_noflags(ptr, LARGE_SIZE2);
	test_invalid_size_post();
}
TEST_END

int
main(void) {
	return test(
	    test_invalid_size_sdallocx,
	    test_invalid_size_sdallocx_nonzero_flag,
	    test_invalid_size_sdallocx_noflags);
}
