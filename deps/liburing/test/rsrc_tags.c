/* SPDX-License-Identifier: MIT */
/*
 * Description: run various file registration tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include "../src/syscall.h"
#include "helpers.h"
#include "liburing.h"

static int pipes[2];

enum {
	TEST_IORING_RSRC_FILE		= 0,
	TEST_IORING_RSRC_BUFFER		= 1,
};

static bool check_cq_empty(struct io_uring *ring)
{
	struct io_uring_cqe *cqe = NULL;
	int ret;

	usleep(1000); /* doesn't happen immediately, so wait */
	ret = io_uring_peek_cqe(ring, &cqe); /* nothing should be there */
	return ret == -EAGAIN;
}

/*
 * There are io_uring_register_buffers_tags() and other wrappers,
 * but they may change, so hand-code to specifically test this ABI.
 */
static int register_rsrc(struct io_uring *ring, int type, int nr,
			  const void *arg, const __u64 *tags)
{
	struct io_uring_rsrc_register reg;
	int reg_type;

	memset(&reg, 0, sizeof(reg));
	reg.nr = nr;
	reg.data = (__u64)(uintptr_t)arg;
	reg.tags = (__u64)(uintptr_t)tags;

	reg_type = IORING_REGISTER_FILES2;
	if (type != TEST_IORING_RSRC_FILE)
		reg_type = IORING_REGISTER_BUFFERS2;

	return __sys_io_uring_register(ring->ring_fd, reg_type, &reg,
				       sizeof(reg));
}

/*
 * There are io_uring_register_buffers_update_tag() and other wrappers,
 * but they may change, so hand-code to specifically test this ABI.
 */
static int update_rsrc(struct io_uring *ring, int type, int nr, int off,
			const void *arg, const __u64 *tags)
{
	struct io_uring_rsrc_update2 up;
	int up_type;

	memset(&up, 0, sizeof(up));
	up.offset = off;
	up.data = (__u64)(uintptr_t)arg;
	up.tags = (__u64)(uintptr_t)tags;
	up.nr = nr;

	up_type = IORING_REGISTER_FILES_UPDATE2;
	if (type != TEST_IORING_RSRC_FILE)
		up_type = IORING_REGISTER_BUFFERS_UPDATE;
	return __sys_io_uring_register(ring->ring_fd, up_type, &up, sizeof(up));
}

static bool has_rsrc_update(void)
{
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init() failed, %d\n", ret);
		exit(1);
	}

	ret = ring.features & IORING_FEAT_RSRC_TAGS;
	io_uring_queue_exit(&ring);
	return ret;
}

static int test_tags_generic(int nr, int type, void *rsrc, int ring_flags)
{
	struct io_uring_cqe *cqe = NULL;
	struct io_uring ring;
	int i, ret;
	__u64 *tags;

	tags = malloc(nr * sizeof(*tags));
	if (!tags)
		return 1;
	for (i = 0; i < nr; i++)
		tags[i] = i + 1;
	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return 1;
	}

	ret = register_rsrc(&ring, type, nr, rsrc, tags);
	if (ret) {
		fprintf(stderr, "rsrc register failed %i\n", ret);
		return 1;
	}

	/* test that tags are set */
	tags[0] = 666;
	ret = update_rsrc(&ring, type, 1, 0, rsrc, &tags[0]);
	assert(ret == 1);
	ret = io_uring_wait_cqe(&ring, &cqe);
	assert(!ret && cqe->user_data == 1);
	io_uring_cqe_seen(&ring, cqe);

	/* test that tags are updated */
	tags[0] = 0;
	ret = update_rsrc(&ring, type, 1, 0, rsrc, &tags[0]);
	assert(ret == 1);
	ret = io_uring_wait_cqe(&ring, &cqe);
	assert(!ret && cqe->user_data == 666);
	io_uring_cqe_seen(&ring, cqe);

	/* test tag=0 doesn't emit CQE */
	tags[0] = 1;
	ret = update_rsrc(&ring, type, 1, 0, rsrc, &tags[0]);
	assert(ret == 1);
	assert(check_cq_empty(&ring));

	free(tags);
	io_uring_queue_exit(&ring);
	return 0;
}

