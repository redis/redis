/* SPDX-License-Identifier: MIT */
/*
 * Description: run various statx(2) tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#include "helpers.h"
#include "liburing.h"

#ifdef __NR_statx
static int do_statx(int dfd, const char *path, int flags, unsigned mask,
		    struct statx *statxbuf)
{
	return syscall(__NR_statx, dfd, path, flags, mask, statxbuf);
}
#else
static int do_statx(int dfd, const char *path, int flags, unsigned mask,
		    struct statx *statxbuf)
{
	errno = ENOSYS;
	return -1;
}
#endif

static int statx_syscall_supported(void)
{
	return errno == ENOSYS ? 0 : -1;
}

static int test_statx(struct io_uring *ring, const char *path)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct statx x1 = { }, x2 = { };
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_statx(sqe, -1, path, 0, STATX_ALL, &x1);

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
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret)
		return ret;
	ret = do_statx(-1, path, 0, STATX_ALL, &x2);
	if (ret < 0)
		return statx_syscall_supported();
	if (memcmp(&x1, &x2, sizeof(x1))) {
		fprintf(stderr, "Miscompare between io_uring and statx\n");
		goto err;
	}
	return 0;
err:
	return -1;
}

static int test_statx_fd(struct io_uring *ring, const char *path)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct statx x1, x2;
	int ret, fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	memset(&x1, 0, sizeof(x1));

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_statx(sqe, fd, "", AT_EMPTY_PATH, STATX_ALL, &x1);

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
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret)
		return ret;
	memset(&x2, 0, sizeof(x2));
	ret = do_statx(fd, "", AT_EMPTY_PATH, STATX_ALL, &x2);
	if (ret < 0)
		return statx_syscall_supported();
	if (memcmp(&x1, &x2, sizeof(x1))) {
		fprintf(stderr, "Miscompare between io_uring and statx\n");
		goto err;
	}
	return 0;
err:
	return -1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	const char *fname;
	int ret;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	if (argc > 1) {
		fname = argv[1];
	} else {
		fname = "/tmp/.statx";
		t_create_file(fname, 4096);
	}

	ret = test_statx(&ring, fname);
	if (ret) {
		if (ret == -EINVAL) {
			fprintf(stdout, "statx not supported, skipping\n");
			goto done;
		}
		fprintf(stderr, "test_statx failed: %d\n", ret);
		goto err;
	}

	ret = test_statx_fd(&ring, fname);
	if (ret) {
		fprintf(stderr, "test_statx_fd failed: %d\n", ret);
		goto err;
	}
done:
	if (fname != argv[1])
		unlink(fname);
	return 0;
err:
	if (fname != argv[1])
		unlink(fname);
	return 1;
}
