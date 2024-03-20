/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring mkdirat handling
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "liburing.h"
#include "helpers.h"

static int do_mkdirat(struct io_uring *ring, const char *fn)
{
	int ret;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "sqe get failed\n");
		goto err;
	}
	io_uring_prep_mkdirat(sqe, AT_FDCWD, fn, 0700);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqes(ring, &cqe, 1, 0, 0);
	if (ret) {
		fprintf(stderr, "wait_cqe failed: %d\n", ret);
		goto err;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
err:
	return 1;
}

static int stat_file(const char *fn)
{
	struct stat sb;

	if (!stat(fn, &sb))
		return 0;

	return errno;
}

int main(int argc, char *argv[])
{
	static const char fn[] = "io_uring-mkdirat-test";
	int ret;
	struct io_uring ring;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		return ret;
	}

	ret = do_mkdirat(&ring, fn);
	if (ret < 0) {
		if (ret == -EBADF || ret == -EINVAL) {
			fprintf(stdout, "mkdirat not supported, skipping\n");
			goto skip;
		}
		fprintf(stderr, "mkdirat: %s\n", strerror(-ret));
		goto err;
	} else if (ret) {
		goto err;
	}

	if (stat_file(fn)) {
		perror("stat");
		goto err;
	}

	ret = do_mkdirat(&ring, fn);
	if (ret != -EEXIST) {
		fprintf(stderr, "do_mkdirat already exists failed: %d\n", ret);
		goto err1;
	}

	ret = do_mkdirat(&ring, "surely/this/wont/exist");
	if (ret != -ENOENT) {
		fprintf(stderr, "do_mkdirat no parent failed: %d\n", ret);
		goto err1;
	}

	unlinkat(AT_FDCWD, fn, AT_REMOVEDIR);
	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
skip:
	unlinkat(AT_FDCWD, fn, AT_REMOVEDIR);
	io_uring_queue_exit(&ring);
	return T_EXIT_SKIP;
err1:
	unlinkat(AT_FDCWD, fn, AT_REMOVEDIR);
err:
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}
