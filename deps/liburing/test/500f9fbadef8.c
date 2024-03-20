/* SPDX-License-Identifier: MIT */
/*
 * Description: Single depth submit+wait poll hang test
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "helpers.h"
#include "liburing.h"

#define BLOCKS	4096

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct iovec iov;
	char buf[32];
	off_t offset;
	unsigned blocks;
	int ret, fd;

	if (argc > 1)
		return T_EXIT_SKIP;

	t_posix_memalign(&iov.iov_base, 4096, 4096);
	iov.iov_len = 4096;

	ret = io_uring_queue_init(1, &ring, IORING_SETUP_IOPOLL);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;

	}

	sprintf(buf, "./XXXXXX");
	fd = mkostemp(buf, O_WRONLY | O_DIRECT | O_CREAT);
	if (fd < 0) {
		if (errno == EINVAL)
			return T_EXIT_SKIP;
		perror("mkostemp");
		return T_EXIT_FAIL;
	}

	offset = 0;
	blocks = BLOCKS;
	do {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}
		io_uring_prep_writev(sqe, fd, &iov, 1, offset);
		ret = io_uring_submit_and_wait(&ring, 1);
		if (ret < 0) {
			fprintf(stderr, "submit_and_wait: %d\n", ret);
			goto err;
		}
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion: %d\n", ret);
			goto err;
		}
		if (cqe->res != 4096) {
			if (cqe->res == -EOPNOTSUPP)
				goto skipped;
			goto err;
		}
		io_uring_cqe_seen(&ring, cqe);
		offset += 4096;
	} while (--blocks);

	close(fd);
	unlink(buf);
	return T_EXIT_PASS;
err:
	close(fd);
	unlink(buf);
	return T_EXIT_FAIL;
skipped:
	fprintf(stderr, "Polling not supported in current dir, test skipped\n");
	close(fd);
	unlink(buf);
	return T_EXIT_SKIP;
}
