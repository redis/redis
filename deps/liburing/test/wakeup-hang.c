/* SPDX-License-Identifier: MIT */
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include "liburing.h"

struct thread_data {
	struct io_uring *ring;
	int write_fd;
};

static void error_exit(char *message)
{
	perror(message);
	exit(1);
}

static void *listener_thread(void *data)
{
	struct thread_data *td = data;
	struct io_uring_cqe *cqe;
	int ret;

        ret = io_uring_wait_cqe(td->ring, &cqe);
        if (ret < 0) {
        	fprintf(stderr, "Error waiting for completion: %s\n",
                	strerror(-ret));
		goto err;
        }
	if (cqe->res < 0) {
		fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
		goto err;
        }
	io_uring_cqe_seen(td->ring, cqe);
	return NULL;
err:
	return (void *) 1;
}

static void *wakeup_io_uring(void *data)
{
	struct thread_data *td = data;
	int res;

	res = eventfd_write(td->write_fd, (eventfd_t) 1L);
	if (res < 0) {
		perror("eventfd_write");
		return (void *) 1;
	}
	return NULL;
}

static int test_pipes(void)
{
	struct io_uring_sqe *sqe;
	struct thread_data td;
	struct io_uring ring;
	pthread_t t1, t2;
	int ret, fds[2];
	void *pret;

	if (pipe(fds) < 0)
		error_exit("eventfd");

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
		return 1;
	}

	td.write_fd = fds[1];
	td.ring = &ring;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, fds[0], POLLIN);
	sqe->user_data = 2;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "ring_submit=%d\n", ret);
		return 1;
	}

	pthread_create(&t1, NULL, listener_thread, &td);

	sleep(1);

	pthread_create(&t2, NULL, wakeup_io_uring, &td);
	pthread_join(t1, &pret);

	io_uring_queue_exit(&ring);
	return pret != NULL;
}

static int test_eventfd(void)
{
	struct io_uring_sqe *sqe;
	struct thread_data td;
	struct io_uring ring;
	pthread_t t1, t2;
	int efd, ret;
	void *pret;

	efd = eventfd(0, 0);
	if (efd < 0)
		error_exit("eventfd");

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
		return 1;
	}

	td.write_fd = efd;
	td.ring = &ring;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, efd, POLLIN);
	sqe->user_data = 2;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "ring_submit=%d\n", ret);
		return 1;
	}

	pthread_create(&t1, NULL, listener_thread, &td);

	sleep(1);

	pthread_create(&t2, NULL, wakeup_io_uring, &td);
	pthread_join(t1, &pret);

	io_uring_queue_exit(&ring);
	return pret != NULL;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_pipes();
	if (ret) {
		fprintf(stderr, "test_pipe failed\n");
		return ret;
	}

	ret = test_eventfd();
	if (ret) {
		fprintf(stderr, "test_eventfd failed\n");
		return ret;
	}

	return 0;
}
