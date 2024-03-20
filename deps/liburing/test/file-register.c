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
#include <limits.h>
#include <sys/resource.h>

#include "helpers.h"
#include "liburing.h"

static int no_update = 0;

static void close_files(int *files, int nr_files, int add)
{
	char fname[32];
	int i;

	for (i = 0; i < nr_files; i++) {
		if (files)
			close(files[i]);
		if (!add)
			sprintf(fname, ".reg.%d", i);
		else
			sprintf(fname, ".add.%d", i + add);
		unlink(fname);
	}
	if (files)
		free(files);
}

static int *open_files(int nr_files, int extra, int add)
{
	char fname[32];
	int *files;
	int i;

	files = t_calloc(nr_files + extra, sizeof(int));

	for (i = 0; i < nr_files; i++) {
		if (!add)
			sprintf(fname, ".reg.%d", i);
		else
			sprintf(fname, ".add.%d", i + add);
		files[i] = open(fname, O_RDWR | O_CREAT, 0644);
		if (files[i] < 0) {
			perror("open");
			free(files);
			files = NULL;
			break;
		}
	}
	if (extra) {
		for (i = nr_files; i < nr_files + extra; i++)
			files[i] = -1;
	}

	return files;
}

static int test_shrink(struct io_uring *ring)
{
	int ret, off, fd;
	int *files;

	files = open_files(50, 0, 0);
	ret = io_uring_register_files(ring, files, 50);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	off = 0;
	do {
		fd = -1;
		ret = io_uring_register_files_update(ring, off, &fd, 1);
		if (ret != 1) {
			if (off == 50 && ret == -EINVAL)
				break;
			fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
			break;
		}
		off++;
	} while (1);

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	close_files(files, 50, 0);
	return 0;
err:
	close_files(files, 50, 0);
	return 1;
}


static int test_grow(struct io_uring *ring)
{
	int ret, off;
	int *files, *fds = NULL;

	files = open_files(50, 250, 0);
	ret = io_uring_register_files(ring, files, 300);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	off = 50;
	do {
		fds = open_files(1, 0, off);
		ret = io_uring_register_files_update(ring, off, fds, 1);
		if (ret != 1) {
			if (off == 300 && ret == -EINVAL)
				break;
			fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
			break;
		}
		if (off >= 300) {
			fprintf(stderr, "%s: Succeeded beyond end-of-list?\n", __FUNCTION__);
			goto err;
		}
		off++;
	} while (1);

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	close_files(files, 100, 0);
	close_files(NULL, 251, 50);
	return 0;
err:
	close_files(files, 100, 0);
	close_files(NULL, 251, 50);
	return 1;
}

