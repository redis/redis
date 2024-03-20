/* SPDX-License-Identifier: MIT */
/*
 * Description: basic read/write tests with buffered, O_DIRECT, and SQPOLL
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>

#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE	(256 * 1024)
#define BS		8192
#define BUFFERS		(FILE_SIZE / BS)

static struct iovec *vecs;
static int no_read;
static int no_buf_select;
static int warned;

static int create_nonaligned_buffers(void)
{
	int i;

	vecs = t_malloc(BUFFERS * sizeof(struct iovec));
	for (i = 0; i < BUFFERS; i++) {
		char *p = t_malloc(3 * BS);

		if (!p)
			return 1;
		vecs[i].iov_base = p + (rand() % BS);
		vecs[i].iov_len = 1 + (rand() % BS);
	}

	return 0;
}

static int __test_io(const char *file, struct io_uring *ring, int write,
		     int buffered, int sqthread, int fixed, int nonvec,
		     int buf_select, int seq, int exp_len)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int open_flags;
	int i, fd = -1, ret;
	off_t offset;

#ifdef VERBOSE
	fprintf(stdout, "%s: start %d/%d/%d/%d/%d: ", __FUNCTION__, write,
							buffered, sqthread,
							fixed, nonvec);
#endif
	if (write)
		open_flags = O_WRONLY;
	else
		open_flags = O_RDONLY;
	if (!buffered)
		open_flags |= O_DIRECT;

	if (fixed) {
		ret = t_register_buffers(ring, vecs, BUFFERS);
		if (ret == T_SETUP_SKIP)
			return 0;
		if (ret != T_SETUP_OK) {
			fprintf(stderr, "buffer reg failed: %d\n", ret);
			goto err;
		}
	}

	fd = open(file, open_flags);
	if (fd < 0) {
		if (errno == EINVAL)
			return 0;
		perror("file open");
		goto err;
	}

	if (sqthread) {
		ret = io_uring_register_files(ring, &fd, 1);
		if (ret) {
			fprintf(stderr, "file reg failed: %d\n", ret);
			goto err;
		}
	}

	offset = 0;
	for (i = 0; i < BUFFERS; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "sqe get failed\n");
			goto err;
		}
		if (!seq)
			offset = BS * (rand() % BUFFERS);
		if (write) {
			int do_fixed = fixed;
			int use_fd = fd;

			if (sqthread)
				use_fd = 0;
			if (fixed && (i & 1))
				do_fixed = 0;
			if (do_fixed) {
				io_uring_prep_write_fixed(sqe, use_fd, vecs[i].iov_base,
								vecs[i].iov_len,
								offset, i);
			} else if (nonvec) {
				io_uring_prep_write(sqe, use_fd, vecs[i].iov_base,
							vecs[i].iov_len, offset);
			} else {
				io_uring_prep_writev(sqe, use_fd, &vecs[i], 1,
								offset);
			}
		} else {
			int do_fixed = fixed;
			int use_fd = fd;

			if (sqthread)
				use_fd = 0;
			if (fixed && (i & 1))
				do_fixed = 0;
			if (do_fixed) {
				io_uring_prep_read_fixed(sqe, use_fd, vecs[i].iov_base,
								vecs[i].iov_len,
								offset, i);
			} else if (nonvec) {
				io_uring_prep_read(sqe, use_fd, vecs[i].iov_base,
							vecs[i].iov_len, offset);
			} else {
				io_uring_prep_readv(sqe, use_fd, &vecs[i], 1,
								offset);
			}

		}
		sqe->user_data = i;
		if (sqthread)
			sqe->flags |= IOSQE_FIXED_FILE;
		if (buf_select) {
			if (nonvec)
				sqe->addr = 0;
			sqe->flags |= IOSQE_BUFFER_SELECT;
			sqe->buf_group = buf_select;
		}
		if (seq)
			offset += BS;
	}

	ret = io_uring_submit(ring);
	if (ret != BUFFERS) {
		fprintf(stderr, "submit got %d, wanted %d\n", ret, BUFFERS);
		goto err;
	}

	for (i = 0; i < BUFFERS; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			goto err;
		}
		if (cqe->res == -EINVAL && nonvec) {
			if (!warned) {
				fprintf(stdout, "Non-vectored IO not "
					"supported, skipping\n");
				warned = 1;
				no_read = 1;
			}
		} else if (exp_len == -1) {
			int iov_len = vecs[cqe->user_data].iov_len;

			if (cqe->res != iov_len) {
				fprintf(stderr, "cqe res %d, wanted %d\n",
					cqe->res, iov_len);
				goto err;
			}
		} else if (cqe->res != exp_len) {
			fprintf(stderr, "cqe res %d, wanted %d\n", cqe->res, exp_len);
			goto err;
		}
		if (buf_select && exp_len == BS) {
			int bid = cqe->flags >> 16;
			unsigned char *ptr = vecs[bid].iov_base;
			int j;

			for (j = 0; j < BS; j++) {
				if (ptr[j] == cqe->user_data)
					continue;

				fprintf(stderr, "Data mismatch! bid=%d, "
						"wanted=%d, got=%d\n", bid,
						(int)cqe->user_data, ptr[j]);
				return 1;
			}
		}
		io_uring_cqe_seen(ring, cqe);
	}

	if (fixed) {
		ret = io_uring_unregister_buffers(ring);
		if (ret) {
			fprintf(stderr, "buffer unreg failed: %d\n", ret);
			goto err;
		}
	}
	if (sqthread) {
		ret = io_uring_unregister_files(ring);
		if (ret) {
			fprintf(stderr, "file unreg failed: %d\n", ret);
			goto err;
		}
	}

	close(fd);
#ifdef VERBOSE
	fprintf(stdout, "PASS\n");
#endif
	return 0;
err:
#ifdef VERBOSE
	fprintf(stderr, "FAILED\n");
#endif
	if (fd != -1)
		close(fd);
	return 1;
}
static int test_io(const char *file, int write, int buffered, int sqthread,
		   int fixed, int nonvec, int exp_len)
{
	struct io_uring ring;
	int ret, ring_flags = 0;

	if (sqthread)
		ring_flags = IORING_SETUP_SQPOLL;

	ret = t_create_ring(64, &ring, ring_flags);
	if (ret == T_SETUP_SKIP)
		return 0;
	if (ret != T_SETUP_OK) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	ret = __test_io(file, &ring, write, buffered, sqthread, fixed, nonvec,
			0, 0, exp_len);
	io_uring_queue_exit(&ring);
	return ret;
}

static int read_poll_link(const char *file)
{
	struct __kernel_timespec ts;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int i, fd, ret, fds[2];

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret)
		return ret;

	fd = open(file, O_WRONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, fd, &vecs[0], 1, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_add(sqe, fds[0], POLLIN);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(&ring);
	if (ret != 3) {
		fprintf(stderr, "submitted %d\n", ret);
		return 1;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	return 0;
}

static int has_nonvec_read(void)
{
	struct io_uring_probe *p;
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		exit(ret);
	}

	p = t_calloc(1, sizeof(*p) + 256 * sizeof(struct io_uring_probe_op));
	ret = io_uring_register_probe(&ring, p, 256);
	/* if we don't have PROBE_REGISTER, we don't have OP_READ/WRITE */
	if (ret == -EINVAL) {
out:
		io_uring_queue_exit(&ring);
		return 0;
	} else if (ret) {
		fprintf(stderr, "register_probe: %d\n", ret);
		goto out;
	}

	if (p->ops_len <= IORING_OP_READ)
		goto out;
	if (!(p->ops[IORING_OP_READ].flags & IO_URING_OP_SUPPORTED))
		goto out;
	io_uring_queue_exit(&ring);
	return 1;
}

