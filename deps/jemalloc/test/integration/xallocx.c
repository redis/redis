#include "test/jemalloc_test.h"

TEST_BEGIN(test_same_size)
{
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz, 0, 0);
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_extra_no_move)
{
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz, sz-42, 0);
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_no_move_fail)
{
	void *p;
	size_t sz, tsz;

	p = mallocx(42, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	sz = sallocx(p, 0);

	tsz = xallocx(p, sz + 5, 0, 0);
	assert_zu_eq(tsz, sz, "Unexpected size change: %zu --> %zu", sz, tsz);

	dallocx(p, 0);
}
TEST_END

int
main(void)
{

	return (test(
	    test_same_size,
	    test_extra_no_move,
	    test_no_move_fail));
}
