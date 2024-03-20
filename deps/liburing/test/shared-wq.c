/* SPDX-License-Identifier: MIT */
/*
 * Description: test wq sharing
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"

static int test_attach_invalid(int ringfd)
{
	struct io_uring_params p;
	struct io_uring ring;
	int ret;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_ATTACH_WQ;
	p.wq_fd = ringfd;
	ret = io_uring_queue_init_params(1, &ring, &p);
	if (ret != -EINVAL) {
		fprintf(stderr, "Attach to zero: %d\n", ret);
		goto err;
	}
	return 0;
err:
	return 1;
}

static int test_attach(int ringfd)
{
	struct io_uring_params p;
	struct io_uring ring2;
	int ret;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_ATTACH_WQ;
	p.wq_fd = ringfd;
	ret = io_uring_queue_init_params(1, &ring2, &p);
	if (ret == -EINVAL) {
		fprintf(stdout, "Sharing not supported, skipping\n");
		return 0;
	} else if (ret) {
		fprintf(stderr, "Attach to id: %d\n", ret);
		goto err;
	}
	io_uring_queue_exit(&ring2);
	return 0;
err:
	return 1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	/* stdout is definitely not an io_uring descriptor */
	ret = test_attach_invalid(2);
	if (ret) {
		fprintf(stderr, "test_attach_invalid failed\n");
		return ret;
	}

	ret = test_attach(ring.ring_fd);
	if (ret) {
		fprintf(stderr, "test_attach failed\n");
		return ret;
	}

	return 0;
}
