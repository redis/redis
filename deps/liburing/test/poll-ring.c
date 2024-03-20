/* SPDX-License-Identifier: MIT */
/*
 * Description: Test poll against ring itself. A buggy kernel will end up
 * 		having io_wq_* workers pending, as the circular reference
 * 		will prevent full exit.
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#include "liburing.h"

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "child: ring setup failed: %d\n", ret);
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return 1;
	}

	io_uring_prep_poll_add(sqe, ring.ring_fd, POLLIN);
	io_uring_sqe_set_data(sqe, sqe);

	ret = io_uring_submit(&ring);
	if (ret <= 0) {
		fprintf(stderr, "child: sqe submit failed: %d\n", ret);
		return 1;
	}

	return 0;
}
