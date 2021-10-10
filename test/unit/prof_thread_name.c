#include "test/jemalloc_test.h"

static void
mallctl_thread_name_get_impl(const char *thread_name_expected, const char *func,
    int line) {
	const char *thread_name_old;
	size_t sz;

	sz = sizeof(thread_name_old);
	assert_d_eq(mallctl("thread.prof.name", (void *)&thread_name_old, &sz,
	    NULL, 0), 0,
	    "%s():%d: Unexpected mallctl failure reading thread.prof.name",
	    func, line);
	assert_str_eq(thread_name_old, thread_name_expected,
	    "%s():%d: Unexpected thread.prof.name value", func, line);
}
#define mallctl_thread_name_get(a)					\
	mallctl_thread_name_get_impl(a, __func__, __LINE__)

static void
mallctl_thread_name_set_impl(const char *thread_name, const char *func,
    int line) {
	assert_d_eq(mallctl("thread.prof.name", NULL, NULL,
	    (void *)&thread_name, sizeof(thread_name)), 0,
	    "%s():%d: Unexpected mallctl failure reading thread.prof.name",
	    func, line);
	mallctl_thread_name_get_impl(thread_name, func, line);
}
#define mallctl_thread_name_set(a)					\
	mallctl_thread_name_set_impl(a, __func__, __LINE__)

TEST_BEGIN(test_prof_thread_name_validation) {
	const char *thread_name;

	test_skip_if(!config_prof);

	mallctl_thread_name_get("");
	mallctl_thread_name_set("hi there");

	/* NULL input shouldn't be allowed. */
	thread_name = NULL;
	assert_d_eq(mallctl("thread.prof.name", NULL, NULL,
	    (void *)&thread_name, sizeof(thread_name)), EFAULT,
	    "Unexpected mallctl result writing \"%s\" to thread.prof.name",
	    thread_name);

	/* '\n' shouldn't be allowed. */
	thread_name = "hi\nthere";
	assert_d_eq(mallctl("thread.prof.name", NULL, NULL,
	    (void *)&thread_name, sizeof(thread_name)), EFAULT,
	    "Unexpected mallctl result writing \"%s\" to thread.prof.name",
	    thread_name);

	/* Simultaneous read/write shouldn't be allowed. */
	{
		const char *thread_name_old;
		size_t sz;

		sz = sizeof(thread_name_old);
		assert_d_eq(mallctl("thread.prof.name",
		    (void *)&thread_name_old, &sz, (void *)&thread_name,
		    sizeof(thread_name)), EPERM,
		    "Unexpected mallctl result writing \"%s\" to "
		    "thread.prof.name", thread_name);
	}

	mallctl_thread_name_set("");
}
TEST_END

#define NTHREADS	4
#define NRESET		25
static void *
thd_start(void *varg) {
	unsigned thd_ind = *(unsigned *)varg;
	char thread_name[16] = "";
	unsigned i;

	malloc_snprintf(thread_name, sizeof(thread_name), "thread %u", thd_ind);

	mallctl_thread_name_get("");
	mallctl_thread_name_set(thread_name);

	for (i = 0; i < NRESET; i++) {
		assert_d_eq(mallctl("prof.reset", NULL, NULL, NULL, 0), 0,
		    "Unexpected error while resetting heap profile data");
		mallctl_thread_name_get(thread_name);
	}

	mallctl_thread_name_set(thread_name);
	mallctl_thread_name_set("");

	return NULL;
}

TEST_BEGIN(test_prof_thread_name_threaded) {
	thd_t thds[NTHREADS];
	unsigned thd_args[NTHREADS];
	unsigned i;

	test_skip_if(!config_prof);

	for (i = 0; i < NTHREADS; i++) {
		thd_args[i] = i;
		thd_create(&thds[i], thd_start, (void *)&thd_args[i]);
	}
	for (i = 0; i < NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}
}
TEST_END
#undef NTHREADS
#undef NRESET

int
main(void) {
	return test(
	    test_prof_thread_name_validation,
	    test_prof_thread_name_threaded);
}
