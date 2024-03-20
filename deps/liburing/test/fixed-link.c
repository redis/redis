/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>

#include "helpers.h"
#include "liburing.h"

#define IOVECS_LEN 2

int main(int argc, char *argv[])
{
	struct iovec iovecs[IOVECS_LEN];
	struct io_uring ring;
	int i, fd, ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	fd = open("/dev/zero", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open /dev/zero\n");
		return T_EXIT_FAIL;
	}

	if (io_uring_queue_init(32, &ring, 0) < 0) {
		fprintf(stderr, "Failed to init io_uring\n");
		close(fd);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < IOVECS_LEN; ++i) {
		iovecs[i].iov_base = t_malloc(64);
		iovecs[i].iov_len = 64;
	}

	ret = io_uring_register_buffers(&ring, iovecs, IOVECS_LEN);
	if (ret) {
		fprintf(stderr, "Failed to register buffers\n");
		return T_EXIT_FAIL;
	}

	for (i = 0; i < IOVECS_LEN; ++i) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
		const char *str = "#include <errno.h>";

		iovecs[i].iov_len = strlen(str);
		io_uring_prep_read_fixed(sqe, fd, iovecs[i].iov_base, strlen(str), 0, i);
		if (i == 0)
			io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
		io_uring_sqe_set_data(sqe, (void *)str);
	}

	ret = io_uring_submit_and_wait(&ring, IOVECS_LEN);
	if (ret < 0) {
		fprintf(stderr, "Failed to submit IO\n");
		return T_EXIT_FAIL;
	} else if (ret < 2) {
		fprintf(stderr, "Submitted %d, wanted %d\n", ret, IOVECS_LEN);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < IOVECS_LEN; i++) {
		struct io_uring_cqe *cqe;

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return T_EXIT_FAIL;
		}
		if (cqe->res != iovecs[i].iov_len) {
			fprintf(stderr, "read: wanted %ld, got %d\n",
					(long) iovecs[i].iov_len, cqe->res);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	close(fd);
	io_uring_queue_exit(&ring);

	for (i = 0; i < IOVECS_LEN; ++i)
		free(iovecs[i].iov_base);

	return T_EXIT_PASS;
}
