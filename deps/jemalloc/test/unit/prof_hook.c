#include "test/jemalloc_test.h"

const char *dump_filename = "/dev/null";

prof_backtrace_hook_t default_hook;

bool mock_bt_hook_called = false;
bool mock_dump_hook_called = false;

void
mock_bt_hook(void **vec, unsigned *len, unsigned max_len) {
	*len = max_len;
	for (unsigned i = 0; i < max_len; ++i) {
		vec[i] = (void *)((uintptr_t)i);
	}
	mock_bt_hook_called = true;
}

void
mock_bt_augmenting_hook(void **vec, unsigned *len, unsigned max_len) {
	default_hook(vec, len, max_len);
	expect_u_gt(*len, 0, "Default backtrace hook returned empty backtrace");
	expect_u_lt(*len, max_len,
	    "Default backtrace hook returned too large backtrace");

	/* Add a separator between default frames and augmented */
	vec[*len] = (void *)0x030303030;
	(*len)++;

	/* Add more stack frames */
	for (unsigned i = 0; i < 3; ++i) {
		if (*len == max_len) {
			break;
		}
		vec[*len] = (void *)((uintptr_t)i);
		(*len)++;
	}


	mock_bt_hook_called = true;
}

void
mock_dump_hook(const char *filename) {
	mock_dump_hook_called = true;
	expect_str_eq(filename, dump_filename,
	    "Incorrect file name passed to the dump hook");
}

TEST_BEGIN(test_prof_backtrace_hook_replace) {

	test_skip_if(!config_prof);

	mock_bt_hook_called = false;

	void *p0 = mallocx(1, 0);
	assert_ptr_not_null(p0, "Failed to allocate");

	expect_false(mock_bt_hook_called, "Called mock hook before it's set");

	prof_backtrace_hook_t null_hook = NULL;
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    NULL, 0, (void *)&null_hook,  sizeof(null_hook)),
		EINVAL, "Incorrectly allowed NULL backtrace hook");

	size_t default_hook_sz = sizeof(prof_backtrace_hook_t);
	prof_backtrace_hook_t hook = &mock_bt_hook;
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    (void *)&default_hook, &default_hook_sz, (void *)&hook,
	    sizeof(hook)), 0, "Unexpected mallctl failure setting hook");

	void *p1 = mallocx(1, 0);
	assert_ptr_not_null(p1, "Failed to allocate");

	expect_true(mock_bt_hook_called, "Didn't call mock hook");

	prof_backtrace_hook_t current_hook;
	size_t current_hook_sz = sizeof(prof_backtrace_hook_t);
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    (void *)&current_hook, &current_hook_sz, (void *)&default_hook,
	    sizeof(default_hook)), 0,
	    "Unexpected mallctl failure resetting hook to default");

	expect_ptr_eq(current_hook, hook,
	    "Hook returned by mallctl is not equal to mock hook");

	dallocx(p1, 0);
	dallocx(p0, 0);
}
TEST_END

TEST_BEGIN(test_prof_backtrace_hook_augment) {

	test_skip_if(!config_prof);

	mock_bt_hook_called = false;

	void *p0 = mallocx(1, 0);
	assert_ptr_not_null(p0, "Failed to allocate");

	expect_false(mock_bt_hook_called, "Called mock hook before it's set");

	size_t default_hook_sz = sizeof(prof_backtrace_hook_t);
	prof_backtrace_hook_t hook = &mock_bt_augmenting_hook;
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    (void *)&default_hook, &default_hook_sz, (void *)&hook,
	    sizeof(hook)), 0, "Unexpected mallctl failure setting hook");

	void *p1 = mallocx(1, 0);
	assert_ptr_not_null(p1, "Failed to allocate");

	expect_true(mock_bt_hook_called, "Didn't call mock hook");

	prof_backtrace_hook_t current_hook;
	size_t current_hook_sz = sizeof(prof_backtrace_hook_t);
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    (void *)&current_hook, &current_hook_sz, (void *)&default_hook,
	    sizeof(default_hook)), 0,
	    "Unexpected mallctl failure resetting hook to default");

	expect_ptr_eq(current_hook, hook,
	    "Hook returned by mallctl is not equal to mock hook");

	dallocx(p1, 0);
	dallocx(p0, 0);
}
TEST_END

TEST_BEGIN(test_prof_dump_hook) {

	test_skip_if(!config_prof);

	mock_dump_hook_called = false;

	expect_d_eq(mallctl("prof.dump", NULL, NULL, (void *)&dump_filename,
	    sizeof(dump_filename)), 0, "Failed to dump heap profile");

	expect_false(mock_dump_hook_called, "Called dump hook before it's set");

	size_t default_hook_sz = sizeof(prof_dump_hook_t);
	prof_dump_hook_t hook = &mock_dump_hook;
	expect_d_eq(mallctl("experimental.hooks.prof_dump",
	    (void *)&default_hook, &default_hook_sz, (void *)&hook,
	    sizeof(hook)), 0, "Unexpected mallctl failure setting hook");

	expect_d_eq(mallctl("prof.dump", NULL, NULL, (void *)&dump_filename,
	    sizeof(dump_filename)), 0, "Failed to dump heap profile");

	expect_true(mock_dump_hook_called, "Didn't call mock hook");

	prof_dump_hook_t current_hook;
	size_t current_hook_sz = sizeof(prof_dump_hook_t);
	expect_d_eq(mallctl("experimental.hooks.prof_dump",
	    (void *)&current_hook, &current_hook_sz, (void *)&default_hook,
	    sizeof(default_hook)), 0,
	    "Unexpected mallctl failure resetting hook to default");

	expect_ptr_eq(current_hook, hook,
	    "Hook returned by mallctl is not equal to mock hook");
}
TEST_END

int
main(void) {
	return test(
	    test_prof_backtrace_hook_replace,
	    test_prof_backtrace_hook_augment,
	    test_prof_dump_hook);
}
