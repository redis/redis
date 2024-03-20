/* SPDX-License-Identifier: MIT */
/*
 * Regression test for incorrect async_list io_should_merge() logic
 * Bug was fixed in 5.5 by (commit: 561fb04 io_uring: replace workqueue usage with io-wq")
 * Affects 5.4 lts branch, at least 5.4.106 is affected.
 */
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, fd, pipe1[2];
	char buf[4096];
	struct iovec vec = {
		.iov_base = buf,
		.iov_len = sizeof(buf)
	};
	struct __kernel_timespec ts = {.tv_sec = 3, .tv_nsec = 0};

	if (argc > 1)
		return 0;

	ret = pipe(pipe1);
	assert(!ret);

	fd = open("testfile", O_RDWR | O_CREAT, 0644);
	assert(fd >= 0);
	unlink("testfile");
	ret = ftruncate(fd, 4096);
	assert(!ret);

	ret = t_create_ring(4, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret < 0)
		return 1;

	/* REQ1 */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_readv(sqe, pipe1[0], &vec, 1, 0);
	sqe->user_data = 1;

	/* REQ2 */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_readv(sqe, fd, &vec, 1, 4096);
	sqe->user_data = 2;

	ret = io_uring_submit(&ring);
	assert(ret == 2);

	ret = io_uring_wait_cqe(&ring, &cqe);
	assert(!ret);
	assert(cqe->res == 0);
	assert(cqe->user_data == 2);
	io_uring_cqe_seen(&ring, cqe);

	/*
	 * REQ3
	 * Prepare request adjacent to previous one, so merge logic may want to
	 * link it to previous request, but because of a bug in merge logic
	 * it may be merged with <REQ1> request
	 */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_readv(sqe, fd, &vec, 1, 2048);
	sqe->user_data = 3;

	ret = io_uring_submit(&ring);
	assert(ret == 1);

	/*
	 * Read may stuck because of bug there request was be incorrectly
	 * merged with <REQ1> request
	 */
	ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
	if (ret == -ETIME) {
		printf("TEST_FAIL: readv req3 stuck\n");
		return 1;
	}
	assert(!ret);

	assert(cqe->res == 2048);
	assert(cqe->user_data == 3);

	io_uring_cqe_seen(&ring, cqe);
	io_uring_queue_exit(&ring);
	return 0;
}
