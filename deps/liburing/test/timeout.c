/* SPDX-License-Identifier: MIT */
/*
 * Description: run various timeout tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "helpers.h"
#include "liburing.h"
#include "../src/syscall.h"

#define TIMEOUT_MSEC	200
static int not_supported;
static int no_modify;
static int no_multishot;

static void msec_to_ts(struct __kernel_timespec *ts, unsigned int msec)
{
	ts->tv_sec = msec / 1000;
	ts->tv_nsec = (msec % 1000) * 1000000;
}

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

/*
 * Test that we return to userspace if a timeout triggers, even if we
 * don't satisfy the number of events asked for.
 */
static int test_single_timeout_many(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	unsigned long long exp;
	struct __kernel_timespec ts;
	struct timeval tv;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, TIMEOUT_MSEC);
	io_uring_prep_timeout(sqe, &ts, 0, 0);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	gettimeofday(&tv, NULL);
	ret = __sys_io_uring_enter(ring->ring_fd, 0, 4, IORING_ENTER_GETEVENTS,
					NULL);
	if (ret < 0) {
		fprintf(stderr, "%s: io_uring_enter %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret == -EINVAL) {
		fprintf(stdout, "Timeout not supported, ignored\n");
		not_supported = 1;
		return 0;
	} else if (ret != -ETIME) {
		fprintf(stderr, "Timeout: %s\n", strerror(-ret));
		goto err;
	}

	exp = mtime_since_now(&tv);
	if (exp >= TIMEOUT_MSEC / 2 && exp <= (TIMEOUT_MSEC * 3) / 2)
		return 0;
	fprintf(stderr, "%s: Timeout seems wonky (got %llu)\n", __FUNCTION__, exp);
err:
	return 1;
}

/*
 * Test numbered trigger of timeout
 */
static int test_single_timeout_nr(struct io_uring *ring, int nr)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int i, ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, TIMEOUT_MSEC);
	io_uring_prep_timeout(sqe, &ts, nr, 0);

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, (void *) 1);
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, (void *) 1);

	ret = io_uring_submit_and_wait(ring, 3);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	i = 0;
	while (i < 3) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		ret = cqe->res;

		/*
		 * NOP commands have user_data as 1. Check that we get the
		 * at least 'nr' NOPs first, then the successfully removed timeout.
		 */
		if (io_uring_cqe_get_data(cqe) == NULL) {
			if (i < nr) {
				fprintf(stderr, "%s: timeout received too early\n", __FUNCTION__);
				goto err;
			}
			if (ret) {
				fprintf(stderr, "%s: timeout triggered by passage of"
					" time, not by events completed\n", __FUNCTION__);
				goto err;
			}
		}

		io_uring_cqe_seen(ring, cqe);
		if (ret) {
			fprintf(stderr, "res: %d\n", ret);
			goto err;
		}
		i++;
	}

	return 0;
err:
	return 1;
}

static int test_single_timeout_wait(struct io_uring *ring,
				    struct io_uring_params *p)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int i, ret;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, (void *) 1);

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, (void *) 1);

	/* no implied submit for newer kernels */
	if (p->features & IORING_FEAT_EXT_ARG) {
		ret = io_uring_submit(ring);
		if (ret != 2) {
			fprintf(stderr, "%s: submit %d\n", __FUNCTION__, ret);
			return 1;
		}
	}

	msec_to_ts(&ts, 1000);

	i = 0;
	do {
		ret = io_uring_wait_cqes(ring, &cqe, 2, &ts, NULL);
		if (ret == -ETIME)
			break;
		if (ret < 0) {
			fprintf(stderr, "%s: wait timeout failed: %d\n", __FUNCTION__, ret);
			goto err;
		}

		ret = cqe->res;
		io_uring_cqe_seen(ring, cqe);
		if (ret < 0) {
			fprintf(stderr, "res: %d\n", ret);
			goto err;
		}
		i++;
	} while (1);

	if (i != 2) {
		fprintf(stderr, "got %d completions\n", i);
		goto err;
	}
	return 0;
err:
	return 1;
}

/*
 * Test single timeout waking us up
 */
