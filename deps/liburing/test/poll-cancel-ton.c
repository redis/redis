/* SPDX-License-Identifier: MIT */
/*
 * Description: test massive amounts of poll with cancel
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>

#include "liburing.h"

#define POLL_COUNT	30000

static void *sqe_index[POLL_COUNT];

static int reap_events(struct io_uring *ring, unsigned nr_events, int nowait)
{
	struct io_uring_cqe *cqe;
	int i, ret = 0;

	for (i = 0; i < nr_events; i++) {
		if (!i && !nowait)
			ret = io_uring_wait_cqe(ring, &cqe);
		else
			ret = io_uring_peek_cqe(ring, &cqe);
		if (ret) {
			if (ret != -EAGAIN)
				fprintf(stderr, "cqe peek failed: %d\n", ret);
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return i ? i : ret;
}

static int del_polls(struct io_uring *ring, int fd, int nr)
{
	int batch, i, ret;
	struct io_uring_sqe *sqe;

	while (nr) {
		batch = 1024;
		if (batch > nr)
			batch = nr;

		for (i = 0; i < batch; i++) {
			void *data;

			sqe = io_uring_get_sqe(ring);
			data = sqe_index[lrand48() % nr];
			io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)data);
		}

		ret = io_uring_submit(ring);
		if (ret != batch) {
			fprintf(stderr, "%s: failed submit, %d\n", __FUNCTION__, ret);
			return 1;
		}
		nr -= batch;
		ret = reap_events(ring, 2 * batch, 0);
	}
	return 0;
}

static int add_polls(struct io_uring *ring, int fd, int nr)
{
	int batch, i, count, ret;
	struct io_uring_sqe *sqe;

	count = 0;
	while (nr) {
		batch = 1024;
		if (batch > nr)
			batch = nr;

		for (i = 0; i < batch; i++) {
			sqe = io_uring_get_sqe(ring);
			io_uring_prep_poll_add(sqe, fd, POLLIN);
			sqe_index[count++] = sqe;
			sqe->user_data = (unsigned long) sqe;
		}

		ret = io_uring_submit(ring);
		if (ret != batch) {
			fprintf(stderr, "%s: failed submit, %d\n", __FUNCTION__, ret);
			return 1;
		}
		nr -= batch;
		reap_events(ring, batch, 1);
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_params p = { };
	int pipe1[2];
	int ret;

	if (argc > 1)
		return 0;

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return 1;
	}

	p.flags = IORING_SETUP_CQSIZE;
	p.cq_entries = 16384;
	ret = io_uring_queue_init_params(1024, &ring, &p);
	if (ret) {
		if (ret == -EINVAL) {
			fprintf(stdout, "No CQSIZE, trying without\n");
			ret = io_uring_queue_init(1024, &ring, 0);
			if (ret) {
				fprintf(stderr, "ring setup failed: %d\n", ret);
				return 1;
			}
		}
	}

	add_polls(&ring, pipe1[0], 30000);
	del_polls(&ring, pipe1[0], 30000);

	io_uring_queue_exit(&ring);
	return 0;
}
