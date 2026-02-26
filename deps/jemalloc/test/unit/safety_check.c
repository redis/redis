#include "test/jemalloc_test.h"

#include "jemalloc/internal/safety_check.h"

/*
 * Note that we get called through safety_check.sh, which turns on sampling for
 * everything.
 */

bool fake_abort_called;
void fake_abort(const char *message) {
	(void)message;
	fake_abort_called = true;
}

static void
buffer_overflow_write(char *ptr, size_t size) {
	/* Avoid overflow warnings. */
	volatile size_t idx = size;
	ptr[idx] = 0;
}

TEST_BEGIN(test_malloc_free_overflow) {
	test_skip_if(!config_prof);
	test_skip_if(!config_opt_safety_checks);

	safety_check_set_abort(&fake_abort);
	/* Buffer overflow! */
	char* ptr = malloc(128);
	buffer_overflow_write(ptr, 128);
	free(ptr);
	safety_check_set_abort(NULL);

	expect_b_eq(fake_abort_called, true, "Redzone check didn't fire.");
	fake_abort_called = false;
}
TEST_END

TEST_BEGIN(test_mallocx_dallocx_overflow) {
	test_skip_if(!config_prof);
	test_skip_if(!config_opt_safety_checks);

	safety_check_set_abort(&fake_abort);
	/* Buffer overflow! */
	char* ptr = mallocx(128, 0);
	buffer_overflow_write(ptr, 128);
	dallocx(ptr, 0);
	safety_check_set_abort(NULL);

	expect_b_eq(fake_abort_called, true, "Redzone check didn't fire.");
	fake_abort_called = false;
}
TEST_END

TEST_BEGIN(test_malloc_sdallocx_overflow) {
	test_skip_if(!config_prof);
	test_skip_if(!config_opt_safety_checks);

	safety_check_set_abort(&fake_abort);
	/* Buffer overflow! */
	char* ptr = malloc(128);
	buffer_overflow_write(ptr, 128);
	sdallocx(ptr, 128, 0);
	safety_check_set_abort(NULL);

	expect_b_eq(fake_abort_called, true, "Redzone check didn't fire.");
	fake_abort_called = false;
}
TEST_END

TEST_BEGIN(test_realloc_overflow) {
	test_skip_if(!config_prof);
	test_skip_if(!config_opt_safety_checks);

	safety_check_set_abort(&fake_abort);
	/* Buffer overflow! */
	char* ptr = malloc(128);
	buffer_overflow_write(ptr, 128);
	ptr = realloc(ptr, 129);
	safety_check_set_abort(NULL);
	free(ptr);

	expect_b_eq(fake_abort_called, true, "Redzone check didn't fire.");
	fake_abort_called = false;
}
TEST_END

TEST_BEGIN(test_rallocx_overflow) {
	test_skip_if(!config_prof);
	test_skip_if(!config_opt_safety_checks);

	safety_check_set_abort(&fake_abort);
	/* Buffer overflow! */
	char* ptr = malloc(128);
	buffer_overflow_write(ptr, 128);
	ptr = rallocx(ptr, 129, 0);
	safety_check_set_abort(NULL);
	free(ptr);

	expect_b_eq(fake_abort_called, true, "Redzone check didn't fire.");
	fake_abort_called = false;
}
TEST_END

TEST_BEGIN(test_xallocx_overflow) {
	test_skip_if(!config_prof);
	test_skip_if(!config_opt_safety_checks);

	safety_check_set_abort(&fake_abort);
	/* Buffer overflow! */
	char* ptr = malloc(128);
	buffer_overflow_write(ptr, 128);
	size_t result = xallocx(ptr, 129, 0, 0);
	expect_zu_eq(result, 128, "");
	free(ptr);
	expect_b_eq(fake_abort_called, true, "Redzone check didn't fire.");
	fake_abort_called = false;
	safety_check_set_abort(NULL);
}
TEST_END

TEST_BEGIN(test_realloc_no_overflow) {
	char* ptr = malloc(128);
	ptr = realloc(ptr, 256);
	ptr[128] = 0;
	ptr[255] = 0;
	free(ptr);

	ptr = malloc(128);
	ptr = realloc(ptr, 64);
	ptr[63] = 0;
	ptr[0] = 0;
	free(ptr);
}
TEST_END

TEST_BEGIN(test_rallocx_no_overflow) {
	char* ptr = malloc(128);
	ptr = rallocx(ptr, 256, 0);
	ptr[128] = 0;
	ptr[255] = 0;
	free(ptr);

	ptr = malloc(128);
	ptr = rallocx(ptr, 64, 0);
	ptr[63] = 0;
	ptr[0] = 0;
	free(ptr);
}
TEST_END

int
main(void) {
	return test(
	    test_malloc_free_overflow,
	    test_mallocx_dallocx_overflow,
	    test_malloc_sdallocx_overflow,
	    test_realloc_overflow,
	    test_rallocx_overflow,
	    test_xallocx_overflow,
	    test_realloc_no_overflow,
	    test_rallocx_no_overflow);
}
