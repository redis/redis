/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring link io with drain io
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "helpers.h"
#include "liburing.h"

static int test_link_drain_one(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe[5];
	struct iovec iovecs;
	int i, fd, ret;
	off_t off = 0;
	char data[5] = {0};
	char expect[5] = {0, 1, 2, 3, 4};

	fd = open("testfile", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	iovecs.iov_base = t_malloc(4096);
	iovecs.iov_len = 4096;

	for (i = 0; i < 5; i++) {
		sqe[i] = io_uring_get_sqe(ring);
		if (!sqe[i]) {
			printf("get sqe failed\n");
			goto err;
		}
	}

	/* normal heavy io */
	io_uring_prep_writev(sqe[0], fd, &iovecs, 1, off);
	sqe[0]->user_data = 0;

	/* link io */
	io_uring_prep_nop(sqe[1]);
	sqe[1]->flags |= IOSQE_IO_LINK;
	sqe[1]->user_data = 1;

	/* link drain io */
	io_uring_prep_nop(sqe[2]);
	sqe[2]->flags |= (IOSQE_IO_LINK | IOSQE_IO_DRAIN);
	sqe[2]->user_data = 2;

	/* link io */
	io_uring_prep_nop(sqe[3]);
	sqe[3]->user_data = 3;

	/* normal nop io */
	io_uring_prep_nop(sqe[4]);
	sqe[4]->user_data = 4;

	ret = io_uring_submit(ring);
	if (ret < 0) {
		printf("sqe submit failed\n");
		goto err;
	} else if (ret < 5) {
		printf("Submitted only %d\n", ret);
		goto err;
	}

	for (i = 0; i < 5; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("child: wait completion %d\n", ret);
			goto err;
		}

		data[i] = cqe->user_data;
		io_uring_cqe_seen(ring, cqe);
	}

	if (memcmp(data, expect, 5) != 0)
		goto err;

	free(iovecs.iov_base);
	close(fd);
	unlink("testfile");
	return 0;
err:
	free(iovecs.iov_base);
	close(fd);
	unlink("testfile");
	return 1;
}

static int test_link_drain_multi(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe[9];
	struct iovec iovecs;
	int i, fd, ret;
	off_t off = 0;
	char data[9] = {0};
	char expect[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};

	fd = open("testfile", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	unlink("testfile");

	iovecs.iov_base = t_malloc(4096);
	iovecs.iov_len = 4096;

	for (i = 0; i < 9; i++) {
		sqe[i] = io_uring_get_sqe(ring);
		if (!sqe[i]) {
			printf("get sqe failed\n");
			goto err;
		}
	}

	/* normal heavy io */
	io_uring_prep_writev(sqe[0], fd, &iovecs, 1, off);
	sqe[0]->user_data = 0;

	/* link1 io head */
	io_uring_prep_nop(sqe[1]);
	sqe[1]->flags |= IOSQE_IO_LINK;
	sqe[1]->user_data = 1;

	/* link1 drain io */
	io_uring_prep_nop(sqe[2]);
	sqe[2]->flags |= (IOSQE_IO_LINK | IOSQE_IO_DRAIN);
	sqe[2]->user_data = 2;

	/* link1 io end*/
	io_uring_prep_nop(sqe[3]);
	sqe[3]->user_data = 3;

	/* link2 io head */
	io_uring_prep_nop(sqe[4]);
	sqe[4]->flags |= IOSQE_IO_LINK;
	sqe[4]->user_data = 4;

	/* link2 io */
	io_uring_prep_nop(sqe[5]);
	sqe[5]->flags |= IOSQE_IO_LINK;
	sqe[5]->user_data = 5;

	/* link2 drain io */
	io_uring_prep_writev(sqe[6], fd, &iovecs, 1, off);
	sqe[6]->flags |= (IOSQE_IO_LINK | IOSQE_IO_DRAIN);
	sqe[6]->user_data = 6;

	/* link2 io end */
	io_uring_prep_nop(sqe[7]);
	sqe[7]->user_data = 7;

	/* normal io */
	io_uring_prep_nop(sqe[8]);
	sqe[8]->user_data = 8;

	ret = io_uring_submit(ring);
	if (ret < 0) {
		printf("sqe submit failed\n");
		goto err;
	} else if (ret < 9) {
		printf("Submitted only %d\n", ret);
		goto err;
	}

	for (i = 0; i < 9; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("child: wait completion %d\n", ret);
			goto err;
		}

		data[i] = cqe->user_data;
		io_uring_cqe_seen(ring, cqe);
	}

	if (memcmp(data, expect, 9) != 0)
		goto err;

	free(iovecs.iov_base);
	close(fd);
	return 0;
err:
	free(iovecs.iov_base);
	close(fd);
	return 1;

}

static int test_drain(bool defer)
{
	struct io_uring ring;
	int i, ret;
	unsigned int flags = 0;

	if (defer)
		flags = IORING_SETUP_SINGLE_ISSUER |
			IORING_SETUP_DEFER_TASKRUN;

	ret = io_uring_queue_init(100, &ring, flags);
	if (ret) {
		printf("ring setup failed\n");
		return 1;
	}

	for (i = 0; i < 1000; i++) {
		ret = test_link_drain_one(&ring);
		if (ret) {
			fprintf(stderr, "test_link_drain_one failed\n");
			break;
		}
		ret = test_link_drain_multi(&ring);
		if (ret) {
			fprintf(stderr, "test_link_drain_multi failed\n");
			break;
		}
	}

	return ret;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = test_drain(false);
	if (ret) {
		fprintf(stderr, "test_drain(false) failed\n");
		return T_EXIT_FAIL;
	}

	if (t_probe_defer_taskrun()) {
		ret = test_drain(true);
		if (ret) {
			fprintf(stderr, "test_drain(true) failed\n");
			return T_EXIT_FAIL;
		}
	}

	return T_EXIT_PASS;
}
