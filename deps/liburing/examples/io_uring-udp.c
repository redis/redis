/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#include "liburing.h"

#define QD 64
#define BUF_SHIFT 12 /* 4k */
#define CQES (QD * 16)
#define BUFFERS CQES
#define CONTROLLEN 0

struct sendmsg_ctx {
	struct msghdr msg;
	struct iovec iov;
};

struct ctx {
	struct io_uring ring;
	struct io_uring_buf_ring *buf_ring;
	unsigned char *buffer_base;
	struct msghdr msg;
	int buf_shift;
	int af;
	bool verbose;
	struct sendmsg_ctx send[BUFFERS];
	size_t buf_ring_size;
};

static size_t buffer_size(struct ctx *ctx)
{
	return 1U << ctx->buf_shift;
}

static unsigned char *get_buffer(struct ctx *ctx, int idx)
{
	return ctx->buffer_base + (idx << ctx->buf_shift);
}

static int setup_buffer_pool(struct ctx *ctx)
{
	int ret, i;
	void *mapped;
	struct io_uring_buf_reg reg = { .ring_addr = 0,
					.ring_entries = BUFFERS,
					.bgid = 0 };

	ctx->buf_ring_size = (sizeof(struct io_uring_buf) + buffer_size(ctx)) * BUFFERS;
	mapped = mmap(NULL, ctx->buf_ring_size, PROT_READ | PROT_WRITE,
		      MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	if (mapped == MAP_FAILED) {
		fprintf(stderr, "buf_ring mmap: %s\n", strerror(errno));
		return -1;
	}
	ctx->buf_ring = (struct io_uring_buf_ring *)mapped;

	io_uring_buf_ring_init(ctx->buf_ring);

	reg = (struct io_uring_buf_reg) {
		.ring_addr = (unsigned long)ctx->buf_ring,
		.ring_entries = BUFFERS,
		.bgid = 0
	};
	ctx->buffer_base = (unsigned char *)ctx->buf_ring +
			   sizeof(struct io_uring_buf) * BUFFERS;

	ret = io_uring_register_buf_ring(&ctx->ring, &reg, 0);
	if (ret) {
		fprintf(stderr, "buf_ring init failed: %s\n"
				"NB This requires a kernel version >= 6.0\n",
				strerror(-ret));
		return ret;
	}

	for (i = 0; i < BUFFERS; i++) {
		io_uring_buf_ring_add(ctx->buf_ring, get_buffer(ctx, i), buffer_size(ctx), i,
				      io_uring_buf_ring_mask(BUFFERS), i);
	}
	io_uring_buf_ring_advance(ctx->buf_ring, BUFFERS);

	return 0;
}

static int setup_context(struct ctx *ctx)
{
	struct io_uring_params params;
	int ret;

	memset(&params, 0, sizeof(params));
	params.cq_entries = QD * 8;
	params.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN |
		       IORING_SETUP_CQSIZE;

	ret = io_uring_queue_init_params(QD, &ctx->ring, &params);
	if (ret < 0) {
		fprintf(stderr, "queue_init failed: %s\n"
				"NB: This requires a kernel version >= 6.0\n",
				strerror(-ret));
		return ret;
	}

	ret = setup_buffer_pool(ctx);
	if (ret)
		io_uring_queue_exit(&ctx->ring);

	memset(&ctx->msg, 0, sizeof(ctx->msg));
	ctx->msg.msg_namelen = sizeof(struct sockaddr_storage);
	ctx->msg.msg_controllen = CONTROLLEN;
	return ret;
}

static int setup_sock(int af, int port)
{
	int ret;
	int fd;
	uint16_t nport = port <= 0 ? 0 : htons(port);

	fd = socket(af, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "sock_init: %s\n", strerror(errno));
		return -1;
	}

	if (af == AF_INET6) {
		struct sockaddr_in6 addr6 = {
			.sin6_family = af,
			.sin6_port = nport,
			.sin6_addr = IN6ADDR_ANY_INIT
		};

		ret = bind(fd, (struct sockaddr *) &addr6, sizeof(addr6));
	} else {
		struct sockaddr_in addr = {
			.sin_family = af,
			.sin_port = nport,
			.sin_addr = { INADDR_ANY }
		};

		ret = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
	}

	if (ret) {
		fprintf(stderr, "sock_bind: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	if (port <= 0) {
		int port;
		struct sockaddr_storage s;
		socklen_t sz = sizeof(s);

		if (getsockname(fd, (struct sockaddr *)&s, &sz)) {
			fprintf(stderr, "getsockname failed\n");
			close(fd);
			return -1;
		}

		port = ntohs(((struct sockaddr_in *)&s)->sin_port);
		fprintf(stderr, "port bound to %d\n", port);
	}

	return fd;
}

static void cleanup_context(struct ctx *ctx)
{
	munmap(ctx->buf_ring, ctx->buf_ring_size);
	io_uring_queue_exit(&ctx->ring);
}

static bool get_sqe(struct ctx *ctx, struct io_uring_sqe **sqe)
{
	*sqe = io_uring_get_sqe(&ctx->ring);

	if (!*sqe) {
		io_uring_submit(&ctx->ring);
		*sqe = io_uring_get_sqe(&ctx->ring);
	}
	if (!*sqe) {
		fprintf(stderr, "cannot get sqe\n");
		return true;
	}
	return false;
}

static int add_recv(struct ctx *ctx, int idx)
{
	struct io_uring_sqe *sqe;

	if (get_sqe(ctx, &sqe))
		return -1;

	io_uring_prep_recvmsg_multishot(sqe, idx, &ctx->msg, MSG_TRUNC);
	sqe->flags |= IOSQE_FIXED_FILE;

	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = 0;
	io_uring_sqe_set_data64(sqe, BUFFERS + 1);
	return 0;
}

static void recycle_buffer(struct ctx *ctx, int idx)
{
	io_uring_buf_ring_add(ctx->buf_ring, get_buffer(ctx, idx), buffer_size(ctx), idx,
			      io_uring_buf_ring_mask(BUFFERS), 0);
	io_uring_buf_ring_advance(ctx->buf_ring, 1);
}

static int process_cqe_send(struct ctx *ctx, struct io_uring_cqe *cqe)
{
	int idx = cqe->user_data;

	if (cqe->res < 0)
		fprintf(stderr, "bad send %s\n", strerror(-cqe->res));
	recycle_buffer(ctx, idx);
	return 0;
}

static int process_cqe_recv(struct ctx *ctx, struct io_uring_cqe *cqe,
			    int fdidx)
{
	int ret, idx;
	struct io_uring_recvmsg_out *o;
	struct io_uring_sqe *sqe;

	if (!(cqe->flags & IORING_CQE_F_MORE)) {
		ret = add_recv(ctx, fdidx);
		if (ret)
			return ret;
	}

	if (cqe->res == -ENOBUFS)
		return 0;

	if (!(cqe->flags & IORING_CQE_F_BUFFER) || cqe->res < 0) {
		fprintf(stderr, "recv cqe bad res %d\n", cqe->res);
		if (cqe->res == -EFAULT || cqe->res == -EINVAL)
			fprintf(stderr,
				"NB: This requires a kernel version >= 6.0\n");
		return -1;
	}
	idx = cqe->flags >> 16;

	o = io_uring_recvmsg_validate(get_buffer(ctx, cqe->flags >> 16),
				      cqe->res, &ctx->msg);
	if (!o) {
		fprintf(stderr, "bad recvmsg\n");
		return -1;
	}
	if (o->namelen > ctx->msg.msg_namelen) {
		fprintf(stderr, "truncated name\n");
		recycle_buffer(ctx, idx);
		return 0;
	}
	if (o->flags & MSG_TRUNC) {
		unsigned int r;

		r = io_uring_recvmsg_payload_length(o, cqe->res, &ctx->msg);
		fprintf(stderr, "truncated msg need %u received %u\n",
				o->payloadlen, r);
		recycle_buffer(ctx, idx);
		return 0;
	}

	if (ctx->verbose) {
		struct sockaddr_in *addr = io_uring_recvmsg_name(o);
		struct sockaddr_in6 *addr6 = (void *)addr;
		char buff[INET6_ADDRSTRLEN + 1];
		const char *name;
		void *paddr;

		if (ctx->af == AF_INET6)
			paddr = &addr6->sin6_addr;
		else
			paddr = &addr->sin_addr;

		name = inet_ntop(ctx->af, paddr, buff, sizeof(buff));
		if (!name)
			name = "<INVALID>";

		fprintf(stderr, "received %u bytes %d from [%s]:%d\n",
			io_uring_recvmsg_payload_length(o, cqe->res, &ctx->msg),
			o->namelen, name, (int)ntohs(addr->sin_port));
	}

	if (get_sqe(ctx, &sqe))
		return -1;

	ctx->send[idx].iov = (struct iovec) {
		.iov_base = io_uring_recvmsg_payload(o, &ctx->msg),
		.iov_len =
			io_uring_recvmsg_payload_length(o, cqe->res, &ctx->msg)
	};
	ctx->send[idx].msg = (struct msghdr) {
		.msg_namelen = o->namelen,
		.msg_name = io_uring_recvmsg_name(o),
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_iov = &ctx->send[idx].iov,
		.msg_iovlen = 1
	};

	io_uring_prep_sendmsg(sqe, fdidx, &ctx->send[idx].msg, 0);
	io_uring_sqe_set_data64(sqe, idx);
	sqe->flags |= IOSQE_FIXED_FILE;

	return 0;
}
static int process_cqe(struct ctx *ctx, struct io_uring_cqe *cqe, int fdidx)
{
	if (cqe->user_data < BUFFERS)
		return process_cqe_send(ctx, cqe);
	else
		return process_cqe_recv(ctx, cqe, fdidx);
}

int main(int argc, char *argv[])
{
	struct ctx ctx;
	int ret;
	int port = -1;
	int sockfd;
	int opt;
	struct io_uring_cqe *cqes[CQES];
	unsigned int count, i;

	memset(&ctx, 0, sizeof(ctx));
	ctx.verbose = false;
	ctx.af = AF_INET;
	ctx.buf_shift = BUF_SHIFT;

	while ((opt = getopt(argc, argv, "6vp:b:")) != -1) {
		switch (opt) {
		case '6':
			ctx.af = AF_INET6;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'b':
			ctx.buf_shift = atoi(optarg);
			break;
		case 'v':
			ctx.verbose = true;
			break;
		default:
			fprintf(stderr, "Usage: %s [-p port] "
					"[-b log2(BufferSize)] [-6] [-v]\n",
					argv[0]);
			exit(-1);
		}
	}

	sockfd = setup_sock(ctx.af, port);
	if (sockfd < 0)
		return 1;

	if (setup_context(&ctx)) {
		close(sockfd);
		return 1;
	}

	ret = io_uring_register_files(&ctx.ring, &sockfd, 1);
	if (ret) {
		fprintf(stderr, "register files: %s\n", strerror(-ret));
		return -1;
	}

	ret = add_recv(&ctx, 0);
	if (ret)
		return 1;

	while (true) {
		ret = io_uring_submit_and_wait(&ctx.ring, 1);
		if (ret == -EINTR)
			continue;
		if (ret < 0) {
			fprintf(stderr, "submit and wait failed %d\n", ret);
			break;
		}

		count = io_uring_peek_batch_cqe(&ctx.ring, &cqes[0], CQES);
		for (i = 0; i < count; i++) {
			ret = process_cqe(&ctx, cqes[i], 0);
			if (ret)
				goto cleanup;
		}
		io_uring_cq_advance(&ctx.ring, count);
	}

cleanup:
	cleanup_context(&ctx);
	close(sockfd);
	return ret;
}
