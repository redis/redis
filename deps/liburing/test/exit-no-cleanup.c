/* SPDX-License-Identifier: MIT */
/*
 * Test case testing exit without cleanup and io-wq work pending or queued.
 *
 * From Florian Fischer <florian.fl.fischer@fau.de>
 * Link: https://lore.kernel.org/io-uring/20211202165606.mqryio4yzubl7ms5@pasture/
 *
 */
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "liburing.h"
#include "helpers.h"

#define IORING_ENTRIES 8

static pthread_t *threads;
static pthread_barrier_t init_barrier;
static int sleep_fd, notify_fd;
static sem_t sem;

static void *thread_func(void *arg)
{
	struct io_uring ring;
	int res;

	res = io_uring_queue_init(IORING_ENTRIES, &ring, 0);
	if (res)
		err(EXIT_FAILURE, "io_uring_queue_init failed");

	pthread_barrier_wait(&init_barrier);

	for(;;) {
		struct io_uring_cqe *cqe;
		struct io_uring_sqe *sqe;
		uint64_t buf;
		int res;

		sqe = io_uring_get_sqe(&ring);
		assert(sqe);

		io_uring_prep_read(sqe, sleep_fd, &buf, sizeof(buf), 0);

		res = io_uring_submit_and_wait(&ring, 1);
		if (res < 0)
			err(EXIT_FAILURE, "io_uring_submit_and_wait failed");

		res = io_uring_peek_cqe(&ring, &cqe);
		assert(!res);
		if (cqe->res < 0) {
			errno = -cqe->res;
			err(EXIT_FAILURE, "read failed");
		}
		assert(cqe->res == sizeof(buf));

		sem_post(&sem);

		io_uring_cqe_seen(&ring, cqe);
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	int res, fds[2], i, cpus;
	const uint64_t n = 0x42;

	if (argc > 1)
		return T_EXIT_SKIP;

	cpus = get_nprocs();
	res = pthread_barrier_init(&init_barrier, NULL, cpus);
	if (res)
		err(EXIT_FAILURE, "pthread_barrier_init failed");

	res = sem_init(&sem, 0, 0);
	if (res)
		err(EXIT_FAILURE, "sem_init failed");

	threads = t_malloc(sizeof(pthread_t) * cpus);

	res = pipe(fds);
	if (res)
		err(EXIT_FAILURE, "pipe failed");

	sleep_fd = fds[0];
	notify_fd = fds[1];

	for (i = 0; i < cpus; i++) {
		errno = pthread_create(&threads[i], NULL, thread_func, NULL);
		if (errno)
			err(EXIT_FAILURE, "pthread_create failed");
	}

	// Write #cpus notifications
	for (i = 0; i < cpus; i++) {
		res = write(notify_fd, &n, sizeof(n));
		if (res < 0)
			err(EXIT_FAILURE, "write failed");
		assert(res == sizeof(n));
	}

	// Await that all notifications were received
	for (i = 0; i < cpus; i++)
		sem_wait(&sem);

	// Exit without resource cleanup
	exit(EXIT_SUCCESS);
}
