/* SPDX-License-Identifier: MIT */
/*
 * Check that repeated IORING_OP_CONNECT to a socket without a listener keeps
 * yielding -ECONNREFUSED rather than -ECONNABORTED. Based on a reproducer
 * from:
 *
 * https://github.com/axboe/liburing/issues/828
 *
 * and adopted to our usual test cases. Other changes made like looping,
 * using different ring types, adding a memset() for reuse, etc.
 *
 */
#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "liburing.h"
#include "helpers.h"

static unsigned long ud;

static int init_test_server(struct sockaddr_in *serv_addr)
{
	socklen_t servaddr_len = sizeof(struct sockaddr_in);
	int fd;

	/* Init server socket. Bind but don't listen */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	serv_addr->sin_family = AF_INET;
	serv_addr->sin_addr.s_addr = inet_addr("127.0.0.1");

	if (bind(fd, (struct sockaddr *) serv_addr, servaddr_len) < 0) {
		perror("bind");
		return -1;
	}

	/*
	 * Get the addresses the socket is bound to because the port is chosen
	 * by the network stack.
	 */
	if (getsockname(fd, (struct sockaddr *)serv_addr, &servaddr_len) < 0) {
		perror("getsockname");
		return -1;
	}

	return fd;
}

static int init_test_client(void)
{
	socklen_t addr_len = sizeof(struct sockaddr_in);
	struct sockaddr_in client_addr = {};
	int clientfd;

	clientfd = socket(AF_INET, SOCK_STREAM, 0);
	if (clientfd < 0) {
		perror("socket");
		return -1;
	}

	client_addr.sin_family = AF_INET;
	client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (bind(clientfd, (struct sockaddr *)&client_addr, addr_len) < 0) {
		perror("bind");
		close(clientfd);
		return -1;
	}

	/*
	 * Get the addresses the socket is bound to because the port is chosen
	 * by the network stack.
	 */
	 if (getsockname(clientfd, (struct sockaddr *)&client_addr, &addr_len) < 0) {
		 perror("getsockname");
		 close(clientfd);
		 return -1;
	 }

	 return clientfd;
}

static int get_completion_and_print(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int ret, res;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait_cqe=%d\n", ret);
		return -1;
	}

	/* Mark this completion as seen */
	res = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return res;
}

static int test_connect(struct io_uring *ring,
			int clientfd, struct sockaddr_in *serv_addr)
{
	struct sockaddr_in local_sa;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_connect(sqe, clientfd, (const struct sockaddr *)serv_addr,
				sizeof(struct sockaddr_in));
	sqe->user_data = ++ud;

	memcpy(&local_sa, serv_addr, sizeof(local_sa));

	ret = io_uring_submit_and_wait(ring, 1);
	if (ret != 1) {
		fprintf(stderr, "submit=%d\n", ret);
		return T_EXIT_FAIL;
	}

	/* check for reuse at the same time */
	memset(&local_sa, 0xff, sizeof(local_sa));

	ret = get_completion_and_print(ring);
	if (ret != -ECONNREFUSED) {
		fprintf(stderr, "Connect got %d\n", ret);
		return T_EXIT_FAIL;
	}
	return T_EXIT_PASS;
}

static int test(int flags)
{
	struct io_uring_params params = { .flags = flags, };
	struct sockaddr_in serv_addr = {};
	struct io_uring ring;
	int ret, clientfd, s_fd, i;

	if (flags & IORING_SETUP_SQPOLL)
		params.sq_thread_idle = 50;

	ret = io_uring_queue_init_params(8, &ring, &params);
	if (ret < 0) {
		fprintf(stderr, "Queue init: %d\n", ret);
		return T_EXIT_FAIL;
	}

	s_fd = init_test_server(&serv_addr);
	if (s_fd < 0)
		return T_EXIT_FAIL;

	clientfd = init_test_client();
	if (clientfd < 0) {
		close(s_fd);
		return T_EXIT_FAIL;
	}

	/* make sure SQPOLL thread is sleeping */
	if (flags & IORING_SETUP_SQPOLL)
		usleep(100000);

	for (i = 0; i < 32; i++) {
		ret = test_connect(&ring, clientfd, &serv_addr);
		if (ret == T_EXIT_SKIP)
			return T_EXIT_SKIP;
		else if (ret == T_EXIT_PASS)
			continue;

		return T_EXIT_FAIL;
	}

	close(s_fd);
	close(clientfd);
	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = test(0);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test(0) failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(IORING_SETUP_SQPOLL);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test(SQPOLL) failed\n");
		return T_EXIT_FAIL;
	}

	return 0;
}
