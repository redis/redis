#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_data.h"

static void
mallctl_bool_get(const char *name, bool expected, const char *func, int line) {
	bool old;
	size_t sz;

	sz = sizeof(old);
	expect_d_eq(mallctl(name, (void *)&old, &sz, NULL, 0), 0,
	    "%s():%d: Unexpected mallctl failure reading %s", func, line, name);
	expect_b_eq(old, expected, "%s():%d: Unexpected %s value", func, line,
	    name);
}

static void
mallctl_bool_set(const char *name, bool old_expected, bool val_new,
    const char *func, int line) {
	bool old;
	size_t sz;

	sz = sizeof(old);
	expect_d_eq(mallctl(name, (void *)&old, &sz, (void *)&val_new,
	    sizeof(val_new)), 0,
	    "%s():%d: Unexpected mallctl failure reading/writing %s", func,
	    line, name);
	expect_b_eq(old, old_expected, "%s():%d: Unexpected %s value", func,
	    line, name);
}

static void
mallctl_prof_active_get_impl(bool prof_active_old_expected, const char *func,
    int line) {
	mallctl_bool_get("prof.active", prof_active_old_expected, func, line);
}
#define mallctl_prof_active_get(a)					\
	mallctl_prof_active_get_impl(a, __func__, __LINE__)

static void
mallctl_prof_active_set_impl(bool prof_active_old_expected,
    bool prof_active_new, const char *func, int line) {
	mallctl_bool_set("prof.active", prof_active_old_expected,
	    prof_active_new, func, line);
}
#define mallctl_prof_active_set(a, b)					\
	mallctl_prof_active_set_impl(a, b, __func__, __LINE__)

static void
mallctl_thread_prof_active_get_impl(bool thread_prof_active_old_expected,
    const char *func, int line) {
	mallctl_bool_get("thread.prof.active", thread_prof_active_old_expected,
	    func, line);
}
#define mallctl_thread_prof_active_get(a)				\
	mallctl_thread_prof_active_get_impl(a, __func__, __LINE__)

static void
mallctl_thread_prof_active_set_impl(bool thread_prof_active_old_expected,
    bool thread_prof_active_new, const char *func, int line) {
	mallctl_bool_set("thread.prof.active", thread_prof_active_old_expected,
	    thread_prof_active_new, func, line);
}
#define mallctl_thread_prof_active_set(a, b)				\
	mallctl_thread_prof_active_set_impl(a, b, __func__, __LINE__)

static void
prof_sampling_probe_impl(bool expect_sample, const char *func, int line) {
	void *p;
	size_t expected_backtraces = expect_sample ? 1 : 0;

	expect_zu_eq(prof_bt_count(), 0, "%s():%d: Expected 0 backtraces", func,
	    line);
	p = mallocx(1, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_zu_eq(prof_bt_count(), expected_backtraces,
	    "%s():%d: Unexpected backtrace count", func, line);
	dallocx(p, 0);
}
#define prof_sampling_probe(a)						\
	prof_sampling_probe_impl(a, __func__, __LINE__)

TEST_BEGIN(test_prof_active) {
	test_skip_if(!config_prof);

	mallctl_prof_active_get(true);
	mallctl_thread_prof_active_get(false);

	mallctl_prof_active_set(true, true);
	mallctl_thread_prof_active_set(false, false);
	/* prof.active, !thread.prof.active. */
	prof_sampling_probe(false);

	mallctl_prof_active_set(true, false);
	mallctl_thread_prof_active_set(false, false);
	/* !prof.active, !thread.prof.active. */
	prof_sampling_probe(false);

	mallctl_prof_active_set(false, false);
	mallctl_thread_prof_active_set(false, true);
	/* !prof.active, thread.prof.active. */
	prof_sampling_probe(false);

	mallctl_prof_active_set(false, true);
	mallctl_thread_prof_active_set(true, true);
	/* prof.active, thread.prof.active. */
	prof_sampling_probe(true);

	/* Restore settings. */
	mallctl_prof_active_set(true, true);
	mallctl_thread_prof_active_set(true, false);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_prof_active);
}
