/* SPDX-License-Identifier: MIT */
/*
 * Description: run various shared buffer ring sanity checks
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "liburing.h"
#include "helpers.h"

static int no_buf_ring;
static int pagesize;

/* test trying to register classic group when ring group exists */
static int test_mixed_reg2(int bgid)
{
	struct io_uring_buf_ring *br;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	void *bufs;
	int ret;

	ret = t_create_ring(1, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret != T_SETUP_OK)
		return 1;

	br = io_uring_setup_buf_ring(&ring, 32, bgid, 0, &ret);
	if (!br) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	/* provide classic buffers, group 1 */
	bufs = malloc(8 * 1024);
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_provide_buffers(sqe, bufs, 1024, 8, bgid, 0);
	io_uring_submit(&ring);
	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe %d\n", ret);
		return 1;
	}
	if (cqe->res != -EEXIST && cqe->res != -EINVAL) {
		fprintf(stderr, "cqe res %d\n", cqe->res);
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);

	io_uring_free_buf_ring(&ring, br, 32, bgid);
	io_uring_queue_exit(&ring);
	return 0;
}

/* test trying to register ring group when  classic group exists */
static int test_mixed_reg(int bgid)
{
	struct io_uring_buf_ring *br;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	void *bufs;
	int ret;

	ret = t_create_ring(1, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret != T_SETUP_OK)
		return 1;

	/* provide classic buffers, group 1 */
	bufs = malloc(8 * 1024);
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_provide_buffers(sqe, bufs, 1024, 8, bgid, 0);
	io_uring_submit(&ring);
	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe %d\n", ret);
		return 1;
	}
	if (cqe->res) {
		fprintf(stderr, "cqe res %d\n", cqe->res);
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);

	br = io_uring_setup_buf_ring(&ring, 32, bgid, 0, &ret);
	if (br) {
		fprintf(stderr, "Buffer ring setup succeeded unexpectedly %d\n", ret);
		return 1;
	}

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_double_reg_unreg(int bgid)
{
	struct io_uring_buf_reg reg = { };
	struct io_uring_buf_ring *br;
	struct io_uring ring;
	int ret;

	ret = t_create_ring(1, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret != T_SETUP_OK)
		return 1;

	br = io_uring_setup_buf_ring(&ring, 32, bgid, 0, &ret);
	if (!br) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	/* check that 2nd register with same bgid fails */
	reg.ring_addr = (unsigned long) br;
	reg.ring_entries = 32;
	reg.bgid = bgid;

	ret = io_uring_register_buf_ring(&ring, &reg, 0);
	if (ret != -EEXIST) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	ret = io_uring_free_buf_ring(&ring, br, 32, bgid);
	if (ret) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	ret = io_uring_unregister_buf_ring(&ring, bgid);
	if (ret != -EINVAL && ret != -ENOENT) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_reg_unreg(int bgid)
{
	struct io_uring_buf_ring *br;
	struct io_uring ring;
	int ret;

	ret = t_create_ring(1, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret != T_SETUP_OK)
		return 1;

	br = io_uring_setup_buf_ring(&ring, 32, bgid, 0, &ret);
	if (!br) {
		if (ret == -EINVAL) {
			no_buf_ring = 1;
			return 0;
		}
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	ret = io_uring_free_buf_ring(&ring, br, 32, bgid);
	if (ret) {
		fprintf(stderr, "Buffer ring unregister failed %d\n", ret);
		return 1;
	}

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_bad_reg(int bgid)
{
	struct io_uring ring;
	int ret;
	struct io_uring_buf_reg reg = { };

	ret = t_create_ring(1, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret != T_SETUP_OK)
		return 1;

	reg.ring_addr = 4096;
	reg.ring_entries = 32;
	reg.bgid = bgid;

	ret = io_uring_register_buf_ring(&ring, &reg, 0);
	if (!ret)
		fprintf(stderr, "Buffer ring register worked unexpectedly\n");

	io_uring_queue_exit(&ring);
	return !ret;
}

static int test_full_page_reg(int bgid)
{
#if defined(__hppa__)
	return T_EXIT_SKIP;
#else
	struct io_uring ring;
	int ret;
	void *ptr;
	struct io_uring_buf_reg reg = { };
	int entries = pagesize / sizeof(struct io_uring_buf);

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue init failed %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = posix_memalign(&ptr, pagesize, pagesize * 2);
	if (ret) {
		fprintf(stderr, "posix_memalign failed %d\n", ret);
		goto err;
	}

	ret = mprotect(ptr + pagesize, pagesize, PROT_NONE);
	if (ret) {
		fprintf(stderr, "mprotect failed %d\n", errno);
		goto err1;
	}

	reg.ring_addr = (unsigned long) ptr;
	reg.ring_entries = entries;
	reg.bgid = bgid;

	ret = io_uring_register_buf_ring(&ring, &reg, 0);
	if (ret)
		fprintf(stderr, "register buf ring failed %d\n", ret);

	if (mprotect(ptr + pagesize, pagesize, PROT_READ | PROT_WRITE))
		fprintf(stderr, "reverting mprotect failed %d\n", errno);

err1:
	free(ptr);
err:
	io_uring_queue_exit(&ring);
	return ret ? T_EXIT_FAIL : T_EXIT_PASS;
#endif
}

static int test_one_read(int fd, int bgid, struct io_uring *ring)
{
	int ret;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return -1;
	}

	io_uring_prep_read(sqe, fd, NULL, 1, 0);
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = bgid;
	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return -1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		return -1;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);

	if (ret == -ENOBUFS)
		return ret;

	if (ret != 1) {
		fprintf(stderr, "read result %d\n", ret);
		return -1;
	}

	return cqe->flags >> 16;
}

static int test_running(int bgid, int entries, int loops)
{
	int ring_mask = io_uring_buf_ring_mask(entries);
	struct io_uring_buf_ring *br;
	int ret, loop, idx, read_fd;
	struct io_uring ring;
	char buffer[8];
	bool *buffers;

	ret = t_create_ring(1, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return 0;
	else if (ret != T_SETUP_OK)
		return 1;

	br = io_uring_setup_buf_ring(&ring, entries, bgid, 0, &ret);
	if (!br) {
		/* by now should have checked if this is supported or not */
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	buffers = malloc(sizeof(bool) * entries);
	if (!buffers)
		return 1;

	read_fd = open("/dev/zero", O_RDONLY);
	if (read_fd < 0)
		return 1;

	for (loop = 0; loop < loops; loop++) {
		memset(buffers, 0, sizeof(bool) * entries);
		for (idx = 0; idx < entries; idx++)
			io_uring_buf_ring_add(br, buffer, sizeof(buffer), idx, ring_mask, idx);
		io_uring_buf_ring_advance(br, entries);

		for (idx = 0; idx < entries; idx++) {
			memset(buffer, 1, sizeof(buffer));
			ret = test_one_read(read_fd, bgid, &ring);
			if (ret < 0) {
				fprintf(stderr, "bad run %d/%d = %d\n", loop, idx, ret);
				return ret;
			}
			if (buffers[ret]) {
				fprintf(stderr, "reused buffer %d/%d = %d!\n", loop, idx, ret);
				return 1;
			}
			if (buffer[0] != 0) {
				fprintf(stderr, "unexpected read %d %d/%d = %d!\n",
						(int)buffer[0], loop, idx, ret);
				return 1;
			}
			if (buffer[1] != 1) {
				fprintf(stderr, "unexpected spilled read %d %d/%d = %d!\n",
						(int)buffer[1], loop, idx, ret);
				return 1;
			}
			buffers[ret] = true;
		}
		ret = test_one_read(read_fd, bgid, &ring);
		if (ret != -ENOBUFS) {
			fprintf(stderr, "expected enobufs run %d = %d\n", loop, ret);
			return 1;
		}

	}

	ret = io_uring_unregister_buf_ring(&ring, bgid);
	if (ret) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	close(read_fd);
	io_uring_queue_exit(&ring);
	free(buffers);
	return 0;
}

int main(int argc, char *argv[])
{
	int bgids[] = { 1, 127, -1 };
	int entries[] = {1, 32768, 4096, -1 };
	int ret, i;

	if (argc > 1)
		return T_EXIT_SKIP;

	pagesize = getpagesize();

	for (i = 0; bgids[i] != -1; i++) {
		ret = test_reg_unreg(bgids[i]);
		if (ret) {
			fprintf(stderr, "test_reg_unreg failed\n");
			return T_EXIT_FAIL;
		}
		if (no_buf_ring)
			break;

		ret = test_bad_reg(bgids[i]);
		if (ret) {
			fprintf(stderr, "test_bad_reg failed\n");
			return T_EXIT_FAIL;
		}

		ret = test_double_reg_unreg(bgids[i]);
		if (ret) {
			fprintf(stderr, "test_double_reg_unreg failed\n");
			return T_EXIT_FAIL;
		}

		ret = test_mixed_reg(bgids[i]);
		if (ret) {
			fprintf(stderr, "test_mixed_reg failed\n");
			return T_EXIT_FAIL;
		}

		ret = test_mixed_reg2(bgids[i]);
		if (ret) {
			fprintf(stderr, "test_mixed_reg2 failed\n");
			return T_EXIT_FAIL;
		}

		ret = test_full_page_reg(bgids[i]);
		if (ret == T_EXIT_FAIL) {
			fprintf(stderr, "test_full_page_reg failed\n");
			return T_EXIT_FAIL;
		}
	}

	for (i = 0; !no_buf_ring && entries[i] != -1; i++) {
		ret = test_running(2, entries[i], 3);
		if (ret) {
			fprintf(stderr, "test_running(%d) failed\n", entries[i]);
			return T_EXIT_FAIL;
		}
	}

	return T_EXIT_PASS;
}
