#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

#define NTHREADS 10

void *
thread_start(void *arg)
{
	unsigned main_arena_ind = *(unsigned *)arg;
	void *p;
	unsigned arena_ind;
	size_t size;
	int err;

	p = JEMALLOC_P(malloc)(1);
	if (p == NULL) {
		fprintf(stderr, "%s(): Error in malloc()\n", __func__);
		return (void *)1;
	}

	size = sizeof(arena_ind);
	if ((err = JEMALLOC_P(mallctl)("thread.arena", &arena_ind, &size,
	    &main_arena_ind, sizeof(main_arena_ind)))) {
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		return (void *)1;
	}

	size = sizeof(arena_ind);
	if ((err = JEMALLOC_P(mallctl)("thread.arena", &arena_ind, &size, NULL,
	    0))) {
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		return (void *)1;
	}
	assert(arena_ind == main_arena_ind);

	return (NULL);
}

int
main(void)
{
	int ret = 0;
	void *p;
	unsigned arena_ind;
	size_t size;
	int err;
	pthread_t threads[NTHREADS];
	unsigned i;

	fprintf(stderr, "Test begin\n");

	p = JEMALLOC_P(malloc)(1);
	if (p == NULL) {
		fprintf(stderr, "%s(): Error in malloc()\n", __func__);
		ret = 1;
		goto RETURN;
	}

	size = sizeof(arena_ind);
	if ((err = JEMALLOC_P(mallctl)("thread.arena", &arena_ind, &size, NULL,
	    0))) {
		fprintf(stderr, "%s(): Error in mallctl(): %s\n", __func__,
		    strerror(err));
		ret = 1;
		goto RETURN;
	}

	for (i = 0; i < NTHREADS; i++) {
		if (pthread_create(&threads[i], NULL, thread_start,
		    (void *)&arena_ind) != 0) {
			fprintf(stderr, "%s(): Error in pthread_create()\n",
			    __func__);
			ret = 1;
			goto RETURN;
		}
	}

	for (i = 0; i < NTHREADS; i++)
		pthread_join(threads[i], (void *)&ret);

RETURN:
	fprintf(stderr, "Test end\n");
	return (ret);
}
