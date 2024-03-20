/* SPDX-License-Identifier: MIT */
/*
 * io_uring_enter.c
 *
 * Description: Unit tests for the io_uring_enter system call.
 *
 * Copyright 2019, Red Hat, Inc.
 * Author: Jeff Moyer <jmoyer@redhat.com>
 */
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/sysinfo.h>
#include <poll.h>
#include <assert.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <linux/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>
#include <sys/time.h>

#include "helpers.h"
#include "liburing.h"
#include "liburing/barrier.h"
#include "../src/syscall.h"

#define IORING_MAX_ENTRIES 4096
#define IORING_MAX_ENTRIES_FALLBACK 128

static int expect_fail(int fd, unsigned int to_submit,
		       unsigned int min_complete, unsigned int flags,
		       sigset_t *sig, int error)
{
	int ret;

	ret = io_uring_enter(fd, to_submit, min_complete, flags, sig);
	if (ret >= 0) {
		fprintf(stderr, "expected %s, but call succeeded\n", strerror(-error));
		return 1;
	}

	if (ret != error) {
		fprintf(stderr, "expected %d, got %d\n", error, ret);
		return 1;
	}

	return 0;
}

static int try_io_uring_enter(int fd, unsigned int to_submit,
			      unsigned int min_complete, unsigned int flags,
			      sigset_t *sig, int expect)
{
	int ret;

	if (expect < 0)
		return expect_fail(fd, to_submit, min_complete, flags, sig,
				   expect);

	ret = io_uring_enter(fd, to_submit, min_complete, flags, sig);
	if (ret != expect) {
		fprintf(stderr, "Expected %d, got %d\n", expect, ret);
		return 1;
	}

	return 0;
}

/*
 * prep a read I/O.  index is treated like a block number.
 */
static int setup_file(char *template, off_t len)
{
	int fd, ret;
	char buf[4096];

	fd = mkstemp(template);
	if (fd < 0) {
		perror("mkstemp");
		exit(1);
	}
	ret = ftruncate(fd, len);
	if (ret < 0) {
		perror("ftruncate");
		exit(1);
	}

	ret = read(fd, buf, 4096);
	if (ret != 4096) {
		fprintf(stderr, "read returned %d, expected 4096\n", ret);
		exit(1);
	}

	return fd;
}

static void io_prep_read(struct io_uring_sqe *sqe, int fd, off_t offset,
			 size_t len)
{
	struct iovec *iov;

	iov = t_malloc(sizeof(*iov));
	assert(iov);

	iov->iov_base = t_malloc(len);
	assert(iov->iov_base);
	iov->iov_len = len;

	io_uring_prep_readv(sqe, fd, iov, 1, offset);
	io_uring_sqe_set_data(sqe, iov); // free on completion
}

static void reap_events(struct io_uring *ring, unsigned nr)
{
	int ret;
	unsigned left = nr;
	struct io_uring_cqe *cqe;
	struct iovec *iov;
	struct timeval start, now, elapsed;

	gettimeofday(&start, NULL);
	while (left) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "io_uring_wait_cqe returned %d\n", ret);
			exit(1);
		}
		if (cqe->res != 4096)
			fprintf(stderr, "cqe->res: %d, expected 4096\n", cqe->res);
		iov = io_uring_cqe_get_data(cqe);
		free(iov->iov_base);
		free(iov);
		left--;
		io_uring_cqe_seen(ring, cqe);

		gettimeofday(&now, NULL);
		timersub(&now, &start, &elapsed);
		if (elapsed.tv_sec > 10) {
			fprintf(stderr, "Timed out waiting for I/Os to complete.\n");
			fprintf(stderr, "%u expected, %u completed\n", nr, left);
			break;
		}
	}
}

static void submit_io(struct io_uring *ring, unsigned nr)
{
	int fd, ret;
	off_t file_len;
	unsigned i;
	static char template[32] = "/tmp/io_uring_enter-test.XXXXXX";
	struct io_uring_sqe *sqe;

	file_len = nr * 4096;
	fd = setup_file(template, file_len);
	for (i = 0; i < nr; i++) {
		/* allocate an sqe */
		sqe = io_uring_get_sqe(ring);
		/* fill it in */
		io_prep_read(sqe, fd, i * 4096, 4096);
	}

	/* submit the I/Os */
	ret = io_uring_submit(ring);
	unlink(template);
	if (ret < 0) {
		perror("io_uring_enter");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int ret;
	unsigned int status = 0;
	struct io_uring ring;
	struct io_uring_sq *sq = &ring.sq;
	unsigned ktail, mask, index;
	unsigned sq_entries;
	unsigned completed, dropped;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(IORING_MAX_ENTRIES, &ring, 0);
	if (ret == -ENOMEM)
		ret = io_uring_queue_init(IORING_MAX_ENTRIES_FALLBACK, &ring, 0);
	if (ret < 0) {
		perror("io_uring_queue_init");
		exit(T_EXIT_FAIL);
	}
	mask = sq->ring_mask;

	/* invalid flags */
	status |= try_io_uring_enter(ring.ring_fd, 1, 0, ~0U, NULL, -EINVAL);

	/* invalid fd, EBADF */
	status |= try_io_uring_enter(-1, 0, 0, 0, NULL, -EBADF);

	/* valid, non-ring fd, EOPNOTSUPP */
	status |= try_io_uring_enter(0, 0, 0, 0, NULL, -EOPNOTSUPP);

	/* to_submit: 0, flags: 0;  should get back 0. */
	status |= try_io_uring_enter(ring.ring_fd, 0, 0, 0, NULL, 0);

	/* fill the sq ring */
	sq_entries = ring.sq.ring_entries;
	submit_io(&ring, sq_entries);
	ret = io_uring_enter(ring.ring_fd, 0, sq_entries,
			     IORING_ENTER_GETEVENTS, NULL);
	if (ret < 0) {
		fprintf(stderr, "io_uring_enter: %s\n", strerror(-ret));
		status = 1;
	} else {
		/*
		 * This is a non-IOPOLL ring, which means that io_uring_enter
		 * should not return until min_complete events are available
		 * in the completion queue.
		 */
		completed = *ring.cq.ktail - *ring.cq.khead;
		if (completed != sq_entries) {
			fprintf(stderr, "Submitted %u I/Os, but only got %u completions\n",
			       sq_entries, completed);
			status = 1;
		}
		reap_events(&ring, sq_entries);
	}

	/*
	 * Add an invalid index to the submission queue.  This should
	 * result in the dropped counter increasing.
	 */
	index = sq->ring_entries + 1; // invalid index
	dropped = *sq->kdropped;
	ktail = *sq->ktail;
	sq->array[ktail & mask] = index;
	++ktail;
	/*
	 * Ensure that the kernel sees the SQE update before it sees the tail
	 * update.
	 */
	io_uring_smp_store_release(sq->ktail, ktail);

	ret = io_uring_enter(ring.ring_fd, 1, 0, 0, NULL);
	/* now check to see if our sqe was dropped */
	if (*sq->kdropped == dropped) {
		fprintf(stderr, "dropped counter did not increase\n");
		status = 1;
	}

	if (!status)
		return T_EXIT_PASS;

	fprintf(stderr, "FAIL\n");
	return T_EXIT_FAIL;
}
