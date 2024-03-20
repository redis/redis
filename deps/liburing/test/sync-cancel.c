/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring_register_sync_cancel()
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

static int no_sync_cancel;

static int test_sync_cancel_timeout(struct io_uring *ring, int async)
{
	struct io_uring_sync_cancel_reg reg = { };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, fds[2], to_prep;
	char buf[32];

	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}

	to_prep = 1;
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, fds[0], buf, sizeof(buf), 0);
	sqe->user_data = 0x89;
	if (async)
		sqe->flags |= IOSQE_ASYNC;

	ret = io_uring_submit(ring);
	if (ret != to_prep) {
		fprintf(stderr, "submit=%d\n", ret);
		return 1;
	}

	usleep(10000);

	reg.addr = 0x89;
	reg.timeout.tv_nsec = 1;
	ret = io_uring_register_sync_cancel(ring, &reg);
	if (async) {
		/* we expect -ETIME here, but can race and get 0 */
		if (ret != -ETIME && ret != 0) {
			fprintf(stderr, "sync_cancel=%d\n", ret);
			return 1;
		}
	} else {
		if (ret < 0) {
			fprintf(stderr, "sync_cancel=%d\n", ret);
			return 1;
		}
	}

	/*
	 * we could _almost_ use peek_cqe() here, but there is still
	 * a small gap where io-wq is done with the request and on
	 * its way to posting a completion, but hasn't done it just
	 * yet. the request is canceled and won't be doing any IO
	 * to buffers etc, but the cqe may not have quite arrived yet.
	 */
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "peek=%d\n", ret);
		return 1;
	}
	if (cqe->res >= 0) {
		fprintf(stderr, "cqe->res=%d\n", cqe->res);
		return 1;
	}
	io_uring_cqe_seen(ring, cqe);
	return 0;
}

static int test_sync_cancel(struct io_uring *ring, int async, int nr_all,
			    int use_fd)
{
	struct io_uring_sync_cancel_reg reg = { };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, fds[2], to_prep, i;
	char buf[32];

	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}

	to_prep = 1;
	if (nr_all)
		to_prep = 4;
	for (i = 0; i < to_prep; i++) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_read(sqe, fds[0], buf, sizeof(buf), 0);
		sqe->user_data = 0x89;
		if (async)
			sqe->flags |= IOSQE_ASYNC;
	}

	ret = io_uring_submit(ring);
	if (ret != to_prep) {
		fprintf(stderr, "submit=%d\n", ret);
		return 1;
	}

	usleep(10000);

	if (!use_fd)
		reg.addr = 0x89;
	else
		reg.fd = fds[0];
	reg.timeout.tv_sec = 200;
	if (nr_all)
		reg.flags |= IORING_ASYNC_CANCEL_ALL;
	if (use_fd)
		reg.flags |= IORING_ASYNC_CANCEL_FD;
	ret = io_uring_register_sync_cancel(ring, &reg);
	if (ret < 0) {
		if (ret == -EINVAL && !no_sync_cancel) {
			no_sync_cancel = 1;
			return 0;
		}
		fprintf(stderr, "sync_cancel=%d\n", ret);
		return 1;
	}

	for (i = 0; i < to_prep; i++) {
		/*
		 * we could _almost_ use peek_cqe() here, but there is still
		 * a small gap where io-wq is done with the request and on
		 * its way to posting a completion, but hasn't done it just
		 * yet. the request is canceled and won't be doing any IO
		 * to buffers etc, but the cqe may not have quite arrived yet.
		 */
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "peek=%d\n", ret);
			return 1;
		}
		if (cqe->res >= 0) {
			fprintf(stderr, "cqe->res=%d\n", cqe->res);
			return 1;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = t_create_ring(7, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_SKIP;
	else if (ret != T_SETUP_OK)
		return ret;

	ret = test_sync_cancel(&ring, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 0 0 0 failed\n");
		return T_EXIT_FAIL;
	}
	if (no_sync_cancel)
		return T_EXIT_SKIP;

	ret = test_sync_cancel(&ring, 1, 0, 0);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 1 0 0 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sync_cancel(&ring, 0, 1, 0);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 0 1 0 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sync_cancel(&ring, 1, 1, 0);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 1 1 0 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sync_cancel(&ring, 0, 0, 1);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 0 0 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sync_cancel(&ring, 1, 0, 1);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 1 0 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sync_cancel(&ring, 0, 1, 1);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 0 1 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sync_cancel(&ring, 1, 1, 1);
	if (ret) {
		fprintf(stderr, "test_sync_cancel 1 1 1 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sync_cancel_timeout(&ring, 0);
	if (ret) {
		fprintf(stderr, "test_sync_cancel_timeout 0\n");
		return T_EXIT_FAIL;
	}

	/* must be last, leaves request */
	ret = test_sync_cancel_timeout(&ring, 1);
	if (ret) {
		fprintf(stderr, "test_sync_cancel_timeout 1\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
