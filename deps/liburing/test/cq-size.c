/* SPDX-License-Identifier: MIT */
/*
 * Description: test CQ ring sizing
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring_params p;
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_CQSIZE;
	p.cq_entries = 64;

	ret = io_uring_queue_init_params(4, &ring, &p);
	if (ret) {
		if (ret == -EINVAL) {
			printf("Skipped, not supported on this kernel\n");
			goto done;
		}
		printf("ring setup failed\n");
		return T_EXIT_FAIL;
	}

	if (p.cq_entries < 64) {
		printf("cq entries invalid (%d)\n", p.cq_entries);
		goto err;
	}
	io_uring_queue_exit(&ring);

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_CQSIZE;
	p.cq_entries = 0;

	ret = io_uring_queue_init_params(4, &ring, &p);
	if (ret >= 0) {
		printf("zero sized cq ring succeeded\n");
		io_uring_queue_exit(&ring);
		goto err;
	}

	if (ret != -EINVAL) {
		printf("io_uring_queue_init_params failed, but not with -EINVAL"
		       ", returned error %d (%s)\n", ret, strerror(-ret));
		goto err;
	}

done:
	return T_EXIT_PASS;
err:
	return T_EXIT_FAIL;
}