static int test_single_timeout(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	unsigned long long exp;
	struct __kernel_timespec ts;
	struct timeval tv;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, TIMEOUT_MSEC);
	io_uring_prep_timeout(sqe, &ts, 0, 0);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	gettimeofday(&tv, NULL);
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret == -EINVAL) {
		fprintf(stdout, "%s: Timeout not supported, ignored\n", __FUNCTION__);
		not_supported = 1;
		return 0;
	} else if (ret != -ETIME) {
		fprintf(stderr, "%s: Timeout: %s\n", __FUNCTION__, strerror(-ret));
		goto err;
	}

	exp = mtime_since_now(&tv);
	if (exp >= TIMEOUT_MSEC / 2 && exp <= (TIMEOUT_MSEC * 3) / 2)
		return 0;
	fprintf(stderr, "%s: Timeout seems wonky (got %llu)\n", __FUNCTION__, exp);
err:
	return 1;
}

static int test_single_timeout_remove_notfound(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int ret, i;

	if (no_modify)
		return 0;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, TIMEOUT_MSEC);
	io_uring_prep_timeout(sqe, &ts, 2, 0);
	sqe->user_data = 1;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	io_uring_prep_timeout_remove(sqe, 2, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	/*
	 * We should get two completions. One is our modify request, which should
	 * complete with -ENOENT. The other is the timeout that will trigger after
	 * TIMEOUT_MSEC.
	 */
	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}
		if (cqe->user_data == 2) {
			if (cqe->res != -ENOENT) {
				fprintf(stderr, "%s: modify ret %d, wanted ENOENT\n", __FUNCTION__, cqe->res);
				break;
			}
		} else if (cqe->user_data == 1) {
			if (cqe->res != -ETIME) {
				fprintf(stderr, "%s: timeout ret %d, wanted -ETIME\n", __FUNCTION__, cqe->res);
				break;
			}
		}
		io_uring_cqe_seen(ring, cqe);
	}
	return 0;
err:
	return 1;
}

static int test_single_timeout_remove(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, TIMEOUT_MSEC);
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 1;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	io_uring_prep_timeout_remove(sqe, 1, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	/*
	 * We should have two completions ready. One is for the original timeout
	 * request, user_data == 1, that should have a ret of -ECANCELED. The other
	 * is for our modify request, user_data == 2, that should have a ret of 0.
	 */
	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}
		if (no_modify)
			goto seen;
		if (cqe->res == -EINVAL && cqe->user_data == 2) {
			fprintf(stdout, "Timeout modify not supported, ignoring\n");
			no_modify = 1;
			goto seen;
		}
		if (cqe->user_data == 1) {
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "%s: timeout ret %d, wanted canceled\n", __FUNCTION__, cqe->res);
				break;
			}
		} else if (cqe->user_data == 2) {
			if (cqe->res) {
				fprintf(stderr, "%s: modify ret %d, wanted 0\n", __FUNCTION__, cqe->res);
				break;
			}
		}
seen:
		io_uring_cqe_seen(ring, cqe);
	}
	return 0;
err:
	return 1;
}

/*
 * Test single absolute timeout waking us up
 */
static int test_single_timeout_abs(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	unsigned long long exp;
	struct __kernel_timespec ts;
	struct timespec abs_ts;
	struct timeval tv;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	clock_gettime(CLOCK_MONOTONIC, &abs_ts);
	ts.tv_sec = abs_ts.tv_sec + 1;
	ts.tv_nsec = abs_ts.tv_nsec;
	io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_ABS);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	gettimeofday(&tv, NULL);
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret == -EINVAL) {
		fprintf(stdout, "Absolute timeouts not supported, ignored\n");
		return 0;
	} else if (ret != -ETIME) {
		fprintf(stderr, "Timeout: %s\n", strerror(-ret));
		goto err;
	}

	exp = mtime_since_now(&tv);
	if (exp >= 1000 / 2 && exp <= (1000 * 3) / 2)
		return 0;
	fprintf(stderr, "%s: Timeout seems wonky (got %llu)\n", __FUNCTION__, exp);
err:
	return 1;
}

/*
 * Test that timeout is canceled on exit
 */
