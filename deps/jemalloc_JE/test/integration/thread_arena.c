#include "test/jemalloc_test.h"

#define	NTHREADS 10

void *
thd_start(void *arg)
{
	unsigned main_arena_ind = *(unsigned *)arg;
	void *p;
	unsigned arena_ind;
	size_t size;
	int err;

	p = malloc(1);
	assert_ptr_not_null(p, "Error in malloc()");
	free(p);

	size = sizeof(arena_ind);
	if ((err = mallctl("thread.arena", &arena_ind, &size, &main_arena_ind,
	    sizeof(main_arena_ind)))) {
		char buf[BUFERROR_BUF];

		buferror(err, buf, sizeof(buf));
		test_fail("Error in mallctl(): %s", buf);
	}

	size = sizeof(arena_ind);
	if ((err = mallctl("thread.arena", &arena_ind, &size, NULL, 0))) {
		char buf[BUFERROR_BUF];

		buferror(err, buf, sizeof(buf));
		test_fail("Error in mallctl(): %s", buf);
	}
	assert_u_eq(arena_ind, main_arena_ind,
	    "Arena index should be same as for main thread");

	return (NULL);
}

TEST_BEGIN(test_thread_arena)
{
	void *p;
	unsigned arena_ind;
	size_t size;
	int err;
	thd_t thds[NTHREADS];
	unsigned i;

	p = malloc(1);
	assert_ptr_not_null(p, "Error in malloc()");

	size = sizeof(arena_ind);
	if ((err = mallctl("thread.arena", &arena_ind, &size, NULL, 0))) {
		char buf[BUFERROR_BUF];

		buferror(err, buf, sizeof(buf));
		test_fail("Error in mallctl(): %s", buf);
	}

	for (i = 0; i < NTHREADS; i++) {
		thd_create(&thds[i], thd_start,
		    (void *)&arena_ind);
	}

	for (i = 0; i < NTHREADS; i++) {
		intptr_t join_ret;
		thd_join(thds[i], (void *)&join_ret);
		assert_zd_eq(join_ret, 0, "Unexpected thread join error");
	}
}
TEST_END

int
main(void)
{

	return (test(
	    test_thread_arena));
}
