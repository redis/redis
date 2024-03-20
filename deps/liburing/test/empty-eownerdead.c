/* SPDX-License-Identifier: MIT */
/*
 * Test if entering with nothing to submit/wait for SQPOLL returns an error.
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "liburing.h"
#include "helpers.h"
#include "../src/syscall.h"

int main(int argc, char *argv[])
{
	struct io_uring_params p = {};
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	p.flags = IORING_SETUP_SQPOLL;
	p.sq_thread_idle = 100;

	ret = t_create_ring_params(1, &ring, &p);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_SKIP;
	else if (ret < 0)
		goto err;

	ret = __sys_io_uring_enter(ring.ring_fd, 0, 0, 0, NULL);
	if (ret < 0) {
		int __e = errno;

		if (__e == EOWNERDEAD)
			fprintf(stderr, "sqe submit unexpected failure due old kernel bug: %s\n", strerror(__e));
		else
			fprintf(stderr, "sqe submit unexpected failure: %s\n", strerror(__e));
		goto err;
	}

	return T_EXIT_PASS;
err:
	return T_EXIT_FAIL;
}
