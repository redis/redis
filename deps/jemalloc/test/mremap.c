#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

int
main(void)
{
	int ret, err;
	size_t sz, lg_chunk, chunksize, i;
	char *p, *q;

	fprintf(stderr, "Test begin\n");

	sz = sizeof(lg_chunk);
	if ((err = JEMALLOC_P(mallctl)("opt.lg_chunk", &lg_chunk, &sz, NULL,
	    0))) {
		assert(err != ENOENT);
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		ret = 1;
		goto RETURN;
	}
	chunksize = ((size_t)1U) << lg_chunk;

	p = (char *)malloc(chunksize);
	if (p == NULL) {
		fprintf(stderr, "malloc(%zu) --> %p\n", chunksize, p);
		ret = 1;
		goto RETURN;
	}
	memset(p, 'a', chunksize);

	q = (char *)realloc(p, chunksize * 2);
	if (q == NULL) {
		fprintf(stderr, "realloc(%p, %zu) --> %p\n", p, chunksize * 2,
		    q);
		ret = 1;
		goto RETURN;
	}
	for (i = 0; i < chunksize; i++) {
		assert(q[i] == 'a');
	}

	p = q;

	q = (char *)realloc(p, chunksize);
	if (q == NULL) {
		fprintf(stderr, "realloc(%p, %zu) --> %p\n", p, chunksize, q);
		ret = 1;
		goto RETURN;
	}
	for (i = 0; i < chunksize; i++) {
		assert(q[i] == 'a');
	}

	free(q);

	ret = 0;
RETURN:
	fprintf(stderr, "Test end\n");
	return (ret);
}
