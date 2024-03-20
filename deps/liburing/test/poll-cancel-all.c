/* SPDX-License-Identifier: MIT */
/*
 * Description: Test IORING_ASYNC_CANCEL_{ALL,FD}
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#include "liburing.h"

static int no_cancel_flags;

static int test1(struct io_uring *ring, int *fd, int fixed)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, i, __fd = fd[0];

	if (fixed)
		__fd = 0;

	if (fixed) {
		ret = io_uring_register_files(ring, fd, 1);
		if (ret) {
			fprintf(stderr, "failed file register %d\n", ret);
			return 1;
		}
	}

	for (i = 0; i < 8; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			return 1;
		}

		io_uring_prep_poll_add(sqe, __fd, POLLIN);
		sqe->user_data = i + 1;
		if (fixed)
			sqe->flags |= IOSQE_FIXED_FILE;
	}

	ret = io_uring_submit(ring);
	if (ret < 8) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return 1;
	}

	/*
	 * Mark CANCEL_ALL to cancel all matching the key, and use
	 * CANCEL_FD to cancel requests matching the specified fd.
	 * This should cancel all the pending poll requests on the pipe
	 * input.
	 */
	io_uring_prep_cancel(sqe, 0, IORING_ASYNC_CANCEL_ALL);
	sqe->cancel_flags |= IORING_ASYNC_CANCEL_FD;
	if (fixed)
		sqe->cancel_flags |= IORING_ASYNC_CANCEL_FD_FIXED;
	sqe->fd = __fd;
	sqe->user_data = 100;

	ret = io_uring_submit(ring);
	if (ret < 1) {
		fprintf(stderr, "child: sqe submit failed: %d\n", ret);
		return 1;
	}

	for (i = 0; i < 9; i++) {
		if (no_cancel_flags)
			break;
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait=%d\n", ret);
			return 1;
		}
		switch (cqe->user_data) {
		case 100:
			if (cqe->res == -EINVAL) {
				no_cancel_flags = 1;
				break;
			}
			if (cqe->res != 8) {
				fprintf(stderr, "canceled %d\n", cqe->res);
				return 1;
			}
			break;
		case 1 ... 8:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "poll res %d\n", cqe->res);
				return 1;
			}
			break;
		default:
			fprintf(stderr, "invalid user_data %lu\n",
					(unsigned long) cqe->user_data);
			return 1;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	if (fixed)
		io_uring_unregister_files(ring);

	return 0;
}

static int test2(struct io_uring *ring, int *fd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, i, fd2[2];

	if (pipe(fd2) < 0) {
		perror("pipe");
		return 1;
	}

	for (i = 0; i < 8; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}

		if (!(i & 1))
			io_uring_prep_poll_add(sqe, fd[0], POLLIN);
		else
			io_uring_prep_poll_add(sqe, fd2[0], POLLIN);
		sqe->user_data = i & 1;
	}

	ret = io_uring_submit(ring);
	if (ret < 8) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}

	/*
	 * Mark CANCEL_ALL to cancel all matching the key, and use
	 * CANCEL_FD to cancel requests matching the specified fd.
	 * This should cancel all the pending poll requests on the pipe
	 * input.
	 */
	io_uring_prep_cancel(sqe, 0, IORING_ASYNC_CANCEL_ALL);
	sqe->cancel_flags |= IORING_ASYNC_CANCEL_FD;
	sqe->fd = fd[0];
	sqe->user_data = 100;

	ret = io_uring_submit(ring);
	if (ret < 1) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 5; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait=%d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 100:
			if (cqe->res != 4) {
				fprintf(stderr, "canceled %d\n", cqe->res);
				goto err;
			}
			break;
		case 0:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "poll res %d\n", cqe->res);
				goto err;
			}
			break;
		default:
			fprintf(stderr, "invalid user_data %lu\n",
					(unsigned long) cqe->user_data);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	usleep(1000);

	/*
	 * Should not have any pending CQEs now
	 */
	ret = io_uring_peek_cqe(ring, &cqe);
	if (!ret) {
		fprintf(stderr, "Unexpected extra cancel cqe\n");
		goto err;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}

	/*
	 * Mark CANCEL_ALL to cancel all matching the key, and use
	 * CANCEL_FD to cancel requests matching the specified fd.
	 * This should cancel all the pending poll requests on the pipe
	 * input.
	 */
	io_uring_prep_cancel(sqe, 0, IORING_ASYNC_CANCEL_ALL);
	sqe->cancel_flags |= IORING_ASYNC_CANCEL_FD;
	sqe->fd = fd2[0];
	sqe->user_data = 100;

	ret = io_uring_submit(ring);
	if (ret < 1) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 5; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait=%d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 100:
			if (cqe->res != 4) {
				fprintf(stderr, "canceled %d\n", cqe->res);
				goto err;
			}
			break;
		case 1:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "poll res %d\n", cqe->res);
				goto err;
			}
			break;
		default:
			fprintf(stderr, "invalid user_data %lu\n",
					(unsigned long) cqe->user_data);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(fd2[0]);
	close(fd2[1]);
	return 0;
