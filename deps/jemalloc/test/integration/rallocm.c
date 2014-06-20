#include "test/jemalloc_test.h"

TEST_BEGIN(test_same_size)
{
	void *p, *q;
	size_t sz, tsz;

	assert_d_eq(allocm(&p, &sz, 42, 0), ALLOCM_SUCCESS,
	    "Unexpected allocm() error");

	q = p;
	assert_d_eq(rallocm(&q, &tsz, sz, 0, ALLOCM_NO_MOVE), ALLOCM_SUCCESS,
	    "Unexpected rallocm() error");
	assert_ptr_eq(q, p, "Unexpected object move");
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	assert_d_eq(dallocm(p, 0), ALLOCM_SUCCESS,
	    "Unexpected dallocm() error");
}
TEST_END

TEST_BEGIN(test_extra_no_move)
{
	void *p, *q;
	size_t sz, tsz;

	assert_d_eq(allocm(&p, &sz, 42, 0), ALLOCM_SUCCESS,
	    "Unexpected allocm() error");

	q = p;
	assert_d_eq(rallocm(&q, &tsz, sz, sz-42, ALLOCM_NO_MOVE),
	    ALLOCM_SUCCESS, "Unexpected rallocm() error");
	assert_ptr_eq(q, p, "Unexpected object move");
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	assert_d_eq(dallocm(p, 0), ALLOCM_SUCCESS,
	    "Unexpected dallocm() error");
}
TEST_END

TEST_BEGIN(test_no_move_fail)
{
	void *p, *q;
	size_t sz, tsz;

	assert_d_eq(allocm(&p, &sz, 42, 0), ALLOCM_SUCCESS,
	    "Unexpected allocm() error");

	q = p;
	assert_d_eq(rallocm(&q, &tsz, sz + 5, 0, ALLOCM_NO_MOVE),
	    ALLOCM_ERR_NOT_MOVED, "Unexpected rallocm() result");
	assert_ptr_eq(q, p, "Unexpected object move");
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	assert_d_eq(dallocm(p, 0), ALLOCM_SUCCESS,
	    "Unexpected dallocm() error");
}
TEST_END

TEST_BEGIN(test_grow_and_shrink)
{
	void *p, *q;
	size_t tsz;
#define	NCYCLES 3
	unsigned i, j;
#define	NSZS 2500
	size_t szs[NSZS];
#define	MAXSZ ZU(12 * 1024 * 1024)

	assert_d_eq(allocm(&p, &szs[0], 1, 0), ALLOCM_SUCCESS,
	    "Unexpected allocm() error");

	for (i = 0; i < NCYCLES; i++) {
		for (j = 1; j < NSZS && szs[j-1] < MAXSZ; j++) {
			q = p;
			assert_d_eq(rallocm(&q, &szs[j], szs[j-1]+1, 0, 0),
			    ALLOCM_SUCCESS,
			    "Unexpected rallocm() error for size=%zu-->%zu",
			    szs[j-1], szs[j-1]+1);
			assert_zu_ne(szs[j], szs[j-1]+1,
			    "Expected size to at least: %zu", szs[j-1]+1);
			p = q;
		}

		for (j--; j > 0; j--) {
			q = p;
			assert_d_eq(rallocm(&q, &tsz, szs[j-1], 0, 0),
			    ALLOCM_SUCCESS,
			    "Unexpected rallocm() error for size=%zu-->%zu",
			    szs[j], szs[j-1]);
			assert_zu_eq(tsz, szs[j-1],
			    "Expected size=%zu, got size=%zu", szs[j-1], tsz);
			p = q;
		}
	}

	assert_d_eq(dallocm(p, 0), ALLOCM_SUCCESS,
	    "Unexpected dallocm() error");
}
TEST_END

int
main(void)
{

	return (test(
	    test_same_size,
	    test_extra_no_move,
	    test_no_move_fail,
	    test_grow_and_shrink));
}
