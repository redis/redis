/* SPDX-License-Identifier: MIT */
/*
 * Description: Check to see if wait_nr is being honored.
 */
#include <stdio.h>
#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret;
	struct __kernel_timespec ts = {
		.tv_sec = 0,
		.tv_nsec = 10000000
	};

	if (argc > 1)
		return T_EXIT_SKIP;

	if (io_uring_queue_init(4, &ring, 0) != 0) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;
	}

	/*
	 * First, submit the timeout sqe so we can actually finish the test
	 * if everything is in working order.
	 */
	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return T_EXIT_FAIL;
	}
	io_uring_prep_timeout(sqe, &ts, (unsigned)-1, 0);

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "Got submit %d, expected 1\n", ret);
		return T_EXIT_FAIL;
	}

	/*
	 * Next, submit a nop and wait for two events. If everything is working
	 * as it should, we should be waiting for more than a millisecond and we
	 * should see two cqes. Otherwise, execution continues immediately
	 * and we see only one cqe.
	 */
	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return T_EXIT_FAIL;
	}
	io_uring_prep_nop(sqe);

	ret = io_uring_submit_and_wait(&ring, 2);
	if (ret != 1) {
		fprintf(stderr, "Got submit %d, expected 1\n", ret);
		return T_EXIT_FAIL;
	}

	if (io_uring_peek_cqe(&ring, &cqe) != 0) {
		fprintf(stderr, "Unable to peek cqe!\n");
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(&ring, cqe);

	if (io_uring_peek_cqe(&ring, &cqe) != 0) {
		fprintf(stderr, "Unable to peek cqe!\n");
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
}
