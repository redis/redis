/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <stdbool.h>

#include "helpers.h"
#include "liburing.h"

#define RING_SIZE 128
enum {
	OP_NOP,
	OP_REMOVE_BUFFERS
};

struct test_context {
	struct io_uring *ring;
	struct io_uring_sqe **sqes;
	struct io_uring_cqe *cqes;
	int nr;
};

static void free_context(struct test_context *ctx)
{
	free(ctx->sqes);
	free(ctx->cqes);
	memset(ctx, 0, sizeof(*ctx));
}

static int init_context(struct test_context *ctx, struct io_uring *ring, int nr,
			int op)
{
	struct io_uring_sqe *sqe;
	int i;

	memset(ctx, 0, sizeof(*ctx));
	ctx->nr = nr;
	ctx->ring = ring;
	ctx->sqes = t_malloc(nr * sizeof(*ctx->sqes));
	ctx->cqes = t_malloc(nr * sizeof(*ctx->cqes));

	if (!ctx->sqes || !ctx->cqes)
		goto err;

	for (i = 0; i < nr; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe)
			goto err;
		switch (op) {
		case OP_NOP:
			io_uring_prep_nop(sqe);
			break;
		case OP_REMOVE_BUFFERS:
			io_uring_prep_remove_buffers(sqe, 10, 1);
			break;
		}
		sqe->user_data = i;
		ctx->sqes[i] = sqe;
	}

	return 0;
err:
	free_context(ctx);
	printf("init context failed\n");
	return 1;
}

static int wait_cqes(struct test_context *ctx)
{
	int ret, i;
	struct io_uring_cqe *cqe;

	for (i = 0; i < ctx->nr; i++) {
		ret = io_uring_wait_cqe(ctx->ring, &cqe);

		if (ret < 0) {
			printf("wait_cqes: wait completion %d\n", ret);
			return 1;
		}
		memcpy(&ctx->cqes[i], cqe, sizeof(*cqe));
		io_uring_cqe_seen(ctx->ring, cqe);
	}

	return 0;
}

static int test_cancelled_userdata(struct io_uring *ring)
{
	struct test_context ctx;
	int ret, i, nr = 100;

	if (init_context(&ctx, ring, nr, OP_NOP))
		return 1;

	for (i = 0; i < nr; i++)
		ctx.sqes[i]->flags |= IOSQE_IO_LINK;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	if (wait_cqes(&ctx))
		goto err;

	for (i = 0; i < nr; i++) {
		if (i != ctx.cqes[i].user_data) {
			printf("invalid user data\n");
			goto err;
		}
	}

	free_context(&ctx);
	return 0;
err:
	free_context(&ctx);
	return 1;
}

static int test_thread_link_cancel(struct io_uring *ring)
{
	struct test_context ctx;
	int ret, i, nr = 100;

	if (init_context(&ctx, ring, nr, OP_REMOVE_BUFFERS))
		return 1;

	for (i = 0; i < nr; i++)
		ctx.sqes[i]->flags |= IOSQE_IO_LINK;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	if (wait_cqes(&ctx))
		goto err;

	for (i = 0; i < nr; i++) {
		bool fail = false;

		if (i == 0)
			fail = (ctx.cqes[i].res != -ENOENT);
		else
			fail = (ctx.cqes[i].res != -ECANCELED);

		if (fail) {
			printf("invalid status %d\n", ctx.cqes[i].res);
			goto err;
		}
	}

	free_context(&ctx);
	return 0;
err:
	free_context(&ctx);
	return 1;
}

static int test_drain_with_linked_timeout(struct io_uring *ring)
{
	const int nr = 3;
	struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0, };
	struct test_context ctx;
	int ret, i;

	if (init_context(&ctx, ring, nr * 2, OP_NOP))
		return 1;

	for (i = 0; i < nr; i++) {
		io_uring_prep_timeout(ctx.sqes[2 * i], &ts, 0, 0);
		ctx.sqes[2 * i]->flags |= IOSQE_IO_LINK | IOSQE_IO_DRAIN;
		io_uring_prep_link_timeout(ctx.sqes[2 * i + 1], &ts, 0);
	}

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	if (wait_cqes(&ctx))
		goto err;

	free_context(&ctx);
	return 0;
err:
	free_context(&ctx);
	return 1;
}

static int run_drained(struct io_uring *ring, int nr)
{
	struct test_context ctx;
	int ret, i;

	if (init_context(&ctx, ring, nr, OP_NOP))
		return 1;

	for (i = 0; i < nr; i++)
		ctx.sqes[i]->flags |= IOSQE_IO_DRAIN;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		printf("sqe submit failed: %d\n", ret);
		goto err;
	}

	if (wait_cqes(&ctx))
		goto err;

	free_context(&ctx);
	return 0;
err:
	free_context(&ctx);
	return 1;
}

static int test_overflow_hung(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	int ret, nr = 10;

	while (*ring->cq.koverflow != 1000) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			printf("get sqe failed\n");
			return 1;
		}

		io_uring_prep_nop(sqe);
		ret = io_uring_submit(ring);
		if (ret <= 0) {
			printf("sqe submit failed: %d\n", ret);
			return 1;
		}
	}

	return run_drained(ring, nr);
}

static int test_dropped_hung(struct io_uring *ring)
{
	int nr = 10;

	*ring->sq.kdropped = 1000;
	return run_drained(ring, nr);
}

int main(int argc, char *argv[])
{
	struct io_uring ring, poll_ring, sqthread_ring;
	struct io_uring_params p;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	memset(&p, 0, sizeof(p));
	ret = io_uring_queue_init_params(RING_SIZE, &ring, &p);
	if (ret) {
		printf("ring setup failed %i\n", ret);
		return T_EXIT_FAIL;
	}

	ret = io_uring_queue_init(RING_SIZE, &poll_ring, IORING_SETUP_IOPOLL);
	if (ret) {
		printf("poll_ring setup failed\n");
		return T_EXIT_FAIL;
	}


	ret = test_cancelled_userdata(&poll_ring);
	if (ret) {
		printf("test_cancelled_userdata failed\n");
		return ret;
	}

	if (!(p.features & IORING_FEAT_NODROP)) {
		ret = test_overflow_hung(&ring);
		if (ret) {
			printf("test_overflow_hung failed\n");
			return ret;
		}
	}

	ret = test_dropped_hung(&ring);
	if (ret) {
		printf("test_dropped_hung failed\n");
		return ret;
	}

	ret = test_drain_with_linked_timeout(&ring);
	if (ret) {
		printf("test_drain_with_linked_timeout failed\n");
		return ret;
	}

	ret = t_create_ring(RING_SIZE, &sqthread_ring,
				IORING_SETUP_SQPOLL | IORING_SETUP_IOPOLL);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_SKIP;
	else if (ret < 0)
		return T_EXIT_FAIL;

	ret = test_thread_link_cancel(&sqthread_ring);
	if (ret) {
		printf("test_thread_link_cancel failed\n");
		return ret;
	}

	return T_EXIT_PASS;
}
