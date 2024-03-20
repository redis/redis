/* SPDX-License-Identifier: MIT */
/*
 * Description: run various linked timeout cases
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>

#include "liburing.h"
#include "helpers.h"

static int test_fail_lone_link_timeouts(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_link_timeout(sqe, &ts, 0);
	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	sqe->user_data = 1;
	sqe->flags |= IOSQE_IO_LINK;

	ret = io_uring_submit(ring);
	if (ret != 1) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		printf("wait completion %d\n", ret);
		goto err;
	}

	if (cqe->user_data != 1) {
		fprintf(stderr, "invalid user data %d\n", cqe->res);
		goto err;
	}
	if (cqe->res != -EINVAL) {
		fprintf(stderr, "got %d, wanted -EINVAL\n", cqe->res);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);

	return 0;
err:
	return 1;
}

static int test_fail_two_link_timeouts(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i, nr_wait;

	ts.tv_sec = 1;
	ts.tv_nsec = 0;

	/*
	 * sqe_1: write destined to fail
	 * use buf=NULL, to do that during the issuing stage
	 */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_writev(sqe, 0, NULL, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;


	/* sqe_2: valid linked timeout */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 2;
	sqe->flags |= IOSQE_IO_LINK;


	/* sqe_3: invalid linked timeout */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 3;

	/* sqe_4: invalid linked timeout */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 4;

	ret = io_uring_submit(ring);
	if (ret < 3) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}
	nr_wait = ret;

	for (i = 0; i < nr_wait; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}

		switch (cqe->user_data) {
		case 1:
			if (cqe->res != -EFAULT && cqe->res != -ECANCELED) {
				fprintf(stderr, "write got %d, wanted -EFAULT "
						"or -ECANCELED\n", cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Link timeout got %d, wanted -ECACNCELED\n", cqe->res);
				goto err;
			}
			break;
		case 3:
			/* fall through */
		case 4:
			if (cqe->res != -ECANCELED && cqe->res != -EINVAL) {
				fprintf(stderr, "Invalid link timeout got %d"
					", wanted -ECACNCELED || -EINVAL\n", cqe->res);
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
 * Test linked timeout with timeout (timeoutception)
 */
static int test_single_link_timeout_ception(struct io_uring *ring)
{
	struct __kernel_timespec ts1, ts2;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	ts1.tv_sec = 1;
	ts1.tv_nsec = 0;
	io_uring_prep_timeout(sqe, &ts1, -1U, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	ts2.tv_sec = 2;
	ts2.tv_nsec = 0;
	io_uring_prep_link_timeout(sqe, &ts2, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret != 2) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
			/* newer kernels allow timeout links */
			if (cqe->res != -EINVAL && cqe->res != -ETIME) {
				fprintf(stderr, "Timeout got %d, wanted "
					"-EINVAL or -ETIME\n", cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Link timeout got %d, wanted -ECANCELED\n", cqe->res);
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
 * Test linked timeout with NOP
 */
static int test_single_link_timeout_nop(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret != 2) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
			if (cqe->res) {
				fprintf(stderr, "NOP got %d, wanted 0\n", cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Link timeout got %d, wanted -ECACNCELED\n", cqe->res);
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
 * Test read that will not complete, with a linked timeout behind it that
 * has errors in the SQE
 */
static int test_single_link_timeout_error(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i;
	struct iovec iov;
	char buffer[128];

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	iov.iov_base = buffer;
	iov.iov_len = sizeof(buffer);
	io_uring_prep_readv(sqe, fds[0], &iov, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	/* set invalid field, it'll get failed */
	sqe->ioprio = 89;
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret != 2) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Read got %d, wanted -ECANCELED\n",
						cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -EINVAL) {
				fprintf(stderr, "Link timeout got %d, wanted -EINVAL\n", cqe->res);
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
 * Test read that will complete, with a linked timeout behind it
 */
static int test_single_link_no_timeout(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i;
	struct iovec iov;
	char buffer[128];

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	iov.iov_base = buffer;
	iov.iov_len = sizeof(buffer);
	io_uring_prep_readv(sqe, fds[0], &iov, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	iov.iov_base = buffer;
	iov.iov_len = sizeof(buffer);
	io_uring_prep_writev(sqe, fds[1], &iov, 1, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret != 3) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
		case 3:
			if (cqe->res != sizeof(buffer)) {
				fprintf(stderr, "R/W got %d, wanted %d\n", cqe->res,
						(int) sizeof(buffer));
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Link timeout %d, wanted -ECANCELED\n",
						cqe->res);
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
 * Test read that will not complete, with a linked timeout behind it
 */
static int test_single_link_timeout(struct io_uring *ring, unsigned nsec)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i;
	struct iovec iov;
	char buffer[128];

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	iov.iov_base = buffer;
	iov.iov_len = sizeof(buffer);
	io_uring_prep_readv(sqe, fds[0], &iov, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	ts.tv_sec = 0;
	ts.tv_nsec = nsec;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret != 2) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
			if (cqe->res != -EINTR && cqe->res != -ECANCELED) {
				fprintf(stderr, "Read got %d\n", cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -EALREADY && cqe->res != -ETIME &&
			    cqe->res != 0) {
				fprintf(stderr, "Link timeout got %d\n", cqe->res);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(fds[0]);
	close(fds[1]);
	return 0;
err:
	return 1;
}

static int test_timeout_link_chain1(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i;
	struct iovec iov;
	char buffer[128];

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	iov.iov_base = buffer;
	iov.iov_len = sizeof(buffer);
	io_uring_prep_readv(sqe, fds[0], &iov, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret != 3) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
			if (cqe->res != -EINTR && cqe->res != -ECANCELED) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 2:
			/* FASTPOLL kernels can cancel successfully */
			if (cqe->res != -EALREADY && cqe->res != -ETIME) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 3:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		}

		io_uring_cqe_seen(ring, cqe);
	}

	close(fds[0]);
	close(fds[1]);
	return 0;
err:
	return 1;
}

static int test_timeout_link_chain2(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i;

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_poll_add(sqe, fds[0], POLLIN);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 3;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->user_data = 4;

	ret = io_uring_submit(ring);
	if (ret != 4) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 4; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		/* poll cancel really should return -ECANCEL... */
		case 1:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 3:
		case 4:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(fds[0]);
	close(fds[1]);
	return 0;
err:
	return 1;
}

static int test_timeout_link_chain3(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i;

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_poll_add(sqe, fds[0], POLLIN);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 3;

	/* POLL -> TIMEOUT -> NOP */

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_poll_add(sqe, fds[0], POLLIN);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 4;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 5;

	/* poll on pipe + timeout */

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->user_data = 6;

	/* nop */

	ret = io_uring_submit(ring);
	if (ret != 6) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 6; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 2:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 1:
		case 3:
		case 4:
		case 5:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 6:
			if (cqe->res) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(fds[0]);
	close(fds[1]);
	return 0;
err:
	return 1;
}

static int test_timeout_link_chain4(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i;

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_poll_add(sqe, fds[0], POLLIN);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret != 3) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		/* poll cancel really should return -ECANCEL... */
		case 1:
			if (cqe->res) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		case 3:
			if (cqe->res != -ETIME) {
				fprintf(stderr, "Req %" PRIu64 " got %d\n", (uint64_t) cqe->user_data,
						cqe->res);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(fds[0]);
	close(fds[1]);
	return 0;
err:
	return 1;
}

static int test_timeout_link_chain5(struct io_uring *ring)
{
	struct __kernel_timespec ts1, ts2;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	ts1.tv_sec = 1;
	ts1.tv_nsec = 0;
	io_uring_prep_link_timeout(sqe, &ts1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}
	ts2.tv_sec = 2;
	ts2.tv_nsec = 0;
	io_uring_prep_link_timeout(sqe, &ts2, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret != 3) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
		case 2:
			if (cqe->res && cqe->res != -ECANCELED) {
				fprintf(stderr, "Request got %d, wanted -EINVAL "
						"or -ECANCELED\n",
						cqe->res);
				goto err;
			}
			break;
		case 3:
			if (cqe->res != -ECANCELED && cqe->res != -EINVAL) {
				fprintf(stderr, "Link timeout got %d, wanted -ECANCELED\n", cqe->res);
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

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_timeout_link_chain1(&ring);
	if (ret) {
		printf("test_single_link_chain1 failed\n");
		return ret;
	}

	ret = test_timeout_link_chain2(&ring);
	if (ret) {
		printf("test_single_link_chain2 failed\n");
		return ret;
	}

	ret = test_timeout_link_chain3(&ring);
	if (ret) {
		printf("test_single_link_chain3 failed\n");
		return ret;
	}

	ret = test_timeout_link_chain4(&ring);
	if (ret) {
		printf("test_single_link_chain4 failed\n");
		return ret;
	}

	ret = test_timeout_link_chain5(&ring);
	if (ret) {
		printf("test_single_link_chain5 failed\n");
		return ret;
	}

	ret = test_single_link_timeout(&ring, 10);
	if (ret) {
		printf("test_single_link_timeout 10 failed\n");
		return ret;
	}

	ret = test_single_link_timeout(&ring, 100000ULL);
	if (ret) {
		printf("test_single_link_timeout 100000 failed\n");
		return ret;
	}

	ret = test_single_link_timeout(&ring, 500000000ULL);
	if (ret) {
		printf("test_single_link_timeout 500000000 failed\n");
		return ret;
	}

	ret = test_single_link_no_timeout(&ring);
	if (ret) {
		printf("test_single_link_no_timeout failed\n");
		return ret;
	}

	ret = test_single_link_timeout_error(&ring);
	if (ret) {
		printf("test_single_link_timeout_error failed\n");
		return ret;
	}

	ret = test_single_link_timeout_nop(&ring);
	if (ret) {
		printf("test_single_link_timeout_nop failed\n");
		return ret;
	}

	ret = test_single_link_timeout_ception(&ring);
	if (ret) {
		printf("test_single_link_timeout_ception failed\n");
		return ret;
	}

	ret = test_fail_lone_link_timeouts(&ring);
	if (ret) {
		printf("test_fail_lone_link_timeouts failed\n");
		return ret;
	}

	ret = test_fail_two_link_timeouts(&ring);
	if (ret) {
		printf("test_fail_two_link_timeouts failed\n");
		return ret;
	}

	return T_EXIT_PASS;
}
