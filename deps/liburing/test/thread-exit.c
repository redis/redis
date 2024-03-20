/* SPDX-License-Identifier: MIT */
/*
 * Description: test that thread pool issued requests don't cancel on thread
 *		exit, but do get canceled once the parent exits. Do both
 *		writes that finish and a poll request that sticks around.
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>

#include "helpers.h"
#include "liburing.h"

#define NR_IOS	8
#define WSIZE	512

struct d {
	int fd;
	struct io_uring *ring;
	unsigned long off;
	int pipe_fd;
	int err;
	int i;
};

static char *g_buf[NR_IOS] = {NULL};

static void free_g_buf(void)
{
	int i;
	for (i = 0; i < NR_IOS; i++)
		free(g_buf[i]);
}

static void *do_io(void *data)
{
	struct d *d = data;
	struct io_uring_sqe *sqe;
	char *buffer;
	int ret;

	buffer = t_malloc(WSIZE);
	g_buf[d->i] = buffer;
	memset(buffer, 0x5a, WSIZE);
	sqe = io_uring_get_sqe(d->ring);
	if (!sqe) {
		d->err++;
		return NULL;
	}
	io_uring_prep_write(sqe, d->fd, buffer, WSIZE, d->off);
	sqe->user_data = d->off;

	sqe = io_uring_get_sqe(d->ring);
	if (!sqe) {
		d->err++;
		return NULL;
	}
	io_uring_prep_poll_add(sqe, d->pipe_fd, POLLIN);

	ret = io_uring_submit(d->ring);
	if (ret != 2)
		d->err++;
	return NULL;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	const char *fname;
	pthread_t thread;
	int ret, do_unlink, i, fd;
	struct d d;
	int fds[2];

	if (pipe(fds) < 0) {
		perror("pipe");
		return 1;
	}

	ret = io_uring_queue_init(32, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	if (argc > 1) {
		fname = argv[1];
		do_unlink = 0;
	} else {
		fname = ".thread.exit";
		do_unlink = 1;
		t_create_file(fname, 4096);
	}

	fd = open(fname, O_WRONLY);
	if (do_unlink)
		unlink(fname);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	d.fd = fd;
	d.ring = &ring;
	d.off = 0;
	d.pipe_fd = fds[0];
	d.err = 0;
	for (i = 0; i < NR_IOS; i++) {
		d.i = i;
		memset(&thread, 0, sizeof(thread));
		pthread_create(&thread, NULL, do_io, &d);
		pthread_join(thread, NULL);
		d.off += WSIZE;
	}

	for (i = 0; i < NR_IOS; i++) {
		struct io_uring_cqe *cqe;

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "io_uring_wait_cqe=%d\n", ret);
			goto err;
		}
		if (cqe->res != WSIZE) {
			fprintf(stderr, "cqe->res=%d, Expected %d\n", cqe->res,
								WSIZE);
			goto err;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	free_g_buf();
	return d.err;
err:
	free_g_buf();
	return 1;
}
