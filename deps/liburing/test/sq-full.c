/* SPDX-License-Identifier: MIT */
/*
 * Description: test SQ queue full condition
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int ret, i;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;

	}

	i = 0;
	while ((sqe = io_uring_get_sqe(&ring)) != NULL)
		i++;

	if (i != 8) {
		fprintf(stderr, "Got %d SQEs, wanted 8\n", i);
		goto err;
	}

	io_uring_queue_exit(&ring);
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;
}
