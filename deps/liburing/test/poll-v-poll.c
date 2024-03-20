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
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <pthread.h>
#include <sys/epoll.h>

#include "liburing.h"

struct thread_data {
	struct io_uring *ring;
	int fd;
	int events;
	const char *test;
	int out[2];
};

static void *epoll_wait_fn(void *data)
{
	struct thread_data *td = data;
	struct epoll_event ev;

	if (epoll_wait(td->fd, &ev, 1, -1) < 0) {
		perror("epoll_wait");
		goto err;
	}

	return NULL;
err:
	return (void *) 1;
}

static void *iou_poll(void *data)
{
	struct thread_data *td = data;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(td->ring);
	io_uring_prep_poll_add(sqe, td->fd, td->events);

	ret = io_uring_submit(td->ring);
	if (ret != 1) {
		fprintf(stderr, "submit got %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(td->ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe: %d\n", ret);
		goto err;
	}

	td->out[0] = cqe->res & 0x3f;
	io_uring_cqe_seen(td->ring, cqe);
	return NULL;
err:
	return (void *) 1;
}

static void *poll_pipe(void *data)
{
	struct thread_data *td = data;
	struct pollfd pfd;
	int ret;

	pfd.fd = td->fd;
	pfd.events = td->events;

	ret = poll(&pfd, 1, -1);
	if (ret < 0)
		perror("poll");

	td->out[1] = pfd.revents;
	return NULL;
}

static int do_pipe_pollin_test(struct io_uring *ring)
{
	struct thread_data td;
	pthread_t threads[2];
	int ret, pipe1[2];
	char buf;

	if (pipe(pipe1) < 0) {
		perror("pipe");
		return 1;
	}

	td.ring = ring;
	td.fd = pipe1[0];
	td.events = POLLIN;
	td.test = __FUNCTION__;

	pthread_create(&threads[1], NULL, iou_poll, &td);
	pthread_create(&threads[0], NULL, poll_pipe, &td);
	usleep(100000);

	buf = 0x89;
	ret = write(pipe1[1], &buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		fprintf(stderr, "write failed: %d\n", ret);
		return 1;
	}

	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);

	if (td.out[0] != td.out[1]) {
		fprintf(stderr, "%s: res %x/%x differ\n", __FUNCTION__,
							td.out[0], td.out[1]);
		return 1;
	}
	return 0;
}

static int do_pipe_pollout_test(struct io_uring *ring)
{
	struct thread_data td;
	pthread_t threads[2];
	int ret, pipe1[2];
	char buf;

	if (pipe(pipe1) < 0) {
		perror("pipe");
		return 1;
	}

	td.ring = ring;
	td.fd = pipe1[1];
	td.events = POLLOUT;
	td.test = __FUNCTION__;

	pthread_create(&threads[0], NULL, poll_pipe, &td);
	pthread_create(&threads[1], NULL, iou_poll, &td);
	usleep(100000);

	buf = 0x89;
	ret = write(pipe1[1], &buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		fprintf(stderr, "write failed: %d\n", ret);
		return 1;
	}

	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);

	if (td.out[0] != td.out[1]) {
		fprintf(stderr, "%s: res %x/%x differ\n", __FUNCTION__,
							td.out[0], td.out[1]);
		return 1;
	}

	return 0;
}

static int do_fd_test(struct io_uring *ring, const char *fname, int events)
{
	struct thread_data td;
	pthread_t threads[2];
	int fd;

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	td.ring = ring;
	td.fd = fd;
	td.events = events;
	td.test = __FUNCTION__;

	pthread_create(&threads[0], NULL, poll_pipe, &td);
	pthread_create(&threads[1], NULL, iou_poll, &td);

	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);

	if (td.out[0] != td.out[1]) {
		fprintf(stderr, "%s: res %x/%x differ\n", __FUNCTION__,
							td.out[0], td.out[1]);
		return 1;
	}

	return 0;
}

static int iou_epoll_ctl(struct io_uring *ring, int epfd, int fd,
			 struct epoll_event *ev)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "Failed to get sqe\n");
		return 1;
	}

	io_uring_prep_epoll_ctl(sqe, epfd, fd, EPOLL_CTL_ADD, ev);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe: %d\n", ret);
		return 1;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static int do_test_epoll(struct io_uring *ring, int iou_epoll_add)
{
	struct epoll_event ev;
	struct thread_data td;
	pthread_t threads[2];
	int ret, pipe1[2];
	char buf;
	int fd;

	fd = epoll_create1(0);
	if (fd < 0) {
		perror("epoll_create");
		return 1;
	}

	if (pipe(pipe1) < 0) {
		perror("pipe");
		return 1;
	}

	ev.events = EPOLLIN;
	ev.data.fd = pipe1[0];

	if (!iou_epoll_add) {
		if (epoll_ctl(fd, EPOLL_CTL_ADD, pipe1[0], &ev) < 0) {
			perror("epoll_ctrl");
			return 1;
		}
	} else {
		ret = iou_epoll_ctl(ring, fd, pipe1[0], &ev);
		if (ret == -EINVAL) {
			fprintf(stdout, "epoll not supported, skipping\n");
			return 0;
		} else if (ret < 0) {
			return 1;
		}
	}

	td.ring = ring;
	td.fd = fd;
	td.events = POLLIN;
	td.test = __FUNCTION__;

	pthread_create(&threads[0], NULL, iou_poll, &td);
	pthread_create(&threads[1], NULL, epoll_wait_fn, &td);
	usleep(100000);

	buf = 0x89;
	ret = write(pipe1[1], &buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		fprintf(stderr, "write failed: %d\n", ret);
		return 1;
	}

	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	const char *fname;
	int ret;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = do_pipe_pollin_test(&ring);
	if (ret) {
		fprintf(stderr, "pipe pollin test failed\n");
		return ret;
	}

	ret = do_pipe_pollout_test(&ring);
	if (ret) {
		fprintf(stderr, "pipe pollout test failed\n");
		return ret;
	}

	ret = do_test_epoll(&ring, 0);
	if (ret) {
		fprintf(stderr, "epoll test 0 failed\n");
		return ret;
	}

	ret = do_test_epoll(&ring, 1);
	if (ret) {
		fprintf(stderr, "epoll test 1 failed\n");
		return ret;
	}

	if (argc > 1)
		fname = argv[1];
	else
		fname = argv[0];

	ret = do_fd_test(&ring, fname, POLLIN);
	if (ret) {
		fprintf(stderr, "fd test IN failed\n");
		return ret;
	}

	ret = do_fd_test(&ring, fname, POLLOUT);
	if (ret) {
		fprintf(stderr, "fd test OUT failed\n");
		return ret;
	}

	ret = do_fd_test(&ring, fname, POLLOUT | POLLIN);
	if (ret) {
		fprintf(stderr, "fd test IN|OUT failed\n");
		return ret;
	}

	return 0;

}