err:
	close(fd2[0]);
	close(fd2[1]);
	return 1;
}

static int test3(struct io_uring *ring, int *fd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, i, fd2[2];

	if (pipe(fd2) < 0) {
		perror("pipe");
		return 1;
	}

	for (i = 0; i < 8; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}

		if (!(i & 1)) {
			io_uring_prep_poll_add(sqe, fd[0], POLLIN);
			sqe->flags |= IOSQE_ASYNC;
		} else
			io_uring_prep_poll_add(sqe, fd2[0], POLLIN);
		sqe->user_data = i & 1;
	}

	ret = io_uring_submit(ring);
	if (ret < 8) {
		fprintf(stderr, "child: sqe submit failed: %d\n", ret);
		goto err;
	}

	usleep(10000);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}

	/*
	 * Mark CANCEL_ALL to cancel all matching the key, and use
	 * CANCEL_FD to cancel requests matching the specified fd.
	 * This should cancel all the pending poll requests on the pipe
	 * input.
	 */
	io_uring_prep_cancel(sqe, 0, IORING_ASYNC_CANCEL_ALL);
	sqe->cancel_flags |= IORING_ASYNC_CANCEL_ANY;
	sqe->fd = 0;
	sqe->user_data = 100;

	ret = io_uring_submit(ring);
	if (ret < 1) {
		fprintf(stderr, "child: sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 9; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait=%d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 100:
			if (cqe->res != 8) {
				fprintf(stderr, "canceled %d\n", cqe->res);
				goto err;
			}
			break;
		case 0:
		case 1:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "poll res %d\n", cqe->res);
				goto err;
			}
			break;
		default:
			fprintf(stderr, "invalid user_data %lu\n",
					(unsigned long) cqe->user_data);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(fd2[0]);
	close(fd2[1]);
	return 0;
err:
	close(fd2[0]);
	close(fd2[1]);
	return 1;
}

static int test4(struct io_uring *ring, int *fd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	char buffer[32];
	int ret, i;

	for (i = 0; i < 8; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}

		io_uring_prep_read(sqe, fd[0], &buffer, sizeof(buffer), 0);
		sqe->flags |= IOSQE_ASYNC;
		sqe->user_data = i + 1;
	}

	ret = io_uring_submit(ring);
	if (ret < 8) {
		fprintf(stderr, "child: sqe submit failed: %d\n", ret);
		goto err;
	}

	usleep(10000);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}

	/*
	 * Mark CANCEL_ALL to cancel all matching the key, and use
	 * CANCEL_FD to cancel requests matching the specified fd.
	 * This should cancel all the pending poll requests on the pipe
	 * input.
	 */
	io_uring_prep_cancel(sqe, 0, IORING_ASYNC_CANCEL_ALL);
	sqe->cancel_flags |= IORING_ASYNC_CANCEL_ANY;
	sqe->fd = 0;
	sqe->user_data = 100;

	ret = io_uring_submit(ring);
	if (ret < 1) {
		fprintf(stderr, "child: sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 9; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait=%d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 100:
			if (cqe->res != 8) {
				fprintf(stderr, "canceled %d\n", cqe->res);
				goto err;
			}
			break;
		case 1 ... 8:
			if (cqe->res != -ECANCELED) {
				fprintf(stderr, "poll res %d\n", cqe->res);
				goto err;
			}
			break;
		default:
			fprintf(stderr, "invalid user_data %lu\n",
					(unsigned long) cqe->user_data);
			goto err;
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
	int ret, fd[2];

	if (argc > 1)
		return 0;

	if (pipe(fd) < 0) {
		perror("pipe");
		return 1;
	}

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	ret = test1(&ring, fd, 0);
	if (ret) {
		fprintf(stderr, "test1 failed\n");
		return ret;
	}
	if (no_cancel_flags)
		return 0;

	ret = test1(&ring, fd, 1);
	if (ret) {
		fprintf(stderr, "test1 fixed failed\n");
		return ret;
	}

	ret = test2(&ring, fd);
	if (ret) {
		fprintf(stderr, "test2 failed\n");
		return ret;
	}

	ret = test3(&ring, fd);
	if (ret) {
		fprintf(stderr, "test3 failed\n");
		return ret;
	}

	ret = test4(&ring, fd);
	if (ret) {
		fprintf(stderr, "test4 failed\n");
		return ret;
	}

	return 0;
}
