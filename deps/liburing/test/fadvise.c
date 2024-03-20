/* SPDX-License-Identifier: MIT */
/*
 * Description: basic fadvise test
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>

#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE	(128 * 1024)
#define LOOPS		100
#define MIN_LOOPS	10

static unsigned long long utime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000000;
	return sec + usec;
}

static unsigned long long utime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return utime_since(tv, &end);
}

static int do_fadvise(struct io_uring *ring, int fd, off_t offset, off_t len,
		      int advice)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "failed to get sqe\n");
		return 1;
	}

	io_uring_prep_fadvise(sqe, fd, offset, len, advice);
	sqe->user_data = advice;
	ret = io_uring_submit_and_wait(ring, 1);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait: %d\n", ret);
		return 1;
	}

	ret = cqe->res;
	if (ret == -EINVAL || ret == -EBADF) {
		fprintf(stdout, "Fadvise not supported, skipping\n");
		unlink(".fadvise.tmp");
		exit(T_EXIT_SKIP);
	} else if (ret) {
		fprintf(stderr, "cqe->res=%d\n", cqe->res);
	}
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static long do_read(int fd, char *buf)
{
	struct timeval tv;
	int ret;
	long t;

	ret = lseek(fd, 0, SEEK_SET);
	if (ret) {
		perror("lseek");
		return -1;
	}
	
	gettimeofday(&tv, NULL);
	ret = read(fd, buf, FILE_SIZE);
	t = utime_since_now(&tv);
	if (ret < 0) {
		perror("read");
		return -1;
	} else if (ret != FILE_SIZE) {
		fprintf(stderr, "short read1: %d\n", ret);
		return -1;
	}

	return t;
}

static int test_fadvise(struct io_uring *ring, const char *filename)
{
	unsigned long cached_read, uncached_read, cached_read2;
	int fd, ret;
	char *buf;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	buf = t_malloc(FILE_SIZE);

	cached_read = do_read(fd, buf);
	if (cached_read == -1)
		return 1;

	ret = do_fadvise(ring, fd, 0, FILE_SIZE, POSIX_FADV_DONTNEED);
	if (ret)
		return 1;

	uncached_read = do_read(fd, buf);
	if (uncached_read == -1)
		return 1;

	ret = do_fadvise(ring, fd, 0, FILE_SIZE, POSIX_FADV_DONTNEED);
	if (ret)
		return 1;

	ret = do_fadvise(ring, fd, 0, FILE_SIZE, POSIX_FADV_WILLNEED);
	if (ret)
		return 1;

	fsync(fd);

	cached_read2 = do_read(fd, buf);
	if (cached_read2 == -1)
		return 1;

	if (cached_read < uncached_read &&
	    cached_read2 < uncached_read)
		return 0;

	return 2;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret, i, good, bad;
	char *fname;

	if (argc > 1) {
		fname = argv[1];
	} else {
		fname = ".fadvise.tmp";
		t_create_file(fname, FILE_SIZE);
	}
	if (io_uring_queue_init(8, &ring, 0)) {
		fprintf(stderr, "ring creation failed\n");
		goto err;
	}

	good = bad = 0;
	for (i = 0; i < LOOPS; i++) {
		ret = test_fadvise(&ring, fname);
		if (ret == 1) {
			fprintf(stderr, "read_fadvise failed\n");
			goto err;
		} else if (!ret)
			good++;
		else if (ret == 2)
			bad++;
		if (i >= MIN_LOOPS && !bad)
			break;
	}

	/* too hard to reliably test, just ignore */
	if ((0) && bad > good) {
		fprintf(stderr, "Suspicious timings\n");
		goto err;
	}

	if (fname != argv[1])
		unlink(fname);
	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
err:
	if (fname != argv[1])
		unlink(fname);
	return T_EXIT_FAIL;
}
