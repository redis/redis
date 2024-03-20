/* SPDX-License-Identifier: MIT */
/*
 * Description: run various unlink tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "liburing.h"

static int test_unlink(struct io_uring *ring, const char *old)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_unlink(sqe, old, 0);
	
	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
err:
	return 1;
}

static int stat_file(const char *buf)
{
	struct stat sb;

	if (!stat(buf, &sb))
		return 0;

	return errno;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	char buf[32] = "./XXXXXX";
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	ret = mkstemp(buf);
	if (ret < 0) {
		perror("mkstemp");
		return 1;
	}
	close(ret);

	if (stat_file(buf) != 0) {
		perror("stat");
		return 1;
	}

	ret = test_unlink(&ring, buf);
	if (ret < 0) {
		if (ret == -EBADF || ret == -EINVAL) {
			fprintf(stdout, "Unlink not supported, skipping\n");
			unlink(buf);
			return 0;
		}
		fprintf(stderr, "rename: %s\n", strerror(-ret));
		goto err;
	} else if (ret)
		goto err;

	ret = stat_file(buf);
	if (ret != ENOENT) {
		fprintf(stderr, "stat got %s\n", strerror(ret));
		return 1;
	}

	ret = test_unlink(&ring, "/3/2/3/1/z/y");
	if (ret != -ENOENT) {
		fprintf(stderr, "invalid unlink got %s\n", strerror(-ret));
		return 1;
	}

	return 0;
err:
	unlink(buf);
	return 1;
}