static int test_buffers_update(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;
	struct io_uring ring;
	const int nr = 5;
	int buf_idx = 1, i, ret;
	int pipes[2];
	char tmp_buf[1024];
	char tmp_buf2[1024];
	struct iovec vecs[nr];
	__u64 tags[nr];

	for (i = 0; i < nr; i++) {
		vecs[i].iov_base = tmp_buf;
		vecs[i].iov_len = 1024;
		tags[i] = i + 1;
	}

	ret = test_tags_generic(nr, TEST_IORING_RSRC_BUFFER, vecs, 0);
	if (ret)
		return 1;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return 1;
	}
	if (pipe(pipes) < 0) {
		perror("pipe");
		return 1;
	}
	ret = register_rsrc(&ring, TEST_IORING_RSRC_BUFFER, nr, vecs, tags);
	if (ret) {
		fprintf(stderr, "rsrc register failed %i\n", ret);
		return 1;
	}

	/* test that CQE is not emitted before we're done with a buffer */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read_fixed(sqe, pipes[0], tmp_buf, 10, 0, 0);
	sqe->user_data = 100;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}
	ret = io_uring_peek_cqe(&ring, &cqe);
	assert(ret == -EAGAIN);

	vecs[buf_idx].iov_base = tmp_buf2;
	ret = update_rsrc(&ring, TEST_IORING_RSRC_BUFFER, 1, buf_idx,
			  &vecs[buf_idx], &tags[buf_idx]);
	if (ret != 1) {
		fprintf(stderr, "rsrc update failed %i %i\n", ret, errno);
		return 1;
	}

	ret = io_uring_peek_cqe(&ring, &cqe); /* nothing should be there */
	assert(ret == -EAGAIN);
	close(pipes[0]);
	close(pipes[1]);

	ret = io_uring_wait_cqe(&ring, &cqe);
	assert(!ret && cqe->user_data == 100);
	io_uring_cqe_seen(&ring, cqe);
	ret = io_uring_wait_cqe(&ring, &cqe);
	assert(!ret && cqe->user_data == buf_idx + 1);
	io_uring_cqe_seen(&ring, cqe);

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_buffers_empty_buffers(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;
	struct io_uring ring;
	const int nr = 5;
	int ret, i;
	char tmp_buf[1024];
	struct iovec vecs[nr];

	for (i = 0; i < nr; i++) {
		vecs[i].iov_base = 0;
		vecs[i].iov_len = 0;
	}
	vecs[0].iov_base = tmp_buf;
	vecs[0].iov_len = 10;

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return 1;
	}

	ret = register_rsrc(&ring, TEST_IORING_RSRC_BUFFER, nr, vecs, NULL);
	if (ret) {
		fprintf(stderr, "rsrc register failed %i\n", ret);
		return 1;
	}

	/* empty to buffer */
	vecs[1].iov_base = tmp_buf;
	vecs[1].iov_len = 10;
	ret = update_rsrc(&ring, TEST_IORING_RSRC_BUFFER, 1, 1, &vecs[1], NULL);
	if (ret != 1) {
		fprintf(stderr, "rsrc update failed %i %i\n", ret, errno);
		return 1;
	}

	/* buffer to empty */
	vecs[0].iov_base = 0;
	vecs[0].iov_len = 0;
	ret = update_rsrc(&ring, TEST_IORING_RSRC_BUFFER, 1, 0, &vecs[0], NULL);
	if (ret != 1) {
		fprintf(stderr, "rsrc update failed %i %i\n", ret, errno);
		return 1;
	}

	/* zero to zero is ok */
	ret = update_rsrc(&ring, TEST_IORING_RSRC_BUFFER, 1, 2, &vecs[2], NULL);
	if (ret != 1) {
		fprintf(stderr, "rsrc update failed %i %i\n", ret, errno);
		return 1;
	}

	/* empty buf with non-zero len fails */
	vecs[3].iov_base = 0;
	vecs[3].iov_len = 1;
	ret = update_rsrc(&ring, TEST_IORING_RSRC_BUFFER, 1, 3, &vecs[3], NULL);
	if (ret >= 0) {
		fprintf(stderr, "rsrc update failed %i %i\n", ret, errno);
		return 1;
	}

	/* test rw on empty ubuf is failed */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read_fixed(sqe, pipes[0], tmp_buf, 10, 0, 2);
	sqe->user_data = 100;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}
	ret = io_uring_wait_cqe(&ring, &cqe);
	assert(!ret && cqe->user_data == 100);
	assert(cqe->res);
	io_uring_cqe_seen(&ring, cqe);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read_fixed(sqe, pipes[0], tmp_buf, 0, 0, 2);
	sqe->user_data = 100;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}
	ret = io_uring_wait_cqe(&ring, &cqe);
	assert(!ret && cqe->user_data == 100);
	assert(cqe->res);
	io_uring_cqe_seen(&ring, cqe);

	io_uring_queue_exit(&ring);
	return 0;
}


