/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "helpers.h"
#include "liburing.h"

#define BUF_SIZE (16 * 4096)

struct test_ctx {
	int real_pipe1[2];
	int real_pipe2[2];
	int real_fd_in;
	int real_fd_out;

	/* fds or for registered files */
	int pipe1[2];
	int pipe2[2];
	int fd_in;
	int fd_out;

	void *buf_in;
	void *buf_out;
};

static unsigned int splice_flags = 0;
static unsigned int sqe_flags = 0;
static int has_splice = 0;
static int has_tee = 0;

static int read_buf(int fd, void *buf, int len)
{
	int ret;

	while (len) {
		ret = read(fd, buf, len);
		if (ret < 0)
			return ret;
		len -= ret;
		buf += ret;
	}
	return 0;
}

static int write_buf(int fd, const void *buf, int len)
{
	int ret;

	while (len) {
		ret = write(fd, buf, len);
		if (ret < 0)
			return ret;
		len -= ret;
		buf += ret;
	}
	return 0;
}

static int check_content(int fd, void *buf, int len, const void *src)
{
	int ret;

	ret = read_buf(fd, buf, len);
	if (ret)
		return ret;

	ret = memcmp(buf, src, len);
	return (ret != 0) ? -1 : 0;
}

static int create_file(const char *filename)
{
	int fd, save_errno;

	fd = open(filename, O_RDWR | O_CREAT, 0644);
	save_errno = errno;
	unlink(filename);
	errno = save_errno;
	return fd;
}

static int init_splice_ctx(struct test_ctx *ctx)
{
	int ret, rnd_fd;

	ctx->buf_in = t_calloc(BUF_SIZE, 1);
	ctx->buf_out = t_calloc(BUF_SIZE, 1);

	ctx->fd_in = create_file(".splice-test-in");
	if (ctx->fd_in < 0) {
		perror("file open");
		return 1;
	}

	ctx->fd_out = create_file(".splice-test-out");
	if (ctx->fd_out < 0) {
		perror("file open");
		return 1;
	}

	/* get random data */
	rnd_fd = open("/dev/urandom", O_RDONLY);
	if (rnd_fd < 0)
		return 1;

	ret = read_buf(rnd_fd, ctx->buf_in, BUF_SIZE);
	if (ret != 0)
		return 1;
	close(rnd_fd);

	/* populate file */
	ret = write_buf(ctx->fd_in, ctx->buf_in, BUF_SIZE);
	if (ret)
		return ret;

	if (pipe(ctx->pipe1) < 0)
		return 1;
	if (pipe(ctx->pipe2) < 0)
		return 1;

	ctx->real_pipe1[0] = ctx->pipe1[0];
	ctx->real_pipe1[1] = ctx->pipe1[1];
	ctx->real_pipe2[0] = ctx->pipe2[0];
	ctx->real_pipe2[1] = ctx->pipe2[1];
	ctx->real_fd_in = ctx->fd_in;
	ctx->real_fd_out = ctx->fd_out;
	return 0;
}

static int do_splice_op(struct io_uring *ring,
			int fd_in, loff_t off_in,
			int fd_out, loff_t off_out,
			unsigned int len,
			__u8 opcode)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret = -1;

	do {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			return -1;
		}
		io_uring_prep_splice(sqe, fd_in, off_in, fd_out, off_out,
				     len, splice_flags);
		sqe->flags |= sqe_flags;
		sqe->user_data = 42;
		sqe->opcode = opcode;

		ret = io_uring_submit(ring);
		if (ret != 1) {
			fprintf(stderr, "sqe submit failed: %d\n", ret);
			return ret;
		}

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", cqe->res);
			return ret;
		}

		if (cqe->res <= 0) {
			io_uring_cqe_seen(ring, cqe);
			return cqe->res;
		}

		len -= cqe->res;
		if (off_in != -1)
			off_in += cqe->res;
		if (off_out != -1)
			off_out += cqe->res;
		io_uring_cqe_seen(ring, cqe);
	} while (len);

	return 0;
}

static int do_splice(struct io_uring *ring,
			int fd_in, loff_t off_in,
			int fd_out, loff_t off_out,
			unsigned int len)
{
	return do_splice_op(ring, fd_in, off_in, fd_out, off_out, len,
			    IORING_OP_SPLICE);
}

