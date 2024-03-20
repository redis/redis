/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring poll cancel handling
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

struct poll_data {
	unsigned is_poll;
	unsigned is_cancel;
};

static void sig_alrm(int sig)
{
	fprintf(stderr, "Timed out!\n");
	exit(1);
}

static int test_poll_cancel(void)
{
	struct io_uring ring;
	int pipe1[2];
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct poll_data *pd, pds[2];
	struct sigaction act;
	int ret;

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return 1;
	}

	ret = io_uring_queue_init(2, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_alrm;
	act.sa_flags = SA_RESTART;
	sigaction(SIGALRM, &act, NULL);
	alarm(1);

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return 1;
	}

	io_uring_prep_poll_add(sqe, pipe1[0], POLLIN);

	pds[0].is_poll = 1;
	pds[0].is_cancel = 0;
	io_uring_sqe_set_data(sqe, &pds[0]);

	ret = io_uring_submit(&ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed\n");
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return 1;
	}

	pds[1].is_poll = 0;
	pds[1].is_cancel = 1;
	io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)&pds[0]);
	io_uring_sqe_set_data(sqe, &pds[1]);

	ret = io_uring_submit(&ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return 1;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait cqe failed: %d\n", ret);
		return 1;
	}

	pd = io_uring_cqe_get_data(cqe);
	if (pd->is_poll && cqe->res != -ECANCELED) {
		fprintf(stderr ,"sqe (add=%d/remove=%d) failed with %ld\n",
					pd->is_poll, pd->is_cancel,
					(long) cqe->res);
		return 1;
	} else if (pd->is_cancel && cqe->res) {
		fprintf(stderr, "sqe (add=%d/remove=%d) failed with %ld\n",
					pd->is_poll, pd->is_cancel,
					(long) cqe->res);
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait_cqe: %d\n", ret);
		return 1;
	}

	pd = io_uring_cqe_get_data(cqe);
	if (pd->is_poll && cqe->res != -ECANCELED) {
		fprintf(stderr, "sqe (add=%d/remove=%d) failed with %ld\n",
					pd->is_poll, pd->is_cancel,
					(long) cqe->res);
		return 1;
	} else if (pd->is_cancel && cqe->res) {
		fprintf(stderr, "sqe (add=%d/remove=%d) failed with %ld\n",
					pd->is_poll, pd->is_cancel,
					(long) cqe->res);
		return 1;
	}

	close(pipe1[0]);
	close(pipe1[1]);
	io_uring_cqe_seen(&ring, cqe);
	io_uring_queue_exit(&ring);
	return 0;
}


static int __test_poll_cancel_with_timeouts(void)
{
	struct __kernel_timespec ts = { .tv_sec = 10, };
	struct io_uring ring, ring2;
	struct io_uring_sqe *sqe;
	int ret, off_nr = 1000;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	ret = io_uring_queue_init(1, &ring2, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	/* test timeout-offset triggering path during cancellation */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_timeout(sqe, &ts, off_nr, 0);

	/* poll ring2 to trigger cancellation on exit() */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, ring2.ring_fd, POLLIN);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_link_timeout(sqe, &ts, 0);

	ret = io_uring_submit(&ring);
	if (ret != 3) {
		fprintf(stderr, "sqe submit failed\n");
		return 1;
	}

	/* just drop all rings/etc. intact, exit() will clean them up */
	return 0;
}

static int test_poll_cancel_with_timeouts(void)
{
	int ret;
	pid_t p;

	p = fork();
	if (p == -1) {
		fprintf(stderr, "fork() failed\n");
		return 1;
	}

	if (p == 0) {
		ret = __test_poll_cancel_with_timeouts();
		exit(ret);
	} else {
		int wstatus;

		if (waitpid(p, &wstatus, 0) == (pid_t)-1) {
			perror("waitpid()");
			return 1;
		}
		if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus)) {
			fprintf(stderr, "child failed %i\n", WEXITSTATUS(wstatus));
			return 1;
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_poll_cancel();
	if (ret) {
		fprintf(stderr, "test_poll_cancel failed\n");
		return -1;
	}

	ret = test_poll_cancel_with_timeouts();
	if (ret) {
		fprintf(stderr, "test_poll_cancel_with_timeouts failed\n");
		return -1;
	}

	return 0;
}