static int test_eventfd_read(void)
{
	struct io_uring ring;
	int fd, ret;
	eventfd_t event;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	if (no_read)
		return 0;
	ret = io_uring_queue_init(8, &ring, 0);
	if (ret)
		return ret;

	fd = eventfd(1, 0);
	if (fd < 0) {
		perror("eventfd");
		return 1;
	}
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, fd, &event, sizeof(eventfd_t), 0);
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submitted %d\n", ret);
		return 1;
	}
	eventfd_write(fd, 1);
	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe=%d\n", ret);
		return 1;
	}
	if (cqe->res == -EINVAL) {
		fprintf(stdout, "eventfd IO not supported, skipping\n");
	} else if (cqe->res != sizeof(eventfd_t)) {
		fprintf(stderr, "cqe res %d, wanted %d\n", cqe->res,
						(int) sizeof(eventfd_t));
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);
	return 0;
}

static int test_buf_select_short(const char *filename, int nonvec)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, i, exp_len;

	if (no_buf_select)
		return 0;

	ret = io_uring_queue_init(64, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	exp_len = 0;
	for (i = 0; i < BUFFERS; i++) {
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_provide_buffers(sqe, vecs[i].iov_base,
						vecs[i].iov_len / 2, 1, 1, i);
		if (!exp_len)
			exp_len = vecs[i].iov_len / 2;
	}

	ret = io_uring_submit(&ring);
	if (ret != BUFFERS) {
		fprintf(stderr, "submit: %d\n", ret);
		return -1;
	}

	for (i = 0; i < BUFFERS; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (cqe->res < 0) {
			fprintf(stderr, "cqe->res=%d\n", cqe->res);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	ret = __test_io(filename, &ring, 0, 0, 0, 0, nonvec, 1, 1, exp_len);

	io_uring_queue_exit(&ring);
	return ret;
}

static int provide_buffers_iovec(struct io_uring *ring, int bgid)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int i, ret;

	for (i = 0; i < BUFFERS; i++) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_provide_buffers(sqe, vecs[i].iov_base,
						vecs[i].iov_len, 1, bgid, i);
	}

	ret = io_uring_submit(ring);
	if (ret != BUFFERS) {
		fprintf(stderr, "submit: %d\n", ret);
		return -1;
	}

	for (i = 0; i < BUFFERS; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return 1;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "cqe->res=%d\n", cqe->res);
			return 1;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
}