static int test_single_timeout_exit(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, 30000);
	io_uring_prep_timeout(sqe, &ts, 0, 0);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	io_uring_queue_exit(ring);
	return 0;
err:
	io_uring_queue_exit(ring);
	return 1;
}

/*
 * Test multi timeouts waking us up
 */
static int test_multi_timeout(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts[2];
	unsigned int timeout[2];
	unsigned long long exp;
	struct timeval tv;
	int ret, i;

	/* req_1: timeout req, count = 1, time = (TIMEOUT_MSEC * 2) */
	timeout[0] = TIMEOUT_MSEC * 2;
	msec_to_ts(&ts[0], timeout[0]);
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts[0], 1, 0);
	sqe->user_data = 1;

	/* req_2: timeout req, count = 1, time = TIMEOUT_MSEC */
	timeout[1] = TIMEOUT_MSEC;
	msec_to_ts(&ts[1], timeout[1]);
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts[1], 1, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	gettimeofday(&tv, NULL);
	for (i = 0; i < 2; i++) {
		unsigned int time = 0;
		__u64 user_data = 0;

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		/*
		 * Both of these two reqs should timeout, but req_2 should
		 * return before req_1.
		 */
		switch (i) {
		case 0:
			user_data = 2;
			time = timeout[1];
			break;
		case 1:
			user_data = 1;
			time = timeout[0];
			break;
		}

		if (cqe->user_data != user_data) {
			fprintf(stderr, "%s: unexpected timeout req %d sequence\n",
				__FUNCTION__, i+1);
			goto err;
		}
		if (cqe->res != -ETIME) {
			fprintf(stderr, "%s: Req %d timeout: %s\n",
				__FUNCTION__, i+1, strerror(cqe->res));
			goto err;
		}
		exp = mtime_since_now(&tv);
		if (exp < time / 2 || exp > (time * 3) / 2) {
			fprintf(stderr, "%s: Req %d timeout seems wonky (got %llu)\n",
				__FUNCTION__, i+1, exp);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Test multi timeout req with different count
 */
static int test_multi_timeout_nr(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret, i;

	msec_to_ts(&ts, TIMEOUT_MSEC);

	/* req_1: timeout req, count = 2 */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 2, 0);
	sqe->user_data = 1;

	/* req_2: timeout req, count = 1 */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 1, 0);
	sqe->user_data = 2;

	/* req_3: nop req */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, (void *) 1);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	/*
	 * req_2 (count=1) should return without error and req_1 (count=2)
	 * should timeout.
	 */
	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		switch (i) {
		case 0:
			/* Should be nop req */
			if (io_uring_cqe_get_data(cqe) != (void *) 1) {
				fprintf(stderr, "%s: nop not seen as 1 or 2\n", __FUNCTION__);
				goto err;
			}
			break;
		case 1:
			/* Should be timeout req_2 */
			if (cqe->user_data != 2) {
				fprintf(stderr, "%s: unexpected timeout req %d sequence\n",
					__FUNCTION__, i+1);
				goto err;
			}
			if (cqe->res < 0) {
				fprintf(stderr, "%s: Req %d res %d\n",
					__FUNCTION__, i+1, cqe->res);
				goto err;
			}
			break;
		case 2:
			/* Should be timeout req_1 */
			if (cqe->user_data != 1) {
				fprintf(stderr, "%s: unexpected timeout req %d sequence\n",
					__FUNCTION__, i+1);
				goto err;
			}
			if (cqe->res != -ETIME) {
				fprintf(stderr, "%s: Req %d timeout: %s\n",
					__FUNCTION__, i+1, strerror(cqe->res));
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Test timeout <link> timeout <drain> timeout
 */
static int test_timeout_flags1(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret, i;

	msec_to_ts(&ts, TIMEOUT_MSEC);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 1;
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 2;
	sqe->flags |= IOSQE_IO_DRAIN;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		if (cqe->res == -EINVAL) {
			if (!i)
				fprintf(stdout, "%s: timeout flags not supported\n",
						__FUNCTION__);
			io_uring_cqe_seen(ring, cqe);
			continue;
		}

		switch (cqe->user_data) {
		case 1:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res, -ETIME);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res,
						-ECANCELED);
				goto err;
			}
			break;
		case 3:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res, -ETIME);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Test timeout <link> timeout <link> timeout
 */
