/* SPDX-License-Identifier: MIT */

/*
 * Test that closed pipe reads returns 0, instead of waiting for more
 * data.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "liburing.h"

#define BUFSIZE	512

struct data {
	char *str;
	int fds[2];
};

static void *t(void *data)
{
	struct data *d = data;
	int ret;

	strcpy(d->str, "This is a test string");
	ret = write(d->fds[1], d->str, strlen(d->str));
	close(d->fds[1]);
	if (ret < 0)
		perror("write");

	return NULL;
}

int main(int argc, char *argv[])
{
	static char buf[BUFSIZE];
	struct io_uring ring;
	pthread_t thread;
	struct data d;
	int ret;

	if (pipe(d.fds) < 0) {
		perror("pipe");
		return 1;
	}
	d.str = buf;

	io_uring_queue_init(8, &ring, 0);

	pthread_create(&thread, NULL, t, &d);

	while (1) {
		struct io_uring_sqe *sqe;
		struct io_uring_cqe *cqe;

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_read(sqe, d.fds[0], buf, BUFSIZE, 0);
		ret = io_uring_submit(&ring);
		if (ret != 1) {
			fprintf(stderr, "submit: %d\n", ret);
			return 1;
		}
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait: %d\n", ret);
			return 1;
		}

		if (cqe->res < 0) {
			fprintf(stderr, "Read error: %s\n", strerror(-cqe->res));
			return 1;
		}
		if (cqe->res == 0)
			break;
		io_uring_cqe_seen(&ring, cqe);
	}

	pthread_join(thread, NULL);
	io_uring_queue_exit(&ring);
	return 0;
}
