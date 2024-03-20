/* SPDX-License-Identifier: MIT */
/*
 * Test MSG_WAITALL for recv/recvmsg and include normal sync versions just
 * for comparison.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>

#include "liburing.h"
#include "helpers.h"

#define MAX_MSG	128

struct recv_data {
	pthread_mutex_t mutex;
	int use_recvmsg;
	int use_sync;
	__be16 port;
};

static int get_conn_sock(struct recv_data *rd, int *sockout)
{
	struct sockaddr_in saddr;
	int sockfd, ret, val;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);

	sockfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
	if (sockfd < 0) {
		perror("socket");
		goto err;
	}

	val = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));

	if (t_bind_ephemeral_port(sockfd, &saddr)) {
		perror("bind");
		goto err;
	}
	rd->port = saddr.sin_port;

	ret = listen(sockfd, 16);
	if (ret < 0) {
		perror("listen");
		goto err;
	}

	pthread_mutex_unlock(&rd->mutex);

	ret = accept(sockfd, NULL, NULL);
	if (ret < 0) {
		perror("accept");
		return -1;
	}

	*sockout = sockfd;
	return ret;
err:
	pthread_mutex_unlock(&rd->mutex);
	return -1;
}

static int recv_prep(struct io_uring *ring, struct iovec *iov, int *sock,
		     struct recv_data *rd)
{
	struct io_uring_sqe *sqe;
	struct msghdr msg = { };
	int sockfd, sockout = -1, ret;

	sockfd = get_conn_sock(rd, &sockout);
	if (sockfd < 0)
		goto err;

	sqe = io_uring_get_sqe(ring);
	if (!rd->use_recvmsg) {
		io_uring_prep_recv(sqe, sockfd, iov->iov_base, iov->iov_len,
					MSG_WAITALL);
	} else {
		msg.msg_namelen = sizeof(struct sockaddr_in);
		msg.msg_iov = iov;
		msg.msg_iovlen = 1;
		io_uring_prep_recvmsg(sqe, sockfd, &msg, MSG_WAITALL);
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
	if (sockout != -1) {
		shutdown(sockout, SHUT_RDWR);
		close(sockout);
	}
	if (sockfd != -1) {
		shutdown(sockfd, SHUT_RDWR);
		close(sockfd);
	}
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
	if (cqe->res != MAX_MSG * sizeof(int)) {
		fprintf(stderr, "got wrong length: %d\n", cqe->res);
		goto err;
	}

	io_uring_cqe_seen(ring, cqe);
	return 0;
err:
	return 1;
}

static int recv_sync(struct recv_data *rd)
{
	int buf[MAX_MSG];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf),
	};
	int i, ret, sockfd, sockout = -1;

	sockfd = get_conn_sock(rd, &sockout);

	if (rd->use_recvmsg) {
		struct msghdr msg = { };

		msg.msg_namelen = sizeof(struct sockaddr_in);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		ret = recvmsg(sockfd, &msg, MSG_WAITALL);
	} else {
		ret = recv(sockfd, buf, sizeof(buf), MSG_WAITALL);
	}

	if (ret < 0) {
		perror("receive");
		goto err;
	}

	if (ret != sizeof(buf)) {
		ret = -1;
		goto err;
	}

	for (i = 0; i < MAX_MSG; i++) {
		if (buf[i] != i)
			goto err;
	}
	ret = 0;
err:
	shutdown(sockout, SHUT_RDWR);
	shutdown(sockfd, SHUT_RDWR);
	close(sockout);
	close(sockfd);
	return ret;
}

static int recv_uring(struct recv_data *rd)
{
	int buf[MAX_MSG];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf),
	};
	struct io_uring_params p = { };
	struct io_uring ring;
	int ret, sock = -1, sockout = -1;

	ret = t_create_ring_params(1, &ring, &p);
	if (ret == T_SETUP_SKIP) {
		pthread_mutex_unlock(&rd->mutex);
		ret = 0;
		goto err;
	} else if (ret < 0) {
		pthread_mutex_unlock(&rd->mutex);
		goto err;
	}

	sock = recv_prep(&ring, &iov, &sockout, rd);
	if (ret) {
		fprintf(stderr, "recv_prep failed: %d\n", ret);
		goto err;
	}
	ret = do_recv(&ring);
	if (!ret) {
		int i;

		for (i = 0; i < MAX_MSG; i++) {
			if (buf[i] != i) {
				fprintf(stderr, "found %d at %d\n", buf[i], i);
				ret = 1;
				break;
			}
		}
	}

	shutdown(sockout, SHUT_RDWR);
	shutdown(sock, SHUT_RDWR);
	close(sock);
	close(sockout);
	io_uring_queue_exit(&ring);
err:
	if (sock != -1) {
		shutdown(sock, SHUT_RDWR);
		close(sock);
	}
	if (sockout != -1) {
		shutdown(sockout, SHUT_RDWR);
		close(sockout);
	}
	return ret;
}

static void *recv_fn(void *data)
{
	struct recv_data *rd = data;

	if (rd->use_sync)
		return (void *) (uintptr_t) recv_sync(rd);

	return (void *) (uintptr_t) recv_uring(rd);
}

static int do_send(struct recv_data *rd)
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

	sockfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}

	pthread_mutex_lock(&rd->mutex);
	assert(rd->port != 0);
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = rd->port;
	inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

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

	shutdown(sockfd, SHUT_RDWR);
	close(sockfd);
	return 0;
err:
	shutdown(sockfd, SHUT_RDWR);
	close(sockfd);
	return 1;
}

static int test(int use_recvmsg, int use_sync)
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
	rd.use_recvmsg = use_recvmsg;
	rd.use_sync = use_sync;
	rd.port = 0;

	ret = pthread_create(&recv_thread, NULL, recv_fn, &rd);
	if (ret) {
		fprintf(stderr, "Thread create failed: %d\n", ret);
		pthread_mutex_unlock(&rd.mutex);
		return 1;
	}

	do_send(&rd);
	pthread_join(recv_thread, &retval);
	return (intptr_t)retval;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test(0, 0);
	if (ret) {
		fprintf(stderr, "test recv failed\n");
		return ret;
	}

	ret = test(1, 0);
	if (ret) {
		fprintf(stderr, "test recvmsg failed\n");
		return ret;
	}

	ret = test(0, 1);
	if (ret) {
		fprintf(stderr, "test sync recv failed\n");
		return ret;
	}

	ret = test(1, 1);
	if (ret) {
		fprintf(stderr, "test sync recvmsg failed\n");
		return ret;
	}

	return 0;
}
