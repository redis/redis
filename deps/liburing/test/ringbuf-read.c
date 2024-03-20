/* SPDX-License-Identifier: MIT */
/*
 * Description: ring mapped provided buffers with reads
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "liburing.h"
#include "helpers.h"

#define BUF_SIZE	4096
#define NR_BUFS		64
#define FSIZE		(BUF_SIZE * NR_BUFS)

#define BR_MASK		(NR_BUFS - 1)

static int no_buf_ring;

static int verify_buffer(char *buf, char val)
{
	int i;

	for (i = 0; i < BUF_SIZE; i++) {
		if (buf[i] != val) {
			fprintf(stderr, "got %d, wanted %d\n", buf[i], val);
			return 1;
		}
	}

	return 0;
}

static int test(const char *filename, int dio, int async)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	struct io_uring_buf_ring *br;
	int ret, fd, i;
	char *buf;
	void *ptr;

	ret = io_uring_queue_init(NR_BUFS, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	if (dio) {
		fd = open(filename, O_DIRECT | O_RDONLY);
		if (fd < 0 && errno == EINVAL)
			return T_EXIT_SKIP;
	} else {
		fd = open(filename, O_RDONLY);
	}
	if (fd < 0) {
		perror("open");
		return 1;
	}

	posix_fadvise(fd, 0, FSIZE, POSIX_FADV_DONTNEED);

	if (posix_memalign((void **) &buf, 4096, FSIZE))
		return 1;

	br = io_uring_setup_buf_ring(&ring, NR_BUFS, 1, 0, &ret);
	if (!br) {
		if (ret == -EINVAL) {
			no_buf_ring = 1;
			return 0;
		}
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	ptr = buf;
	for (i = 0; i < NR_BUFS; i++) {
		io_uring_buf_ring_add(br, ptr, BUF_SIZE, i + 1, BR_MASK, i);
		ptr += BUF_SIZE;
	}
	io_uring_buf_ring_advance(br, NR_BUFS);

	for (i = 0; i < NR_BUFS; i++) {
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_read(sqe, fd, NULL, BUF_SIZE, i * BUF_SIZE);
		sqe->buf_group = 1;
		sqe->flags |= IOSQE_BUFFER_SELECT;
		if (async && !(i & 1))
			sqe->flags |= IOSQE_ASYNC;
		sqe->user_data = i + 1;
	}

	ret = io_uring_submit(&ring);
	if (ret != NR_BUFS) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	for (i = 0; i < NR_BUFS; i++) {
		int bid, ud;

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait cqe failed %d\n", ret);
			return 1;
		}
		if (cqe->res != BUF_SIZE) {
			fprintf(stderr, "cqe res %d\n", cqe->res);
			return 1;
		}
		if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
			fprintf(stderr, "no buffer selected\n");
			return 1;
		}
		bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
		ud = cqe->user_data;
		io_uring_cqe_seen(&ring, cqe);
		if (verify_buffer(buf + ((bid - 1) * BUF_SIZE), ud))
			return 1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	char buf[BUF_SIZE];
	char fname[80];
	int ret, fd, i, do_unlink;

	if (argc > 1) {
		strcpy(fname, argv[1]);
		do_unlink = 0;
	} else {
		sprintf(fname, ".ringbuf-read.%d", getpid());
		t_create_file(fname, FSIZE);
		do_unlink = 1;
	}

	fd = open(fname, O_WRONLY);
	if (fd < 0) {
		perror("open");
		goto err;
	}
	for (i = 0; i < NR_BUFS; i++) {
		memset(buf, i + 1, BUF_SIZE);
		ret = write(fd, buf, BUF_SIZE);
		if (ret != BUF_SIZE) {
			fprintf(stderr, "bad file prep write\n");
			close(fd);
			goto err;
		}
	}
	close(fd);

	ret = test(fname, 1, 0);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "dio test failed\n");
		goto err;
	}
	if (no_buf_ring)
		goto pass;

	ret = test(fname, 0, 0);
	if (ret) {
		fprintf(stderr, "buffered test failed\n");
		goto err;
	}

	ret = test(fname, 1, 1);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "dio async test failed\n");
		goto err;
	}

	ret = test(fname, 0, 1);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "buffered async test failed\n");
		goto err;
	}

pass:
	ret = T_EXIT_PASS;
	goto out;
err:
	ret = T_EXIT_FAIL;
out:
	if (do_unlink)
		unlink(fname);
	return ret;
}
