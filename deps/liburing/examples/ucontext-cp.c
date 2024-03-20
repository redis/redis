/* SPDX-License-Identifier: MIT */
/*
 * gcc -Wall -O2 -D_GNU_SOURCE -o ucontext-cp ucontext-cp.c -luring
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ucontext.h>
#include <signal.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <poll.h>
#include "liburing.h"

#define QD	64
#define BS	1024

#ifndef SIGSTKSZ
#define SIGSTKSZ 8192
#endif

typedef struct {
	struct io_uring *ring;
	unsigned char *stack_buf;
	ucontext_t ctx_main, ctx_fnew;
} async_context;

typedef struct {
	async_context *pctx;
	int *psuccess;
	int *pfailure;
	int infd;
	int outfd;
} arguments_bundle;

#define DEFINE_AWAIT_OP(operation) 					\
static ssize_t await_##operation(					\
	async_context *pctx,						\
	int fd,								\
	const struct iovec *ioves,					\
	unsigned int nr_vecs,						\
	off_t offset)							\
{									\
	struct io_uring_sqe *sqe = io_uring_get_sqe(pctx->ring);	\
	struct io_uring_cqe *cqe;					\
									\
	if (!sqe)							\
		return -1;						\
									\
	io_uring_prep_##operation(sqe, fd, ioves, nr_vecs, offset);	\
	io_uring_sqe_set_data(sqe, pctx);				\
	swapcontext(&pctx->ctx_fnew, &pctx->ctx_main);			\
	io_uring_peek_cqe(pctx->ring, &cqe);				\
	assert(cqe);							\
	io_uring_cqe_seen(pctx->ring, cqe);				\
									\
	return cqe->res;						\
}

DEFINE_AWAIT_OP(readv)
DEFINE_AWAIT_OP(writev)
#undef DEFINE_AWAIT_OP

static int await_delay(async_context *pctx, time_t seconds)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(pctx->ring);
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts = {
		.tv_sec = seconds,
		.tv_nsec = 0
	};

	if (!sqe)
		return -1;

	io_uring_prep_timeout(sqe, &ts, 0, 0);
	io_uring_sqe_set_data(sqe, pctx);
	swapcontext(&pctx->ctx_fnew, &pctx->ctx_main);
	io_uring_peek_cqe(pctx->ring, &cqe);
	assert(cqe);
	io_uring_cqe_seen(pctx->ring, cqe);

	return 0;
}

static int setup_context(async_context *pctx, struct io_uring *ring)
{
	int ret;

	pctx->ring = ring;
	ret = getcontext(&pctx->ctx_fnew);
	if (ret < 0) {
		perror("getcontext");
		return -1;
	}
	pctx->stack_buf = malloc(SIGSTKSZ);
	if (!pctx->stack_buf) {
		perror("malloc");
		return -1;
	}
	pctx->ctx_fnew.uc_stack.ss_sp = pctx->stack_buf;
	pctx->ctx_fnew.uc_stack.ss_size = SIGSTKSZ;
	pctx->ctx_fnew.uc_link = &pctx->ctx_main;

	return 0;
}

static int copy_file(async_context *pctx, int infd, int outfd, struct iovec* piov)
{
	off_t offset = 0;

	for (;;) {
		ssize_t bytes_read;

		printf("%d->%d: readv %ld bytes from %ld\n", infd, outfd, (long) piov->iov_len, (long) offset);
		if ((bytes_read = await_readv(pctx, infd, piov, 1, offset)) < 0) {
			perror("await_readv");
			return 1;
		}
		if (bytes_read == 0)
			return 0;

		piov->iov_len = bytes_read;

		printf("%d->%d: writev %ld bytes from %ld\n", infd, outfd, (long) piov->iov_len, (long) offset);
		if (await_writev(pctx, outfd, piov, 1, offset) != bytes_read) {
			perror("await_writev");
			return 1;
		}
		if (bytes_read < BS)
			return 0;
		offset += bytes_read;

		printf("%d->%d: wait %ds\n", infd, outfd, 1);
		await_delay(pctx, 1);
	}
}

static void copy_file_wrapper(arguments_bundle *pbundle)
{
	struct iovec iov = {
		.iov_base = malloc(BS),
		.iov_len = BS,
	};
	async_context *pctx = pbundle->pctx;

	int ret = copy_file(pctx, pbundle->infd, pbundle->outfd, &iov);

	printf("%d->%d: done with ret code %d\n", pbundle->infd, pbundle->outfd, ret);

	if (ret == 0) {
		++*pbundle->psuccess;
	} else {
		++*pbundle->pfailure;
	}

	free(iov.iov_base);
	close(pbundle->infd);
	close(pbundle->outfd);
	free(pbundle->pctx->stack_buf);
	free(pbundle->pctx);
	free(pbundle);

	swapcontext(&pctx->ctx_fnew, &pctx->ctx_main);
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int i, req_count, ret;
	int success = 0, failure = 0;

	if (argc < 3) {
		fprintf(stderr, "%s: infile1 outfile1 [infile2 outfile2 [...]]\n", argv[0]);
		return 1;
	}

	ret = io_uring_queue_init(QD, &ring, 0);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		return -1;
	}

	req_count = (argc - 1) / 2;
	printf("copying %d files...\n", req_count);

	for (i = 1; i < argc; i += 2) {
		int infd, outfd;

		async_context *pctx = malloc(sizeof(*pctx));

		if (!pctx || setup_context(pctx, &ring))
			return 1;

		infd = open(argv[i], O_RDONLY);
		if (infd < 0) {
			perror("open infile");
			return 1;
		}
		outfd = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (outfd < 0) {
			perror("open outfile");
			return 1;
		}

		arguments_bundle *pbundle = malloc(sizeof(*pbundle));
		pbundle->pctx = pctx;
		pbundle->psuccess = &success;
		pbundle->pfailure = &failure;
		pbundle->infd = infd;
		pbundle->outfd = outfd;

		makecontext(&pctx->ctx_fnew, (void (*)(void)) copy_file_wrapper, 1, pbundle);

		if (swapcontext(&pctx->ctx_main, &pctx->ctx_fnew)) {
			perror("swapcontext");
			return 1;
		}
	}

	/* event loop */
	while (success + failure < req_count) {
		struct io_uring_cqe *cqe;

		/* usually be timed waiting */
		ret = io_uring_submit_and_wait(&ring, 1);
		if (ret < 0) {
			fprintf(stderr, "submit_and_wait: %s\n", strerror(-ret));
			return 1;
		}

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait_cqe: %s\n", strerror(-ret));
			return 1;
		}

		async_context *pctx = io_uring_cqe_get_data(cqe);

		if (swapcontext(&pctx->ctx_main, &pctx->ctx_fnew)) {
			perror("swapcontext");
			return 1;
		}
	}

	io_uring_queue_exit(&ring);

	printf("finished with %d success(es) and %d failure(s)\n", success, failure);

	return failure > 0;
}
