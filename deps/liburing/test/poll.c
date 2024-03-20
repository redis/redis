/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring poll handling
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <assert.h>

#include "helpers.h"
#include "liburing.h"

static void do_setsockopt(int fd, int level, int optname, int val)
{
	if (setsockopt(fd, level, optname, &val, sizeof(val)))
		t_error(1, errno, "setsockopt %d.%d: %d", level, optname, val);
}

static bool check_cq_empty(struct io_uring *ring)
{
	struct io_uring_cqe *cqe = NULL;
	int ret;

	ret = io_uring_peek_cqe(ring, &cqe); /* nothing should be there */
	return ret == -EAGAIN;
}

static int test_basic(void)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int pipe1[2];
	pid_t p;
	int ret;

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return 1;
	}

	p = fork();
	if (p == -1) {
		perror("fork");
		exit(2);
	} else if (p == 0) {
		ret = io_uring_queue_init(1, &ring, 0);
		if (ret) {
			fprintf(stderr, "child: ring setup failed: %d\n", ret);
			return 1;
		}

		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			return 1;
		}

		io_uring_prep_poll_add(sqe, pipe1[0], POLLIN);
		io_uring_sqe_set_data(sqe, sqe);

		ret = io_uring_submit(&ring);
		if (ret <= 0) {
			fprintf(stderr, "child: sqe submit failed: %d\n", ret);
			return 1;
		}

		do {
			ret = io_uring_wait_cqe(&ring, &cqe);
			if (ret < 0) {
				fprintf(stderr, "child: wait completion %d\n", ret);
				break;
			}
			io_uring_cqe_seen(&ring, cqe);
		} while (ret != 0);

		if (ret < 0)
			return 1;
		if (cqe->user_data != (unsigned long) sqe) {
			fprintf(stderr, "child: cqe doesn't match sqe\n");
			return 1;
		}
		if ((cqe->res & POLLIN) != POLLIN) {
			fprintf(stderr, "child: bad return value %ld\n",
							(long) cqe->res);
			return 1;
		}

		io_uring_queue_exit(&ring);
		exit(0);
	}

	do {
		errno = 0;
		ret = write(pipe1[1], "foo", 3);
	} while (ret == -1 && errno == EINTR);

	if (ret != 3) {
		fprintf(stderr, "parent: bad write return %d\n", ret);
		return 1;
	}
	close(pipe1[0]);
	close(pipe1[1]);
	return 0;
}

static int test_missing_events(void)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int i, ret, sp[2];
	char buf[2] = {};
	int res_mask = 0;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_DEFER_TASKRUN);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
		perror("Failed to create Unix-domain socket pair\n");
		return 1;
	}
	do_setsockopt(sp[0], SOL_SOCKET, SO_SNDBUF, 1);
	ret = send(sp[0], buf, sizeof(buf), 0);
	if (ret != sizeof(buf)) {
		perror("send failed\n");
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_multishot(sqe, sp[0], POLLIN|POLLOUT);
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return 1;
	}

	/* trigger EPOLLIN */
	ret = send(sp[1], buf, sizeof(buf), 0);
	if (ret != sizeof(buf)) {
		fprintf(stderr, "send sp[1] failed %i %i\n", ret, errno);
		return 1;
	}

	/* trigger EPOLLOUT */
	ret = recv(sp[1], buf, sizeof(buf), 0);
	if (ret != sizeof(buf)) {
		perror("recv failed\n");
		return 1;
	}

	for (i = 0; ; i++) {
		if (i == 0)
			ret = io_uring_wait_cqe(&ring, &cqe);
		else
			ret = io_uring_peek_cqe(&ring, &cqe);

		if (i != 0 && ret == -EAGAIN) {
			break;
		}
		if (ret) {
			fprintf(stderr, "wait completion %d, %i\n", ret, i);
			return 1;
		}
		res_mask |= cqe->res;
		io_uring_cqe_seen(&ring, cqe);
	}

	if ((res_mask & (POLLIN|POLLOUT)) != (POLLIN|POLLOUT)) {
		fprintf(stderr, "missing poll events %i\n", res_mask);
		return 1;
	}
	io_uring_queue_exit(&ring);
	close(sp[0]);
	close(sp[1]);
	return 0;
}

#define NR_SQES		2048

static int test_self_poll(void)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int ret, i, j;

	ret = io_uring_queue_init(NR_SQES, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	for (j = 0; j < 32; j++) {
		for (i = 0; i < NR_SQES; i++) {
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_poll_add(sqe, ring.ring_fd, POLLIN);
		}

		ret = io_uring_submit(&ring);
		assert(ret == NR_SQES);
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);
	ret = io_uring_submit(&ring);
	assert(ret == 1);

	ret = io_uring_wait_cqe(&ring, &cqe);
	io_uring_cqe_seen(&ring, cqe);

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
}

static int test_disabled_ring_lazy_polling(int early_poll)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring, ring2;
	unsigned head;
	int ret, i = 0;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					     IORING_SETUP_DEFER_TASKRUN |
					     IORING_SETUP_R_DISABLED);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}
	ret = io_uring_queue_init(8, &ring2, 0);
	if (ret) {
		fprintf(stderr, "ring2 setup failed: %d\n", ret);
		return 1;
	}

	if (early_poll) {
		/* start polling disabled DEFER_TASKRUN ring */
		sqe = io_uring_get_sqe(&ring2);
		io_uring_prep_poll_add(sqe, ring.ring_fd, POLLIN);
		ret = io_uring_submit(&ring2);
		assert(ret == 1);
		assert(check_cq_empty(&ring2));
	}

	/* enable rings, which should also activate pollwq */
	ret = io_uring_enable_rings(&ring);
	assert(ret >= 0);

	if (!early_poll) {
		/* start polling enabled DEFER_TASKRUN ring */
		sqe = io_uring_get_sqe(&ring2);
		io_uring_prep_poll_add(sqe, ring.ring_fd, POLLIN);
		ret = io_uring_submit(&ring2);
		assert(ret == 1);
		assert(check_cq_empty(&ring2));
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);
	ret = io_uring_submit(&ring);
	assert(ret == 1);

	io_uring_for_each_cqe(&ring2, head, cqe) {
		i++;
	}
	if (i !=  1) {
		fprintf(stderr, "fail, polling stuck\n");
		return 1;
	}
	io_uring_queue_exit(&ring);
	io_uring_queue_exit(&ring2);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_basic();
	if (ret) {
		fprintf(stderr, "test_basic() failed %i\n", ret);
		return T_EXIT_FAIL;
	}


	if (t_probe_defer_taskrun()) {
		ret = test_missing_events();
		if (ret) {
			fprintf(stderr, "test_missing_events() failed %i\n", ret);
			return T_EXIT_FAIL;
		}

		ret = test_disabled_ring_lazy_polling(false);
		if (ret) {
			fprintf(stderr, "test_disabled_ring_lazy_polling(false) failed %i\n", ret);
			return T_EXIT_FAIL;
		}

		ret = test_disabled_ring_lazy_polling(true);
		if (ret) {
			fprintf(stderr, "test_disabled_ring_lazy_polling(true) failed %i\n", ret);
			return T_EXIT_FAIL;
		}
	}

	ret = test_self_poll();
	if (ret) {
		fprintf(stderr, "test_self_poll failed\n");
		return T_EXIT_FAIL;
	}

	return 0;
}