static int test_replace_all(struct io_uring *ring)
{
	int *files, *fds = NULL;
	int ret, i;

	files = open_files(100, 0, 0);
	ret = io_uring_register_files(ring, files, 100);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	fds = t_malloc(100 * sizeof(int));
	for (i = 0; i < 100; i++)
		fds[i] = -1;

	ret = io_uring_register_files_update(ring, 0, fds, 100);
	if (ret != 100) {
		fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	close_files(files, 100, 0);
	if (fds)
		free(fds);
	return 0;
err:
	close_files(files, 100, 0);
	if (fds)
		free(fds);
	return 1;
}

static int test_replace(struct io_uring *ring)
{
	int *files, *fds = NULL;
	int ret;

	files = open_files(100, 0, 0);
	ret = io_uring_register_files(ring, files, 100);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	fds = open_files(10, 0, 1);
	ret = io_uring_register_files_update(ring, 90, fds, 10);
	if (ret != 10) {
		fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	close_files(files, 100, 0);
	if (fds)
		close_files(fds, 10, 1);
	return 0;
err:
	close_files(files, 100, 0);
	if (fds)
		close_files(fds, 10, 1);
	return 1;
}

static int test_removals(struct io_uring *ring)
{
	int *files, *fds = NULL;
	int ret, i;

	files = open_files(100, 0, 0);
	ret = io_uring_register_files(ring, files, 100);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	fds = t_calloc(10, sizeof(int));
	for (i = 0; i < 10; i++)
		fds[i] = -1;

	ret = io_uring_register_files_update(ring, 50, fds, 10);
	if (ret != 10) {
		fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	close_files(files, 100, 0);
	if (fds)
		free(fds);
	return 0;
err:
	close_files(files, 100, 0);
	if (fds)
		free(fds);
	return 1;
}

static int test_additions(struct io_uring *ring)
{
	int *files, *fds = NULL;
	int ret;

	files = open_files(100, 100, 0);
	ret = io_uring_register_files(ring, files, 200);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	fds = open_files(2, 0, 1);
	ret = io_uring_register_files_update(ring, 100, fds, 2);
	if (ret != 2) {
		fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	close_files(files, 100, 0);
	if (fds)
		close_files(fds, 2, 1);
	return 0;
err:
	close_files(files, 100, 0);
	if (fds)
		close_files(fds, 2, 1);
	return 1;
}

static int test_sparse(struct io_uring *ring)
{
	int *files;
	int ret;

	files = open_files(100, 100, 0);
	ret = io_uring_register_files(ring, files, 200);
	if (ret) {
		if (ret == -EBADF) {
			fprintf(stdout, "Sparse files not supported, skipping\n");
			no_update = 1;
			goto done;
		}
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}
	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}
done:
	close_files(files, 100, 0);
	return 0;
err:
	close_files(files, 100, 0);
	return 1;
}

static int test_basic_many(struct io_uring *ring)
{
	int *files;
	int ret;

	files = open_files(768, 0, 0);
	ret = io_uring_register_files(ring, files, 768);
	if (ret) {
		fprintf(stderr, "%s: register %d\n", __FUNCTION__, ret);
		goto err;
	}
	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister %d\n", __FUNCTION__, ret);
		goto err;
	}
	close_files(files, 768, 0);
	return 0;
err:
	close_files(files, 768, 0);
	return 1;
}

static int test_basic(struct io_uring *ring, int fail)
{
	int *files;
	int ret;
	int nr_files = fail ? 10 : 100;

	files = open_files(nr_files, 0, 0);
	ret = io_uring_register_files(ring, files, 100);
	if (ret) {
		if (fail) {
			if (ret == -EBADF || ret == -EFAULT)
				return 0;
		}
		fprintf(stderr, "%s: register %d\n", __FUNCTION__, ret);
		goto err;
	}
	if (fail) {
		fprintf(stderr, "Registration succeeded, but expected fail\n");
		goto err;
	}
	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister %d\n", __FUNCTION__, ret);
		goto err;
	}
	close_files(files, nr_files, 0);
	return 0;
err:
	close_files(files, nr_files, 0);
	return 1;
}

/*
 * Register 0 files, but reserve space for 10.  Then add one file.
 */
static int test_zero(struct io_uring *ring)
{
	int *files, *fds = NULL;
	int ret;

	files = open_files(0, 10, 0);
	ret = io_uring_register_files(ring, files, 10);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	fds = open_files(1, 0, 1);
	ret = io_uring_register_files_update(ring, 0, fds, 1);
	if (ret != 1) {
		fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	if (fds)
		close_files(fds, 1, 1);
	free(files);
	return 0;
err:
	if (fds)
		close_files(fds, 1, 1);
	free(files);
	return 1;
}

static int test_fixed_read_write(struct io_uring *ring, int index)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct iovec iov[2];
	int ret;

	iov[0].iov_base = t_malloc(4096);
	iov[0].iov_len = 4096;
	memset(iov[0].iov_base, 0x5a, 4096);

	iov[1].iov_base = t_malloc(4096);
	iov[1].iov_len = 4096;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: failed to get sqe\n", __FUNCTION__);
		return 1;
	}
	io_uring_prep_writev(sqe, index, &iov[0], 1, 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	sqe->user_data = 1;

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: io_uring_wait_cqe=%d\n", __FUNCTION__, ret);
		return 1;
	}
	if (cqe->res != 4096) {
		fprintf(stderr, "%s: write cqe->res=%d\n", __FUNCTION__, cqe->res);
		return 1;
	}
	io_uring_cqe_seen(ring, cqe);

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "%s: failed to get sqe\n", __FUNCTION__);
		return 1;
	}
	io_uring_prep_readv(sqe, index, &iov[1], 1, 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: io_uring_wait_cqe=%d\n", __FUNCTION__, ret);
		return 1;
	}
	if (cqe->res != 4096) {
		fprintf(stderr, "%s: read cqe->res=%d\n", __FUNCTION__, cqe->res);
		return 1;
	}
	io_uring_cqe_seen(ring, cqe);

	if (memcmp(iov[1].iov_base, iov[0].iov_base, 4096)) {
		fprintf(stderr, "%s: data mismatch\n", __FUNCTION__);
		return 1;
	}

	free(iov[0].iov_base);
	free(iov[1].iov_base);
	return 0;
}