static int test_timeout_flags2(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret, i;

	msec_to_ts(&ts, TIMEOUT_MSEC);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 1;
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 2;
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		if (cqe->res == -EINVAL) {
			if (!i)
				fprintf(stdout, "%s: timeout flags not supported\n",
						__FUNCTION__);
			io_uring_cqe_seen(ring, cqe);
			continue;
		}

		switch (cqe->user_data) {
		case 1:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res, -ETIME);
				goto err;
			}
			break;
		case 2:
		case 3:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res,
						-ECANCELED);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Test timeout <drain> timeout <link> timeout
 */
static int test_timeout_flags3(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret, i;

	msec_to_ts(&ts, TIMEOUT_MSEC);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 1;
	sqe->flags |= IOSQE_IO_DRAIN;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 2;
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		if (cqe->res == -EINVAL) {
			if (!i)
				fprintf(stdout, "%s: timeout flags not supported\n",
						__FUNCTION__);
			io_uring_cqe_seen(ring, cqe);
			continue;
		}

		switch (cqe->user_data) {
		case 1:
		case 2:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res, -ETIME);
				goto err;
			}
			break;
		case 3:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res,
						-ECANCELED);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

static int test_update_timeout(struct io_uring *ring, unsigned long ms,
				bool abs, bool async, bool linked)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts, ts_upd;
	unsigned long long exp_ms, base_ms = 10000;
	struct timeval tv;
	int ret, i, nr = 2;
	__u32 mode = abs ? IORING_TIMEOUT_ABS : 0;

	msec_to_ts(&ts_upd, ms);
	gettimeofday(&tv, NULL);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	msec_to_ts(&ts, base_ms);
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->user_data = 1;

	if (linked) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
			goto err;
		}
		io_uring_prep_nop(sqe);
		sqe->user_data = 3;
		sqe->flags = IOSQE_IO_LINK;
		if (async)
			sqe->flags |= IOSQE_ASYNC;
		nr++;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout_update(sqe, &ts_upd, 1, mode);
	sqe->user_data = 2;
	if (async)
		sqe->flags |= IOSQE_ASYNC;

	ret = io_uring_submit(ring);
	if (ret != nr) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	for (i = 0; i < nr; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		switch (cqe->user_data) {
		case 1:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res, -ETIME);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != 0) {
				fprintf(stderr, "%s: got %d, wanted %d\n",
						__FUNCTION__, cqe->res,
						0);
				goto err;
			}
			break;
		case 3:
			if (cqe->res != 0) {
				fprintf(stderr, "nop failed\n");
				goto err;
			}
			break;
		default:
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	exp_ms = mtime_since_now(&tv);
	if (exp_ms >= base_ms / 2) {
		fprintf(stderr, "too long, timeout wasn't updated\n");
		goto err;
	}
	if (ms >= 1000 && !abs && exp_ms < ms / 2) {
		fprintf(stderr, "fired too early, potentially updated to 0 ms"
					"instead of %lu\n", ms);
		goto err;
	}
	return 0;
err:
	return 1;
}

static int test_update_nonexistent_timeout(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	msec_to_ts(&ts, 0);
	io_uring_prep_timeout_update(sqe, &ts, 42, 0);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = cqe->res;
	if (ret == -ENOENT)
		ret = 0;
	io_uring_cqe_seen(ring, cqe);
	return ret;
err:
	return 1;
}

static int test_update_invalid_flags(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	io_uring_prep_timeout_remove(sqe, 0, IORING_TIMEOUT_ABS);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}
	if (cqe->res != -EINVAL) {
		fprintf(stderr, "%s: got %d, wanted %d\n",
				__FUNCTION__, cqe->res, -EINVAL);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);


	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}
	msec_to_ts(&ts, 0);
	io_uring_prep_timeout_update(sqe, &ts, 0, -1);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}
	if (cqe->res != -EINVAL) {
		fprintf(stderr, "%s: got %d, wanted %d\n",
				__FUNCTION__, cqe->res, -EINVAL);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);

	return 0;
