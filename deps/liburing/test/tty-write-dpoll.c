/* SPDX-License-Identifier: MIT */
/*
 * Test double poll tty write. A test case for the regression fixed by:
 *
 * commit 6e295a664efd083ac9a5c1a8130c45be1db0cde7
 * Author: Jens Axboe <axboe@kernel.dk>
 * Date:   Tue Mar 22 13:11:28 2022 -0600
 *
 *   io_uring: fix assuming triggered poll waitqueue is the single poll
 *
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

#define	SQES	128
#define BUFSIZE	512

int main(int argc, char *argv[])
{
	static char buf[BUFSIZE];
	struct iovec vecs[SQES];
	struct io_uring ring;
	int ret, i, fd;

	if (argc > 1)
		return 0;

	fd = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 0;

	ret = t_create_ring(SQES, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret < 0)
		return 1;

	for (i = 0; i < SQES; i++) {
		struct io_uring_sqe *sqe;

		sqe = io_uring_get_sqe(&ring);
		vecs[i].iov_base = buf;
		vecs[i].iov_len = sizeof(buf);
		io_uring_prep_writev(sqe, fd, &vecs[i], 1, 0);
	}

	ret = io_uring_submit(&ring);
	if (ret != SQES) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	return 0;
}
