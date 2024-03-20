/* SPDX-License-Identifier: MIT */
/*
 * Test reads that will punt to blocking context, with immediate overwrite
 * of iovec->iov_base to NULL. If the kernel doesn't properly handle
 * reuse of the iovec, we should get -EFAULT.
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>

#include "helpers.h"
#include "liburing.h"

#define STR_SIZE	32768
#define FILE_SIZE	65536

struct thread_data {
	int fd1, fd2;
	volatile int do_exit;
};

static void *flusher(void *__data)
{
	struct thread_data *data = __data;

	while (!data->do_exit) {
		posix_fadvise(data->fd1, 0, FILE_SIZE, POSIX_FADV_DONTNEED);
		posix_fadvise(data->fd2, 0, FILE_SIZE, POSIX_FADV_DONTNEED);
		usleep(10);
	}

	return NULL;
}

static char str1[STR_SIZE];
static char str2[STR_SIZE];

static struct io_uring ring;

static int no_stable;

static int prep(int fd, char *str, int split, int async)
{
	struct io_uring_sqe *sqe;
	struct iovec iovs[16];
	int ret, i;

	if (split) {
		int vsize = STR_SIZE / 16;
		void *ptr = str;

		for (i = 0; i < 16; i++) {
			iovs[i].iov_base = ptr;
			iovs[i].iov_len = vsize;
			ptr += vsize;
		}
	} else {
		iovs[0].iov_base = str;
		iovs[0].iov_len = STR_SIZE;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_readv(sqe, fd, iovs, split ? 16 : 1, 0);
	sqe->user_data = fd;
	if (async)
		sqe->flags = IOSQE_ASYNC;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit got %d\n", ret);
		return 1;
	}
	if (split) {
		for (i = 0; i < 16; i++)
			iovs[i].iov_base = NULL;
	} else {
		iovs[0].iov_base = NULL;
	}
	return 0;
}

static int wait_nr(int nr)
{
	int i, ret;

	for (i = 0; i < nr; i++) {
		struct io_uring_cqe *cqe;

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret)
			return ret;
		if (cqe->res < 0) {
			fprintf(stderr, "cqe->res=%d\n", cqe->res);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	return 0;
}

static unsigned long long mtime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000;
	usec /= 1000;
	return sec + usec;
}

static unsigned long long mtime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return mtime_since(tv, &end);
}

static int test_reuse(int argc, char *argv[], int split, int async)
{
	struct thread_data data;
	struct io_uring_params p = { };
	int fd1, fd2, ret, i;
	struct timeval tv;
	pthread_t thread;
	char *fname1 = ".reuse.1";
	int do_unlink = 1;
	void *tret;

	ret = io_uring_queue_init_params(32, &ring, &p);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init: %d\n", ret);
		return 1;
	}

	if (!(p.features & IORING_FEAT_SUBMIT_STABLE)) {
		fprintf(stdout, "FEAT_SUBMIT_STABLE not there, skipping\n");
		io_uring_queue_exit(&ring);
		no_stable = 1;
		return 0;
	}

	if (argc > 1) {
		fname1 = argv[1];
		do_unlink = 0;
	} else {
		t_create_file(fname1, FILE_SIZE);
	}

	fd1 = open(fname1, O_RDONLY);
	if (do_unlink)
		unlink(fname1);
	if (fd1 < 0) {
		perror("open fname1");
		goto err;
	}

	t_create_file(".reuse.2", FILE_SIZE);
	fd2 = open(".reuse.2", O_RDONLY);
	unlink(".reuse.2");
	if (fd2 < 0) {
		perror("open .reuse.2");
		goto err;
	}

	data.fd1 = fd1;
	data.fd2 = fd2;
	data.do_exit = 0;
	pthread_create(&thread, NULL, flusher, &data);
	usleep(10000);

	gettimeofday(&tv, NULL);
	for (i = 0; i < 1000; i++) {
		ret = prep(fd1, str1, split, async);
		if (ret) {
			fprintf(stderr, "prep1 failed: %d\n", ret);
			goto err;
		}
		ret = prep(fd2, str2, split, async);
		if (ret) {
			fprintf(stderr, "prep1 failed: %d\n", ret);
			goto err;
		}
		ret = wait_nr(2);
		if (ret) {
			fprintf(stderr, "wait_nr: %d\n", ret);
			goto err;
		}
		if (mtime_since_now(&tv) > 5000)
			break;
	}

	data.do_exit = 1;
	pthread_join(thread, &tret);

	close(fd2);
	close(fd1);
	io_uring_queue_exit(&ring);
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;

}

int main(int argc, char *argv[])
{
	int ret, i;

	for (i = 0; i < 4; i++) {
		int split, async;

		split = (i & 1) != 0;
		async = (i & 2) != 0;

		ret = test_reuse(argc, argv, split, async);
		if (ret) {
			fprintf(stderr, "test_reuse %d %d failed\n", split, async);
			return ret;
		}
		if (no_stable)
			break;
	}

	return 0;
}
