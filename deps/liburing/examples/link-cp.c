/* SPDX-License-Identifier: MIT */
/*
 * Very basic proof-of-concept for doing a copy with linked SQEs. Needs a
 * bit of error handling and short read love.
 */
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "liburing.h"

#define QD	64
#define BS	(32*1024)

struct io_data {
	size_t offset;
	int index;
	struct iovec iov;
};

static int infd, outfd;
static int inflight;

static int setup_context(unsigned entries, struct io_uring *ring)
{
	int ret;

	ret = io_uring_queue_init(entries, ring, 0);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		return -1;
	}

	return 0;
}

static int get_file_size(int fd, off_t *size)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return -1;
	if (S_ISREG(st.st_mode)) {
		*size = st.st_size;
		return 0;
	} else if (S_ISBLK(st.st_mode)) {
		unsigned long long bytes;

		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
			return -1;

		*size = bytes;
		return 0;
	}

	return -1;
}

static void queue_rw_pair(struct io_uring *ring, off_t size, off_t offset)
{
	struct io_uring_sqe *sqe;
	struct io_data *data;
	void *ptr;

	ptr = malloc(size + sizeof(*data));
	data = ptr + size;
	data->index = 0;
	data->offset = offset;
	data->iov.iov_base = ptr;
	data->iov.iov_len = size;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_readv(sqe, infd, &data->iov, 1, offset);
	sqe->flags |= IOSQE_IO_LINK;
	io_uring_sqe_set_data(sqe, data);

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_writev(sqe, outfd, &data->iov, 1, offset);
	io_uring_sqe_set_data(sqe, data);
}

static int handle_cqe(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct io_data *data = io_uring_cqe_get_data(cqe);
	int ret = 0;

	data->index++;

	if (cqe->res < 0) {
		if (cqe->res == -ECANCELED) {
			queue_rw_pair(ring, data->iov.iov_len, data->offset);
			inflight += 2;
		} else {
			printf("cqe error: %s\n", strerror(-cqe->res));
			ret = 1;
		}
	}

	if (data->index == 2) {
		void *ptr = (void *) data - data->iov.iov_len;

		free(ptr);
	}
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static int copy_file(struct io_uring *ring, off_t insize)
{
	struct io_uring_cqe *cqe;
	off_t this_size;
	off_t offset;

	offset = 0;
	while (insize) {
		int has_inflight = inflight;
		int depth;

		while (insize && inflight < QD) {
			this_size = BS;
			if (this_size > insize)
				this_size = insize;
			queue_rw_pair(ring, this_size, offset);
			offset += this_size;
			insize -= this_size;
			inflight += 2;
		}

		if (has_inflight != inflight)
			io_uring_submit(ring);

		if (insize)
			depth = QD;
		else
			depth = 1;
		while (inflight >= depth) {
			int ret;

			ret = io_uring_wait_cqe(ring, &cqe);
			if (ret < 0) {
				printf("wait cqe: %s\n", strerror(-ret));
				return 1;
			}
			if (handle_cqe(ring, cqe))
				return 1;
			inflight--;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	off_t insize;
	int ret;

	if (argc < 3) {
		printf("%s: infile outfile\n", argv[0]);
		return 1;
	}

	infd = open(argv[1], O_RDONLY);
	if (infd < 0) {
		perror("open infile");
		return 1;
	}
	outfd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (outfd < 0) {
		perror("open outfile");
		return 1;
	}

	if (setup_context(QD, &ring))
		return 1;
	if (get_file_size(infd, &insize))
		return 1;

	ret = copy_file(&ring, insize);

	close(infd);
	close(outfd);
	io_uring_queue_exit(&ring);
	return ret;
}