err:
	return 1;
}

static int fill_exec_target(char *dst, char *path)
{
	struct stat sb;

	/*
	 * Should either be ./exec-target.t or test/exec-target.t
	 */
	sprintf(dst, "%s", path);
	return stat(dst, &sb);
}

static int test_timeout_link_cancel(void)
{
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	char prog_path[PATH_MAX];
	pid_t p;
	int ret, i, wstatus;

	if (fill_exec_target(prog_path, "./exec-target.t") &&
	    fill_exec_target(prog_path, "test/exec-target.t")) {
		fprintf(stdout, "Can't find exec-target, skipping\n");
		return 0;
	}

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	p = fork();
	if (p == -1) {
		fprintf(stderr, "fork() failed\n");
		return 1;
	}

	if (p == 0) {
		struct io_uring_sqe *sqe;
		struct __kernel_timespec ts;

		msec_to_ts(&ts, 10000);
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_timeout(sqe, &ts, 0, 0);
		sqe->flags |= IOSQE_IO_LINK;
		sqe->user_data = 0;

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_nop(sqe);
		sqe->user_data = 1;

		ret = io_uring_submit(&ring);
		if (ret != 2) {
			fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
			exit(1);
		}

		/* trigger full cancellation */
		ret = execl(prog_path, prog_path, NULL);
		if (ret) {
			fprintf(stderr, "exec failed %i\n", errno);
			exit(1);
		}
		exit(0);
	}

	if (waitpid(p, &wstatus, 0) == (pid_t)-1) {
		perror("waitpid()");
		return 1;
	}
	if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus)) {
		fprintf(stderr, "child failed %i\n", WEXITSTATUS(wstatus));
		return 1;
	}

	for (i = 0; i < 2; ++i) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return 1;
		}
		if (cqe->res != -ECANCELED) {
			fprintf(stderr, "invalid result, user_data: %i res: %i\n",
					(int)cqe->user_data, cqe->res);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	return 0;
}


static int test_not_failing_links(void)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	msec_to_ts(&ts, 1);
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_ETIME_SUCCESS);
	sqe->user_data = 1;
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);
	sqe->user_data = 2;

	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		return 1;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		return 1;
	} else if (cqe->user_data == 1 && cqe->res == -EINVAL) {
		goto done;
	} else if (cqe->res != -ETIME || cqe->user_data != 1) {
		fprintf(stderr, "timeout failed %i %i\n", cqe->res,
				(int)cqe->user_data);
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		return 1;
	} else if (cqe->res || cqe->user_data != 2) {
		fprintf(stderr, "nop failed %i %i\n", cqe->res,
				(int)cqe->user_data);
		return 1;
	}
done:
	io_uring_cqe_seen(&ring, cqe);
	io_uring_queue_exit(&ring);
	return 0;
}


static int test_timeout_multishot(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, TIMEOUT_MSEC);
	io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_MULTISHOT);
	io_uring_sqe_set_data(sqe, (void *) 1);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	for (int i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		ret = cqe->res;
		if (ret == -EINVAL) {
			no_multishot = 1;
			return T_EXIT_SKIP;
		}

		if (!(cqe->flags & IORING_CQE_F_MORE)) {
			fprintf(stderr, "%s: flag not set in cqe\n", __FUNCTION__);
			goto err;
		}

		if (ret != -ETIME) {
			fprintf(stderr, "%s: Timeout: %s\n", __FUNCTION__, strerror(-ret));
			goto err;
		}

		io_uring_cqe_seen(ring, cqe);
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	io_uring_prep_timeout_remove(sqe, 1, 0);
	io_uring_sqe_set_data(sqe, (void *) 2);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = cqe->res;
	if (ret < 0) {
		fprintf(stderr, "%s: remove failed: %s\n", __FUNCTION__, strerror(-ret));
		goto err;
	}

	io_uring_cqe_seen(ring, cqe);

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = cqe->res;
	if (ret != -ECANCELED) {
		fprintf(stderr, "%s: timeout canceled: %s %llu\n", __FUNCTION__, strerror(-ret), cqe->user_data);
		goto err;
	}

	io_uring_cqe_seen(ring, cqe);
	return 0;
err:
	return 1;
}


