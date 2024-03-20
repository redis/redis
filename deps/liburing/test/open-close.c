/* SPDX-License-Identifier: MIT */
/*
 * Description: run various openat(2) tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include "helpers.h"
#include "liburing.h"

static int submit_wait(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int ret;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return 1;
	}
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		return 1;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static inline int try_close(struct io_uring *ring, int fd, int slot)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_close(sqe, fd);
	__io_uring_set_target_fixed_file(sqe, slot);
	return submit_wait(ring);
}

static int test_close_fixed(void)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	int ret, fds[2];
	char buf[1];

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return -1;
	}
	if (pipe(fds)) {
		perror("pipe");
		return -1;
	}

	ret = try_close(&ring, 0, 0);
	if (ret == -EINVAL) {
		fprintf(stderr, "close for fixed files is not supported\n");
		return 0;
	} else if (ret != -ENXIO) {
		fprintf(stderr, "no table failed %i\n", ret);
		return -1;
	}

	ret = try_close(&ring, 1, 0);
	if (ret != -EINVAL) {
		fprintf(stderr, "set fd failed %i\n", ret);
		return -1;
	}

	ret = io_uring_register_files(&ring, fds, 2);
	if (ret) {
		fprintf(stderr, "file_register: %d\n", ret);
		return ret;
	}

	ret = try_close(&ring, 0, 2);
	if (ret != -EINVAL) {
		fprintf(stderr, "out of table failed %i\n", ret);
		return -1;
	}

	ret = try_close(&ring, 0, 0);
	if (ret != 0) {
		fprintf(stderr, "close failed %i\n", ret);
		return -1;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, 0, buf, sizeof(buf), 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	ret = submit_wait(&ring);
	if (ret != -EBADF) {
		fprintf(stderr, "read failed %i\n", ret);
		return -1;
	}

	ret = try_close(&ring, 0, 1);
	if (ret != 0) {
		fprintf(stderr, "close 2 failed %i\n", ret);
		return -1;
	}

	ret = try_close(&ring, 0, 0);
	if (ret != -EBADF) {
		fprintf(stderr, "empty slot failed %i\n", ret);
		return -1;
	}

	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

static int test_close(struct io_uring *ring, int fd, int is_ring_fd)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_close(sqe, fd);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		if (!(is_ring_fd && ret == -EBADF)) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		return ret;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
err:
	return -1;
}

static int test_openat(struct io_uring *ring, const char *path, int dfd)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_openat(sqe, dfd, path, O_RDONLY, 0);

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
	const char *path, *path_rel;
	int ret, do_unlink;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	if (argc > 1) {
		path = "/tmp/.open.close";
		path_rel = argv[1];
		do_unlink = 0;
	} else {
		path = "/tmp/.open.close";
		path_rel = ".open.close";
		do_unlink = 1;
	}

	t_create_file(path, 4096);

	if (do_unlink)
		t_create_file(path_rel, 4096);

	ret = test_openat(&ring, path, -1);
	if (ret < 0) {
		if (ret == -EINVAL) {
			fprintf(stdout, "Open not supported, skipping\n");
			goto done;
		}
		fprintf(stderr, "test_openat absolute failed: %d\n", ret);
		goto err;
	}

	ret = test_openat(&ring, path_rel, AT_FDCWD);
	if (ret < 0) {
		fprintf(stderr, "test_openat relative failed: %d\n", ret);
		goto err;
	}

	ret = test_close(&ring, ret, 0);
	if (ret) {
		fprintf(stderr, "test_close normal failed\n");
		goto err;
	}

	ret = test_close(&ring, ring.ring_fd, 1);
	if (ret != -EBADF) {
		fprintf(stderr, "test_close ring_fd failed\n");
		goto err;
	}

	ret = test_close_fixed();
	if (ret) {
		fprintf(stderr, "test_close_fixed failed\n");
		goto err;
	}

done:
	unlink(path);
	if (do_unlink)
		unlink(path_rel);
	return 0;
err:
	unlink(path);
	if (do_unlink)
		unlink(path_rel);
	return 1;
}
