/* SPDX-License-Identifier: MIT */
/*
 * Test that we don't recursively generate completion events if an io_uring
 * fd is added to an epoll context, and the ring itself polls for events on
 * the epollfd. Older kernels will stop on overflow, newer kernels will
 * detect this earlier and abort correctly.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <poll.h>
#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct epoll_event ev = { };
	int epollfd, ret, i;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "Ring init failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	epollfd = epoll_create1(0);
	if (epollfd < 0) {
		perror("epoll_create");
		return T_EXIT_FAIL;
	}

	ev.events = EPOLLIN;
	ev.data.fd = ring.ring_fd;
	ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, ring.ring_fd, &ev);
	if (ret < 0) {
		perror("epoll_ctl");
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_multishot(sqe, epollfd, POLLIN);
	sqe->user_data = 1;
	io_uring_submit(&ring);

	sqe = io_uring_get_sqe(&ring);
	sqe->user_data = 2;
	io_uring_prep_nop(sqe);
	io_uring_submit(&ring);

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe ret = %d\n", ret);
			break;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	ret = io_uring_peek_cqe(&ring, &cqe);
	if (!ret) {
		fprintf(stderr, "Generated too many events\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