static void adjust_nfiles(int want_files)
{
	struct rlimit rlim;

	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
		return;
	if (rlim.rlim_cur >= want_files)
		return;
	rlim.rlim_cur = want_files;
	setrlimit(RLIMIT_NOFILE, &rlim);
}

/*
 * Register 8K of sparse files, update one at a random spot, then do some
 * file IO to verify it works.
 */
static int test_huge(struct io_uring *ring)
{
	int *files;
	int ret;

	adjust_nfiles(16384);

	files = open_files(0, 8192, 0);
	ret = io_uring_register_files(ring, files, 8192);
	if (ret) {
		/* huge sets not supported */
		if (ret == -EMFILE) {
			fprintf(stdout, "%s: No huge file set support, skipping\n", __FUNCTION__);
			goto out;
		}
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	files[7193] = open(".reg.7193", O_RDWR | O_CREAT, 0644);
	if (files[7193] < 0) {
		fprintf(stderr, "%s: open=%d\n", __FUNCTION__, errno);
		goto err;
	}

	ret = io_uring_register_files_update(ring, 7193, &files[7193], 1);
	if (ret != 1) {
		fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	if (test_fixed_read_write(ring, 7193))
		goto err;

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	if (files[7193] != -1) {
		close(files[7193]);
		unlink(".reg.7193");
	}
out:
	free(files);
	return 0;
err:
	if (files[7193] != -1) {
		close(files[7193]);
		unlink(".reg.7193");
	}
	free(files);
	return 1;
}

static int test_skip(struct io_uring *ring)
{
	int *files;
	int ret;

	files = open_files(100, 0, 0);
	ret = io_uring_register_files(ring, files, 100);
	if (ret) {
		fprintf(stderr, "%s: register ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	files[90] = IORING_REGISTER_FILES_SKIP;
	ret = io_uring_register_files_update(ring, 90, &files[90], 1);
	if (ret != 1) {
		if (ret == -EBADF) {
			fprintf(stdout, "Skipping files not supported\n");
			goto done;
		}
		fprintf(stderr, "%s: update ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

	/* verify can still use file index 90 */
	if (test_fixed_read_write(ring, 90))
		goto err;

	ret = io_uring_unregister_files(ring);
	if (ret) {
		fprintf(stderr, "%s: unregister ret=%d\n", __FUNCTION__, ret);
		goto err;
	}

done:
	close_files(files, 100, 0);
	return 0;
err:
	close_files(files, 100, 0);
	return 1;
}

static int test_sparse_updates(void)
{
	struct io_uring ring;
	int ret, i, *fds, newfd;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue_init: %d\n", ret);
		return ret;
	}

	fds = t_malloc(256 * sizeof(int));
	for (i = 0; i < 256; i++)
		fds[i] = -1;

	ret = io_uring_register_files(&ring, fds, 256);
	if (ret) {
		fprintf(stderr, "file_register: %d\n", ret);
		return ret;
	}

	newfd = 1;
	for (i = 0; i < 256; i++) {
		ret = io_uring_register_files_update(&ring, i, &newfd, 1);
		if (ret != 1) {
			fprintf(stderr, "file_update: %d\n", ret);
			return ret;
		}
	}
	io_uring_unregister_files(&ring);

	for (i = 0; i < 256; i++)
		fds[i] = 1;

	ret = io_uring_register_files(&ring, fds, 256);
	if (ret) {
		fprintf(stderr, "file_register: %d\n", ret);
		return ret;
	}

	newfd = -1;
	for (i = 0; i < 256; i++) {
		ret = io_uring_register_files_update(&ring, i, &newfd, 1);
		if (ret != 1) {
			fprintf(stderr, "file_update: %d\n", ret);
			return ret;
		}
	}
	io_uring_unregister_files(&ring);

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_fixed_removal_ordering(void)
{
	char buffer[128];
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int ret, fd, i, fds[2];

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret < 0) {
		fprintf(stderr, "failed to init io_uring: %s\n", strerror(-ret));
		return ret;
	}
	if (pipe(fds)) {
		perror("pipe");
		return -1;
	}
	ret = io_uring_register_files(&ring, fds, 2);
	if (ret) {
		fprintf(stderr, "file_register: %d\n", ret);
		return ret;
	}
	/* ring should have fds referenced, can close them */
	close(fds[0]);
	close(fds[1]);

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		return 1;
	}
	/* outwait file recycling delay */
	ts.tv_sec = 3;
	ts.tv_nsec = 0;
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->flags |= IOSQE_IO_LINK | IOSQE_IO_HARDLINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		printf("get sqe failed\n");
		return -1;
	}
	io_uring_prep_write(sqe, 1, buffer, sizeof(buffer), 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	sqe->user_data = 2;

	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "%s: got %d, wanted 2\n", __FUNCTION__, ret);
		return -1;
	}

	/* remove unused pipe end */
	fd = -1;
	ret = io_uring_register_files_update(&ring, 0, &fd, 1);
	if (ret != 1) {
		fprintf(stderr, "update off=0 failed\n");
		return -1;
	}

	/* remove used pipe end */
	fd = -1;
	ret = io_uring_register_files_update(&ring, 1, &fd, 1);
	if (ret != 1) {
		fprintf(stderr, "update off=1 failed\n");
		return -1;
	}

	for (i = 0; i < 2; ++i) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "%s: io_uring_wait_cqe=%d\n", __FUNCTION__, ret);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	return 0;
}

/* mix files requiring SCM-accounting and not in a single register */
static int test_mixed_af_unix(void)
{
	struct io_uring ring;
	int i, ret, fds[2];
	int reg_fds[32];
	int sp[2];

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret < 0) {
		fprintf(stderr, "failed to init io_uring: %s\n", strerror(-ret));
		return ret;
	}
	if (pipe(fds)) {
		perror("pipe");
		return -1;
	}
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) != 0) {
		perror("Failed to create Unix-domain socket pair\n");
		return 1;
	}

	for (i = 0; i < 16; i++) {
		reg_fds[i * 2] = fds[0];
		reg_fds[i * 2 + 1] = sp[0];
	}

	ret = io_uring_register_files(&ring, reg_fds, 32);
	if (ret) {
		fprintf(stderr, "file_register: %d\n", ret);
		return ret;
	}

	close(fds[0]);
	close(fds[1]);
	close(sp[0]);
	close(sp[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

static int test_partial_register_fail(void)
{
	char buffer[128];
	struct io_uring ring;
	int ret, fds[2];
	int reg_fds[5];

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret < 0) {
		fprintf(stderr, "failed to init io_uring: %s\n", strerror(-ret));
		return ret;
	}
	if (pipe(fds)) {
		perror("pipe");
		return -1;
	}

	/*
	 * Expect register to fail as it doesn't support io_uring fds, shouldn't
	 * leave any fds referenced afterwards.
	 */
	reg_fds[0] = fds[0];
	reg_fds[1] = fds[1];
	reg_fds[2] = -1;
	reg_fds[3] = ring.ring_fd;
	reg_fds[4] = -1;
	ret = io_uring_register_files(&ring, reg_fds, 5);
	if (!ret) {
		fprintf(stderr, "file_register unexpectedly succeeded\n");
		return 1;
	}

	/* ring should have fds referenced, can close them */
	close(fds[1]);

	/* confirm that fds[1] is actually close and to ref'ed by io_uring */
	ret = read(fds[0], buffer, 10);
	if (ret < 0)
		perror("read");
	close(fds[0]);
	io_uring_queue_exit(&ring);
	return 0;
}

static int file_update_alloc(struct io_uring *ring, int *fd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_files_update(sqe, fd, 1, IORING_FILE_INDEX_ALLOC);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return -1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "%s: io_uring_wait_cqe=%d\n", __FUNCTION__, ret);
		return -1;
	}
	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static int test_out_of_range_file_ranges(struct io_uring *ring)
{
	int ret;

	ret = io_uring_register_file_alloc_range(ring, 8, 3);
	if (ret != -EINVAL) {
		fprintf(stderr, "overlapping range %i\n", ret);
		return 1;
	}

	ret = io_uring_register_file_alloc_range(ring, 10, 1);
	if (ret != -EINVAL) {
		fprintf(stderr, "out of range index %i\n", ret);
		return 1;
	}

	ret = io_uring_register_file_alloc_range(ring, 7, ~1U);
	if (ret != -EOVERFLOW) {
		fprintf(stderr, "overflow %i\n", ret);
		return 1;
	}

	return 0;
}

