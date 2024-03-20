/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>

#include "liburing.h"
#include "helpers.h"

static const int RSIZE = 2;
static const int OPEN_FLAGS = O_RDWR | O_CREAT | O_LARGEFILE;
static const mode_t OPEN_MODE = S_IRUSR | S_IWUSR;

#define DIE(...)				\
	do {					\
		fprintf(stderr, __VA_ARGS__);	\
		abort();			\
	} while(0)

static int do_write(struct io_uring *ring, int fd, off_t offset)
{
	char buf[] = "some test write buf";
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int res, ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "failed to get sqe\n");
		return 1;
	}
	io_uring_prep_write(sqe, fd, buf, sizeof(buf), offset);

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "failed to submit write: %s\n", strerror(-ret));
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait_cqe failed: %s\n", strerror(-ret));
		return 1;
	}

	res = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (res < 0) {
		fprintf(stderr, "write failed: %s\n", strerror(-res));
		return 1;
	}

	return 0;
}

static int test_open_write(struct io_uring *ring, int dfd, const char *fn)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, fd = -1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "failed to get sqe\n");
		return 1;
	}
	io_uring_prep_openat(sqe, dfd, fn, OPEN_FLAGS, OPEN_MODE);

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "failed to submit openat: %s\n", strerror(-ret));
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait_cqe failed: %s\n", strerror(-ret));
		return 1;
	}

	fd = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (fd < 0) {
		fprintf(stderr, "openat failed: %s\n", strerror(-fd));
		return 1;
	}

	return do_write(ring, fd, 1ULL << 32);
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int dfd, ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
	if (dfd < 0)
		DIE("open /tmp: %s\n", strerror(errno));

	ret = io_uring_queue_init(RSIZE, &ring, 0);
	if (ret < 0)
		DIE("failed to init io_uring: %s\n", strerror(-ret));

	ret = test_open_write(&ring, dfd, "io_uring_openat_write_test1");

	io_uring_queue_exit(&ring);
	close(dfd);
	unlink("/tmp/io_uring_openat_write_test1");
	return ret;
}