static int test_buf_select_pipe(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, i;
	int fds[2];

	if (no_buf_select)
		return 0;

	ret = io_uring_queue_init(64, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	ret = provide_buffers_iovec(&ring, 0);
	if (ret) {
		fprintf(stderr, "provide buffers failed: %d\n", ret);
		return 1;
	}

	ret = pipe(fds);
	if (ret) {
		fprintf(stderr, "pipe failed: %d\n", ret);
		return 1;
	}

	for (i = 0; i < 5; i++) {
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_read(sqe, fds[0], NULL, 1 /* max read 1 per go */, -1);
		sqe->flags |= IOSQE_BUFFER_SELECT;
		sqe->buf_group = 0;
	}
	io_uring_submit(&ring);

	ret = write(fds[1], "01234", 5);
	if (ret != 5) {
		fprintf(stderr, "pipe write failed %d\n", ret);
		return 1;
	}

	for (i = 0; i < 5; i++) {
		const char *buff;

		if (io_uring_wait_cqe(&ring, &cqe)) {
			fprintf(stderr, "bad wait %d\n", i);
			return 1;
		}
		if (cqe->res != 1) {
			fprintf(stderr, "expected read %d\n", cqe->res);
			return 1;
		}
		if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
			fprintf(stderr, "no buffer %d\n", cqe->res);
			return 1;
		}
		buff = vecs[cqe->flags >> 16].iov_base;
		if (*buff != '0' + i) {
			fprintf(stderr, "%d: expected %c, got %c\n", i, '0' + i, *buff);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}


	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

static int test_buf_select(const char *filename, int nonvec)
{
	struct io_uring_probe *p;
	struct io_uring ring;
	int ret, i;

	ret = io_uring_queue_init(64, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	p = io_uring_get_probe_ring(&ring);
	if (!p || !io_uring_opcode_supported(p, IORING_OP_PROVIDE_BUFFERS)) {
		no_buf_select = 1;
		fprintf(stdout, "Buffer select not supported, skipping\n");
		return 0;
	}
	io_uring_free_probe(p);

	/*
	 * Write out data with known pattern
	 */
	for (i = 0; i < BUFFERS; i++)
		memset(vecs[i].iov_base, i, vecs[i].iov_len);

	ret = __test_io(filename, &ring, 1, 0, 0, 0, 0, 0, 1, BS);
	if (ret) {
		fprintf(stderr, "failed writing data\n");
		return 1;
	}

	for (i = 0; i < BUFFERS; i++)
		memset(vecs[i].iov_base, 0x55, vecs[i].iov_len);

	ret = provide_buffers_iovec(&ring, 1);
	if (ret)
		return ret;

	ret = __test_io(filename, &ring, 0, 0, 0, 0, nonvec, 1, 1, BS);
	io_uring_queue_exit(&ring);
	return ret;
}

static int test_rem_buf(int batch, int sqe_flags)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int left, ret, nr = 0;
	int bgid = 1;

	if (no_buf_select)
		return 0;

	ret = io_uring_queue_init(64, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	ret = provide_buffers_iovec(&ring, bgid);
	if (ret)
		return ret;

	left = BUFFERS;
	while (left) {
		int to_rem = (left < batch) ? left : batch;

		left -= to_rem;
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_remove_buffers(sqe, to_rem, bgid);
		sqe->user_data = to_rem;
		sqe->flags |= sqe_flags;
		++nr;
	}

	ret = io_uring_submit(&ring);
	if (ret != nr) {
		fprintf(stderr, "submit: %d\n", ret);
		return -1;
	}

	for (; nr > 0; nr--) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return 1;
		}
		if (cqe->res != cqe->user_data) {
			fprintf(stderr, "cqe->res=%d\n", cqe->res);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	return ret;
}

static int test_rem_buf_single(int to_rem)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, expected;
	int bgid = 1;

	if (no_buf_select)
		return 0;

	ret = io_uring_queue_init(64, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}

	ret = provide_buffers_iovec(&ring, bgid);
	if (ret)
		return ret;

	expected = (to_rem > BUFFERS) ? BUFFERS : to_rem;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_remove_buffers(sqe, to_rem, bgid);

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return -1;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe=%d\n", ret);
		return 1;
	}
	if (cqe->res != expected) {
		fprintf(stderr, "cqe->res=%d, expected=%d\n", cqe->res, expected);
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);

	io_uring_queue_exit(&ring);
	return ret;
}

static int test_io_link(const char *file)
{
	const int nr_links = 100;
	const int link_len = 100;
	const int nr_sqes = nr_links * link_len;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int i, j, fd, ret;

	fd = open(file, O_WRONLY);
	if (fd < 0) {
		perror("file open");
		goto err;
	}

	ret = io_uring_queue_init(nr_sqes, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < nr_links; ++i) {
		for (j = 0; j < link_len; ++j) {
			sqe = io_uring_get_sqe(&ring);
			if (!sqe) {
				fprintf(stderr, "sqe get failed\n");
				goto err;
			}
			io_uring_prep_writev(sqe, fd, &vecs[0], 1, 0);
			sqe->flags |= IOSQE_ASYNC;
			if (j != link_len - 1)
				sqe->flags |= IOSQE_IO_LINK;
		}
	}

	ret = io_uring_submit(&ring);
	if (ret != nr_sqes) {
		ret = io_uring_peek_cqe(&ring, &cqe);
		if (!ret && cqe->res == -EINVAL) {
			fprintf(stdout, "IOSQE_ASYNC not supported, skipped\n");
			goto out;
		}
		fprintf(stderr, "submit got %d, wanted %d\n", ret, nr_sqes);
		goto err;
	}

	for (i = 0; i < nr_sqes; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			goto err;
		}
		if (cqe->res == -EINVAL) {
			if (!warned) {
				fprintf(stdout, "Non-vectored IO not "
					"supported, skipping\n");
				warned = 1;
				no_read = 1;
			}
		} else if (cqe->res != BS) {
			fprintf(stderr, "cqe res %d, wanted %d\n", cqe->res, BS);
			goto err;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

out:
	io_uring_queue_exit(&ring);
	close(fd);
	return 0;
err:
	if (fd != -1)
		close(fd);
	return 1;
}

static int test_write_efbig(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	struct rlimit rlim, old_rlim;
	int i, fd, ret;
	loff_t off;

	if (geteuid()) {
		fprintf(stdout, "Not root, skipping %s\n", __FUNCTION__);
		return 0;
	}

	if (getrlimit(RLIMIT_FSIZE, &old_rlim) < 0) {
		perror("getrlimit");
		return 1;
	}
	rlim = old_rlim;
	rlim.rlim_cur = 128 * 1024;
	rlim.rlim_max = 128 * 1024;
	if (setrlimit(RLIMIT_FSIZE, &rlim) < 0) {
		perror("setrlimit");
		return 1;
	}

	fd = open(".efbig", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		perror("file open");
		goto err;
	}
	unlink(".efbig");

	ret = io_uring_queue_init(32, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		goto err;
	}

	off = 0;
	for (i = 0; i < 32; i++) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "sqe get failed\n");
			goto err;
		}
		io_uring_prep_writev(sqe, fd, &vecs[i], 1, off);
		io_uring_sqe_set_data64(sqe, i);
		off += BS;
	}

	ret = io_uring_submit(&ring);
	if (ret != 32) {
		fprintf(stderr, "submit got %d, wanted %d\n", ret, 32);
		goto err;
	}

	for (i = 0; i < 32; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			goto err;
		}
		if (cqe->user_data < 16) {
			if (cqe->res != BS) {
				fprintf(stderr, "bad write: %d\n", cqe->res);
				goto err;
			}
		} else {
			if (cqe->res != -EFBIG) {
				fprintf(stderr, "Expected -EFBIG: %d\n", cqe->res);
				goto err;
			}
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	close(fd);
	unlink(".efbig");

	if (setrlimit(RLIMIT_FSIZE, &old_rlim) < 0) {
		perror("setrlimit");
		return 1;
	}
	return 0;
err:
	if (fd != -1)
		close(fd);
	return 1;
}

