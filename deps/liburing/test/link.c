/* SPDX-License-Identifier: MIT */
/*
 * Description: run various linked sqe tests
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

static int no_hardlink;

/*
 * Timer with single nop
 */
static int test_single_hardlink(struct io_uring *ring)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	ts.tv_sec = 0;
	ts.tv_nsec = 10000000ULL;
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->flags |= IOSQE_IO_LINK | IOSQE_IO_HARDLINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (!cqe) {
			fprintf(stderr, "failed to get cqe\n");
			goto err;
		}
		if (no_hardlink)
			goto next;
		if (cqe->user_data == 1 && cqe->res == -EINVAL) {
			fprintf(stdout, "Hard links not supported, skipping\n");
			no_hardlink = 1;
			goto next;
		}
		if (cqe->user_data == 1 && cqe->res != -ETIME) {
			fprintf(stderr, "timeout failed with %d\n", cqe->res);
			goto err;
		}
		if (cqe->user_data == 2 && cqe->res) {
			fprintf(stderr, "nop failed with %d\n", cqe->res);
			goto err;
		}
next:
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Timer -> timer -> nop
 */
static int test_double_hardlink(struct io_uring *ring)
{
	struct __kernel_timespec ts1, ts2;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	if (no_hardlink)
		return 0;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	ts1.tv_sec = 0;
	ts1.tv_nsec = 10000000ULL;
	io_uring_prep_timeout(sqe, &ts1, 0, 0);
	sqe->flags |= IOSQE_IO_LINK | IOSQE_IO_HARDLINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	ts2.tv_sec = 0;
	ts2.tv_nsec = 15000000ULL;
	io_uring_prep_timeout(sqe, &ts2, 0, 0);
	sqe->flags |= IOSQE_IO_LINK | IOSQE_IO_HARDLINK;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}
	io_uring_prep_nop(sqe);
	sqe->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (!cqe) {
			fprintf(stderr, "failed to get cqe\n");
			goto err;
		}
		if (cqe->user_data == 1 && cqe->res != -ETIME) {
			fprintf(stderr, "timeout failed with %d\n", cqe->res);
			goto err;
		}
		if (cqe->user_data == 2 && cqe->res != -ETIME) {
			fprintf(stderr, "timeout failed with %d\n", cqe->res);
			goto err;
		}
		if (cqe->user_data == 3 && cqe->res) {
			fprintf(stderr, "nop failed with %d\n", cqe->res);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;

}

/*
 * Test failing head of chain, and dependent getting -ECANCELED
 */
static int test_single_link_fail(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_remove_buffers(sqe, 10, 1);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_peek_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		if (!cqe) {
			printf("failed to get cqe\n");
			goto err;
		}
		if (i == 0 && cqe->res != -ENOENT) {
			printf("sqe0 failed with %d, wanted -ENOENT\n", cqe->res);
			goto err;
		}
		if (i == 1 && cqe->res != -ECANCELED) {
			printf("sqe1 failed with %d, wanted -ECANCELED\n", cqe->res);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Test two independent chains
 */
static int test_double_chain(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 4; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Test multiple dependents
 */
static int test_double_link(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

/*
 * Test single dependency
 */
static int test_single_link(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

static int test_early_fail_and_wait(void)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	int ret, invalid_fd = 42;
	struct iovec iov = { .iov_base = NULL, .iov_len = 0 };

	/* create a new ring as it leaves it dirty */
	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_readv(sqe, invalid_fd, &iov, 1, 0);
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		printf("get sqe failed\n");
		goto err;
	}

	io_uring_prep_nop(sqe);

	ret = io_uring_submit_and_wait(&ring, 2);
	if (ret <= 0 && ret != -EAGAIN) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	io_uring_queue_exit(&ring);
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring, poll_ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return T_EXIT_FAIL;

	}

	ret = io_uring_queue_init(8, &poll_ring, IORING_SETUP_IOPOLL);
	if (ret) {
		printf("poll_ring setup failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_single_link(&ring);
	if (ret) {
		printf("test_single_link failed\n");
		return ret;
	}

	ret = test_double_link(&ring);
	if (ret) {
		printf("test_double_link failed\n");
		return ret;
	}

	ret = test_double_chain(&ring);
	if (ret) {
		printf("test_double_chain failed\n");
		return ret;
	}

	ret = test_single_link_fail(&poll_ring);
	if (ret) {
		printf("test_single_link_fail failed\n");
		return ret;
	}

	ret = test_single_hardlink(&ring);
	if (ret) {
		fprintf(stderr, "test_single_hardlink\n");
		return ret;
	}

	ret = test_double_hardlink(&ring);
	if (ret) {
		fprintf(stderr, "test_double_hardlink\n");
		return ret;
	}

	ret = test_early_fail_and_wait();
	if (ret) {
		fprintf(stderr, "test_early_fail_and_wait\n");
		return ret;
	}

	return T_EXIT_PASS;
}
