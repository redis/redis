/* SPDX-License-Identifier: MIT */
/*
 * Description: test SQPOLL with IORING_SETUP_ATTACH_WQ and closing of
 * the original ring descriptor.
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/resource.h>

#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE	(128 * 1024 * 1024)
#define BS		4096
#define BUFFERS		64

#define NR_RINGS	4

static struct iovec *vecs;
static struct io_uring rings[NR_RINGS];

static int wait_io(struct io_uring *ring, int nr_ios)
{
	struct io_uring_cqe *cqe;
	int ret;

	while (nr_ios) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_ret=%d\n", ret);
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

static int do_io(int fd, int ring_start, int ring_end)
{
	int i, rets[NR_RINGS];
	unsigned ios = 0;

	while (ios < 32) {
		for (i = ring_start; i < ring_end; i++) {
			int ret = queue_io(&rings[i], fd, BUFFERS);
			if (ret < 0)
				goto err;
			rets[i] = ret;
		}
		for (i = ring_start; i < ring_end; i++) {
			if (wait_io(&rings[i], rets[i]))
				goto err;
		}
		ios += BUFFERS;
	}

	return 0;
err:
	return 1;
}

static int test(int fd, int do_dup_and_close, int close_ring)
{
	int i, ret, ring_fd;

	for (i = 0; i < NR_RINGS; i++) {
		struct io_uring_params p = { };

		p.flags = IORING_SETUP_SQPOLL;
		p.sq_thread_idle = 100;
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

	/* test all rings */
	if (do_io(fd, 0, NR_RINGS))
		goto err;

	/* dup and close original ring fd */
	ring_fd = dup(rings[0].ring_fd);
	if (close_ring)
		close(rings[0].ring_fd);
	rings[0].ring_fd = rings[0].enter_ring_fd = ring_fd;
	if (do_dup_and_close)
		goto done;

	/* test all but closed one */
	if (do_io(fd, 1, NR_RINGS))
		goto err;

	/* test closed one */
	if (do_io(fd, 0, 1))
		goto err;

	/* make sure thread is idle so we enter the kernel */
	usleep(200000);

	/* test closed one */
	if (do_io(fd, 0, 1))
		goto err;


done:
	for (i = 0; i < NR_RINGS; i++)
		io_uring_queue_exit(&rings[i]);

	return 0;
err:
	return 1;
}

int main(int argc, char *argv[])
{
	char *fname;
	int ret, fd;

	if (argc > 1) {
		fname = argv[1];
	} else {
		fname = ".basic-rw-poll-dup";
		t_create_file(fname, FILE_SIZE);
	}

	vecs = t_create_buffers(BUFFERS, BS);

	fd = open(fname, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		int __e = errno;

		if (fname != argv[1])
			unlink(fname);

		if (__e == EINVAL)
			return T_EXIT_SKIP;
		perror("open");
		return -1;
	}

	if (fname != argv[1])
		unlink(fname);

	ret = test(fd, 0, 0);
	if (ret) {
		fprintf(stderr, "test 0 0 failed\n");
		goto err;
	}

	ret = test(fd, 0, 1);
	if (ret) {
		fprintf(stderr, "test 0 1 failed\n");
		goto err;
	}


	ret = test(fd, 1, 0);
	if (ret) {
		fprintf(stderr, "test 1 0 failed\n");
		goto err;
	}

	return 0;
err:
	return 1;
}
