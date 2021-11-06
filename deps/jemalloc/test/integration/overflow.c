#include "test/jemalloc_test.h"

/*
 * GCC "-Walloc-size-larger-than" warning detects when one of the memory
 * allocation functions is called with a size larger than the maximum size that
 * they support. Here we want to explicitly test that the allocation functions
 * do indeed fail properly when this is the case, which triggers the warning.
 * Therefore we disable the warning for these tests.
 */
JEMALLOC_DIAGNOSTIC_PUSH
JEMALLOC_DIAGNOSTIC_IGNORE_ALLOC_SIZE_LARGER_THAN

TEST_BEGIN(test_overflow) {
	unsigned nlextents;
	size_t mib[4];
	size_t sz, miblen, max_size_class;
	void *p;

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.nlextents", (void *)&nlextents, &sz, NULL,
	    0), 0, "Unexpected mallctl() error");

	miblen = sizeof(mib) / sizeof(size_t);
	assert_d_eq(mallctlnametomib("arenas.lextent.0.size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() error");
	mib[2] = nlextents - 1;

	sz = sizeof(size_t);
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&max_size_class, &sz,
	    NULL, 0), 0, "Unexpected mallctlbymib() error");

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

/* Re-enable the "-Walloc-size-larger-than=" warning */
JEMALLOC_DIAGNOSTIC_POP

int
main(void) {
	return test(
	    test_overflow);
}
