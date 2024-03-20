/* SPDX-License-Identifier: MIT */
/*
 * Simple test case showing using send and recv through io_uring
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

#include "liburing.h"
#include "helpers.h"

static char str[] = "This is a test of send and recv over io_uring!";

#define MAX_MSG	128

#define PORT	10202
#define HOST	"127.0.0.1"

static int recv_prep(struct io_uring *ring, struct iovec *iov, int *sock,
		     int registerfiles)
{
	struct sockaddr_in saddr;
	struct io_uring_sqe *sqe;
	int sockfd, ret, val, use_fd;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	saddr.sin_port = htons(PORT);

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}

	val = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	ret = bind(sockfd, (struct sockaddr *)&saddr, sizeof(saddr));
	if (ret < 0) {
		perror("bind");
		goto err;
	}

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

static int do_send(void)
{
	struct sockaddr_in saddr;
	struct iovec iov = {
		.iov_base = str,
		.iov_len = sizeof(str),
	};
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int sockfd, ret;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		return 1;
	}

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(PORT);
	inet_pton(AF_INET, HOST, &saddr.sin_addr);

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		goto err2;
	}

	ret = connect(sockfd, (struct sockaddr *)&saddr, sizeof(saddr));
	if (ret < 0) {
		perror("connect");
		goto err;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_send(sqe, sockfd, iov.iov_base, iov.iov_len, 0);
	sqe->user_data = 1;

	ret = io_uring_submit(&ring);
	if (ret <= 0) {
		fprintf(stderr, "submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (cqe->res == -EINVAL) {
		fprintf(stdout, "send not supported, skipping\n");
		goto err;
	}
	if (cqe->res != iov.iov_len) {
		fprintf(stderr, "failed cqe: %d\n", cqe->res);
		goto err;
	}

	close(sockfd);
	io_uring_queue_exit(&ring);
	return 0;

err:
	close(sockfd);
err2:
	io_uring_queue_exit(&ring);
	return 1;
}

static int test(int use_sqthread, int regfiles)
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
	do_send();
	pthread_join(recv_thread, &retval);
	return (intptr_t)retval;
}

static int test_invalid(void)
{
	struct io_uring ring;
	int ret, i;
	int fds[2];
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;

	ret = t_create_ring(8, &ring, 0);
	if (ret)
		return ret;

	ret = t_create_socket_pair(fds, true);
	if (ret)
		return ret;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_sendmsg(sqe, fds[0], NULL, MSG_WAITALL);
	sqe->flags |= IOSQE_ASYNC;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_recvmsg(sqe, fds[1], NULL, 0);
	sqe->flags |= IOSQE_ASYNC;

	ret = io_uring_submit_and_wait(&ring, 2);
	if (ret != 2)
		return ret;

	for (i = 0; i < 2; i++) {
		ret = io_uring_peek_cqe(&ring, &cqe);
		if (ret || cqe->res != -EFAULT)
			return -1;
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	close(fds[0]);
	close(fds[1]);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_invalid();
	if (ret) {
		fprintf(stderr, "test_invalid failed\n");
		return ret;
	}

	ret = test(0, 0);
	if (ret) {
		fprintf(stderr, "test sqthread=0 failed\n");
		return ret;
	}

	ret = test(1, 1);
	if (ret) {
		fprintf(stderr, "test sqthread=1 reg=1 failed\n");
		return ret;
	}

	ret = test(1, 0);
	if (ret) {
		fprintf(stderr, "test sqthread=1 reg=0 failed\n");
		return ret;
	}

	return 0;
}