static int test_timeout_multishot_nr(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int ret;

	if (no_multishot)
		return T_EXIT_SKIP;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, TIMEOUT_MSEC);
	io_uring_prep_timeout(sqe, &ts, 3, IORING_TIMEOUT_MULTISHOT);
	io_uring_sqe_set_data(sqe, (void *) 1);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	for (int i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
			goto err;
		}

		if (i < 2 && !(cqe->flags & IORING_CQE_F_MORE)) {
			fprintf(stderr, "%s: flag not set in cqe\n", __FUNCTION__);
			goto err;
		}
		if (i == 3 && (cqe->flags & IORING_CQE_F_MORE)) {
			fprintf(stderr, "%s: flag set in cqe\n", __FUNCTION__);
			goto err;
		}

		ret = cqe->res;
		if (ret != -ETIME) {
			fprintf(stderr, "%s: Timeout: %s\n", __FUNCTION__, strerror(-ret));
			goto err;
		}

		io_uring_cqe_seen(ring, cqe);
	}

	msec_to_ts(&ts, 2 * TIMEOUT_MSEC);
	ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
	if (ret != -ETIME) {
		fprintf(stderr, "%s: wait completion timeout %s\n", __FUNCTION__, strerror(-ret));
		goto err;
	}

	return 0;
err:
	return 1;
}


static int test_timeout_multishot_overflow(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts;
	int ret;

	if (no_multishot)
		return T_EXIT_SKIP;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	msec_to_ts(&ts, 10);
	io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_MULTISHOT);
	io_uring_sqe_set_data(sqe, (void *) 1);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = cqe->res;
	if (ret != -ETIME) {
		fprintf(stderr, "%s: Timeout: %s\n", __FUNCTION__, strerror(-ret));
		goto err;
	}

	io_uring_cqe_seen(ring, cqe);
	sleep(1);

	if (!((*ring->sq.kflags) & IORING_SQ_CQ_OVERFLOW)) {
		goto err;
	}

	/* multishot timer should be gone */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	io_uring_prep_timeout_remove(sqe, 1, 0);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: wait completion %d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret != -ETIME) {
		fprintf(stderr, "%s: remove failed: %d %s\n", __FUNCTION__, ret, strerror(-ret));
		goto err;
	}

	return 0;
err:
	return 1;
}


