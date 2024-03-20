/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "liburing.h"
#include "helpers.h"

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
	struct __kernel_timespec ts1, ts2;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	unsigned long msec;
	struct timeval tv;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(32, &ring, 0);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init=%d\n", ret);
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "io_uring_submit1=%d\n", ret);
		return T_EXIT_FAIL;
	}


	ts1.tv_sec = 5,
	ts1.tv_nsec = 0;
	ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts1);
	if (ret) {
		fprintf(stderr, "io_uring_wait_cqe_timeout=%d\n", ret);
		return T_EXIT_FAIL;
	}
	io_uring_cqe_seen(&ring, cqe);
	gettimeofday(&tv, NULL);

	ts2.tv_sec = 1;
	ts2.tv_nsec = 0;
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_timeout(sqe, &ts2, 0, 0);
	sqe->user_data = 89;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "io_uring_submit2=%d\n", ret);
		return T_EXIT_FAIL;
	}

	io_uring_wait_cqe(&ring, &cqe);
	io_uring_cqe_seen(&ring, cqe);
	msec = mtime_since_now(&tv);
	if (msec >= 900 && msec <= 1100) {
		io_uring_queue_exit(&ring);
		return T_EXIT_PASS;
	}

	fprintf(stderr, "%s: Timeout seems wonky (got %lu)\n", __FUNCTION__,
								msec);
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}
