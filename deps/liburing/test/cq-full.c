/* SPDX-License-Identifier: MIT */
/*
 * Description: test CQ ring overflow
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

int main(int argc, char *argv[])
{
	struct io_uring_cqe *cqe;
	struct io_uring_params p;
	struct io_uring ring;
	int i, ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	memset(&p, 0, sizeof(p));
	ret = io_uring_queue_init_params(4, &ring, &p);
	if (ret) {
		printf("ring setup failed\n");
		return T_EXIT_FAIL;

	}

	if (queue_n_nops(&ring, 4))
		goto err;
	if (queue_n_nops(&ring, 4))
		goto err;
	if (queue_n_nops(&ring, 4))
		goto err;

	i = 0;
	do {
		ret = io_uring_peek_cqe(&ring, &cqe);
		if (ret < 0) {
			if (ret == -EAGAIN)
				break;
			printf("wait completion %d\n", ret);
			goto err;
		}
		io_uring_cqe_seen(&ring, cqe);
		if (!cqe)
			break;
		i++;
	} while (1);

	if (i < 8 ||
	    ((*ring.cq.koverflow != 4) && !(p.features & IORING_FEAT_NODROP))) {
		printf("CQ overflow fail: %d completions, %u overflow\n", i,
				*ring.cq.koverflow);
		goto err;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
err:
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}
