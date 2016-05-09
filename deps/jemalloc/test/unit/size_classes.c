#include "test/jemalloc_test.h"

static size_t
get_max_size_class(void)
{
	unsigned nhchunks;
	size_t mib[4];
	size_t sz, miblen, max_size_class;

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

	return (max_size_class);
}

TEST_BEGIN(test_size_classes)
{
	size_t size_class, max_size_class;
	szind_t index, max_index;

	max_size_class = get_max_size_class();
	max_index = size2index(max_size_class);

	for (index = 0, size_class = index2size(index); index < max_index ||
	    size_class < max_size_class; index++, size_class =
	    index2size(index)) {
		assert_true(index < max_index,
		    "Loop conditionals should be equivalent; index=%u, "
		    "size_class=%zu (%#zx)", index, size_class, size_class);
		assert_true(size_class < max_size_class,
		    "Loop conditionals should be equivalent; index=%u, "
		    "size_class=%zu (%#zx)", index, size_class, size_class);

		assert_u_eq(index, size2index(size_class),
		    "size2index() does not reverse index2size(): index=%u -->"
		    " size_class=%zu --> index=%u --> size_class=%zu", index,
		    size_class, size2index(size_class),
		    index2size(size2index(size_class)));
		assert_zu_eq(size_class, index2size(size2index(size_class)),
		    "index2size() does not reverse size2index(): index=%u -->"
		    " size_class=%zu --> index=%u --> size_class=%zu", index,
		    size_class, size2index(size_class),
		    index2size(size2index(size_class)));

		assert_u_eq(index+1, size2index(size_class+1),
		    "Next size_class does not round up properly");

		assert_zu_eq(size_class, (index > 0) ?
		    s2u(index2size(index-1)+1) : s2u(1),
		    "s2u() does not round up to size class");
		assert_zu_eq(size_class, s2u(size_class-1),
		    "s2u() does not round up to size class");
		assert_zu_eq(size_class, s2u(size_class),
		    "s2u() does not compute same size class");
		assert_zu_eq(s2u(size_class+1), index2size(index+1),
		    "s2u() does not round up to next size class");
	}

	assert_u_eq(index, size2index(index2size(index)),
	    "size2index() does not reverse index2size()");
	assert_zu_eq(max_size_class, index2size(size2index(max_size_class)),
	    "index2size() does not reverse size2index()");

	assert_zu_eq(size_class, s2u(index2size(index-1)+1),
	    "s2u() does not round up to size class");
	assert_zu_eq(size_class, s2u(size_class-1),
	    "s2u() does not round up to size class");
	assert_zu_eq(size_class, s2u(size_class),
	    "s2u() does not compute same size class");
}
TEST_END

int
main(void)
{

	return (test(
	    test_size_classes));
}
