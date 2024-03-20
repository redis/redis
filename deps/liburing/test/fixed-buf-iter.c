/* SPDX-License-Identifier: MIT */
/*
 * Test fixed buffers with non-iterators.
 *
 * Taken from: https://github.com/axboe/liburing/issues/549
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>

#include "liburing.h"
#include "helpers.h"

#define BUF_SIZE    4096
#define BUFFERS     1
#define IN_FD       "/dev/urandom"
#define OUT_FD      "/dev/zero"

static int test(struct io_uring *ring)
{
	struct iovec iov[BUFFERS];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, fd_in, fd_out, i;

	fd_in = open(IN_FD, O_RDONLY, 0644);
	if (fd_in < 0) {
		perror("open in");
		return 1;
	}

	fd_out = open(OUT_FD, O_RDWR, 0644);
	if (fd_out < 0) {
		perror("open out");
		return 1;
	}

	for (i = 0; i < BUFFERS; i++) {
		iov[i].iov_base = malloc(BUF_SIZE);
		iov[i].iov_len = BUF_SIZE;
		memset(iov[i].iov_base, 0, BUF_SIZE);
	}

	ret = io_uring_register_buffers(ring, iov, BUFFERS);
	if (ret) {
		fprintf(stderr, "Error registering buffers: %s", strerror(-ret));
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "Could not get SQE.\n");
		return 1;
	}

	io_uring_prep_read_fixed(sqe, fd_in, iov[0].iov_base, BUF_SIZE, 0, 0);
	io_uring_submit(ring);

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
		return 1;
	}

	if (cqe->res < 0) { 
		fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
		return 1;
	}
	io_uring_cqe_seen(ring, cqe);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "Could not get SQE.\n");
		return 1;
	}
	io_uring_prep_write_fixed(sqe, fd_out, iov[0].iov_base, BUF_SIZE, 0, 0);
	io_uring_submit(ring);

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
		return 1;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
		return 1;
	}
	io_uring_cqe_seen(ring, cqe);
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = t_create_ring(8, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_SKIP;
	else if (ret < 0)
		return T_EXIT_FAIL;

	ret = test(&ring);
	if (ret) {
		fprintf(stderr, "Test failed\n");
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
}
