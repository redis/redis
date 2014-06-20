#include "test/jemalloc_test.h"

TEST_BEGIN(test_mremap)
{
	int err;
	size_t sz, lg_chunk, chunksize, i;
	char *p, *q;

	sz = sizeof(lg_chunk);
	err = mallctl("opt.lg_chunk", &lg_chunk, &sz, NULL, 0);
	assert_d_eq(err, 0, "Error in mallctl(): %s", strerror(err));
	chunksize = ((size_t)1U) << lg_chunk;

	p = (char *)malloc(chunksize);
	assert_ptr_not_null(p, "malloc(%zu) --> %p", chunksize, p);
	memset(p, 'a', chunksize);

	q = (char *)realloc(p, chunksize * 2);
	assert_ptr_not_null(q, "realloc(%p, %zu) --> %p", p, chunksize * 2,
	    q);
	for (i = 0; i < chunksize; i++) {
		assert_c_eq(q[i], 'a',
		    "realloc() should preserve existing bytes across copies");
	}

	p = q;

	q = (char *)realloc(p, chunksize);
	assert_ptr_not_null(q, "realloc(%p, %zu) --> %p", p, chunksize, q);
	for (i = 0; i < chunksize; i++) {
		assert_c_eq(q[i], 'a',
		    "realloc() should preserve existing bytes across copies");
	}

	free(q);
}
TEST_END

int
main(void)
{

	return (test(
	    test_mremap));
}
