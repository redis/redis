/* SPDX-License-Identifier: MIT */
/*
 * Description: test many files being polled for
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/resource.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

#define	NFILES	5000
#define BATCH	500
#define NLOOPS	1000

#define RING_SIZE	512

struct p {
	int fd[2];
	int triggered;
};

static struct p p[NFILES];

static int arm_poll(struct io_uring *ring, int off)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "failed getting sqe\n");
		return 1;
	}

	io_uring_prep_poll_add(sqe, p[off].fd[0], POLLIN);
	sqe->user_data = off;
	return 0;
}

static int reap_polls(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int i, ret, off;
	char c;

	for (i = 0; i < BATCH; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait cqe %d\n", ret);
			return ret;
		}
		off = cqe->user_data;
		p[off].triggered = 0;
		ret = read(p[off].fd[0], &c, 1);
		if (ret != 1) {
			fprintf(stderr, "read got %d/%d\n", ret, errno);
			break;
		}
		if (arm_poll(ring, off))
			break;
		io_uring_cqe_seen(ring, cqe);
	}

	if (i != BATCH) {
		fprintf(stderr, "gave up at %d\n", i);
		return 1;
	}

	ret = io_uring_submit(ring);
	if (ret != BATCH) {
		fprintf(stderr, "submitted %d, %d\n", ret, BATCH);
		return 1;
	}

	return 0;
}

static int trigger_polls(void)
{
	char c = 89;
	int i, ret;

	for (i = 0; i < BATCH; i++) {
		int off;

		do {
			off = rand() % NFILES;
			if (!p[off].triggered)
				break;
		} while (1);

		p[off].triggered = 1;
		ret = write(p[off].fd[1], &c, 1);
		if (ret != 1) {
			fprintf(stderr, "write got %d/%d\n", ret, errno);
			return 1;
		}
	}

	return 0;
}

static int arm_polls(struct io_uring *ring)
{
	int ret, to_arm = NFILES, i, off;

	off = 0;
	while (to_arm) {
		int this_arm;

		this_arm = to_arm;
		if (this_arm > RING_SIZE)
			this_arm = RING_SIZE;

		for (i = 0; i < this_arm; i++) {
			if (arm_poll(ring, off)) {
				fprintf(stderr, "arm failed at %d\n", off);
				return 1;
			}
			off++;
		}

		ret = io_uring_submit(ring);
		if (ret != this_arm) {
			fprintf(stderr, "submitted %d, %d\n", ret, this_arm);
			return 1;
		}
		to_arm -= this_arm;
	}

	return 0;
}

static int do_test(struct io_uring *ring)
{
	int i;

	if (arm_polls(ring))
		return 1;

	for (i = 0; i < NLOOPS; i++) {
		trigger_polls();
		if (reap_polls(ring))
			return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_params params = { };
	struct rlimit rlim;
	int i, ret;

	if (argc > 1)
		return 0;

	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
		perror("getrlimit");
		return T_EXIT_FAIL;
	}

	if (rlim.rlim_cur < (2 * NFILES + 5)) {
		rlim.rlim_cur = (2 * NFILES + 5);
		rlim.rlim_max = rlim.rlim_cur;
		if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
			if (errno == EPERM)
				goto err_nofail;
			perror("setrlimit");
			return T_EXIT_FAIL;
		}
	}

	for (i = 0; i < NFILES; i++) {
		if (pipe(p[i].fd) < 0) {
			perror("pipe");
			return T_EXIT_FAIL;
		}
	}

	params.flags = IORING_SETUP_CQSIZE;
	params.cq_entries = 4096;
	ret = io_uring_queue_init_params(RING_SIZE, &ring, &params);
	if (ret) {
		if (ret == -EINVAL) {
			fprintf(stdout, "No CQSIZE, trying without\n");

			params.flags &= ~IORING_SETUP_CQSIZE;
			params.cq_entries = 0;
			ret = io_uring_queue_init_params(RING_SIZE, &ring, &params);
			if (ret) {
				fprintf(stderr, "ring setup failed: %d\n", ret);
				return T_EXIT_FAIL;
			}
		}
	}

	if (do_test(&ring)) {
		fprintf(stderr, "test (normal) failed\n");
		return T_EXIT_FAIL;
	}
	io_uring_queue_exit(&ring);

	if (t_probe_defer_taskrun()) {
		params.flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
		ret = io_uring_queue_init_params(RING_SIZE, &ring, &params);
		if (ret) {
			fprintf(stderr, "ring DEFER setup failed: %d\n", ret);
			return T_EXIT_FAIL;
		}
		if (do_test(&ring)) {
			fprintf(stderr, "test (DEFER) failed\n");
			return T_EXIT_FAIL;
		}
		io_uring_queue_exit(&ring);
	}
	return 0;
err_nofail:
	fprintf(stderr, "poll-many: not enough files available (and not root), "
			"skipped\n");
	return 0;
}
