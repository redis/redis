/* SPDX-License-Identifier: MIT */
/*
 * Test case for SQPOLL missing a 'ret' clear in case of busy.
 *
 * Heavily based on a test case from
 * Xiaoguang Wang <xiaoguang.wang@linux.alibaba.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE	(128 * 1024)

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int i, fd, ret;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct iovec *iovecs;
	struct io_uring_params p;
	char *fname;
	void *buf;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SQPOLL;
	ret = t_create_ring_params(16, &ring, &p);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_SKIP;
	else if (ret < 0)
		return T_EXIT_FAIL;

	if (argc > 1) {
		fname = argv[1];
	} else {
		fname = ".sqpoll.tmp";
		t_create_file(fname, FILE_SIZE);
	}

	fd = open(fname, O_RDONLY | O_DIRECT);
	if (fname != argv[1])
		unlink(fname);
	if (fd < 0) {
		perror("open");
		goto out;
	}

	iovecs = t_calloc(10, sizeof(struct iovec));
	for (i = 0; i < 10; i++) {
		t_posix_memalign(&buf, 4096, 4096);
		iovecs[i].iov_base = buf;
		iovecs[i].iov_len = 4096;
	}

	ret = io_uring_register_files(&ring, &fd, 1);
	if (ret < 0) {
		fprintf(stderr, "register files %d\n", ret);
		goto out;
	}

	for (i = 0; i < 10; i++) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe)
			break;

		io_uring_prep_readv(sqe, 0, &iovecs[i], 1, 0);
		sqe->flags |= IOSQE_FIXED_FILE;

		ret = io_uring_submit(&ring);
		usleep(1000);
	}

	for (i = 0; i < 10; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			break;
		}
		if (cqe->res != 4096) {
			fprintf(stderr, "ret=%d, wanted 4096\n", cqe->res);
			ret = 1;
			break;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	close(fd);
out:
	io_uring_queue_exit(&ring);
	return ret;
}
