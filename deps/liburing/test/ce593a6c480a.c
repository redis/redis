/* SPDX-License-Identifier: MIT */
/*
 * Test 5.7 regression with task_work not being run while a task is
 * waiting on another event in the kernel.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include "liburing.h"
#include "helpers.h"

static int use_sqpoll = 0;

static void notify_fd(int fd)
{
	char buf[8] = {0, 0, 0, 0, 0, 0, 1};
	int ret;

	ret = write(fd, &buf, 8);
	if (ret < 0)
		perror("write");
}

static void *delay_set_fd_from_thread(void *data)
{
	int fd = (intptr_t) data;

	sleep(1);
	notify_fd(fd);
	return NULL;
}

int main(int argc, char *argv[])
{
	struct io_uring_params p = {};
	struct io_uring ring;
	int loop_fd, other_fd;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;
	int ret, use_fd;
	char buf[8] = {0, 0, 0, 0, 0, 0, 1};
	pthread_t tid;

	if (argc > 1)
		return T_EXIT_SKIP;

	/* Create an eventfd to be registered with the loop to be
	 * notified of events being ready
	 */
	loop_fd = eventfd(0, EFD_CLOEXEC);
	if (loop_fd == -1) {
		fprintf(stderr, "eventfd errno=%d\n", errno);
		return T_EXIT_FAIL;
	}

	/* Create an eventfd that can create events */
	use_fd = other_fd = eventfd(0, EFD_CLOEXEC);
	if (other_fd == -1) {
		fprintf(stderr, "eventfd errno=%d\n", errno);
		return T_EXIT_FAIL;
	}

	if (use_sqpoll)
		p.flags = IORING_SETUP_SQPOLL;

	/* Setup the ring with a registered event fd to be notified on events */
	ret = t_create_ring_params(8, &ring, &p);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_PASS;
	else if (ret < 0)
		return ret;

	ret = io_uring_register_eventfd(&ring, loop_fd);
	if (ret < 0) {
		fprintf(stderr, "register_eventfd=%d\n", ret);
		return T_EXIT_FAIL;
	}

	if (use_sqpoll) {
		ret = io_uring_register_files(&ring, &other_fd, 1);
		if (ret < 0) {
			fprintf(stderr, "register_files=%d\n", ret);
			return T_EXIT_FAIL;
		}
		use_fd = 0;
	}

	/* Submit a poll operation to wait on an event in other_fd */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, use_fd, POLLIN);
	sqe->user_data = 1;
	if (use_sqpoll)
		sqe->flags |= IOSQE_FIXED_FILE;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit=%d\n", ret);
		return T_EXIT_FAIL;
	}

	/*
	 * CASE 3: Hangs forever in Linux 5.7.5; Works in Linux 5.6.0 When this
	 * code is uncommented, we don't se a notification on other_fd until
	 * _after_ we have started the read on loop_fd. In that case, the read() on
	 * loop_fd seems to hang forever.
	*/
    	pthread_create(&tid, NULL, delay_set_fd_from_thread,
			(void*) (intptr_t) other_fd);

	/* Wait on the event fd for an event to be ready */
	do {
		ret = read(loop_fd, buf, 8);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		perror("read");
		return T_EXIT_FAIL;
	} else if (ret != 8) {
		fprintf(stderr, "Odd-sized eventfd read: %d\n", ret);
		return T_EXIT_FAIL;
	}


	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe=%d\n", ret);
		return ret;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "cqe->res=%d\n", cqe->res);
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(&ring, cqe);
	return T_EXIT_PASS;
}
