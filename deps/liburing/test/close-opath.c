// SPDX-License-Identifier: MIT

#define _GNU_SOURCE 1
#define _FILE_OFFSET_BITS 64

// Test program for io_uring IORING_OP_CLOSE with O_PATH file.
// Author: Clayton Harris <bugs@claycon.org>, 2020-06-07

// linux                5.6.14-300.fc32.x86_64
// gcc                  10.1.1-1.fc32
// liburing.x86_64      0.5-1.fc32

// gcc -O2 -Wall -Wextra -std=c11 -o close_opath close_opath.c -luring
// ./close_opath testfilepath

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "liburing.h"

typedef struct
{
	const char *const flnames;
	const int oflags;
} oflgs_t;

static int test_io_uring_close(struct io_uring *ring, int fd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "io_uring_get_sqe() failed\n");
		return -ENOENT;
	}

	io_uring_prep_close(sqe, fd);

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "io_uring_submit() failed, errno %d: %s\n",
			-ret, strerror(-ret));
		return ret;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "io_uring_wait_cqe() failed, errno %d: %s\n",
			-ret, strerror(-ret));
		return ret;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);

	if (ret < 0 && ret != -EOPNOTSUPP && ret != -EINVAL && ret != -EBADF) {
		fprintf(stderr, "io_uring close() failed, errno %d: %s\n",
			-ret, strerror(-ret));
		return ret;
	}

	return 0;
}

static int open_file(const char *path, const oflgs_t *oflgs)
{
	int fd;

	fd = openat(AT_FDCWD, path, oflgs->oflags, 0);
	if (fd < 0) {
		int err = errno;
		fprintf(stderr, "openat(%s, %s) failed, errno %d: %s\n",
			path, oflgs->flnames, err, strerror(err));
		return -err;
	}

	return fd;
}

int main(int argc, char *argv[])
{
	const char *fname = ".";
	struct io_uring ring;
	int ret, i;
	static const oflgs_t oflgs[] = {
		{ "O_RDONLY", O_RDONLY },
		{ "O_PATH", O_PATH }
	};

	ret = io_uring_queue_init(2, &ring, 0);
	if (ret < 0) {
		fprintf(stderr, "io_uring_queue_init() failed, errno %d: %s\n",
			-ret, strerror(-ret));
		return 0x02;
	}

#define OFLGS_SIZE (sizeof(oflgs) / sizeof(oflgs[0]))

	ret = 0;
	for (i = 0; i < OFLGS_SIZE; i++) {
		int fd;

		fd = open_file(fname, &oflgs[i]);
		if (fd < 0) {
			ret |= 0x02;
			break;
		}

		/* Should always succeed */
		if (test_io_uring_close(&ring, fd) < 0)
			ret |= 0x04 << i;
	}
#undef OFLGS_SIZE

	io_uring_queue_exit(&ring);
	return ret;
}
