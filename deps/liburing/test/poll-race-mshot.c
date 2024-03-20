/* SPDX-License-Identifier: MIT */
/*
 * Description: check that racing wakeups don't re-issue a poll multishot,
 *		this can leak ring provided buffers. also test if ring
 *		provided buffers for regular receive can leak if we hit a
 *		poll race.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>

#include "liburing.h"
#include "helpers.h"

#define NREQS		64
#define BUF_SIZE	64

static int no_buf_ring;

struct data {
	pthread_barrier_t barrier;
	int fd;
};

static void *thread(void *data)
{
	struct data *d = data;
	char buf[BUF_SIZE];
	int ret, i, fd;

	memset(buf, 0x5a, BUF_SIZE);
	pthread_barrier_wait(&d->barrier);
	fd = d->fd;
	for (i = 0; i < NREQS; i++) {
		ret = write(fd, buf, sizeof(buf));
		if (ret != BUF_SIZE) {
			if (ret < 0) {
				perror("write");
				printf("bad fd %d\n", fd);
			} else
				fprintf(stderr, "wrote short %d\n", ret);
		}
	}
	return NULL;
}

static int test(struct io_uring *ring, struct data *d)
{
	struct io_uring_buf_ring *br;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int fd[2], ret, i;
	pthread_t t;
	void *buf, *ptr;
	void *ret2;

	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, fd) < 0) {
		perror("socketpair");
		return T_EXIT_FAIL;
	}

	d->fd = fd[1];

	if (posix_memalign((void **) &buf, 16384, BUF_SIZE * NREQS))
		return T_EXIT_FAIL;

	br = io_uring_setup_buf_ring(ring, NREQS, 1, 0, &ret);
	if (!br) {
		if (ret == -EINVAL) {
			no_buf_ring = 1;
			return T_EXIT_SKIP;
		}
		fprintf(stderr, "buf ring reg %d\n", ret);
		return T_EXIT_FAIL;
	}

	ptr = buf;
	for (i = 0; i < NREQS; i++) {
		io_uring_buf_ring_add(br, ptr, BUF_SIZE, i + 1,
				io_uring_buf_ring_mask(NREQS), i);
		ptr += BUF_SIZE;
	}
	io_uring_buf_ring_advance(br, NREQS);

	pthread_create(&t, NULL, thread, d);

	for (i = 0; i < NREQS; i++) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_recv(sqe, fd[0], NULL, 0, 0);
		sqe->flags |= IOSQE_BUFFER_SELECT;
		sqe->buf_group = 1;
	}

	pthread_barrier_wait(&d->barrier);

	ret = io_uring_submit(ring);
	if (ret != NREQS) {
		fprintf(stderr, "submit %d\n", ret);
		return T_EXIT_FAIL;
	}

	i = 0;
	do {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "cqe wait %d\n", ret);
			return T_EXIT_FAIL;
		}
		i++;
		if (cqe->res != BUF_SIZE) {
			fprintf(stderr, "Bad cqe res %d\n", cqe->res);
			break;
		}
		if (cqe->flags & IORING_CQE_F_BUFFER) {
			int bid = cqe->flags >> 16;

			if (bid > NREQS) {
				fprintf(stderr, "Bad BID %d\n", bid);
				return T_EXIT_FAIL;
			}
		} else {
			fprintf(stderr, "No BID set!\n");
			printf("ret=%d\n", cqe->res);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
		if (i > NREQS) {
			fprintf(stderr, "Got too many requests?\n");
			return T_EXIT_FAIL;
		}
	} while (i < NREQS);

	pthread_join(t, &ret2);
	free(buf);
	io_uring_free_buf_ring(ring, br, NREQS, 1);
	close(fd[0]);
	close(fd[1]);
	return T_EXIT_PASS;
}

static int test_mshot(struct io_uring *ring, struct data *d)
{
	struct io_uring_buf_ring *br;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int fd[2], ret, i;
	pthread_t t;
	void *buf, *ptr;
	void *ret2;

	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, fd) < 0) {
		perror("socketpair");
		return T_EXIT_FAIL;
	}

	d->fd = fd[1];

	if (posix_memalign((void *) &buf, 16384, BUF_SIZE * NREQS))
		return T_EXIT_FAIL;

	br = io_uring_setup_buf_ring(ring, NREQS, 1, 0, &ret);
	if (!br) {
		fprintf(stderr, "buf ring reg %d\n", ret);
		return T_EXIT_FAIL;
	}

	ptr = buf;
	for (i = 0; i < NREQS; i++) {
		io_uring_buf_ring_add(br, ptr, BUF_SIZE, i + 1,
				io_uring_buf_ring_mask(NREQS), i);
		ptr += BUF_SIZE;
	}
	io_uring_buf_ring_advance(br, NREQS);

	pthread_create(&t, NULL, thread, d);

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_recv_multishot(sqe, fd[0], NULL, 0, 0);
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = 1;

	pthread_barrier_wait(&d->barrier);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "submit %d\n", ret);
		return T_EXIT_FAIL;
	}

	i = 0;
	do {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "cqe wait %d\n", ret);
			return T_EXIT_FAIL;
		}
		i++;
		if (!(cqe->flags & IORING_CQE_F_MORE))
			break;
		if (cqe->res != BUF_SIZE) {
			fprintf(stderr, "Bad cqe res %d\n", cqe->res);
			break;
		}
		if (cqe->flags & IORING_CQE_F_BUFFER) {
			int bid = cqe->flags >> 16;

			if (bid > NREQS) {
				fprintf(stderr, "Bad BID %d\n", bid);
				return T_EXIT_FAIL;
			}
		} else {
			fprintf(stderr, "No BID set!\n");
			printf("ret=%d\n", cqe->res);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
		if (i > NREQS) {
			fprintf(stderr, "Got too many requests?\n");
			return T_EXIT_FAIL;
		}
	} while (1);

	if (i != NREQS + 1) {
		fprintf(stderr, "Only got %d requests\n", i);
		return T_EXIT_FAIL;
	}

	pthread_join(t, &ret2);
	io_uring_free_buf_ring(ring, br, NREQS, 1);
	free(buf);
	close(fd[0]);
	close(fd[1]);
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

	for (i = 0; i < 1000; i++) {
		io_uring_queue_init(NREQS, &ring, 0);
		ret = test(&ring, &d);
		if (ret != T_EXIT_PASS) {
			if (no_buf_ring)
				break;
			fprintf(stderr, "Test failed loop %d\n", i);
			return T_EXIT_FAIL;
		}
		io_uring_queue_exit(&ring);
	}

	if (no_buf_ring)
		return T_EXIT_SKIP;

	for (i = 0; i < 1000; i++) {
		io_uring_queue_init(NREQS, &ring, 0);
		ret = test_mshot(&ring, &d);
		if (ret != T_EXIT_PASS) {
			fprintf(stderr, "Test mshot failed loop %d\n", i);
			return T_EXIT_FAIL;
		}
		io_uring_queue_exit(&ring);
	}

	return T_EXIT_PASS;
}
