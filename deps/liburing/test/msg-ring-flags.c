/* SPDX-License-Identifier: MIT */
/*
 * Description: test ring messaging with flags command
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

#include "liburing.h"
#include "helpers.h"

#define CUSTOM_FLAG 0x42
#define USER_DATA 0x5aa5
#define LEN 0x20
#define ID 0x1

struct data {
	pthread_barrier_t barrier;
	int fd;
};

static int recv_msg(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int ret;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait cqe %d\n", ret);
		return T_EXIT_FAIL;
	}
	if (cqe->user_data != USER_DATA) {
		fprintf(stderr, "user_data %llx\n", (long long) cqe->user_data);
		return T_EXIT_FAIL;
	}
	if (cqe->res != LEN) {
		fprintf(stderr, "len %x\n", cqe->res);
		return T_EXIT_FAIL;
	}
	if (cqe->flags != CUSTOM_FLAG) {
		fprintf(stderr, "flags %x\n", cqe->flags);
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}

static int send_msg(struct io_uring *ring, int target_fd)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return T_EXIT_FAIL;
	}

	io_uring_prep_msg_ring_cqe_flags(sqe, target_fd, LEN, USER_DATA,
					 0, CUSTOM_FLAG);
	sqe->user_data = ID;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		if (ret == -EINVAL)
			return T_EXIT_SKIP;

		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		return T_EXIT_FAIL;
	}
	if (cqe->res != 0) {
		if (cqe->res == -EINVAL)
			return T_EXIT_SKIP;
		fprintf(stderr, "cqe res %d\n", cqe->res);
		return T_EXIT_FAIL;
	}
	if (cqe->user_data != ID) {
		fprintf(stderr, "user_data %llx\n", (long long) cqe->user_data);
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(ring, cqe);
	return T_EXIT_PASS;
}

static void *thread_fn(void *data)
{
	struct data *d = data;
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init(2, &ring, IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER);
	if (ret) {
		fprintf(stderr, "ring init failed %d\n", ret);
		pthread_barrier_wait(&d->barrier);
		return NULL;
	}

	d->fd = ring.ring_fd;
	pthread_barrier_wait(&d->barrier);

	if (recv_msg(&ring))
		return (void *) 1;

	return NULL;
}

int main(int argc, char *argv[])
{
	struct io_uring ring, ring2;
	pthread_t thread;
	struct data d;
	void *ret2;
	int ret, i;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(2, &ring, 0);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init failed for ring1: %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = io_uring_queue_init(2, &ring2, 0);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init failed for ring2: %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = send_msg(&ring, ring2.ring_fd);
	if (ret) {
		if (ret != T_EXIT_SKIP)
			fprintf(stderr, "send_msg failed: %d\n", ret);
		return ret;
	}

	ret = recv_msg(&ring2);
	if (ret) {
		fprintf(stderr, "recv_msg failed: %d\n", ret);
		return ret;
	}

	for (i = 0; i < 8; i++) {
		ret = send_msg(&ring, ring2.ring_fd);
		if (ret) {
			if (ret != T_EXIT_SKIP)
				fprintf(stderr, "send_msg failed: %d\n", ret);
			return ret;
		}
	}

	for (i = 0; i < 8; i++) {
		ret = recv_msg(&ring2);
		if (ret) {
			fprintf(stderr, "recv_msg failed: %d\n", ret);
			return ret;
		}
	}

	pthread_barrier_init(&d.barrier, NULL, 2);
	d.fd = -1;
	pthread_create(&thread, NULL, thread_fn, &d);
	pthread_barrier_wait(&d.barrier);
	if (d.fd == -1)
		return T_EXIT_FAIL;

	ret = send_msg(&ring, d.fd);
	if (ret) {
		fprintf(stderr, "send msg failed: %d\n", ret);
		return ret;
	}
	pthread_join(thread, &ret2);
	if (ret2) {
		fprintf(stderr, "Remote test failed\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