static int do_tee(struct io_uring *ring, int fd_in, int fd_out, 
		  unsigned int len)
{
	return do_splice_op(ring, fd_in, 0, fd_out, 0, len, IORING_OP_TEE);
}

static void check_splice_support(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_splice(ring, -1, 0, -1, 0, BUF_SIZE);
	has_splice = (ret == -EBADF);
}

static void check_tee_support(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_tee(ring, -1, -1, BUF_SIZE);
	has_tee = (ret == -EBADF);
}

static int check_zero_splice(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_splice(ring, ctx->fd_in, -1, ctx->pipe1[1], -1, 0);
	if (ret)
		return ret;

	ret = do_splice(ring, ctx->pipe2[0], -1, ctx->pipe1[1], -1, 0);
	if (ret)
		return ret;

	return 0;
}

static int splice_to_pipe(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = lseek(ctx->real_fd_in, 0, SEEK_SET);
	if (ret)
		return ret;

	/* implicit file offset */
	ret = do_splice(ring, ctx->fd_in, -1, ctx->pipe1[1], -1, BUF_SIZE);
	if (ret)
		return ret;

	ret = check_content(ctx->real_pipe1[0], ctx->buf_out, BUF_SIZE,
			     ctx->buf_in);
	if (ret)
		return ret;

	/* explicit file offset */
	ret = do_splice(ring, ctx->fd_in, 0, ctx->pipe1[1], -1, BUF_SIZE);
	if (ret)
		return ret;

	return check_content(ctx->real_pipe1[0], ctx->buf_out, BUF_SIZE,
			     ctx->buf_in);
}

static int splice_from_pipe(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = write_buf(ctx->real_pipe1[1], ctx->buf_in, BUF_SIZE);
	if (ret)
		return ret;
	ret = do_splice(ring, ctx->pipe1[0], -1, ctx->fd_out, 0, BUF_SIZE);
	if (ret)
		return ret;
	ret = check_content(ctx->real_fd_out, ctx->buf_out, BUF_SIZE,
			     ctx->buf_in);
	if (ret)
		return ret;

	ret = ftruncate(ctx->real_fd_out, 0);
	if (ret)
		return ret;
	return lseek(ctx->real_fd_out, 0, SEEK_SET);
}

static int splice_pipe_to_pipe(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_splice(ring, ctx->fd_in, 0, ctx->pipe1[1], -1, BUF_SIZE);
	if (ret)
		return ret;
	ret = do_splice(ring, ctx->pipe1[0], -1, ctx->pipe2[1], -1, BUF_SIZE);
	if (ret)
		return ret;

	return check_content(ctx->real_pipe2[0], ctx->buf_out, BUF_SIZE,
				ctx->buf_in);
}

static int fail_splice_pipe_offset(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_splice(ring, ctx->fd_in, 0, ctx->pipe1[1], 0, BUF_SIZE);
	if (ret != -ESPIPE && ret != -EINVAL)
		return ret;

	ret = do_splice(ring, ctx->pipe1[0], 0, ctx->fd_out, 0, BUF_SIZE);
	if (ret != -ESPIPE && ret != -EINVAL)
		return ret;

	return 0;
}

static int fail_tee_nonpipe(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_tee(ring, ctx->fd_in, ctx->pipe1[1], BUF_SIZE);
	if (ret != -ESPIPE && ret != -EINVAL)
		return ret;

	return 0;
}

static int fail_tee_offset(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_splice_op(ring, ctx->pipe2[0], -1, ctx->pipe1[1], 0,
			   BUF_SIZE, IORING_OP_TEE);
	if (ret != -ESPIPE && ret != -EINVAL)
		return ret;

	ret = do_splice_op(ring, ctx->pipe2[0], 0, ctx->pipe1[1], -1,
			   BUF_SIZE, IORING_OP_TEE);
	if (ret != -ESPIPE && ret != -EINVAL)
		return ret;

	return 0;
}