static int test_overallocating_file_range(struct io_uring *ring, int fds[2])
{
	int roff = 7, rlen = 2;
	int ret, i, fd;

	ret = io_uring_register_file_alloc_range(ring, roff, rlen);
	if (ret) {
		fprintf(stderr, "io_uring_register_file_alloc_range %i\n", ret);
		return 1;
	}

	for (i = 0; i < rlen; i++) {
		fd = fds[0];
		ret = file_update_alloc(ring, &fd);
		if (ret != 1) {
			fprintf(stderr, "file_update_alloc\n");
			return 1;
		}

		if (fd < roff || fd >= roff + rlen) {
			fprintf(stderr, "invalid off result %i\n", fd);
			return 1;
		}
	}

	fd = fds[0];
	ret = file_update_alloc(ring, &fd);
	if (ret != -ENFILE) {
		fprintf(stderr, "overallocated %i, off %i\n", ret, fd);
		return 1;
	}

	return 0;
}

static int test_zero_range_alloc(struct io_uring *ring, int fds[2])
{
	int ret, fd;

	ret = io_uring_register_file_alloc_range(ring, 7, 0);
	if (ret) {
		fprintf(stderr, "io_uring_register_file_alloc_range failed %i\n", ret);
		return 1;
	}

	fd = fds[0];
	ret = file_update_alloc(ring, &fd);
	if (ret != -ENFILE) {
		fprintf(stderr, "zero alloc %i\n", ret);
		return 1;
	}
	return 0;
}

