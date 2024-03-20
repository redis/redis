/* SPDX-License-Identifier: MIT */
/*
 * Description: check that STDOUT write works
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "helpers.h"
#include "liburing.h"

static int test_pipe_io_fixed(struct io_uring *ring)
{
	const char str[] = "This is a fixed pipe test\n";
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct iovec vecs[2];
	char buffer[128];
	int i, ret, fds[2];

	t_posix_memalign(&vecs[0].iov_base, 4096, 4096);
	memcpy(vecs[0].iov_base, str, strlen(str));
	vecs[0].iov_len = strlen(str);

	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}

	ret = io_uring_register_buffers(ring, vecs, 1);
	if (ret) {
		fprintf(stderr, "Failed to register buffers: %d\n", ret);
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_write_fixed(sqe, fds[1], vecs[0].iov_base,
					vecs[0].iov_len, 0, 0);
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	vecs[1].iov_base = buffer;
	vecs[1].iov_len = sizeof(buffer);
	io_uring_prep_readv(sqe, fds[0], &vecs[1], 1, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	} else if (ret != 2) {
		fprintf(stderr, "Submitted only %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "I/O write error on %lu: %s\n",
					(unsigned long) cqe->user_data,
					 strerror(-cqe->res));
			goto err;
		}
		if (cqe->res != strlen(str)) {
			fprintf(stderr, "Got %d bytes, wanted %d on %lu\n",
					cqe->res, (int)strlen(str),
					(unsigned long) cqe->user_data);
			goto err;
		}
		if (cqe->user_data == 2 && memcmp(str, buffer, strlen(str))) {
			fprintf(stderr, "read data mismatch\n");
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}
	io_uring_unregister_buffers(ring);
	return 0;
err:
	return 1;
}

static int test_stdout_io_fixed(struct io_uring *ring)
{
	const char str[] = "This is a fixed pipe test\n";
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct iovec vecs;
	int ret;

	t_posix_memalign(&vecs.iov_base, 4096, 4096);
	memcpy(vecs.iov_base, str, strlen(str));
	vecs.iov_len = strlen(str);

	ret = io_uring_register_buffers(ring, &vecs, 1);
	if (ret) {
		fprintf(stderr, "Failed to register buffers: %d\n", ret);
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_write_fixed(sqe, STDOUT_FILENO, vecs.iov_base, vecs.iov_len, 0, 0);

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	} else if (ret < 1) {
		fprintf(stderr, "Submitted only %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "STDOUT write error: %s\n", strerror(-cqe->res));
		goto err;
	}
	if (cqe->res != vecs.iov_len) {
		fprintf(stderr, "Got %d write, wanted %d\n", cqe->res, (int)vecs.iov_len);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);
	io_uring_unregister_buffers(ring);
	return 0;
err:
	return 1;
}

static int test_stdout_io(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct iovec vecs;
	int ret;

	vecs.iov_base = "This is a pipe test\n";
	vecs.iov_len = strlen(vecs.iov_base);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_writev(sqe, STDOUT_FILENO, &vecs, 1, 0);

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	} else if (ret < 1) {
		fprintf(stderr, "Submitted only %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "STDOUT write error: %s\n",
				strerror(-cqe->res));
		goto err;
	}
	if (cqe->res != vecs.iov_len) {
		fprintf(stderr, "Got %d write, wanted %d\n", cqe->res,
				(int)vecs.iov_len);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);

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

	ret = test_stdout_io(&ring);
	if (ret) {
		fprintf(stderr, "test_pipe_io failed\n");
		return ret;
	}

	ret = test_stdout_io_fixed(&ring);
	if (ret) {
		fprintf(stderr, "test_pipe_io_fixed failed\n");
		return ret;
	}

	ret = test_pipe_io_fixed(&ring);
	if (ret) {
		fprintf(stderr, "test_pipe_io_fixed failed\n");
		return ret;
	}

	return 0;
}
