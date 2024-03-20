/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "liburing.h"
#include "test.h"
#include "helpers.h"

static pid_t pid;

static pid_t fork_t(void)
{
	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "fork failed\n");
		exit(T_EXIT_FAIL);
	}
	return pid;
}

static void wait_child_t(void)
{
	int wstatus;

	if (waitpid(pid, &wstatus, 0) == (pid_t)-1) {
		perror("waitpid()");
		exit(T_EXIT_FAIL);
	}
	if (!WIFEXITED(wstatus)) {
		fprintf(stderr, "child failed %i\n", WEXITSTATUS(wstatus));
		exit(T_EXIT_FAIL);
	}
	if (WEXITSTATUS(wstatus))
		exit(T_EXIT_FAIL);
}

static int try_submit(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);
	sqe->user_data = 42;

	ret = io_uring_submit(ring);
	if (ret < 0)
		return ret;

	if (ret != 1)
		t_error(1, ret, "submit %i", ret);
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret)
		t_error(1, ret, "wait fail %i", ret);

	if (cqe->res || cqe->user_data != 42)
		t_error(1, ret, "invalid cqe");

	io_uring_cqe_seen(ring, cqe);
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER);
	if (ret == -EINVAL) {
		return T_EXIT_SKIP;
	} else if (ret) {
		fprintf(stderr, "io_uring_queue_init() failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	/* test that the creator iw allowed to submit */
	ret = try_submit(&ring);
	if (ret) {
		fprintf(stderr, "the creator can't submit %i\n", ret);
		return T_EXIT_FAIL;
	}

	/* test that a second submitter doesn't succeed */
	if (!fork_t()) {
		ret = try_submit(&ring);
		if (ret != -EEXIST)
			fprintf(stderr, "1: not owner child could submit %i\n", ret);
		return ret != -EEXIST;
	}
	wait_child_t();
	io_uring_queue_exit(&ring);

	/* test that the first submitter but not creator can submit */
	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_R_DISABLED);
	if (ret)
		t_error(1, ret, "ring init (2) %i", ret);

	if (!fork_t()) {
		io_uring_enable_rings(&ring);
		ret = try_submit(&ring);
		if (ret)
			fprintf(stderr, "2: not owner child could submit %i\n", ret);
		return !!ret;
	}
	wait_child_t();
	io_uring_queue_exit(&ring);

	/* test that only the first enabler can submit */
	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_R_DISABLED);
	if (ret)
		t_error(1, ret, "ring init (3) %i", ret);

	io_uring_enable_rings(&ring);
	if (!fork_t()) {
		ret = try_submit(&ring);
		if (ret != -EEXIST)
			fprintf(stderr, "3: not owner child could submit %i\n", ret);
		return ret != -EEXIST;
	}
	wait_child_t();
	io_uring_queue_exit(&ring);

	/* test that anyone can submit to a SQPOLL|SINGLE_ISSUER ring */
	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER|IORING_SETUP_SQPOLL);
	if (ret)
		t_error(1, ret, "ring init (4) %i", ret);

	ret = try_submit(&ring);
	if (ret) {
		fprintf(stderr, "SQPOLL submit failed (creator) %i\n", ret);
		return T_EXIT_FAIL;
	}

	if (!fork_t()) {
		ret = try_submit(&ring);
		if (ret)
			fprintf(stderr, "SQPOLL submit failed (child) %i\n", ret);
		return !!ret;
	}
	wait_child_t();
	io_uring_queue_exit(&ring);

	/* test that IORING_ENTER_REGISTERED_RING doesn't break anything */
	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER);
	if (ret)
		t_error(1, ret, "ring init (5) %i", ret);

	if (!fork_t()) {
		ret = try_submit(&ring);
		if (ret != -EEXIST)
			fprintf(stderr, "4: not owner child could submit %i\n", ret);
		return ret != -EEXIST;
	}
	wait_child_t();
	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
}
