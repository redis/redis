/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "liburing.h"

static unsigned long runtime_ms = 10000;

static unsigned long gettimeofday_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

int main(void)
{
	unsigned long tstop;
	unsigned long nr_reqs = 0;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int pipe1[2];
	int ret, i, qd = 32;
	int table_size = 128;

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return 1;
	}

	ret = io_uring_queue_init(1024, &ring, IORING_SETUP_SINGLE_ISSUER |
					       IORING_SETUP_DEFER_TASKRUN);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init failed: %d\n", ret);
		return 1;
	}
	ret = io_uring_register_ring_fd(&ring);
	if (ret < 0) {
		fprintf(stderr, "io_uring_register_ring_fd failed\n");
		return 1;
	}
	ret = io_uring_register_files_sparse(&ring, table_size);
	if (ret < 0) {
		fprintf(stderr, "io_uring_register_files_sparse failed\n");
		return 1;
	}

	for (i = 0; i < table_size; i++) {
		ret = io_uring_register_files_update(&ring, i, pipe1, 1);
		if (ret < 0) {
			fprintf(stderr, "io_uring_register_files_update failed\n");
			return 1;
		}
	}

	srand(time(NULL));

	tstop = gettimeofday_ms() + runtime_ms;
	do {
		int off = rand();

		for (i = 0; i < qd; i++) {
			sqe = io_uring_get_sqe(&ring);
			int roff = (off + i) % table_size;
			io_uring_prep_files_update(sqe, pipe1, 1, roff);
		}

		ret = io_uring_submit(&ring);
		if (ret != qd) {
			fprintf(stderr, "child: sqe submit failed: %d\n", ret);
			return 1;
		}

		for (i = 0; i < qd; i++) {
			ret = io_uring_wait_cqe(&ring, &cqe);
			if (ret < 0) {
				fprintf(stderr, "child: wait completion %d\n", ret);
				break;
			}
			io_uring_cqe_seen(&ring, cqe);
			nr_reqs++;
		}
	} while (gettimeofday_ms() < tstop);

	fprintf(stderr, "max updates/s: %lu\n", nr_reqs * 1000UL / runtime_ms);

	io_uring_queue_exit(&ring);
	close(pipe1[0]);
	close(pipe1[1]);
	return 0;
}
