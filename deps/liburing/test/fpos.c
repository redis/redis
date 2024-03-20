/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring fpos handling
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE 5000
#define QUEUE_SIZE 2048

static void create_file(const char *file, size_t size)
{
	ssize_t ret;
	char *buf;
	size_t idx;
	int fd;

	buf = t_malloc(size);
	for (idx = 0; idx < size; ++idx) {
		/* write 0 or 1 */
		buf[idx] = (unsigned char)(idx & 0x01);
	}

	fd = open(file, O_WRONLY | O_CREAT, 0644);
	assert(fd >= 0);

	ret = write(fd, buf, size);
	fsync(fd);
	close(fd);
	free(buf);
	assert(ret == size);
}

static int test_read(struct io_uring *ring, bool async, int blocksize)
{
	int ret, fd, i;
	bool done = false;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	loff_t current, expected = 0;
	int count_ok;
	int count_0 = 0, count_1 = 0;
	unsigned char buff[QUEUE_SIZE * blocksize];
	unsigned char reordered[QUEUE_SIZE * blocksize];

	memset(buff, 0, QUEUE_SIZE * blocksize);
	memset(reordered, 0, QUEUE_SIZE * blocksize);

	create_file(".test_fpos_read", FILE_SIZE);
	fd = open(".test_fpos_read", O_RDONLY);
	unlink(".test_fpos_read");
	assert(fd >= 0);

	while (!done) {
		for (i = 0; i < QUEUE_SIZE; ++i) {
			sqe = io_uring_get_sqe(ring);
			if (!sqe) {
				fprintf(stderr, "no sqe\n");
				return -1;
			}
			io_uring_prep_read(sqe, fd,
					buff + i * blocksize,
					blocksize, -1);
			sqe->user_data = i;
			if (async)
				sqe->flags |= IOSQE_ASYNC;
			if (i != QUEUE_SIZE - 1)
				sqe->flags |= IOSQE_IO_LINK;
		}
		ret = io_uring_submit_and_wait(ring, QUEUE_SIZE);
		if (ret != QUEUE_SIZE) {
			fprintf(stderr, "submit failed: %d\n", ret);
			return 1;
		}
		count_ok  = 0;
		for (i = 0; i < QUEUE_SIZE; ++i) {
			int res;

			ret = io_uring_peek_cqe(ring, &cqe);
			if (ret) {
				fprintf(stderr, "peek failed: %d\n", ret);
				return ret;
			}
			assert(cqe->user_data < QUEUE_SIZE);
			memcpy(reordered + count_ok,
				buff + cqe->user_data * blocksize, blocksize);
			res = cqe->res;
			io_uring_cqe_seen(ring, cqe);
			if (res == 0) {
				done = true;
			} else if (res == -ECANCELED) {
				/* cancelled, probably ok */
			} else if (res < 0 || res > blocksize) {
				fprintf(stderr, "bad read: %d\n", res);
				return -1;
			} else {
				expected += res;
				count_ok += res;
			}
		}
		ret = 0;
		for (i = 0; i < count_ok; i++) {
			if (reordered[i] == 1) {
				count_1++;
			} else if (reordered[i] == 0) {
				count_0++;
			} else {
				fprintf(stderr, "odd read %d\n",
						(int)reordered[i]);
				ret = -1;
				break;
			}
		}
		if (labs(count_1 - count_0) > 1) {
			fprintf(stderr, "inconsistent reads, got 0s:%d 1s:%d\n",
					count_0, count_1);
			ret = -1;
		}
		current = lseek(fd, 0, SEEK_CUR);
		if (current != expected) {
			fprintf(stderr, "f_pos incorrect, expected %ld have %ld\n",
					(long) expected, (long) current);
			ret = -1;
		}
		if (ret)
			return ret;
	}
	return 0;
}


static int test_write(struct io_uring *ring, bool async, int blocksize)
{
	int ret, fd, i;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	bool fail = false;
	loff_t current;
	char data[blocksize+1];
	char readbuff[QUEUE_SIZE*blocksize+1];

	fd = open(".test_fpos_write", O_RDWR | O_CREAT, 0644);
	unlink(".test_fpos_write");
	assert(fd >= 0);

	for (i = 0; i < blocksize; i++)
		data[i] = 'A' + i;

	data[blocksize] = '\0';

	for (i = 0; i < QUEUE_SIZE; ++i) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "no sqe\n");
			return -1;
		}
		io_uring_prep_write(sqe, fd, data + (i % blocksize), 1, -1);
		sqe->user_data = 1;
		if (async)
			sqe->flags |= IOSQE_ASYNC;
		if (i != QUEUE_SIZE - 1)
			sqe->flags |= IOSQE_IO_LINK;
	}
	ret = io_uring_submit_and_wait(ring, QUEUE_SIZE);
	if (ret != QUEUE_SIZE) {
		fprintf(stderr, "submit failed: %d\n", ret);
		return 1;
	}
	for (i = 0; i < QUEUE_SIZE; ++i) {
		int res;

		ret = io_uring_peek_cqe(ring, &cqe);
		res = cqe->res;
		if (ret) {
			fprintf(stderr, "peek failed: %d\n", ret);
			return ret;
		}
		io_uring_cqe_seen(ring, cqe);
		if (!fail && res != 1) {
			fprintf(stderr, "bad result %d\n", res);
			fail = true;
		}
	}
	current = lseek(fd, 0, SEEK_CUR);
	if (current != QUEUE_SIZE) {
		fprintf(stderr, "f_pos incorrect, expected %ld have %d\n",
				(long) current, QUEUE_SIZE);
		fail = true;
	}
	current = lseek(fd, 0, SEEK_SET);
	if (current != 0) {
		perror("seek to start");
		return -1;
	}
	ret = read(fd, readbuff, QUEUE_SIZE);
	if (ret != QUEUE_SIZE) {
		fprintf(stderr, "did not write enough: %d\n", ret);
		return -1;
	}
	i = 0;
	while (i < QUEUE_SIZE - blocksize) {
		if (strncmp(readbuff + i, data, blocksize)) {
			char bad[QUEUE_SIZE+1];

			memcpy(bad, readbuff + i, blocksize);
			bad[blocksize] = '\0';
			fprintf(stderr, "unexpected data %s\n", bad);
			fail = true;
		}
		i += blocksize;
	}

	return fail ? -1 : 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(QUEUE_SIZE, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;
	}

	for (int test = 0; test < 8; test++) {
		int async = test & 0x01;
		int write = test & 0x02;
		int blocksize = test & 0x04 ? 1 : 7;

		ret = write
			? test_write(&ring, !!async, blocksize)
			: test_read(&ring, !!async, blocksize);
		if (ret) {
			fprintf(stderr, "failed %s async=%d blocksize=%d\n",
					write ? "write" : "read",
					async, blocksize);
			return -1;
		}
	}
	return T_EXIT_PASS;
}
