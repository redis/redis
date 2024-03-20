/* SPDX-License-Identifier: MIT */
/*
 * Test that we exit properly with SQPOLL and having a request that
 * adds a circular reference to the ring itself.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <poll.h>
#include "liburing.h"

static unsigned long long mtime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000;
	usec /= 1000;
	return sec + usec;
}

static unsigned long long mtime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return mtime_since(tv, &end);
}

int main(int argc, char *argv[])
{
	struct io_uring_params p = {};
	struct timeval tv;
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	int ret;

	if (argc > 1)
		return 0;

	p.flags = IORING_SETUP_SQPOLL;
	p.sq_thread_idle = 100;

	ret = io_uring_queue_init_params(1, &ring, &p);
	if (ret) {
		if (geteuid()) {
			printf("%s: skipped, not root\n", argv[0]);
			return 0;
		}
		fprintf(stderr, "queue_init=%d\n", ret);
		return 1;
	}

	if (!(p.features & IORING_FEAT_SQPOLL_NONFIXED)) {
		fprintf(stdout, "Skipping\n");
		return 0;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, ring.ring_fd, POLLIN);
	io_uring_submit(&ring);

	gettimeofday(&tv, NULL);
	do {
		usleep(1000);
	} while (mtime_since_now(&tv) < 1000);

	return 0;
}