int main(int argc, char *argv[])
{
	struct io_uring ring, sqpoll_ring;
	bool has_timeout_update, sqpoll;
	struct io_uring_params p = { };
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = io_uring_queue_init(8, &sqpoll_ring, IORING_SETUP_SQPOLL);
	sqpoll = !ret;

	ret = test_single_timeout(&ring);
	if (ret) {
		fprintf(stderr, "test_single_timeout failed\n");
		return ret;
	}
	if (not_supported)
		return 0;

	ret = test_multi_timeout(&ring);
	if (ret) {
		fprintf(stderr, "test_multi_timeout failed\n");
		return ret;
	}

	ret = test_single_timeout_abs(&ring);
	if (ret) {
		fprintf(stderr, "test_single_timeout_abs failed\n");
		return ret;
	}

	ret = test_single_timeout_remove(&ring);
	if (ret) {
		fprintf(stderr, "test_single_timeout_remove failed\n");
		return ret;
	}

	ret = test_single_timeout_remove_notfound(&ring);
	if (ret) {
		fprintf(stderr, "test_single_timeout_remove_notfound failed\n");
		return ret;
	}

	ret = test_single_timeout_many(&ring);
	if (ret) {
		fprintf(stderr, "test_single_timeout_many failed\n");
		return ret;
	}

	ret = test_single_timeout_nr(&ring, 1);
	if (ret) {
		fprintf(stderr, "test_single_timeout_nr(1) failed\n");
		return ret;
	}
	ret = test_single_timeout_nr(&ring, 2);
	if (ret) {
		fprintf(stderr, "test_single_timeout_nr(2) failed\n");
		return ret;
	}

	ret = test_multi_timeout_nr(&ring);
	if (ret) {
		fprintf(stderr, "test_multi_timeout_nr failed\n");
		return ret;
	}

	ret = test_timeout_flags1(&ring);
	if (ret) {
		fprintf(stderr, "test_timeout_flags1 failed\n");
		return ret;
	}

	ret = test_timeout_flags2(&ring);
	if (ret) {
		fprintf(stderr, "test_timeout_flags2 failed\n");
		return ret;
	}

	ret = test_timeout_flags3(&ring);
	if (ret) {
		fprintf(stderr, "test_timeout_flags3 failed\n");
		return ret;
	}

	ret = test_timeout_multishot(&ring);
	if (ret && ret != T_EXIT_SKIP) {
		fprintf(stderr, "test_timeout_multishot failed\n");
		return ret;
	}

	ret = test_timeout_multishot_nr(&ring);
	if (ret && ret != T_EXIT_SKIP) {
		fprintf(stderr, "test_timeout_multishot_nr failed\n");
		return ret;
	}

	/* io_uring_wait_cqe_timeout() may have left a timeout, reinit ring */
	io_uring_queue_exit(&ring);
	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = test_timeout_multishot_overflow(&ring);
	if (ret && ret != T_EXIT_SKIP) {
		fprintf(stderr, "test_timeout_multishot_overflow failed\n");
		return ret;
	}

	/* io_uring_wait_cqe_timeout() may have left a timeout, reinit ring */
	io_uring_queue_exit(&ring);
	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = test_single_timeout_wait(&ring, &p);
	if (ret) {
		fprintf(stderr, "test_single_timeout_wait failed\n");
		return ret;
	}

	/* io_uring_wait_cqes() may have left a timeout, reinit ring */
	io_uring_queue_exit(&ring);
	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = test_update_nonexistent_timeout(&ring);
	has_timeout_update = (ret != -EINVAL);
	if (has_timeout_update) {
		if (ret) {
			fprintf(stderr, "test_update_nonexistent_timeout failed\n");
			return ret;
		}

		ret = test_update_invalid_flags(&ring);
		if (ret) {
			fprintf(stderr, "test_update_invalid_flags failed\n");
			return ret;
		}

		ret = test_update_timeout(&ring, 0, false, false, false);
		if (ret) {
			fprintf(stderr, "test_update_timeout failed\n");
			return ret;
		}

		ret = test_update_timeout(&ring, 1, false, false, false);
		if (ret) {
			fprintf(stderr, "test_update_timeout 1ms failed\n");
			return ret;
		}

		ret = test_update_timeout(&ring, 1000, false, false, false);
		if (ret) {
			fprintf(stderr, "test_update_timeout 1s failed\n");
			return ret;
		}

		ret = test_update_timeout(&ring, 0, true, true, false);
		if (ret) {
			fprintf(stderr, "test_update_timeout abs failed\n");
			return ret;
		}


		ret = test_update_timeout(&ring, 0, false, true, false);
		if (ret) {
			fprintf(stderr, "test_update_timeout async failed\n");
			return ret;
		}

		ret = test_update_timeout(&ring, 0, false, false, true);
		if (ret) {
			fprintf(stderr, "test_update_timeout linked failed\n");
			return ret;
		}

		if (sqpoll) {
			ret = test_update_timeout(&sqpoll_ring, 0, false, false,
						  false);
			if (ret) {
				fprintf(stderr, "test_update_timeout sqpoll"
						"failed\n");
				return ret;
			}
		}
	}

	/*
	 * this test must go last, it kills the ring
	 */
	ret = test_single_timeout_exit(&ring);
	if (ret) {
		fprintf(stderr, "test_single_timeout_exit failed\n");
		return ret;
	}

	ret = test_timeout_link_cancel();
	if (ret) {
		fprintf(stderr, "test_timeout_link_cancel failed\n");
		return ret;
	}

	ret = test_not_failing_links();
	if (ret) {
		fprintf(stderr, "test_not_failing_links failed\n");
		return ret;
	}

	if (sqpoll)
		io_uring_queue_exit(&sqpoll_ring);
	return 0;
}
