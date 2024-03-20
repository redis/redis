/* SPDX-License-Identifier: MIT */
/*
 * Simple test case using the socket op
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <assert.h>

#include "liburing.h"
#include "helpers.h"

static char str[] = "This is a test of send and recv over io_uring!";

#define MAX_MSG	128

#define HOST	"127.0.0.1"

static int no_socket;
static __be32 g_port;

static int recv_prep(struct io_uring *ring, struct iovec *iov, int *sock,
		     int registerfiles)
{
	struct sockaddr_in saddr;
	struct io_uring_sqe *sqe;
	int sockfd, ret, val, use_fd;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}

	val = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	if (t_bind_ephemeral_port(sockfd, &saddr)) {
		perror("bind");
		goto err;
	}
	g_port = saddr.sin_port;

	if (registerfiles) {
		ret = io_uring_register_files(ring, &sockfd, 1);
		if (ret) {
			fprintf(stderr, "file reg failed\n");
			goto err;
		}
		use_fd = 0;
	} else {
		use_fd = sockfd;
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_recv(sqe, use_fd, iov->iov_base, iov->iov_len, 0);
	if (registerfiles)
		sqe->flags |= IOSQE_FIXED_FILE;
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "submit failed: %d\n", ret);
		goto err;
	}

	*sock = sockfd;
	return 0;
err:
	close(sockfd);
	return 1;
}

static int do_recv(struct io_uring *ring, struct iovec *iov)
{
	struct io_uring_cqe *cqe;
	int ret;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stdout, "wait_cqe: %d\n", ret);
		goto err;
	}
	if (cqe->res == -EINVAL) {
		fprintf(stdout, "recv not supported, skipping\n");
		return 0;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "failed cqe: %d\n", cqe->res);
		goto err;
	}

	if (cqe->res -1 != strlen(str)) {
		fprintf(stderr, "got wrong length: %d/%d\n", cqe->res,
							(int) strlen(str) + 1);
		goto err;
	}

	if (strcmp(str, iov->iov_base)) {
		fprintf(stderr, "string mismatch\n");
		goto err;
	}

	return 0;
err:
	return 1;
}

struct recv_data {
	pthread_mutex_t mutex;
	int use_sqthread;
	int registerfiles;
};

static void *recv_fn(void *data)
{
	struct recv_data *rd = data;
	char buf[MAX_MSG + 1];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf) - 1,
	};
	struct io_uring_params p = { };
	struct io_uring ring;
	int ret, sock;

	if (rd->use_sqthread)
		p.flags = IORING_SETUP_SQPOLL;
	ret = t_create_ring_params(1, &ring, &p);
	if (ret == T_SETUP_SKIP) {
		pthread_mutex_unlock(&rd->mutex);
		ret = 0;
		goto err;
	} else if (ret < 0) {
		pthread_mutex_unlock(&rd->mutex);
		goto err;
	}

	if (rd->use_sqthread && !rd->registerfiles) {
		if (!(p.features & IORING_FEAT_SQPOLL_NONFIXED)) {
			fprintf(stdout, "Non-registered SQPOLL not available, skipping\n");
			pthread_mutex_unlock(&rd->mutex);
			goto err;
		}
	}

	ret = recv_prep(&ring, &iov, &sock, rd->registerfiles);
	if (ret) {
		fprintf(stderr, "recv_prep failed: %d\n", ret);
		goto err;
	}
	pthread_mutex_unlock(&rd->mutex);
	ret = do_recv(&ring, &iov);

	close(sock);
	io_uring_queue_exit(&ring);
err:
	return (void *)(intptr_t)ret;
}

static int fallback_send(struct io_uring *ring, struct sockaddr_in *saddr)
{
	struct iovec iov = {
		.iov_base = str,
		.iov_len = sizeof(str),
	};
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int sockfd, ret;

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}

	ret = connect(sockfd, (struct sockaddr *)saddr, sizeof(*saddr));
	if (ret < 0) {
		perror("connect");
		return 1;
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_send(sqe, sockfd, iov.iov_base, iov.iov_len, 0);
	sqe->user_data = 1;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (cqe->res == -EINVAL) {
		fprintf(stdout, "send not supported, skipping\n");
		close(sockfd);
		return 0;
	}
	if (cqe->res != iov.iov_len) {
		fprintf(stderr, "failed cqe: %d\n", cqe->res);
		goto err;
	}

	close(sockfd);
	return 0;
err:
	close(sockfd);
	return 1;
}

static int do_send(int socket_direct, int alloc)
{
	struct sockaddr_in saddr;
	struct iovec iov = {
		.iov_base = str,
		.iov_len = sizeof(str),
	};
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int sockfd, ret, fd = -1;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		return 1;
	}

	if (socket_direct) {
		ret = io_uring_register_files(&ring, &fd, 1);
		if (ret) {
			fprintf(stderr, "file register %d\n", ret);
			return 1;
		}
	}

	assert(g_port != 0);
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = g_port;
	inet_pton(AF_INET, HOST, &saddr.sin_addr);

	sqe = io_uring_get_sqe(&ring);
	if (socket_direct) {
		unsigned file_index = 0;
		if (alloc)
			file_index = IORING_FILE_INDEX_ALLOC - 1;
		io_uring_prep_socket_direct(sqe, AF_INET, SOCK_DGRAM, 0,
						file_index, 0);
	} else {
		io_uring_prep_socket(sqe, AF_INET, SOCK_DGRAM, 0, 0);
	}
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "socket submit: %d\n", ret);
		return 1;
	}
	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe: %d\n", ret);
		return 1;
	}
	if (cqe->res < 0) {
		if (cqe->res == -EINVAL) {
			no_socket = 1;
			io_uring_cqe_seen(&ring, cqe);
			return fallback_send(&ring, &saddr);
		}

		fprintf(stderr, "socket res: %d\n", ret);
		return 1;
	}

	sockfd = cqe->res;
	if (socket_direct && !alloc)
		sockfd = 0;
	io_uring_cqe_seen(&ring, cqe);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_connect(sqe, sockfd, (struct sockaddr *) &saddr,
				sizeof(saddr));
	if (socket_direct)
		sqe->flags |= IOSQE_FIXED_FILE;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "connect submit: %d\n", ret);
		return 1;
	}
	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe: %d\n", ret);
		return 1;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "connect res: %d\n", cqe->res);
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_send(sqe, sockfd, iov.iov_base, iov.iov_len, 0);
	sqe->user_data = 1;
	if (socket_direct)
		sqe->flags |= IOSQE_FIXED_FILE;

	ret = io_uring_submit(&ring);
	if (ret <= 0) {
		fprintf(stderr, "submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (cqe->res == -EINVAL) {
		fprintf(stdout, "send not supported, skipping\n");
		close(sockfd);
		return 0;
	}
	if (cqe->res != iov.iov_len) {
		fprintf(stderr, "failed cqe: %d\n", cqe->res);
		goto err;
	}

	close(sockfd);
	return 0;
err:
	close(sockfd);
	return 1;
}

static int test(int use_sqthread, int regfiles, int socket_direct, int alloc)
{
	pthread_mutexattr_t attr;
	pthread_t recv_thread;
	struct recv_data rd;
	int ret;
	void *retval;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_setpshared(&attr, 1);
	pthread_mutex_init(&rd.mutex, &attr);
	pthread_mutex_lock(&rd.mutex);
	rd.use_sqthread = use_sqthread;
	rd.registerfiles = regfiles;

	ret = pthread_create(&recv_thread, NULL, recv_fn, &rd);
	if (ret) {
		fprintf(stderr, "Thread create failed: %d\n", ret);
		pthread_mutex_unlock(&rd.mutex);
		return 1;
	}

	pthread_mutex_lock(&rd.mutex);
	do_send(socket_direct, alloc);
	pthread_join(recv_thread, &retval);
	return (intptr_t)retval;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test(0, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "test sqthread=0 failed\n");
		return ret;
	}
	if (no_socket)
		return 0;

	ret = test(1, 1, 0, 0);
	if (ret) {
		fprintf(stderr, "test sqthread=1 reg=1 failed\n");
		return ret;
	}

	ret = test(1, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "test sqthread=1 reg=0 failed\n");
		return ret;
	}

	ret = test(0, 0, 1, 0);
	if (ret) {
		fprintf(stderr, "test sqthread=0 direct=1 failed\n");
		return ret;
	}

	ret = test(0, 0, 1, 1);
	if (ret) {
		fprintf(stderr, "test sqthread=0 direct=alloc failed\n");
		return ret;
	}

	return 0;
}