int main(int argc, char *argv[])
{
	int i, ret, nr;
	char buf[256];
	char *fname;

	if (argc > 1) {
		fname = argv[1];
	} else {
		srand((unsigned)time(NULL));
		snprintf(buf, sizeof(buf), ".basic-rw-%u-%u",
			(unsigned)rand(), (unsigned)getpid());
		fname = buf;
		t_create_file(fname, FILE_SIZE);
	}

	signal(SIGXFSZ, SIG_IGN);

	vecs = t_create_buffers(BUFFERS, BS);

	/* if we don't have nonvec read, skip testing that */
	nr = has_nonvec_read() ? 32 : 16;

	for (i = 0; i < nr; i++) {
		int write = (i & 1) != 0;
		int buffered = (i & 2) != 0;
		int sqthread = (i & 4) != 0;
		int fixed = (i & 8) != 0;
		int nonvec = (i & 16) != 0;

		ret = test_io(fname, write, buffered, sqthread, fixed, nonvec,
			      BS);
		if (ret) {
			fprintf(stderr, "test_io failed %d/%d/%d/%d/%d\n",
				write, buffered, sqthread, fixed, nonvec);
			goto err;
		}
	}

	ret = test_buf_select(fname, 1);
	if (ret) {
		fprintf(stderr, "test_buf_select nonvec failed\n");
		goto err;
	}

	ret = test_buf_select(fname, 0);
	if (ret) {
		fprintf(stderr, "test_buf_select vec failed\n");
		goto err;
	}

	ret = test_buf_select_short(fname, 1);
	if (ret) {
		fprintf(stderr, "test_buf_select_short nonvec failed\n");
		goto err;
	}

	ret = test_buf_select_short(fname, 0);
	if (ret) {
		fprintf(stderr, "test_buf_select_short vec failed\n");
		goto err;
	}

	ret = test_buf_select_pipe();
	if (ret) {
		fprintf(stderr, "test_buf_select_pipe failed\n");
		goto err;
	}

	ret = test_eventfd_read();
	if (ret) {
		fprintf(stderr, "test_eventfd_read failed\n");
		goto err;
	}

	ret = read_poll_link(fname);
	if (ret) {
		fprintf(stderr, "read_poll_link failed\n");
		goto err;
	}

	ret = test_io_link(fname);
	if (ret) {
		fprintf(stderr, "test_io_link failed\n");
		goto err;
	}

	ret = test_write_efbig();
	if (ret) {
		fprintf(stderr, "test_write_efbig failed\n");
		goto err;
	}

	ret = test_rem_buf(1, 0);
	if (ret) {
		fprintf(stderr, "test_rem_buf by 1 failed\n");
		goto err;
	}

	ret = test_rem_buf(10, 0);
	if (ret) {
		fprintf(stderr, "test_rem_buf by 10 failed\n");
		goto err;
	}

	ret = test_rem_buf(2, IOSQE_IO_LINK);
	if (ret) {
		fprintf(stderr, "test_rem_buf link failed\n");
		goto err;
	}

	ret = test_rem_buf(2, IOSQE_ASYNC);
	if (ret) {
		fprintf(stderr, "test_rem_buf async failed\n");
		goto err;
	}

	srand((unsigned)time(NULL));
	if (create_nonaligned_buffers()) {
		fprintf(stderr, "file creation failed\n");
		goto err;
	}

	/* test fixed bufs with non-aligned len/offset */
	for (i = 0; i < nr; i++) {
		int write = (i & 1) != 0;
		int buffered = (i & 2) != 0;
		int sqthread = (i & 4) != 0;
		int fixed = (i & 8) != 0;
		int nonvec = (i & 16) != 0;

		/* direct IO requires alignment, skip it */
		if (!buffered || !fixed || nonvec)
			continue;

		ret = test_io(fname, write, buffered, sqthread, fixed, nonvec,
			      -1);
		if (ret) {
			fprintf(stderr, "test_io failed %d/%d/%d/%d/%d\n",
				write, buffered, sqthread, fixed, nonvec);
			goto err;
		}
	}

	ret = test_rem_buf_single(BUFFERS + 1);
	if (ret) {
		fprintf(stderr, "test_rem_buf_single(BUFFERS + 1) failed\n");
		goto err;
	}

	if (fname != argv[1])
		unlink(fname);
	return 0;
err:
	if (fname != argv[1])
		unlink(fname);
	return 1;
}
