/* SPDX-License-Identifier: MIT */
/*
 * Description: IOPOLL with overflow test case
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include "helpers.h"
#include "liburing.h"
#include "../src/syscall.h"

#define FILE_SIZE	(128 * 1024)
#define BS		4096
#define BUFFERS		(FILE_SIZE / BS)

static struct iovec *vecs;

static int test(struct io_uring *ring, int fd)
{
	struct io_uring_sqe *sqe;
	int i, j, ret;
	loff_t off;

	off = FILE_SIZE - BS;
	for (j = 0; j < 8; j++) {
		for (i = 0; i < BUFFERS; i++) {
			sqe = io_uring_get_sqe(ring);
			io_uring_prep_read(sqe, fd, vecs[i].iov_base,
						vecs[i].iov_len, off);
			if (!off)
				off = FILE_SIZE - BS;
			else
				off -= BS;
		}
		ret = io_uring_submit(ring);
		if (ret != BUFFERS) {
			fprintf(stderr, "submitted %d\n", ret);
			return T_EXIT_FAIL;
		}
	}

	sleep(1);

	ret = __sys_io_uring_enter(ring->ring_fd, 0, BUFFERS * 8,
					IORING_ENTER_GETEVENTS, NULL);

	for (i = 0; i < BUFFERS * 8; i++) {
		struct io_uring_cqe *cqe;

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait=%d\n", ret);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	struct io_uring_params p = { };
	struct io_uring ring;
	char buf[256];
	char *fname;
	int ret, fd;

	p.flags = IORING_SETUP_IOPOLL | IORING_SETUP_CQSIZE;
	p.cq_entries = 64;
	ret = t_create_ring_params(64, &ring, &p);
	if (ret == T_SETUP_SKIP)
		return 0;
	if (ret != T_SETUP_OK) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	if (argc > 1) {
		fname = argv[1];
	} else {
		srand((unsigned)time(NULL));
		snprintf(buf, sizeof(buf), ".basic-rw-%u-%u",
			(unsigned)rand(), (unsigned)getpid());
		fname = buf;
		t_create_file(fname, FILE_SIZE);
	}

	fd = open(fname, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		if (errno == EINVAL) {
			if (fname != argv[1])
				unlink(fname);
			return T_EXIT_SKIP;
		}
		perror("open");
		goto err;
	}

	vecs = t_create_buffers(BUFFERS, BS);

	ret = test(&ring, fd);

	if (fname != argv[1])
		unlink(fname);
	return ret;
err:
	if (fname != argv[1])
		unlink(fname);
	return T_EXIT_FAIL;
}
