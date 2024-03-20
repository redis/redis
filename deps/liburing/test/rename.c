/* SPDX-License-Identifier: MIT */
/*
 * Description: run various rename tests
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

static int test_rename(struct io_uring *ring, const char *old, const char *new)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}

	memset(sqe, 0, sizeof(*sqe));

	io_uring_prep_rename(sqe, old, new);
	
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
	char src[32] = "./XXXXXX";
	char dst[32] = "./XXXXXX";
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	ret = mkstemp(src);
	if (ret < 0) {
		perror("mkstemp");
		return 1;
	}
	close(ret);

	ret = mkstemp(dst);
	if (ret < 0) {
		perror("mkstemp");
		return 1;
	}
	close(ret);

	if (stat_file(src) != 0) {
		perror("stat");
		return 1;
	}
	if (stat_file(dst) != 0) {
		perror("stat");
		return 1;
	}

	ret = test_rename(&ring, src, dst);
	if (ret < 0) {
		if (ret == -EBADF || ret == -EINVAL) {
			fprintf(stdout, "Rename not supported, skipping\n");
			goto out;
		}
		fprintf(stderr, "rename: %s\n", strerror(-ret));
		goto err;
	} else if (ret)
		goto err;

	if (stat_file(src) != ENOENT) {
		fprintf(stderr, "stat got %s\n", strerror(ret));
		return 1;
	}

	if (stat_file(dst) != 0) {
		perror("stat");
		return 1;
	}

	ret = test_rename(&ring, "/x/y/1/2", "/2/1/y/x");
	if (ret != -ENOENT) {
		fprintf(stderr, "test_rename invalid failed: %d\n", ret);
		return ret;
	}
out:
	unlink(dst);
	return 0;
err:
	unlink(src);
	unlink(dst);
	return 1;
}
