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

#define DIE(...)				\
	do {					\
		fprintf(stderr, __VA_ARGS__);	\
		abort();			\
	} while(0)

static const int RSIZE = 2;
static const int OPEN_FLAGS = O_RDWR | O_CREAT | O_LARGEFILE;
static const mode_t OPEN_MODE = S_IRUSR | S_IWUSR;

static int open_io_uring(struct io_uring *ring, int dfd, const char *fn)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, fd;

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
	fd = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret < 0) {
		fprintf(stderr, "wait_cqe failed: %s\n", strerror(-ret));
		return 1;
	} else if (fd < 0) {
		fprintf(stderr, "io_uring openat failed: %s\n", strerror(-fd));
		return 1;
	}

	close(fd);
	return 0;
}

static int prepare_file(int dfd, const char* fn)
{
	const char buf[] = "foo";
	int fd, res;

	fd = openat(dfd, fn, OPEN_FLAGS, OPEN_MODE);
	if (fd < 0) {
		fprintf(stderr, "prepare/open: %s\n", strerror(errno));
		return -1;
	}

	res = pwrite(fd, buf, sizeof(buf), 1ull << 32);
	if (res < 0)
		fprintf(stderr, "prepare/pwrite: %s\n", strerror(errno));

	close(fd);
	return res < 0 ? res : 0;
}

static int test_linked_files(int dfd, const char *fn, bool async)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	char buffer[128];
	struct iovec iov = {.iov_base = buffer, .iov_len = sizeof(buffer), };
	int ret, fd;
	int fds[2];

	ret = io_uring_queue_init(10, &ring, 0);
	if (ret < 0)
		DIE("failed to init io_uring: %s\n", strerror(-ret));

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		printf("get sqe failed\n");
		return -1;
	}
	io_uring_prep_readv(sqe, fds[0], &iov, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	if (async)
		sqe->flags |= IOSQE_ASYNC;

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "failed to get sqe\n");
		return 1;
	}
	io_uring_prep_openat(sqe, dfd, fn, OPEN_FLAGS, OPEN_MODE);

	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "failed to submit openat: %s\n", strerror(-ret));
		return 1;
	}

	fd = dup(ring.ring_fd);
	if (fd < 0) {
		fprintf(stderr, "dup() failed: %s\n", strerror(-fd));
		return 1;
	}

	/* io_uring->flush() */
	close(fd);

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_drained_files(int dfd, const char *fn, bool linked, bool prepend)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	char buffer[128];
	struct iovec iov = {.iov_base = buffer, .iov_len = sizeof(buffer), };
	int ret, fd, fds[2], to_cancel = 0;

	ret = io_uring_queue_init(10, &ring, 0);
	if (ret < 0)
		DIE("failed to init io_uring: %s\n", strerror(-ret));

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		printf("get sqe failed\n");
		return -1;
	}
	io_uring_prep_readv(sqe, fds[0], &iov, 1, 0);
	sqe->user_data = 0;

	if (prepend) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "failed to get sqe\n");
			return 1;
		}
		io_uring_prep_nop(sqe);
		sqe->flags |= IOSQE_IO_DRAIN;
		to_cancel++;
		sqe->user_data = to_cancel;
	}

	if (linked) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "failed to get sqe\n");
			return 1;
		}
		io_uring_prep_nop(sqe);
		sqe->flags |= IOSQE_IO_DRAIN | IOSQE_IO_LINK;
		to_cancel++;
		sqe->user_data = to_cancel;
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "failed to get sqe\n");
		return 1;
	}
	io_uring_prep_openat(sqe, dfd, fn, OPEN_FLAGS, OPEN_MODE);
	sqe->flags |= IOSQE_IO_DRAIN;
	to_cancel++;
	sqe->user_data = to_cancel;


	ret = io_uring_submit(&ring);
	if (ret != 1 + to_cancel) {
		fprintf(stderr, "failed to submit openat: %s\n", strerror(-ret));
		return 1;
	}

	fd = dup(ring.ring_fd);
	if (fd < 0) {
		fprintf(stderr, "dup() failed: %s\n", strerror(-fd));
		return 1;
	}

	/*
	 * close(), which triggers ->flush(), and io_uring_queue_exit()
	 * should successfully return and not hang.
	 */
	close(fd);
	io_uring_queue_exit(&ring);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *fn = "io_uring_openat_test";
	struct io_uring ring;
	int ret, dfd;

	if (argc > 1)
		return 0;

	dfd = open("/tmp", O_PATH);
	if (dfd < 0)
		DIE("open /tmp: %s\n", strerror(errno));

	ret = io_uring_queue_init(RSIZE, &ring, 0);
	if (ret < 0)
		DIE("failed to init io_uring: %s\n", strerror(-ret));

	if (prepare_file(dfd, fn))
		return 1;

	ret = open_io_uring(&ring, dfd, fn);
	if (ret) {
		fprintf(stderr, "open_io_uring() failed\n");
		goto out;
	}

	ret = test_linked_files(dfd, fn, false);
	if (ret) {
		fprintf(stderr, "test_linked_files() !async failed\n");
		goto out;
	}

	ret = test_linked_files(dfd, fn, true);
	if (ret) {
		fprintf(stderr, "test_linked_files() async failed\n");
		goto out;
	}

	ret = test_drained_files(dfd, fn, false, false);
	if (ret) {
		fprintf(stderr, "test_drained_files() failed\n");
		goto out;
	}

	ret = test_drained_files(dfd, fn, false, true);
	if (ret) {
		fprintf(stderr, "test_drained_files() middle failed\n");
		goto out;
	}

	ret = test_drained_files(dfd, fn, true, false);
	if (ret) {
		fprintf(stderr, "test_drained_files() linked failed\n");
		goto out;
	}
out:
	io_uring_queue_exit(&ring);
	close(dfd);
	unlink("/tmp/io_uring_openat_test");
	return ret;
}
