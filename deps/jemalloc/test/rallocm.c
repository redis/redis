#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

int
main(void)
{
	void *p, *q;
	size_t sz, tsz;
	int r;

	fprintf(stderr, "Test begin\n");

	r = JEMALLOC_P(allocm)(&p, &sz, 42, 0);
	if (r != ALLOCM_SUCCESS) {
		fprintf(stderr, "Unexpected allocm() error\n");
		abort();
	}

	q = p;
	r = JEMALLOC_P(rallocm)(&q, &tsz, sz, 0, ALLOCM_NO_MOVE);
	if (r != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected rallocm() error\n");
	if (q != p)
		fprintf(stderr, "Unexpected object move\n");
	if (tsz != sz) {
		fprintf(stderr, "Unexpected size change: %zu --> %zu\n",
		    sz, tsz);
	}

	q = p;
	r = JEMALLOC_P(rallocm)(&q, &tsz, sz, 5, ALLOCM_NO_MOVE);
	if (r != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected rallocm() error\n");
	if (q != p)
		fprintf(stderr, "Unexpected object move\n");
	if (tsz != sz) {
		fprintf(stderr, "Unexpected size change: %zu --> %zu\n",
		    sz, tsz);
	}

	q = p;
	r = JEMALLOC_P(rallocm)(&q, &tsz, sz + 5, 0, ALLOCM_NO_MOVE);
	if (r != ALLOCM_ERR_NOT_MOVED)
		fprintf(stderr, "Unexpected rallocm() result\n");
	if (q != p)
		fprintf(stderr, "Unexpected object move\n");
	if (tsz != sz) {
		fprintf(stderr, "Unexpected size change: %zu --> %zu\n",
		    sz, tsz);
	}

	q = p;
	r = JEMALLOC_P(rallocm)(&q, &tsz, sz + 5, 0, 0);
	if (r != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected rallocm() error\n");
	if (q == p)
		fprintf(stderr, "Expected object move\n");
	if (tsz == sz) {
		fprintf(stderr, "Expected size change: %zu --> %zu\n",
		    sz, tsz);
	}
	p = q;
	sz = tsz;

	r = JEMALLOC_P(rallocm)(&q, &tsz, 8192, 0, 0);
	if (r != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected rallocm() error\n");
	if (q == p)
		fprintf(stderr, "Expected object move\n");
	if (tsz == sz) {
		fprintf(stderr, "Expected size change: %zu --> %zu\n",
		    sz, tsz);
	}
	p = q;
	sz = tsz;

	r = JEMALLOC_P(rallocm)(&q, &tsz, 16384, 0, 0);
	if (r != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected rallocm() error\n");
	if (tsz == sz) {
		fprintf(stderr, "Expected size change: %zu --> %zu\n",
		    sz, tsz);
	}
	p = q;
	sz = tsz;

	r = JEMALLOC_P(rallocm)(&q, &tsz, 8192, 0, ALLOCM_NO_MOVE);
	if (r != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected rallocm() error\n");
	if (q != p)
		fprintf(stderr, "Unexpected object move\n");
	if (tsz == sz) {
		fprintf(stderr, "Expected size change: %zu --> %zu\n",
		    sz, tsz);
	}
	sz = tsz;

	r = JEMALLOC_P(rallocm)(&q, &tsz, 16384, 0, ALLOCM_NO_MOVE);
	if (r != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected rallocm() error\n");
	if (q != p)
		fprintf(stderr, "Unexpected object move\n");
	if (tsz == sz) {
		fprintf(stderr, "Expected size change: %zu --> %zu\n",
		    sz, tsz);
	}
	sz = tsz;

	JEMALLOC_P(dallocm)(p, 0);

	fprintf(stderr, "Test end\n");
	return (0);
}
