#include "test/jemalloc_test.h"

TEST_BEGIN(test_overflow)
{
	unsigned nhchunks;
	size_t mib[4];
	size_t sz, miblen, max_size_class;
	void *p;

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.nhchunks", &nhchunks, &sz, NULL, 0), 0,
	    "Unexpected mallctl() error");

	miblen = sizeof(mib) / sizeof(size_t);
	assert_d_eq(mallctlnametomib("arenas.hchunk.0.size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() error");
	mib[2] = nhchunks - 1;

	sz = sizeof(size_t);
	assert_d_eq(mallctlbymib(mib, miblen, &max_size_class, &sz, NULL, 0), 0,
	    "Unexpected mallctlbymib() error");

	assert_ptr_null(malloc(max_size_class + 1),
	    "Expected OOM due to over-sized allocation request");
	assert_ptr_null(malloc(SIZE_T_MAX),
	    "Expected OOM due to over-sized allocation request");

	assert_ptr_null(calloc(1, max_size_class + 1),
	    "Expected OOM due to over-sized allocation request");
	assert_ptr_null(calloc(1, SIZE_T_MAX),
	    "Expected OOM due to over-sized allocation request");

	p = malloc(1);
	assert_ptr_not_null(p, "Unexpected malloc() OOM");
	assert_ptr_null(realloc(p, max_size_class + 1),
	    "Expected OOM due to over-sized allocation request");
	assert_ptr_null(realloc(p, SIZE_T_MAX),
	    "Expected OOM due to over-sized allocation request");
	free(p);
}
TEST_END

int
main(void)
{

	return (test(
	    test_overflow));
}
