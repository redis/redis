/* SPDX-License-Identifier: MIT */
/*
 * Description: test CQ ready
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

static int queue_n_nops(struct io_uring *ring, int n)
{
	struct io_uring_sqe *sqe;
	int i, ret;

	for (i = 0; i < n; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			printf("get sqe failed\n");
			goto err;
		}

		io_uring_prep_nop(sqe);
	}

	ret = io_uring_submit(ring);
	if (ret < n) {
		printf("Submitted only %d\n", ret);
		goto err;
	} else if (ret < 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	return 0;
err:
	return 1;
}

#define CHECK_READY(ring, expected) do {\
	ready = io_uring_cq_ready((ring));\
	if (ready != expected) {\
		printf("Got %d CQs ready, expected %d\n", ready, expected);\
		goto err;\
	}\
} while(0)

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;
	unsigned ready;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(4, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return T_EXIT_FAIL;

	}

	CHECK_READY(&ring, 0);
	if (queue_n_nops(&ring, 4))
		goto err;

	CHECK_READY(&ring, 4);
	io_uring_cq_advance(&ring, 4);
	CHECK_READY(&ring, 0);
	if (queue_n_nops(&ring, 4))
		goto err;

	CHECK_READY(&ring, 4);

	io_uring_cq_advance(&ring, 1);
	CHECK_READY(&ring, 3);

	io_uring_cq_advance(&ring, 2);
	CHECK_READY(&ring, 1);

	io_uring_cq_advance(&ring, 1);
	CHECK_READY(&ring, 0);

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
err:
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}
