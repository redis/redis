/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring fallocate
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>

#include "liburing.h"
#include "helpers.h"

static int no_fallocate;

static int test_fallocate_rlimit(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct rlimit rlim;
	char buf[32];
	int fd, ret;

	if (getrlimit(RLIMIT_FSIZE, &rlim) < 0) {
		perror("getrlimit");
		return 1;
	}
	rlim.rlim_cur = 64 * 1024;
	rlim.rlim_max = 64 * 1024;
	if (setrlimit(RLIMIT_FSIZE, &rlim) < 0) {
		perror("setrlimit");
		return 1;
	}

	sprintf(buf, "./XXXXXX");
	fd = mkstemp(buf);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	unlink(buf);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_fallocate(sqe, fd, 0, 0, 128*1024);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}

	if (cqe->res == -EINVAL) {
		fprintf(stdout, "Fallocate not supported, skipping\n");
		no_fallocate = 1;
		goto skip;
	} else if (cqe->res != -EFBIG) {
		fprintf(stderr, "Expected -EFBIG: %d\n", cqe->res);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);
	return 0;
skip:
	return T_EXIT_SKIP;
err:
	return 1;
}

static int test_fallocate(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct stat st;
	char buf[32];
	int fd, ret;

	sprintf(buf, "./XXXXXX");
	fd = mkstemp(buf);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	unlink(buf);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_fallocate(sqe, fd, 0, 0, 128*1024);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}

	if (cqe->res == -EINVAL) {
		fprintf(stdout, "Fallocate not supported, skipping\n");
		no_fallocate = 1;
		goto skip;
	}
	if (cqe->res) {
		fprintf(stderr, "cqe->res=%d\n", cqe->res);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);

	if (fstat(fd, &st) < 0) {
		perror("stat");
		goto err;
	}

	if (st.st_size != 128*1024) {
		fprintf(stderr, "Size mismatch: %llu\n",
					(unsigned long long) st.st_size);
		goto err;
	}

	return 0;
skip:
	return T_EXIT_SKIP;
err:
	return 1;
}

static int test_fallocate_fsync(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct stat st;
	char buf[32];
	int fd, ret, i;

	if (no_fallocate)
		return 0;

	sprintf(buf, "./XXXXXX");
	fd = mkstemp(buf);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	unlink(buf);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_fallocate(sqe, fd, 0, 0, 128*1024);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_fsync(sqe, fd, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (cqe->res) {
			fprintf(stderr, "cqe->res=%d,data=%" PRIu64 "\n", cqe->res,
							(uint64_t) cqe->user_data);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	if (fstat(fd, &st) < 0) {
		perror("stat");
		goto err;
	}

	if (st.st_size != 128*1024) {
		fprintf(stderr, "Size mismatch: %llu\n",
					(unsigned long long) st.st_size);
		goto err;
	}

	return 0;
err:
	return 1;
}

static void sig_xfsz(int sig)
{
}

int main(int argc, char *argv[])
{
	struct sigaction act = { };
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	act.sa_handler = sig_xfsz;
	sigaction(SIGXFSZ, &act, NULL);

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_fallocate(&ring);
	if (ret) {
		if (ret != T_EXIT_SKIP) {
			fprintf(stderr, "test_fallocate failed\n");
		}
		return ret;
	}

	ret = test_fallocate_fsync(&ring);
	if (ret) {
		fprintf(stderr, "test_fallocate_fsync failed\n");
		return ret;
	}

	ret = test_fallocate_rlimit(&ring);
	if (ret) {
		if (ret != T_EXIT_SKIP) {
			fprintf(stderr, "test_fallocate_rlimit failed\n");
		}
		return ret;
	}

	return T_EXIT_PASS;
}
