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

#include "helpers.h"
#include "liburing.h"

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

static int test_update_multiring(struct io_uring *r1, struct io_uring *r2,
				 struct io_uring *r3, int do_unreg)
{
	int *fds, *newfds;

	fds = open_files(10, 0, 0);
	newfds = open_files(10, 0, 1);

	if (io_uring_register_files(r1, fds, 10) ||
	    io_uring_register_files(r2, fds, 10) ||
	    io_uring_register_files(r3, fds, 10)) {
		fprintf(stderr, "%s: register files failed\n", __FUNCTION__);
		goto err;
	}

	if (io_uring_register_files_update(r1, 0, newfds, 10) != 10 ||
	    io_uring_register_files_update(r2, 0, newfds, 10) != 10 ||
	    io_uring_register_files_update(r3, 0, newfds, 10) != 10) {
		fprintf(stderr, "%s: update files failed\n", __FUNCTION__);
		goto err;
	}

	if (!do_unreg)
		goto done;

	if (io_uring_unregister_files(r1) ||
	    io_uring_unregister_files(r2) ||
	    io_uring_unregister_files(r3)) {
		fprintf(stderr, "%s: unregister files failed\n", __FUNCTION__);
		goto err;
	}

done:
	close_files(fds, 10, 0);
	close_files(newfds, 10, 1);
	return 0;
err:
	close_files(fds, 10, 0);
	close_files(newfds, 10, 1);
	return 1;
}

static int test_sqe_update(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int *fds, i, ret;

	fds = t_malloc(sizeof(int) * 10);
	for (i = 0; i < 10; i++)
		fds[i] = -1;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_files_update(sqe, fds, 10, 0);
	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait: %d\n", ret);
		return 1;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	free(fds);
	if (ret == -EINVAL) {
		fprintf(stdout, "IORING_OP_FILES_UPDATE not supported, skipping\n");
		return T_EXIT_SKIP;
	}
	return ret != 10;
}

static int test_update_no_table(void)
{
	int up_fd, fds[4] = {-1, 0, 1, 4};
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret;

	ret = t_create_ring(2, &ring, 0);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_SKIP;
	else if (ret != T_SETUP_OK)
		return ret;

	ret = io_uring_register_files(&ring, fds, 4);
	/* ignore other failures */
	if (ret && ret != -EBADF) {
		fprintf(stderr, "Failed registering file table: %d\n", ret);
		goto fail;
	}

	sqe = io_uring_get_sqe(&ring);
	up_fd = ring.ring_fd;
	io_uring_prep_files_update(sqe, &up_fd, 1, -1); //offset = -1
	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "Failed submit: %d\n", ret);
		goto fail;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "Failed wait: %d\n", ret);
		goto fail;
	}
	ret = cqe->res;
	io_uring_cqe_seen(&ring, cqe);
	if (ret != -EMFILE && ret != -EINVAL && ret != -EOVERFLOW &&
	    ret != -ENXIO) {
		fprintf(stderr, "Bad cqe res: %d\n", ret);
		goto fail;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
fail:
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}

int main(int argc, char *argv[])
{
	struct io_uring r1, r2, r3;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	if (io_uring_queue_init(8, &r1, 0) ||
	    io_uring_queue_init(8, &r2, 0) ||
	    io_uring_queue_init(8, &r3, 0)) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = test_update_multiring(&r1, &r2, &r3, 1);
	if (ret) {
		fprintf(stderr, "test_update_multiring w/unreg\n");
		return ret;
	}

	ret = test_update_multiring(&r1, &r2, &r3, 0);
	if (ret) {
		fprintf(stderr, "test_update_multiring wo/unreg\n");
		return ret;
	}

	ret = test_sqe_update(&r1);
	if (ret) {
		if (ret != T_EXIT_SKIP)
			fprintf(stderr, "test_sqe_update failed\n");
		return ret;
	}

	ret = test_update_no_table();
	if (ret) {
		if (ret != T_EXIT_SKIP)
			fprintf(stderr, "test_sqe_update failed\n");
		return ret;
	}

	return T_EXIT_PASS;
}
