/* SPDX-License-Identifier: MIT */
/*
 * Description: test CQ peek-batch
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

static int queue_n_nops(struct io_uring *ring, int n, int offset)
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
		sqe->user_data = i + offset;
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

#define CHECK_BATCH(ring, got, cqes, count, expected) do {\
	got = io_uring_peek_batch_cqe((ring), cqes, count);\
	if (got != expected) {\
		printf("Got %d CQs, expected %d\n", got, expected);\
		goto err;\
	}\
} while(0)

int main(int argc, char *argv[])
{
	struct io_uring_cqe *cqes[8];
	struct io_uring ring;
	int ret, i;
	unsigned got;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(4, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return T_EXIT_FAIL;

	}

	CHECK_BATCH(&ring, got, cqes, 4, 0);
	if (queue_n_nops(&ring, 4, 0))
		goto err;

	CHECK_BATCH(&ring, got, cqes, 4, 4);
	for (i=0;i<4;i++) {
		if (i != cqes[i]->user_data) {
			printf("Got user_data %" PRIu64 ", expected %d\n",
				(uint64_t) cqes[i]->user_data, i);
			goto err;
		}
	}

	if (queue_n_nops(&ring, 4, 4))
		goto err;

	io_uring_cq_advance(&ring, 4);
	CHECK_BATCH(&ring, got, cqes, 4, 4);
	for (i=0;i<4;i++) {
		if (i + 4 != cqes[i]->user_data) {
			printf("Got user_data %" PRIu64 ", expected %d\n",
				(uint64_t) cqes[i]->user_data, i + 4);
			goto err;
		}
	}

	io_uring_cq_advance(&ring, 8);
	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
err:
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}
