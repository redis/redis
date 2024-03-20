/* SPDX-License-Identifier: MIT */
/*
 * Description: test that sigfd reading/polling works. A regression test for
 * the upstream commit:
 *
 * fd7d6de22414 ("io_uring: don't recurse on tsk->sighand->siglock with signalfd")
 */
#include <unistd.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <poll.h>
#include <stdio.h>
#include "liburing.h"
#include "helpers.h"

static int setup_signal(void)
{
	sigset_t mask;
	int sfd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);

	sigprocmask(SIG_BLOCK, &mask, NULL);
	sfd = signalfd(-1, &mask, SFD_NONBLOCK);
	if (sfd < 0)
		perror("signalfd");
	return sfd;
}

static int test_uring(int sfd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init(32, &ring, 0);
	if (ret)
		return T_EXIT_FAIL;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, sfd, POLLIN);
	ret = io_uring_submit(&ring);
	if (ret < 0) {
		ret = T_EXIT_FAIL;
		goto err_exit;
	}

	kill(getpid(), SIGINT);

	io_uring_wait_cqe(&ring, &cqe);
	if (cqe->res == -EOPNOTSUPP) {
		fprintf(stderr, "signalfd poll not supported\n");
		ret = T_EXIT_SKIP;
	} else if (cqe->res < 0) {
		fprintf(stderr, "poll failed: %d\n", cqe->res);
		ret = T_EXIT_FAIL;
	} else if (cqe->res & POLLIN) {
		ret = T_EXIT_PASS;
	} else {
		fprintf(stderr, "Unexpected poll mask %x\n", cqe->res);
		ret = T_EXIT_FAIL;
	}
	io_uring_cqe_seen(&ring, cqe);
err_exit:
	io_uring_queue_exit(&ring);
	return ret;
}

int main(int argc, char *argv[])
{
	int sfd, ret;

	if (argc > 1)
		return T_EXIT_PASS;

	sfd = setup_signal();
	if (sfd < 0)
		return T_EXIT_FAIL;

	ret = test_uring(sfd);
	if (ret == T_EXIT_FAIL)
		fprintf(stderr, "test_uring signalfd failed\n");

	close(sfd);
	return ret;
}
