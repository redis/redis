#include "test/jemalloc_test.h"

#define N_PARAM 100
#define N_THREADS 10

static void assert_rep() {
	assert_b_eq(prof_log_rep_check(), false, "Rep check failed");
}

static void assert_log_empty() {
	assert_zu_eq(prof_log_bt_count(), 0,
	    "The log has backtraces; it isn't empty");
	assert_zu_eq(prof_log_thr_count(), 0,
	    "The log has threads; it isn't empty");
	assert_zu_eq(prof_log_alloc_count(), 0,
	    "The log has allocations; it isn't empty");
}

void *buf[N_PARAM];

static void f() {
	int i;
	for (i = 0; i < N_PARAM; i++) {
		buf[i] = malloc(100);
	}
	for (i = 0; i < N_PARAM; i++) {
		free(buf[i]);
	}
}

TEST_BEGIN(test_prof_log_many_logs) {
	int i;

	test_skip_if(!config_prof);

	for (i = 0; i < N_PARAM; i++) {
		assert_b_eq(prof_log_is_logging(), false,
		    "Logging shouldn't have started yet");
		assert_d_eq(mallctl("prof.log_start", NULL, NULL, NULL, 0), 0,
		    "Unexpected mallctl failure when starting logging");
		assert_b_eq(prof_log_is_logging(), true,
		    "Logging should be started by now");
		assert_log_empty();
		assert_rep();
		f();
		assert_zu_eq(prof_log_thr_count(), 1, "Wrong thread count");
		assert_rep();
		assert_b_eq(prof_log_is_logging(), true,
		    "Logging should still be on");
		assert_d_eq(mallctl("prof.log_stop", NULL, NULL, NULL, 0), 0,
		    "Unexpected mallctl failure when stopping logging");
		assert_b_eq(prof_log_is_logging(), false,
		    "Logging should have turned off");
	}
}
TEST_END

thd_t thr_buf[N_THREADS];

static void *f_thread(void *unused) {
	int i;
	for (i = 0; i < N_PARAM; i++) {
		void *p = malloc(100);
		memset(p, 100, sizeof(char));
		free(p);
	}

	return NULL;
}

TEST_BEGIN(test_prof_log_many_threads) {

	test_skip_if(!config_prof);

	int i;
	assert_d_eq(mallctl("prof.log_start", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl failure when starting logging");
	for (i = 0; i < N_THREADS; i++) {
		thd_create(&thr_buf[i], &f_thread, NULL);
	}

	for (i = 0; i < N_THREADS; i++) {
		thd_join(thr_buf[i], NULL);
	}
	assert_zu_eq(prof_log_thr_count(), N_THREADS,
	    "Wrong number of thread entries");
	assert_rep();
	assert_d_eq(mallctl("prof.log_stop", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl failure when stopping logging");
}
TEST_END

static void f3() {
	void *p = malloc(100);
	free(p);
}

static void f1() {
	void *p = malloc(100);
	f3();
	free(p);
}

static void f2() {
	void *p = malloc(100);
	free(p);
}

TEST_BEGIN(test_prof_log_many_traces) {

	test_skip_if(!config_prof);

	assert_d_eq(mallctl("prof.log_start", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl failure when starting logging");
	int i;
	assert_rep();
	assert_log_empty();
	for (i = 0; i < N_PARAM; i++) {
		assert_rep();
		f1();
		assert_rep();
		f2();
		assert_rep();
		f3();
		assert_rep();
	}
	/*
	 * There should be 8 total backtraces: two for malloc/free in f1(), two
	 * for malloc/free in f2(), two for malloc/free in f3(), and then two
	 * for malloc/free in f1()'s call to f3().  However compiler
	 * optimizations such as loop unrolling might generate more call sites.
	 * So >= 8 traces are expected.
	 */
	assert_zu_ge(prof_log_bt_count(), 8,
	    "Expect at least 8 backtraces given sample workload");
	assert_d_eq(mallctl("prof.log_stop", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl failure when stopping logging");
}
TEST_END

int
main(void) {
	prof_log_dummy_set(true);
	return test_no_reentrancy(
	    test_prof_log_many_logs,
	    test_prof_log_many_traces,
	    test_prof_log_many_threads);
}
