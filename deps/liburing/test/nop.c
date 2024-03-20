/* SPDX-License-Identifier: MIT */
/*
 * Description: run various nop tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"
#include "test.h"

static int seq;

static int test_single_nop(struct io_uring *ring, unsigned req_flags)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;
	bool cqe32 = (ring->flags & IORING_SETUP_CQE32);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);
	sqe->user_data = ++seq;
	sqe->flags |= req_flags;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}
	if (!cqe->user_data) {
		fprintf(stderr, "Unexpected 0 user_data\n");
		goto err;
	}
	if (cqe32) {
		if (cqe->big_cqe[0] != 0) {
			fprintf(stderr, "Unexpected extra1\n");
			goto err;

		}
		if (cqe->big_cqe[1] != 0) {
			fprintf(stderr, "Unexpected extra2\n");
			goto err;
		}
	}
	io_uring_cqe_seen(ring, cqe);
	return 0;
err:
	return 1;
}

static int test_barrier_nop(struct io_uring *ring, unsigned req_flags)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;
	bool cqe32 = (ring->flags & IORING_SETUP_CQE32);

	for (i = 0; i < 8; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}

		io_uring_prep_nop(sqe);
		if (i == 4)
			sqe->flags = IOSQE_IO_DRAIN;
		sqe->user_data = ++seq;
		sqe->flags |= req_flags;
	}

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	} else if (ret < 8) {
		fprintf(stderr, "Submitted only %d\n", ret);
		goto err;
	}

	for (i = 0; i < 8; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (!cqe->user_data) {
			fprintf(stderr, "Unexpected 0 user_data\n");
			goto err;
		}
		if (cqe32) {
			if (cqe->big_cqe[0] != 0) {
				fprintf(stderr, "Unexpected extra1\n");
				goto err;
			}
			if (cqe->big_cqe[1] != 0) {
				fprintf(stderr, "Unexpected extra2\n");
				goto err;
			}
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

static int test_ring(unsigned flags)
{
	struct io_uring ring;
	struct io_uring_params p = { };
	int ret, i;

	p.flags = flags;
	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		if (ret == -EINVAL)
			return 0;
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	for (i = 0; i < 1000; i++) {
		unsigned req_flags = (i & 1) ? IOSQE_ASYNC : 0;

		ret = test_single_nop(&ring, req_flags);
		if (ret) {
			fprintf(stderr, "test_single_nop failed\n");
			goto err;
		}

		ret = test_barrier_nop(&ring, req_flags);
		if (ret) {
			fprintf(stderr, "test_barrier_nop failed\n");
			goto err;
		}
	}
err:
	io_uring_queue_exit(&ring);
	return ret;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	FOR_ALL_TEST_CONFIGS {
		ret = test_ring(IORING_GET_TEST_CONFIG_FLAGS());
		if (ret) {
			fprintf(stderr, "Normal ring test failed: %s\n",
					IORING_GET_TEST_CONFIG_DESCRIPTION());
			return ret;
		}
	}

	return 0;
}
