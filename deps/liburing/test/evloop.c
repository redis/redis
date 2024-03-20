/* SPDX-License-Identifier: MIT */
/*
 * Test that we don't recursively generate completion events if an io_uring
 * has an eventfd registered that triggers on completions, and we add a poll
 * request with multishot on the eventfd. Older kernels will stop on overflow,
 * newer kernels will detect this earlier and abort correctly.
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <poll.h>
#include <assert.h>
#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, efd, i;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "Ring init failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	efd = eventfd(0, 0);
	if (efd < 0) {
		perror("eventfd");
		return T_EXIT_FAIL;
	}

	ret = io_uring_register_eventfd(&ring, efd);
	if (ret) {
		fprintf(stderr, "Ring eventfd register failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_multishot(sqe, efd, POLLIN);
	sqe->user_data = 1;
	io_uring_submit(&ring);

	sqe = io_uring_get_sqe(&ring);
	sqe->user_data = 2;
	io_uring_prep_nop(sqe);
	io_uring_submit(&ring);

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe ret = %d\n", ret);
			break;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	ret = io_uring_peek_cqe(&ring, &cqe);
	if (!ret) {
		fprintf(stderr, "Generated too many events\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
