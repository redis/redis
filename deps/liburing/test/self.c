/* SPDX-License-Identifier: MIT */
/*
 * Description: test that pathname resolution works from async context when
 * using /proc/self/ which should be the original submitting task, not the
 * async worker.
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"

static int io_openat2(struct io_uring *ring, const char *path, int dfd)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct open_how how;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	memset(&how, 0, sizeof(how));
	how.flags = O_RDONLY;
	io_uring_prep_openat2(sqe, dfd, path, &how);

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
	return -1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	char buf[64];
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = io_openat2(&ring, "/proc/self/comm", -1);
	if (ret < 0) {
		if (ret == -EOPNOTSUPP)
			return 0;
		if (ret == -EINVAL) {
			fprintf(stdout, "openat2 not supported, skipping\n");
			return 0;
		}
		fprintf(stderr, "openat2 failed: %s\n", strerror(-ret));
		return 1;
	}

	memset(buf, 0, sizeof(buf));
	ret = read(ret, buf, sizeof(buf));
	if (ret < 0) {
		perror("read");
		return 1;
	}

	if (strncmp(buf, "self", 4)) {
		fprintf(stderr, "got comm=<%s>, wanted <self>\n", buf);
		return 1;
	}

	return 0;
}
