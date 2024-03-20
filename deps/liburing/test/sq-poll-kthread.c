/* SPDX-License-Identifier: MIT */
/*
 * Description: test if io_uring SQ poll kthread is stopped when the userspace
 *              process ended with or without closing the io_uring fd
 *
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/epoll.h>

#include "liburing.h"
#include "helpers.h"

#define SQ_THREAD_IDLE  2000
#define BUF_SIZE        128
#define KTHREAD_NAME    "io_uring-sq"

enum {
	TEST_OK = 0,
	TEST_SKIPPED = 1,
	TEST_FAILED = 2,
};

static int do_test_sq_poll_kthread_stopped(bool do_exit)
{
	int ret = 0, pipe1[2];
	struct io_uring_params param;
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	uint8_t buf[BUF_SIZE];
	struct iovec iov;

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return TEST_FAILED;
	}

	memset(&param, 0, sizeof(param));
	param.flags |= IORING_SETUP_SQPOLL;
	param.sq_thread_idle = SQ_THREAD_IDLE;

	ret = t_create_ring_params(16, &ring, &param);
	if (ret == T_SETUP_SKIP) {
		ret = TEST_FAILED;
		goto err_pipe;
	} else if (ret != T_SETUP_OK) {
		fprintf(stderr, "ring setup failed\n");
		ret = TEST_FAILED;
		goto err_pipe;
	}

	ret = io_uring_register_files(&ring, &pipe1[1], 1);
	if (ret) {
		fprintf(stderr, "file reg failed: %d\n", ret);
		ret = TEST_FAILED;
		goto err_uring;
	}

	iov.iov_base = buf;
	iov.iov_len = BUF_SIZE;

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "io_uring_get_sqe failed\n");
		ret = TEST_FAILED;
		goto err_uring;
	}

	io_uring_prep_writev(sqe, 0, &iov, 1, 0);
	sqe->flags |= IOSQE_FIXED_FILE;

	ret = io_uring_submit(&ring);
	if (ret < 0) {
		fprintf(stderr, "io_uring_submit failed - ret: %d\n",
			ret);
		ret = TEST_FAILED;
		goto err_uring;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "io_uring_wait_cqe - ret: %d\n",
			ret);
		ret = TEST_FAILED;
		goto err_uring;
	}

	if (cqe->res != BUF_SIZE) {
		fprintf(stderr, "unexpected cqe->res %d [expected %d]\n",
			cqe->res, BUF_SIZE);
		ret = TEST_FAILED;
		goto err_uring;

	}

	io_uring_cqe_seen(&ring, cqe);

	ret = TEST_OK;

err_uring:
	if (do_exit)
		io_uring_queue_exit(&ring);
err_pipe:
	close(pipe1[0]);
	close(pipe1[1]);

	return ret;
}

static int test_sq_poll_kthread_stopped(bool do_exit)
{
	pid_t pid;
	int status = 0;

	pid = fork();

	if (pid == 0) {
		int ret = do_test_sq_poll_kthread_stopped(do_exit);
		exit(ret);
	}

	pid = wait(&status);
	if (status != 0)
		return WEXITSTATUS(status);

	sleep(1);
	if (system("ps --ppid 2 | grep " KTHREAD_NAME) == 0) {
		fprintf(stderr, "%s kthread still running!\n", KTHREAD_NAME);
		return TEST_FAILED;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_sq_poll_kthread_stopped(true);
	if (ret == TEST_SKIPPED) {
		printf("test_sq_poll_kthread_stopped_exit: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_sq_poll_kthread_stopped_exit failed\n");
		return ret;
	}

	ret = test_sq_poll_kthread_stopped(false);
	if (ret == TEST_SKIPPED) {
		printf("test_sq_poll_kthread_stopped_noexit: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_sq_poll_kthread_stopped_noexit failed\n");
		return ret;
	}

	return 0;
}