static int check_tee(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = write_buf(ctx->real_pipe1[1], ctx->buf_in, BUF_SIZE);
	if (ret)
		return ret;
	ret = do_tee(ring, ctx->pipe1[0], ctx->pipe2[1], BUF_SIZE);
	if (ret)
		return ret;

	ret = check_content(ctx->real_pipe1[0], ctx->buf_out, BUF_SIZE,
				ctx->buf_in);
	if (ret) {
		fprintf(stderr, "tee(), invalid src data\n");
		return ret;
	}

	ret = check_content(ctx->real_pipe2[0], ctx->buf_out, BUF_SIZE,
				ctx->buf_in);
	if (ret) {
		fprintf(stderr, "tee(), invalid dst data\n");
		return ret;
	}

	return 0;
}

static int check_zero_tee(struct io_uring *ring, struct test_ctx *ctx)
{
	return do_tee(ring, ctx->pipe2[0], ctx->pipe1[1], 0);
}

static int test_splice(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	if (has_splice) {
		ret = check_zero_splice(ring, ctx);
		if (ret) {
			fprintf(stderr, "check_zero_splice failed %i %i\n",
				ret, errno);
			return ret;
		}

		ret = splice_to_pipe(ring, ctx);
		if (ret) {
			fprintf(stderr, "splice_to_pipe failed %i %i\n",
				ret, errno);
			return ret;
		}

		ret = splice_from_pipe(ring, ctx);
		if (ret) {
			fprintf(stderr, "splice_from_pipe failed %i %i\n",
				ret, errno);
			return ret;
		}

		ret = splice_pipe_to_pipe(ring, ctx);
		if (ret) {
			fprintf(stderr, "splice_pipe_to_pipe failed %i %i\n",
				ret, errno);
			return ret;
		}

		ret = fail_splice_pipe_offset(ring, ctx);
		if (ret) {
			fprintf(stderr, "fail_splice_pipe_offset failed %i %i\n",
				ret, errno);
			return ret;
		}
	}

	if (has_tee) {
		ret = check_zero_tee(ring, ctx);
		if (ret) {
			fprintf(stderr, "check_zero_tee() failed %i %i\n",
				ret, errno);
			return ret;
		}

		ret = fail_tee_nonpipe(ring, ctx);
		if (ret) {
			fprintf(stderr, "fail_tee_nonpipe() failed %i %i\n",
				ret, errno);
			return ret;
		}

		ret = fail_tee_offset(ring, ctx);
		if (ret) {
			fprintf(stderr, "fail_tee_offset failed %i %i\n",
				ret, errno);
			return ret;
		}

		ret = check_tee(ring, ctx);
		if (ret) {
			fprintf(stderr, "check_tee() failed %i %i\n",
				ret, errno);
			return ret;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_params p = { };
	struct test_ctx ctx;
	int ret;
	int reg_fds[6];

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}
	if (!(p.features & IORING_FEAT_FAST_POLL)) {
		fprintf(stdout, "No splice support, skipping\n");
		return 0;
	}

	ret = init_splice_ctx(&ctx);
	if (ret) {
		fprintf(stderr, "init failed %i %i\n", ret, errno);
		return 1;
	}

	check_splice_support(&ring, &ctx);
	if (!has_splice)
		fprintf(stdout, "skip, doesn't support splice()\n");
	check_tee_support(&ring, &ctx);
	if (!has_tee)
		fprintf(stdout, "skip, doesn't support tee()\n");

	ret = test_splice(&ring, &ctx);
	if (ret) {
		fprintf(stderr, "basic splice tests failed\n");
		return ret;
	}

	reg_fds[0] = ctx.real_pipe1[0];
	reg_fds[1] = ctx.real_pipe1[1];
	reg_fds[2] = ctx.real_pipe2[0];
	reg_fds[3] = ctx.real_pipe2[1];
	reg_fds[4] = ctx.real_fd_in;
	reg_fds[5] = ctx.real_fd_out;
	ret = io_uring_register_files(&ring, reg_fds, 6);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		return 1;
	}

	/* remap fds to registered */
	ctx.pipe1[0] = 0;
	ctx.pipe1[1] = 1;
	ctx.pipe2[0] = 2;
	ctx.pipe2[1] = 3;
	ctx.fd_in = 4;
	ctx.fd_out = 5;

	splice_flags = SPLICE_F_FD_IN_FIXED;
	sqe_flags = IOSQE_FIXED_FILE;
	ret = test_splice(&ring, &ctx);
	if (ret) {
		fprintf(stderr, "registered fds splice tests failed\n");
		return ret;
	}
	return 0;
}
