/* SPDX-License-Identifier: MIT */
/*
 * Description: test many files being polled for and updated
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
#include <pthread.h>

#include "liburing.h"

#define	NFILES	5000
#define BATCH	500
#define NLOOPS	1000

#define RING_SIZE	512

struct p {
	int fd[2];
	int triggered;
};

static struct p p[NFILES];

static int has_poll_update(void)
{
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	bool has_update = false;
	int ret;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret)
		return -1;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_update(sqe, 0, 0, POLLIN, IORING_TIMEOUT_UPDATE);

	ret = io_uring_submit(&ring);
	if (ret != 1)
		return -1;

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (!ret) {
		if (cqe->res == -ENOENT)
			has_update = true;
		else if (cqe->res != -EINVAL)
			return -1;
		io_uring_cqe_seen(&ring, cqe);
	}
	io_uring_queue_exit(&ring);
	return has_update;
}

static int arm_poll(struct io_uring *ring, int off)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "failed getting sqe\n");
		return 1;
	}

	io_uring_prep_poll_multishot(sqe, p[off].fd[0], POLLIN);
	sqe->user_data = off;
	return 0;
}

static int submit_arm_poll(struct io_uring *ring, int off)
{
	int ret;

	ret = arm_poll(ring, off);
	if (ret)
		return ret;

	ret = io_uring_submit(ring);
	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -1;
}

static int reap_polls(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int i, ret, off;
	char c;

	for (i = 0; i < BATCH; i++) {
		struct io_uring_sqe *sqe;

		sqe = io_uring_get_sqe(ring);
		/* update event */
		io_uring_prep_poll_update(sqe, i, 0, POLLIN,
						IORING_POLL_UPDATE_EVENTS);
		sqe->user_data = 0x12345678;
	}

	ret = io_uring_submit(ring);
	if (ret != BATCH) {
		fprintf(stderr, "submitted %d, %d\n", ret, BATCH);
		return 1;
	}

	for (i = 0; i < 2 * BATCH; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait cqe %d\n", ret);
			return ret;
		}
		off = cqe->user_data;
		if (off == 0x12345678)
			goto seen;
		if (!(cqe->flags & IORING_CQE_F_MORE)) {
			/* need to re-arm poll */
			ret = submit_arm_poll(ring, off);
			if (ret)
				break;
			if (cqe->res <= 0) {
				/* retry this one */
				i--;
				goto seen;
			}
		}

		ret = read(p[off].fd[0], &c, 1);
		if (ret != 1) {
			if (ret == -1 && errno == EAGAIN)
				goto seen;
			fprintf(stderr, "read got %d/%d\n", ret, errno);
			break;
		}
seen:
		io_uring_cqe_seen(ring, cqe);
	}

	if (i != 2 * BATCH) {
		fprintf(stderr, "gave up at %d\n", i);
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

static void *trigger_polls_fn(void *data)
{
	trigger_polls();
	return NULL;
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

static int run(int cqe)
{
	struct io_uring ring;
	struct io_uring_params params = { };
	pthread_t thread;
	int i, j, ret;

	for (i = 0; i < NFILES; i++) {
		if (pipe(p[i].fd) < 0) {
			perror("pipe");
			return 1;
		}
		fcntl(p[i].fd[0], F_SETFL, O_NONBLOCK);
	}

	params.flags = IORING_SETUP_CQSIZE;
	params.cq_entries = cqe;
	ret = io_uring_queue_init_params(RING_SIZE, &ring, &params);
	if (ret) {
		if (ret == -EINVAL) {
			fprintf(stdout, "No CQSIZE, trying without\n");
			ret = io_uring_queue_init(RING_SIZE, &ring, 0);
			if (ret) {
				fprintf(stderr, "ring setup failed: %d\n", ret);
				return 1;
			}
		}
	}

	if (arm_polls(&ring))
		goto err;

	for (i = 0; i < NLOOPS; i++) {
		pthread_create(&thread, NULL, trigger_polls_fn, NULL);
		ret = reap_polls(&ring);
		if (ret)
			goto err;
		pthread_join(thread, NULL);

		for (j = 0; j < NFILES; j++)
			p[j].triggered = 0;
	}

	io_uring_queue_exit(&ring);
	for (i = 0; i < NFILES; i++) {
		close(p[i].fd[0]);
		close(p[i].fd[1]);
	}
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;
}

int main(int argc, char *argv[])
{
	struct rlimit rlim;
	int ret;

	if (argc > 1)
		return 0;

	ret = has_poll_update();
	if (ret < 0) {
		fprintf(stderr, "poll update check failed %i\n", ret);
		return -1;
	} else if (!ret) {
		fprintf(stderr, "no poll update, skip\n");
		return 0;
	}

	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
		perror("getrlimit");
		goto err;
	}

	if (rlim.rlim_cur < (2 * NFILES + 5)) {
		rlim.rlim_cur = (2 * NFILES + 5);
		rlim.rlim_max = rlim.rlim_cur;
		if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
			if (errno == EPERM)
				goto err_nofail;
			perror("setrlimit");
			goto err;
		}
	}

	ret = run(1024);
	if (ret) {
		fprintf(stderr, "run(1024) failed\n");
		goto err;
	}

	ret = run(8192);
	if (ret) {
		fprintf(stderr, "run(8192) failed\n");
		goto err;
	}

	return 0;
err:
	fprintf(stderr, "poll-many failed\n");
	return 1;
err_nofail:
	fprintf(stderr, "poll-many: not enough files available (and not root), "
			"skipped\n");
	return 0;
}
