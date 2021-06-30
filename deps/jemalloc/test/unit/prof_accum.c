#include "test/jemalloc_test.h"

#define NTHREADS		4
#define NALLOCS_PER_THREAD	50
#define DUMP_INTERVAL		1
#define BT_COUNT_CHECK_INTERVAL	5

static int
prof_dump_open_intercept(bool propagate_err, const char *filename) {
	int fd;

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return fd;
}

static void *
alloc_from_permuted_backtrace(unsigned thd_ind, unsigned iteration) {
	return btalloc(1, thd_ind*NALLOCS_PER_THREAD + iteration);
}

static void *
thd_start(void *varg) {
	unsigned thd_ind = *(unsigned *)varg;
	size_t bt_count_prev, bt_count;
	unsigned i_prev, i;

	i_prev = 0;
	bt_count_prev = 0;
	for (i = 0; i < NALLOCS_PER_THREAD; i++) {
		void *p = alloc_from_permuted_backtrace(thd_ind, i);
		dallocx(p, 0);
		if (i % DUMP_INTERVAL == 0) {
			assert_d_eq(mallctl("prof.dump", NULL, NULL, NULL, 0),
			    0, "Unexpected error while dumping heap profile");
		}

		if (i % BT_COUNT_CHECK_INTERVAL == 0 ||
		    i+1 == NALLOCS_PER_THREAD) {
			bt_count = prof_bt_count();
			assert_zu_le(bt_count_prev+(i-i_prev), bt_count,
			    "Expected larger backtrace count increase");
			i_prev = i;
			bt_count_prev = bt_count;
		}
	}

	return NULL;
}

TEST_BEGIN(test_idump) {
	bool active;
	thd_t thds[NTHREADS];
	unsigned thd_args[NTHREADS];
	unsigned i;

	test_skip_if(!config_prof);

	active = true;
	assert_d_eq(mallctl("prof.active", NULL, NULL, (void *)&active,
	    sizeof(active)), 0,
	    "Unexpected mallctl failure while activating profiling");

	prof_dump_open = prof_dump_open_intercept;

	for (i = 0; i < NTHREADS; i++) {
		thd_args[i] = i;
		thd_create(&thds[i], thd_start, (void *)&thd_args[i]);
	}
	for (i = 0; i < NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_idump);
}
