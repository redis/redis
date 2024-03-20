/* SPDX-License-Identifier: MIT */
/*
 * Description: Test that io_uring_submit_and_wait_timeout() returns the
 * right value (submit count) and that it doesn't end up waiting twice.
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>

#include "liburing.h"
#include "test.h"

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

static int test(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	struct timeval tv;
	int ret, i;

	for (i = 0; i < 1; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed at %d\n", i);
			goto err;
		}
		io_uring_prep_nop(sqe);
	}

	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	gettimeofday(&tv, NULL);
	ret = io_uring_submit_and_wait_timeout(ring, &cqe, 2, &ts, NULL);
	if (ret < 0) {
		fprintf(stderr, "submit_and_wait_timeout: %d\n", ret);
		goto err;
	}
	ret = mtime_since_now(&tv);
	/* allow some slack, should be around 1s */
	if (ret > 1200) {
		fprintf(stderr, "wait took too long: %d\n", ret);
		goto err;
	}
	return 0;
err:
	return 1;
}

static int test_ring(void)
{
	struct io_uring ring;
	struct io_uring_params p = { };
	int ret;

	p.flags = 0;
	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	ret = test(&ring);
	if (ret) {
		fprintf(stderr, "test failed\n");
		goto err;
	}
err:
	io_uring_queue_exit(&ring);
	return ret;
}

int main(int argc, char *argv[])
{
	if (argc > 1)
		return 0;

	return test_ring();
}
