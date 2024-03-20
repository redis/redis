/* SPDX-License-Identifier: MIT */
/*
 * Description: run various eventfd tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>

#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring_params p = {};
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	uint64_t ptr;
	struct iovec vec = {
		.iov_base = &ptr,
		.iov_len = sizeof(ptr)
	};
	int ret, evfd, i;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return T_EXIT_FAIL;
	}
	if (!(p.features & IORING_FEAT_CUR_PERSONALITY)) {
		fprintf(stdout, "Skipping\n");
		return T_EXIT_SKIP;
	}

	evfd = eventfd(0, EFD_CLOEXEC);
	if (evfd < 0) {
		perror("eventfd");
		return T_EXIT_FAIL;
	}

	ret = io_uring_register_eventfd(&ring, evfd);
	if (ret) {
		fprintf(stderr, "failed to register evfd: %d\n", ret);
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, evfd, POLLIN);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_readv(sqe, evfd, &vec, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "submit: %d\n", ret);
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);
	sqe->user_data = 3;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait: %d\n", ret);
			return T_EXIT_FAIL;
		}
		switch (cqe->user_data) {
		case 1:
			/* POLLIN */
			if (cqe->res != 1) {
				fprintf(stderr, "poll: %d\n", cqe->res);
				return T_EXIT_FAIL;
			}
			break;
		case 2:
			if (cqe->res != sizeof(ptr)) {
				fprintf(stderr, "read: %d\n", cqe->res);
				return T_EXIT_FAIL;
			}
			break;
		case 3:
			if (cqe->res) {
				fprintf(stderr, "nop: %d\n", cqe->res);
				return T_EXIT_FAIL;
			}
			break;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	return T_EXIT_PASS;
}