static int test_files(int ring_flags)
{
	struct io_uring_cqe *cqe = NULL;
	struct io_uring ring;
	const int nr = 50;
	int off = 5, i, ret, fd;
	__s32 files[nr];
	__u64 tags[nr], tag;

	for (i = 0; i < nr; ++i) {
		files[i] = pipes[0];
		tags[i] = i + 1;
	}

	ret = test_tags_generic(nr, TEST_IORING_RSRC_FILE, files, ring_flags);
	if (ret)
		return 1;

	ret = io_uring_queue_init(1, &ring, ring_flags);
	if (ret) {
		printf("ring setup failed\n");
		return 1;
	}
	ret = register_rsrc(&ring, TEST_IORING_RSRC_FILE, nr, files, tags);
	if (ret) {
		fprintf(stderr, "rsrc register failed %i\n", ret);
		return 1;
	}

	/* check update did update tag */
	fd = -1;
	ret = io_uring_register_files_update(&ring, off, &fd, 1);
	assert(ret == 1);
	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "io_uring wait ret=%d\n", ret);
		return 1;
	}
	if (cqe->user_data != tags[off]) {
		fprintf(stderr, "data %lx != %lx\n",
				(unsigned long) cqe->user_data,
				(unsigned long) tags[off]);
		return 1;
	}
	io_uring_cqe_seen(&ring, cqe);

	/* remove removed file, shouldn't emit old tag */
	ret = io_uring_register_files_update(&ring, off, &fd, 1);
	assert(ret <= 1);
	assert(check_cq_empty(&ring));

	/* non-zero tag with remove update is disallowed */
	tag = 1;
	fd = -1;
	ret = update_rsrc(&ring, TEST_IORING_RSRC_FILE, 1, off + 1, &fd, &tag);
	assert(ret);

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_notag(void)
{
	struct io_uring_cqe *cqe = NULL;
	struct io_uring ring;
	int i, ret, fd;
	const int nr = 50;
	int files[nr];

	ret = io_uring_queue_init(1, &ring, 0);
	if (ret) {
		printf("ring setup failed\n");
		return 1;
	}
	for (i = 0; i < nr; ++i)
		files[i] = pipes[0];

	ret = io_uring_register_files(&ring, files, nr);
	assert(!ret);

	/* default register, update shouldn't emit CQE */
	fd = -1;
	ret = io_uring_register_files_update(&ring, 0, &fd, 1);
	assert(ret == 1);
	assert(check_cq_empty(&ring));

	ret = io_uring_unregister_files(&ring);
	assert(!ret);
	ret = io_uring_peek_cqe(&ring, &cqe); /* nothing should be there */
	assert(ret);

	io_uring_queue_exit(&ring);
	return 0;
}

int main(int argc, char *argv[])
{
	int ring_flags[] = {0, IORING_SETUP_IOPOLL, IORING_SETUP_SQPOLL,
			    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN};
	int i, ret;

	if (argc > 1)
		return 0;
	if (!has_rsrc_update()) {
		fprintf(stderr, "doesn't support rsrc tags, skip\n");
		return 0;
	}

	if (pipe(pipes) < 0) {
		perror("pipe");
		return 1;
	}

	ret = test_notag();
	if (ret) {
		printf("test_notag failed\n");
		return ret;
	}

	for (i = 0; i < sizeof(ring_flags) / sizeof(ring_flags[0]); i++) {
		int flag = ring_flags[i];

		if (flag & IORING_SETUP_DEFER_TASKRUN && !t_probe_defer_taskrun())
			continue;

		ret = test_files(flag);
		if (ret) {
			printf("test_tag failed, type %i\n", i);
			return ret;
		}
	}

	ret = test_buffers_update();
	if (ret) {
		printf("test_buffers_update failed\n");
		return ret;
	}

	ret = test_buffers_empty_buffers();
	if (ret) {
		printf("test_buffers_empty_buffers failed\n");
		return ret;
	}

	return 0;
}
