/* SPDX-License-Identifier: MIT */
/*
 * Based on a test case from Josef Grieb - test that we can exit without
 * hanging if we have the task file table pinned by a request that is linked
 * to another request that doesn't finish.
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include "liburing.h"
#include "helpers.h"

#define BACKLOG 512

#define PORT 9100

static struct io_uring ring;

static struct __kernel_timespec ts = {
	.tv_sec		= 300,
	.tv_nsec	= 0,
};

static void add_timeout(struct io_uring *ring, int fd)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_timeout(sqe, &ts, 100, 0);
	sqe->flags |= IOSQE_IO_LINK;
}

static void add_accept(struct io_uring *ring, int fd)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_accept(sqe, fd, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
	sqe->flags |= IOSQE_IO_LINK;
}

static int setup_io_uring(void)
{
	int ret;
       
	ret = io_uring_queue_init(16, &ring, 0);
	if (ret) {
		fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
		return 1;
	}

	return 0;
}

static void alarm_sig(int sig)
{
	exit(0);
}

int main(int argc, char *argv[])
{
	struct sockaddr_in serv_addr;
	struct io_uring_cqe *cqe;
	int ret, sock_listen_fd;
	const int val = 1;
	int i;

	if (argc > 1)
		return T_EXIT_SKIP;

	sock_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (sock_listen_fd < 0) {
		perror("socket");
		return T_EXIT_FAIL;
	}

	setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;

	for (i = 0; i < 100; i++) {
		serv_addr.sin_port = htons(PORT + i);

		ret = bind(sock_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
		if (!ret)
			break;
		if (errno != EADDRINUSE) {
			fprintf(stderr, "bind: %s\n", strerror(errno));
			return T_EXIT_FAIL;
		}
		if (i == 99) {
			printf("Gave up on finding a port, skipping\n");
			goto skip;
		}
	}

	if (listen(sock_listen_fd, BACKLOG) < 0) {
		perror("Error listening on socket\n");
		return T_EXIT_FAIL;
	}

	if (setup_io_uring())
		return T_EXIT_FAIL;

	add_timeout(&ring, sock_listen_fd);
	add_accept(&ring, sock_listen_fd);

	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "submit=%d\n", ret);
		return T_EXIT_FAIL;
	}

	signal(SIGALRM, alarm_sig);
	alarm(1);

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe=%d\n", ret);
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
skip:
	io_uring_queue_exit(&ring);
	return T_EXIT_SKIP;
}
