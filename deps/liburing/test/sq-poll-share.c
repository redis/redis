/* SPDX-License-Identifier: MIT */
/*
 * Description: test SQPOLL with IORING_SETUP_ATTACH_WQ
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

#define FILE_SIZE	(128 * 1024 * 1024)
#define BS		4096
#define BUFFERS		64

#define NR_RINGS	4

static struct iovec *vecs;

static int wait_io(struct io_uring *ring, int nr_ios)
{
	struct io_uring_cqe *cqe;

	while (nr_ios) {
		int ret = io_uring_wait_cqe(ring, &cqe);

		if (ret == -EAGAIN) {
			continue;
		} else if (ret) {
			fprintf(stderr, "io_uring_wait_cqe failed %i\n", ret);
			return 1;
		}
		if (cqe->res != BS) {
			fprintf(stderr, "Unexpected ret %d\n", cqe->res);
			return 1;
		}
		io_uring_cqe_seen(ring, cqe);
		nr_ios--;
	}

	return 0;
}

static int queue_io(struct io_uring *ring, int fd, int nr_ios)
{
	unsigned long off;
	int i;

	i = 0;
	off = 0;
	while (nr_ios) {
		struct io_uring_sqe *sqe;

		sqe = io_uring_get_sqe(ring);
		if (!sqe)
			break;
		io_uring_prep_read(sqe, fd, vecs[i].iov_base, vecs[i].iov_len, off);
		nr_ios--;
		i++;
		off += BS;
	}

	io_uring_submit(ring);
	return i;
}

int main(int argc, char *argv[])
{
	struct io_uring rings[NR_RINGS];
	int rets[NR_RINGS];
	unsigned long ios;
	int i, ret, fd;
	char *fname;

	if (argc > 1) {
		fname = argv[1];
	} else {
		fname = ".basic-rw-poll-share";
		t_create_file(fname, FILE_SIZE);
	}

	vecs = t_create_buffers(BUFFERS, BS);

	fd = open(fname, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	if (fname != argv[1])
		unlink(fname);

	for (i = 0; i < NR_RINGS; i++) {
		struct io_uring_params p = { };

		p.flags = IORING_SETUP_SQPOLL;
		if (i) {
			p.wq_fd = rings[0].ring_fd;
			p.flags |= IORING_SETUP_ATTACH_WQ;
		}
		ret = io_uring_queue_init_params(BUFFERS, &rings[i], &p);
		if (ret) {
			fprintf(stderr, "queue_init: %d/%d\n", ret, i);
			goto err;
		}
		/* no sharing for non-fixed either */
		if (!(p.features & IORING_FEAT_SQPOLL_NONFIXED)) {
			fprintf(stdout, "No SQPOLL sharing, skipping\n");
			return 0;
		}
	}

	ios = 0;
	while (ios < (FILE_SIZE / BS)) {
		for (i = 0; i < NR_RINGS; i++) {
			ret = queue_io(&rings[i], fd, BUFFERS);
			if (ret < 0)
				goto err;
			rets[i] = ret;
		}
		for (i = 0; i < NR_RINGS; i++) {
			if (wait_io(&rings[i], rets[i]))
				goto err;
		}
		ios += BUFFERS;
	}

	return 0;
err:
	return 1;
}
