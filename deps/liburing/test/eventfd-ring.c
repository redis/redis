/* SPDX-License-Identifier: MIT */
/*
 * Description: test use of eventfds with multiple rings
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
	struct io_uring ring1, ring2;
	struct io_uring_sqe *sqe;
	int ret, evfd1, evfd2;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init_params(8, &ring1, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return T_EXIT_FAIL;
	}
	if (!(p.features & IORING_FEAT_CUR_PERSONALITY)) {
		fprintf(stdout, "Skipping\n");
		return T_EXIT_SKIP;
	}
	ret = io_uring_queue_init(8, &ring2, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	evfd1 = eventfd(0, EFD_CLOEXEC);
	if (evfd1 < 0) {
		perror("eventfd");
		return T_EXIT_FAIL;
	}

	evfd2 = eventfd(0, EFD_CLOEXEC);
	if (evfd2 < 0) {
		perror("eventfd");
		return T_EXIT_FAIL;
	}

	ret = io_uring_register_eventfd(&ring1, evfd1);
	if (ret) {
		fprintf(stderr, "failed to register evfd: %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = io_uring_register_eventfd(&ring2, evfd2);
	if (ret) {
		fprintf(stderr, "failed to register evfd: %d\n", ret);
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring1);
	io_uring_prep_poll_add(sqe, evfd2, POLLIN);
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring2);
	io_uring_prep_poll_add(sqe, evfd1, POLLIN);
	sqe->user_data = 1;

	ret = io_uring_submit(&ring1);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = io_uring_submit(&ring2);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring1);
	io_uring_prep_nop(sqe);
	sqe->user_data = 3;

	ret = io_uring_submit(&ring1);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
