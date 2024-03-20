/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

static int register_file(struct io_uring *ring)
{
	char buf[32];
	int ret, fd;

	sprintf(buf, "./XXXXXX");
	fd = mkstemp(buf);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	ret = io_uring_register_files(ring, &fd, 1);
	if (ret) {
		fprintf(stderr, "file register %d\n", ret);
		return 1;
	}

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "file register %d\n", ret);
		return 1;
	}

	unlink(buf);
	close(fd);
	return 0;
}

static int test_single_fsync(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	char buf[32];
	int fd, ret;

	sprintf(buf, "./XXXXXX");
	fd = mkstemp(buf);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_fsync(sqe, fd, 0);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		printf("wait completion %d\n", ret);
		goto err;
	}

	io_uring_cqe_seen(ring, cqe);
	unlink(buf);
	return 0;
err:
	unlink(buf);
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

	ret = register_file(&ring);
	if (ret)
		return ret;
	ret = test_single_fsync(&ring);
	if (ret) {
		printf("test_single_fsync failed\n");
		return ret;
	}

	return T_EXIT_PASS;
}
