/* SPDX-License-Identifier: MIT */
/*
 * Test MSG_WAITALL with datagram sockets, with a send splice into two.
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

#define MAX_MSG	128
#define HOST	"127.0.0.1"
static __be16 bind_port;
struct recv_data {
	pthread_barrier_t barrier;
	int use_recvmsg;
	struct msghdr msg;
};

static int recv_prep(struct io_uring *ring, struct iovec *iov, int *sock,
		     struct recv_data *rd)
{
	struct sockaddr_in saddr;
	struct io_uring_sqe *sqe;
	int sockfd, ret, val;

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
	bind_port = saddr.sin_port;

	sqe = io_uring_get_sqe(ring);
	if (!rd->use_recvmsg) {
		io_uring_prep_recv(sqe, sockfd, iov->iov_base, iov->iov_len,
					MSG_WAITALL);
	} else {
		struct msghdr *msg = &rd->msg;

		memset(msg, 0, sizeof(*msg));
		msg->msg_namelen = sizeof(struct sockaddr_in);
		msg->msg_iov = iov;
		msg->msg_iovlen = 1;
		io_uring_prep_recvmsg(sqe, sockfd, msg, MSG_WAITALL);
	}

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

static int do_recv(struct io_uring *ring)
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
	if (cqe->res != MAX_MSG * sizeof(int) / 2) {
		fprintf(stderr, "got wrong length: %d\n", cqe->res);
		goto err;
	}

	io_uring_cqe_seen(ring, cqe);
	return 0;
err:
	return 1;
}

static void *recv_fn(void *data)
{
	struct recv_data *rd = data;
	int buf[MAX_MSG];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf),
	};
	struct io_uring_params p = { };
	struct io_uring ring;
	int ret, sock;

	ret = t_create_ring_params(1, &ring, &p);
	if (ret == T_SETUP_SKIP) {
		pthread_barrier_wait(&rd->barrier);
		ret = 0;
		goto err;
	} else if (ret < 0) {
		pthread_barrier_wait(&rd->barrier);
		goto err;
	}

	ret = recv_prep(&ring, &iov, &sock, rd);
	if (ret) {
		fprintf(stderr, "recv_prep failed: %d\n", ret);
		goto err;
	}
	pthread_barrier_wait(&rd->barrier);
	ret = do_recv(&ring);
	close(sock);
	io_uring_queue_exit(&ring);
err:
	return (void *)(intptr_t)ret;
}

static int do_send(void)
{
	struct sockaddr_in saddr;
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int sockfd, ret, i;
	struct iovec iov;
	int *buf;

	ret = io_uring_queue_init(2, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		return 1;
	}

	buf = malloc(MAX_MSG * sizeof(int));
	for (i = 0; i < MAX_MSG; i++)
		buf[i] = i;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = bind_port;
	inet_pton(AF_INET, HOST, &saddr.sin_addr);

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}

	ret = connect(sockfd, (struct sockaddr *)&saddr, sizeof(saddr));
	if (ret < 0) {
		perror("connect");
		return 1;
	}

	iov.iov_base = buf;
	iov.iov_len = MAX_MSG * sizeof(int) / 2;
	for (i = 0; i < 2; i++) {
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_send(sqe, sockfd, iov.iov_base, iov.iov_len, 0);
		sqe->user_data = 1;

		ret = io_uring_submit(&ring);
		if (ret <= 0) {
			fprintf(stderr, "submit failed: %d\n", ret);
			goto err;
		}
		usleep(10000);
		iov.iov_base += iov.iov_len;
	}

	for (i = 0; i < 2; i++) {
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
		io_uring_cqe_seen(&ring, cqe);
	}

	close(sockfd);
	return 0;
err:
	close(sockfd);
	return 1;
}

static int test(int use_recvmsg)
{
	pthread_t recv_thread;
	struct recv_data rd;
	int ret;
	void *retval;

	pthread_barrier_init(&rd.barrier, NULL, 2);
	rd.use_recvmsg = use_recvmsg;

	ret = pthread_create(&recv_thread, NULL, recv_fn, &rd);
	if (ret) {
		fprintf(stderr, "Thread create failed: %d\n", ret);
		return 1;
	}

	pthread_barrier_wait(&rd.barrier);
	do_send();
	pthread_join(recv_thread, &retval);
	pthread_barrier_destroy(&rd.barrier);
	return (intptr_t)retval;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test(0);
	if (ret) {
		fprintf(stderr, "test recv failed\n");
		return ret;
	}

	ret = test(1);
	if (ret) {
		fprintf(stderr, "test recvmsg failed\n");
		return ret;
	}

	return 0;
}
