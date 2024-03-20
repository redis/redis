/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring symlinkat handling
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "liburing.h"


static int do_symlinkat(struct io_uring *ring, const char *oldname, const char *newname)
{
	int ret;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "sqe get failed\n");
		goto err;
	}
	io_uring_prep_symlinkat(sqe, oldname, AT_FDCWD, newname);

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

static int test_link_contents(const char* linkname,
			      const char *expected_contents)
{
	char buf[128];
	int ret = readlink(linkname, buf, 127);
	if (ret < 0) {
		perror("readlink");
		return ret;
	}
	buf[ret] = 0;
	if (strncmp(buf, expected_contents, 128)) {
		fprintf(stderr, "link contents differs from expected: '%s' vs '%s'",
			buf, expected_contents);
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	static const char target[] = "io_uring-symlinkat-test-target";
	static const char linkname[] = "io_uring-symlinkat-test-link";
	int ret;
	struct io_uring ring;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		return ret;
	}

	ret = do_symlinkat(&ring, target, linkname);
	if (ret < 0) {
		if (ret == -EBADF || ret == -EINVAL) {
			fprintf(stdout, "symlinkat not supported, skipping\n");
			goto out;
		}
		fprintf(stderr, "symlinkat: %s\n", strerror(-ret));
		goto err;
	} else if (ret) {
		goto err;
	}

	ret = test_link_contents(linkname, target);
	if (ret < 0)
		goto err1;

	ret = do_symlinkat(&ring, target, linkname);
	if (ret != -EEXIST) {
		fprintf(stderr, "test_symlinkat linkname already exists failed: %d\n", ret);
		goto err1;
	}

	ret = do_symlinkat(&ring, target, "surely/this/does/not/exist");
	if (ret != -ENOENT) {
		fprintf(stderr, "test_symlinkat no parent failed: %d\n", ret);
		goto err1;
	}

out:
	unlinkat(AT_FDCWD, linkname, 0);
	io_uring_queue_exit(&ring);
	return 0;
err1:
	unlinkat(AT_FDCWD, linkname, 0);
err:
	io_uring_queue_exit(&ring);
	return 1;
}
