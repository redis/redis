/* SPDX-License-Identifier: MIT */
/*
 * Description: check that multiple receives on the same socket don't get
 *		stalled if multiple wakers race with the socket readiness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

#include "liburing.h"
#include "helpers.h"

#define NREQS	64

struct data {
	pthread_barrier_t barrier;
	int fd;
};

static void *thread(void *data)
{
	struct data *d = data;
	char buf[64];
	int ret, i;

	pthread_barrier_wait(&d->barrier);
	for (i = 0; i < NREQS; i++) {
		ret = write(d->fd, buf, sizeof(buf));
		if (ret != 64)
			fprintf(stderr, "wrote short %d\n", ret);
	}
	return NULL;
}

static int test(struct io_uring *ring, struct data *d)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int fd[2], ret, i;
	char buf[64];
	pthread_t t;
	void *ret2;

	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, fd) < 0) {
		perror("socketpair");
		return T_EXIT_FAIL;
	}

	d->fd = fd[1];

	pthread_create(&t, NULL, thread, d);

	for (i = 0; i < NREQS; i++) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_recv(sqe, fd[0], buf, sizeof(buf), 0);
	}

	pthread_barrier_wait(&d->barrier);

	ret = io_uring_submit(ring);
	if (ret != NREQS) {
		fprintf(stderr, "submit %d\n", ret);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < NREQS; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "cqe wait %d\n", ret);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(fd[0]);
	close(fd[1]);
	pthread_join(t, &ret2);
	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct data d;
	int i, ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	pthread_barrier_init(&d.barrier, NULL, 2);

	io_uring_queue_init(NREQS, &ring, 0);

	for (i = 0; i < 1000; i++) {
		ret = test(&ring, &d);
		if (ret != T_EXIT_PASS) {
			fprintf(stderr, "Test failed\n");
			return T_EXIT_FAIL;
		}
	}

	return T_EXIT_PASS;
}
