/* SPDX-License-Identifier: MIT */
/*
 * Description: test IORING_SETUP_SUBMIT_ALL
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "liburing.h"
#include "helpers.h"

static int test(struct io_uring *ring, int expect_drops)
{
	struct io_uring_sqe *sqe;
	char buf[32];
	int ret, i;

	for (i = 0; i < 4; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}

		io_uring_prep_nop(sqe);
	}

	/* prep two invalid reads, these will fail */
	for (i = 0; i < 2; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}

		io_uring_prep_read(sqe, 128, buf, sizeof(buf), 0);
		sqe->ioprio = (short) -1;
	}


	ret = io_uring_submit(ring);
	if (expect_drops) {
		if (ret != 5) {
			fprintf(stderr, "drops submit failed: %d\n", ret);
			goto err;
		}
	} else {
		if (ret != 6) {
			fprintf(stderr, "no drops submit failed: %d\n", ret);
			goto err;
		}
	}

	return 0;
err:
	return 1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SUBMIT_ALL);
	if (ret)
		return 0;

	ret = test(&ring, 0);
	if (ret) {
		fprintf(stderr, "test no drops failed\n");
		return ret;
	}

	io_uring_queue_exit(&ring);

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(&ring, 1);
	if (ret) {
		fprintf(stderr, "test drops failed\n");
		return ret;
	}

	return T_EXIT_PASS;
}
