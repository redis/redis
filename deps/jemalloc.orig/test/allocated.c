#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

void *
thread_start(void *arg)
{
	int err;
	void *p;
	uint64_t a0, a1, d0, d1;
	uint64_t *ap0, *ap1, *dp0, *dp1;
	size_t sz, usize;

	sz = sizeof(a0);
	if ((err = JEMALLOC_P(mallctl)("thread.allocated", &a0, &sz, NULL,
	    0))) {
		if (err == ENOENT) {
#ifdef JEMALLOC_STATS
			assert(false);
#endif
			goto RETURN;
		}
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		exit(1);
	}
	sz = sizeof(ap0);
	if ((err = JEMALLOC_P(mallctl)("thread.allocatedp", &ap0, &sz, NULL,
	    0))) {
		if (err == ENOENT) {
#ifdef JEMALLOC_STATS
			assert(false);
#endif
			goto RETURN;
		}
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		exit(1);
	}
	assert(*ap0 == a0);

	sz = sizeof(d0);
	if ((err = JEMALLOC_P(mallctl)("thread.deallocated", &d0, &sz, NULL,
	    0))) {
		if (err == ENOENT) {
#ifdef JEMALLOC_STATS
			assert(false);
#endif
			goto RETURN;
		}
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		exit(1);
	}
	sz = sizeof(dp0);
	if ((err = JEMALLOC_P(mallctl)("thread.deallocatedp", &dp0, &sz, NULL,
	    0))) {
		if (err == ENOENT) {
#ifdef JEMALLOC_STATS
			assert(false);
#endif
			goto RETURN;
		}
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		exit(1);
	}
	assert(*dp0 == d0);

	p = JEMALLOC_P(malloc)(1);
	if (p == NULL) {
		fprintf(stderr, "%s(): Error in malloc()\n", __func__);
		exit(1);
	}

	sz = sizeof(a1);
	JEMALLOC_P(mallctl)("thread.allocated", &a1, &sz, NULL, 0);
	sz = sizeof(ap1);
	JEMALLOC_P(mallctl)("thread.allocatedp", &ap1, &sz, NULL, 0);
	assert(*ap1 == a1);
	assert(ap0 == ap1);

	usize = JEMALLOC_P(malloc_usable_size)(p);
	assert(a0 + usize <= a1);

	JEMALLOC_P(free)(p);

	sz = sizeof(d1);
	JEMALLOC_P(mallctl)("thread.deallocated", &d1, &sz, NULL, 0);
	sz = sizeof(dp1);
	JEMALLOC_P(mallctl)("thread.deallocatedp", &dp1, &sz, NULL, 0);
	assert(*dp1 == d1);
	assert(dp0 == dp1);

	assert(d0 + usize <= d1);

RETURN:
	return (NULL);
}

int
main(void)
{
	int ret = 0;
	pthread_t thread;

	fprintf(stderr, "Test begin\n");

	thread_start(NULL);

	if (pthread_create(&thread, NULL, thread_start, NULL)
	    != 0) {
		fprintf(stderr, "%s(): Error in pthread_create()\n", __func__);
		ret = 1;
		goto RETURN;
	}
	pthread_join(thread, (void *)&ret);

	thread_start(NULL);

	if (pthread_create(&thread, NULL, thread_start, NULL)
	    != 0) {
		fprintf(stderr, "%s(): Error in pthread_create()\n", __func__);
		ret = 1;
		goto RETURN;
	}
	pthread_join(thread, (void *)&ret);

	thread_start(NULL);

RETURN:
	fprintf(stderr, "Test end\n");
	return (ret);
}