static int test_defer_taskrun(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int ret, fds[2];
	char buff = 'x';

	ret = io_uring_queue_init(8, &ring,
				  IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER);
	if (ret) {
		fprintf(stderr, "ring init\n");
		return 1;
	}

	ret = pipe(fds);
	if (ret) {
		fprintf(stderr, "bad pipes\n");
		return 1;
	}

	ret = io_uring_register_files(&ring, &fds[0], 2);
	if (ret) {
		fprintf(stderr, "bad register %d\n", ret);
		return 1;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, 0, &buff, 1, 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "bad submit\n");
		return 1;
	}

	ret = write(fds[1], &buff, 1);
	if (ret != 1) {
		fprintf(stderr, "bad pipe write\n");
		return 1;
	}

	ret = io_uring_unregister_files(&ring);
	if (ret) {
		fprintf(stderr, "bad unregister %d\n", ret);
		return 1;
	}

	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

static int test_file_alloc_ranges(void)
{
	struct io_uring ring;
	int ret, pipe_fds[2];

	if (pipe(pipe_fds)) {
		fprintf(stderr, "pipes\n");
		return 1;
	}
	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "queue_init: %d\n", ret);
		return 1;
	}

	ret = io_uring_register_files_sparse(&ring, 10);
	if (ret == -EINVAL) {
not_supported:
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		io_uring_queue_exit(&ring);
		printf("file alloc ranges are not supported, skip\n");
		return 0;
	} else if (ret) {
		fprintf(stderr, "io_uring_register_files_sparse %i\n", ret);
		return ret;
	}

	ret = io_uring_register_file_alloc_range(&ring, 0, 1);
	if (ret) {
		if (ret == -EINVAL)
			goto not_supported;
		fprintf(stderr, "io_uring_register_file_alloc_range %i\n", ret);
		return 1;
	}

	ret = test_overallocating_file_range(&ring, pipe_fds);
	if (ret) {
		fprintf(stderr, "test_overallocating_file_range() failed\n");
		return 1;
	}

	ret = test_out_of_range_file_ranges(&ring);
	if (ret) {
		fprintf(stderr, "test_out_of_range_file_ranges() failed\n");
		return 1;
	}

	ret = test_zero_range_alloc(&ring, pipe_fds);
	if (ret) {
		fprintf(stderr, "test_zero_range_alloc() failed\n");
		return 1;
	}

	close(pipe_fds[0]);
	close(pipe_fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_basic(&ring, 0);
	if (ret) {
		fprintf(stderr, "test_basic failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_basic(&ring, 1);
	if (ret) {
		fprintf(stderr, "test_basic failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_basic_many(&ring);
	if (ret) {
		fprintf(stderr, "test_basic_many failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sparse(&ring);
	if (ret) {
		fprintf(stderr, "test_sparse failed\n");
		return T_EXIT_FAIL;
	}

	if (no_update)
		return T_EXIT_SKIP;

	ret = test_additions(&ring);
	if (ret) {
		fprintf(stderr, "test_additions failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_removals(&ring);
	if (ret) {
		fprintf(stderr, "test_removals failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_replace(&ring);
	if (ret) {
		fprintf(stderr, "test_replace failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_replace_all(&ring);
	if (ret) {
		fprintf(stderr, "test_replace_all failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_grow(&ring);
	if (ret) {
		fprintf(stderr, "test_grow failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_shrink(&ring);
	if (ret) {
		fprintf(stderr, "test_shrink failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_zero(&ring);
	if (ret) {
		fprintf(stderr, "test_zero failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_huge(&ring);
	if (ret) {
		fprintf(stderr, "test_huge failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_skip(&ring);
	if (ret) {
		fprintf(stderr, "test_skip failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_sparse_updates();
	if (ret) {
		fprintf(stderr, "test_sparse_updates failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_fixed_removal_ordering();
	if (ret) {
		fprintf(stderr, "test_fixed_removal_ordering failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_mixed_af_unix();
	if (ret) {
		fprintf(stderr, "test_mixed_af_unix failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_partial_register_fail();
	if (ret) {
		fprintf(stderr, "test_partial_register_fail failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_file_alloc_ranges();
	if (ret) {
		fprintf(stderr, "test_partial_register_fail failed\n");
		return T_EXIT_FAIL;
	}

	if (t_probe_defer_taskrun()) {
		ret = test_defer_taskrun();
		if (ret) {
			fprintf(stderr, "test_defer_taskrun failed\n");
			return T_EXIT_FAIL;
		}
	}

	return T_EXIT_PASS;
}
