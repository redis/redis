/* SPDX-License-Identifier: MIT */
/*
 * Description: test a mem leak with IOPOLL
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE	(128 * 1024)
#define BS		4096
#define BUFFERS		(FILE_SIZE / BS)

static int do_iopoll(const char *fname)
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	struct iovec *iov;
	int fd;

	fd = open(fname, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return T_EXIT_SKIP;
	}

	iov = t_create_buffers(1, 4096);

	t_create_ring(2, &ring, IORING_SETUP_IOPOLL);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, fd, iov->iov_base, iov->iov_len, 0);
	io_uring_submit(&ring);

	close(fd);
	return T_EXIT_PASS;
}

static int test(const char *fname)
{
	if (fork()) {
		int stat;

		wait(&stat);
		return WEXITSTATUS(stat);
	} else {
		int ret;

		ret = do_iopoll(fname);
		exit(ret);
	}
}

int main(int argc, char *argv[])
{
	char buf[256];
	char *fname;
	int i, ret;

	if (argc > 1) {
		fname = argv[1];
	} else {
		srand((unsigned)time(NULL));
		snprintf(buf, sizeof(buf), ".iopoll-leak-%u-%u",
			(unsigned)rand(), (unsigned)getpid());
		fname = buf;
		t_create_file(fname, FILE_SIZE);
	}

	for (i = 0; i < 16; i++) {
		ret = test(fname);
		if (ret == T_EXIT_SKIP || ret == T_EXIT_FAIL)
			break;
	}

	if (fname != argv[1])
		unlink(fname);
	return ret;
}
