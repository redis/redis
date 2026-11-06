#include "test/jemalloc_test.h"

#define NTHREADS	2
#define NINCRS		2000000

TEST_BEGIN(test_mtx_basic) {
	mtx_t mtx;

	expect_false(mtx_init(&mtx), "Unexpected mtx_init() failure");
	mtx_lock(&mtx);
	mtx_unlock(&mtx);
	mtx_fini(&mtx);
}
TEST_END

typedef struct {
	mtx_t		mtx;
	unsigned	x;
} thd_start_arg_t;

static void *
thd_start(void *varg) {
	thd_start_arg_t *arg = (thd_start_arg_t *)varg;
	unsigned i;

	for (i = 0; i < NINCRS; i++) {
		mtx_lock(&arg->mtx);
		arg->x++;
		mtx_unlock(&arg->mtx);
	}
	return NULL;
}

TEST_BEGIN(test_mtx_race) {
	thd_start_arg_t arg;
	thd_t thds[NTHREADS];
	unsigned i;

	expect_false(mtx_init(&arg.mtx), "Unexpected mtx_init() failure");
	arg.x = 0;
	for (i = 0; i < NTHREADS; i++) {
		thd_create(&thds[i], thd_start, (void *)&arg);
	}
	for (i = 0; i < NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}
	expect_u_eq(arg.x, NTHREADS * NINCRS,
	    "Race-related counter corruption");
}
TEST_END

int
main(void) {
	return test(
	    test_mtx_basic,
	    test_mtx_race);
}
