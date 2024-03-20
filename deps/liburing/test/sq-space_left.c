/* SPDX-License-Identifier: MIT */
/*
 * Description: test SQ queue space left
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"

static int test_left(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int ret, i = 0, s;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;

	}

	if ((s = io_uring_sq_space_left(&ring)) != 8) {
		fprintf(stderr, "Got %d SQEs left, expected %d\n", s, 8);
		goto err;
	}

	i = 0;
	while ((sqe = io_uring_get_sqe(&ring)) != NULL) {
		i++;
		if ((s = io_uring_sq_space_left(&ring)) != 8 - i) {
			fprintf(stderr, "Got %d SQEs left, expected %d\n", s, 8 - i);
			goto err;
		}
	}

	if (i != 8) {
		fprintf(stderr, "Got %d SQEs, expected %d\n", i, 8);
		goto err;
	}

	io_uring_queue_exit(&ring);
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;
}

static int test_sync(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int ret, i;

	ret = io_uring_queue_init(32, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;

	}

	/* prep 8 NOPS */
	for (i = 0; i < 8; i++) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}
		io_uring_prep_nop(sqe);
	}

	/* prep known bad command, this should terminate submission */
	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->opcode = 0xfe;

	/* prep 8 NOPS */
	for (i = 0; i < 8; i++) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}
		io_uring_prep_nop(sqe);
	}

	/* we should have 8 + 1 + 8 pending now */
	ret = io_uring_sq_ready(&ring);
	if (ret != 17) {
		fprintf(stderr, "%d ready, wanted 17\n", ret);
		goto err;
	}

	ret = io_uring_submit(&ring);

	/* should submit 8 successfully, then error #9 and stop */
	if (ret != 9) {
		fprintf(stderr, "submitted %d, wanted 9\n", ret);
		goto err;
	}

	/* should now have 8 ready, with 9 gone */
	ret = io_uring_sq_ready(&ring);
	if (ret != 8) {
		fprintf(stderr, "%d ready, wanted 8\n", ret);
		goto err;
	}

	ret = io_uring_submit(&ring);

	/* the last 8 should submit fine */
	if (ret != 8) {
		fprintf(stderr, "submitted %d, wanted 8\n", ret);
		goto err;
	}

	ret = io_uring_sq_ready(&ring);
	if (ret) {
		fprintf(stderr, "%d ready, wanted 0\n", ret);
		goto err;
	}

	io_uring_queue_exit(&ring);
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_left();
	if (ret) {
		fprintf(stderr, "test_left failed\n");
		return ret;
	}

	ret = test_sync();
	if (ret) {
		fprintf(stderr, "test_sync failed\n");
		return ret;
	}

	return 0;
}
