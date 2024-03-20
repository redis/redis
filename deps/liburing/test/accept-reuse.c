/* SPDX-License-Identifier: MIT */
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "liburing.h"
#include "helpers.h"
#include "../src/syscall.h"

static struct io_uring io_uring;

static int sys_io_uring_enter(const int fd, const unsigned to_submit,
			      const unsigned min_complete,
			      const unsigned flags, sigset_t * const sig)
{
	return __sys_io_uring_enter(fd, to_submit, min_complete, flags, sig);
}

static int submit_sqe(void)
{
	struct io_uring_sq *sq = &io_uring.sq;
	const unsigned tail = *sq->ktail;

	sq->array[tail & sq->ring_mask] = 0;
	io_uring_smp_store_release(sq->ktail, tail + 1);

	return sys_io_uring_enter(io_uring.ring_fd, 1, 0, 0, NULL);
}

int main(int argc, char **argv)
{
	struct addrinfo *addr_info_list = NULL;
	struct addrinfo *ai, *addr_info = NULL;
	struct io_uring_params params;
	struct io_uring_sqe *sqe;
	struct addrinfo hints;
	struct sockaddr sa;
	socklen_t sa_size = sizeof(sa);
	int ret, listen_fd, connect_fd, val, i;

	if (argc > 1)
		return T_EXIT_SKIP;

	memset(&params, 0, sizeof(params));
	ret = io_uring_queue_init_params(4, &io_uring, &params);
	if (ret) {
		fprintf(stderr, "io_uring_init_failed: %d\n", ret);
		return T_EXIT_FAIL;
	}
	if (!(params.features & IORING_FEAT_SUBMIT_STABLE)) {
		fprintf(stdout, "FEAT_SUBMIT_STABLE not there, skipping\n");
		return T_EXIT_SKIP;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

	ret = getaddrinfo(NULL, "12345", &hints, &addr_info_list);
	if (ret < 0) {
		perror("getaddrinfo");
		return T_EXIT_FAIL;
	}

	for (ai = addr_info_list; ai; ai = ai->ai_next) {
		if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
			addr_info = ai;
			break;
		}
	}
	if (!addr_info) {
		fprintf(stderr, "addrinfo not found\n");
		return T_EXIT_FAIL;
	}

	sqe = &io_uring.sq.sqes[0];
	listen_fd = -1;

	ret = socket(addr_info->ai_family, SOCK_STREAM,
			   addr_info->ai_protocol);
	if (ret < 0) {
		perror("socket");
		return T_EXIT_FAIL;
	}
	listen_fd = ret;

	val = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int));
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(int));

	ret = bind(listen_fd, addr_info->ai_addr, addr_info->ai_addrlen);
	if (ret < 0) {
		perror("bind");
		return T_EXIT_FAIL;
	}

	ret = listen(listen_fd, SOMAXCONN);
	if (ret < 0) {
		perror("listen");
		return T_EXIT_FAIL;
	}

	memset(&sa, 0, sizeof(sa));

	io_uring_prep_accept(sqe, listen_fd, &sa, &sa_size, 0);
	sqe->user_data = 1;
	ret = submit_sqe();
	if (ret != 1) {
		fprintf(stderr, "submit failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	connect_fd = -1;
	ret = socket(addr_info->ai_family, SOCK_STREAM, addr_info->ai_protocol);
	if (ret < 0) {
		perror("socket");
		return T_EXIT_FAIL;
	}
	connect_fd = ret;

	io_uring_prep_connect(sqe, connect_fd, addr_info->ai_addr,
				addr_info->ai_addrlen);
	sqe->user_data = 2;
	ret = submit_sqe();
	if (ret != 1) {
		fprintf(stderr, "submit failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 2; i++) {
		struct io_uring_cqe *cqe = NULL;

		ret = io_uring_wait_cqe(&io_uring, &cqe);
		if (ret) {
			fprintf(stderr, "io_uring_wait_cqe: %d\n", ret);
			return T_EXIT_FAIL;
		}

		switch (cqe->user_data) {
		case 1:
			if (cqe->res < 0) {
				fprintf(stderr, "accept failed: %d\n", cqe->res);
				return T_EXIT_FAIL;
			}
			break;
		case 2:
			if (cqe->res) {
				fprintf(stderr, "connect failed: %d\n", cqe->res);
				return T_EXIT_FAIL;
			}
			break;
		}
		io_uring_cq_advance(&io_uring, 1);
	}

	freeaddrinfo(addr_info_list);
	io_uring_queue_exit(&io_uring);
	return T_EXIT_PASS;
}
