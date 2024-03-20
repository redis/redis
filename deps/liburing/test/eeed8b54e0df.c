/* SPDX-License-Identifier: MIT */
/*
 * Description: -EAGAIN handling
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "helpers.h"
#include "liburing.h"

#define BLOCK	4096

#ifndef RWF_NOWAIT
#define RWF_NOWAIT	8
#endif

static int get_file_fd(void)
{
	ssize_t ret;
	char *buf;
	int fd;

	fd = open("testfile", O_RDWR | O_CREAT, 0644);
	unlink("testfile");
	if (fd < 0) {
		perror("open file");
		return -1;
	}

	buf = t_malloc(BLOCK);
	memset(buf, 0, BLOCK);
	ret = write(fd, buf, BLOCK);
	if (ret != BLOCK) {
		if (ret < 0)
			perror("write");
		else
			printf("Short write\n");
		goto err;
	}
	fsync(fd);

	if (posix_fadvise(fd, 0, 4096, POSIX_FADV_DONTNEED)) {
		perror("fadvise");
err:
		close(fd);
		free(buf);
		return -1;
	}

	free(buf);
	return fd;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct iovec iov;
	int ret, fd;

	if (argc > 1)
		return T_EXIT_SKIP;

	iov.iov_base = t_malloc(4096);
	iov.iov_len = 4096;

	ret = io_uring_queue_init(2, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return T_EXIT_FAIL;

	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		printf("get sqe failed\n");
		return T_EXIT_FAIL;
	}

	fd = get_file_fd();
	if (fd < 0)
		return T_EXIT_FAIL;

	io_uring_prep_readv(sqe, fd, &iov, 1, 0);
	sqe->rw_flags = RWF_NOWAIT;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		printf("Got submit %d, expected 1\n", ret);
		goto err;
	}

	ret = io_uring_peek_cqe(&ring, &cqe);
	if (ret) {
		printf("Ring peek got %d\n", ret);
		goto err;
	}

	ret = T_EXIT_PASS;
	if (cqe->res != -EAGAIN && cqe->res != 4096) {
		if (cqe->res == -EOPNOTSUPP) {
			ret = T_EXIT_SKIP;
		} else {
			printf("cqe error: %d\n", cqe->res);
			goto err;
		}
	}

	close(fd);
	return ret;
err:
	close(fd);
	return T_EXIT_FAIL;
}
