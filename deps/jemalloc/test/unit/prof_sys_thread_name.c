#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_sys.h"

static const char *test_thread_name = "test_name";

static int
test_prof_sys_thread_name_read_error(char *buf, size_t limit) {
	return ENOSYS;
}

static int
test_prof_sys_thread_name_read(char *buf, size_t limit) {
	assert(strlen(test_thread_name) < limit);
	strncpy(buf, test_thread_name, limit);
	return 0;
}

static int
test_prof_sys_thread_name_read_clear(char *buf, size_t limit) {
	assert(limit > 0);
	buf[0] = '\0';
	return 0;
}

TEST_BEGIN(test_prof_sys_thread_name) {
	test_skip_if(!config_prof);

	bool oldval;
	size_t sz = sizeof(oldval);
	assert_d_eq(mallctl("opt.prof_sys_thread_name", &oldval, &sz, NULL, 0),
	    0, "mallctl failed");
	assert_true(oldval, "option was not set correctly");

	const char *thread_name;
	sz = sizeof(thread_name);
	assert_d_eq(mallctl("thread.prof.name", &thread_name, &sz, NULL, 0), 0,
	    "mallctl read for thread name should not fail");
	expect_str_eq(thread_name, "", "Initial thread name should be empty");

	thread_name = test_thread_name;
	assert_d_eq(mallctl("thread.prof.name", NULL, NULL, &thread_name, sz),
	    ENOENT, "mallctl write for thread name should fail");
	assert_ptr_eq(thread_name, test_thread_name,
	    "Thread name should not be touched");

	prof_sys_thread_name_read = test_prof_sys_thread_name_read_error;
	void *p = malloc(1);
	free(p);
	assert_d_eq(mallctl("thread.prof.name", &thread_name, &sz, NULL, 0), 0,
	    "mallctl read for thread name should not fail");
	assert_str_eq(thread_name, "",
	    "Thread name should stay the same if the system call fails");

	prof_sys_thread_name_read = test_prof_sys_thread_name_read;
	p = malloc(1);
	free(p);
	assert_d_eq(mallctl("thread.prof.name", &thread_name, &sz, NULL, 0), 0,
	    "mallctl read for thread name should not fail");
	assert_str_eq(thread_name, test_thread_name,
	    "Thread name should be changed if the system call succeeds");

	prof_sys_thread_name_read = test_prof_sys_thread_name_read_clear;
	p = malloc(1);
	free(p);
	assert_d_eq(mallctl("thread.prof.name", &thread_name, &sz, NULL, 0), 0,
	    "mallctl read for thread name should not fail");
	expect_str_eq(thread_name, "", "Thread name should be updated if the "
	    "system call returns a different name");
}
TEST_END

int
main(void) {
	return test(
	    test_prof_sys_thread_name);
}
