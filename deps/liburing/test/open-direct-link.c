/* SPDX-License-Identifier: MIT */
/*
 * Description: link <open file><read from file><close file>
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

#define MAX_FILES	8
#define FNAME		".link.direct"

static int test(struct io_uring *ring, int skip_success, int drain, int async)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	char buf[4096];
	int ret, i;

	/* drain and cqe skip are mutually exclusive */
	if (skip_success && drain)
		return 1;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_openat_direct(sqe, AT_FDCWD, FNAME, O_RDONLY, 0, 0);
	if (!drain)
		sqe->flags |= IOSQE_IO_LINK;
	if (skip_success)
		sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
	if (async)
		sqe->flags |= IOSQE_ASYNC;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, 0, buf, sizeof(buf), 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	if (drain)
		sqe->flags |= IOSQE_IO_DRAIN;
	else
		sqe->flags |= IOSQE_IO_LINK;
	if (async)
		sqe->flags |= IOSQE_ASYNC;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_close_direct(sqe, 0);
	sqe->user_data = 3;
	if (skip_success)
		sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
	if (drain)
		sqe->flags |= IOSQE_IO_DRAIN;
	if (async)
		sqe->flags |= IOSQE_ASYNC;

	ret = io_uring_submit(ring);
	if (ret != 3) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	if (skip_success) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (cqe->user_data != 2) {
			fprintf(stderr, "Unexpected cqe %lu/%d\n",
					(unsigned long) cqe->user_data,
					cqe->res);
			goto err;
		}
		if (cqe->res != sizeof(buf)) {
			fprintf(stderr, "bad read %d\n", cqe->res);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
		return 0;
	}
	
	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 1:
			if (cqe->res) {
				fprintf(stderr, "bad open %d\n", cqe->res);
				goto err;
			}
			break;
		case 2:
			if (cqe->res != sizeof(buf)) {
				fprintf(stderr, "bad read %d\n", cqe->res);
				goto err;
			}
			break;
		case 3:
			if (cqe->res) {
				fprintf(stderr, "bad close %d\n", cqe->res);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
err:
	return 1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_params p = { };
	int ret, files[MAX_FILES];

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}
	if (!(p.features & IORING_FEAT_CQE_SKIP))
		return 0;

	memset(files, -1, sizeof(files));
	ret = io_uring_register_files(&ring, files, ARRAY_SIZE(files));
	if (ret) {
		fprintf(stderr, "Failed registering files\n");
		return 1;
	}

	t_create_file(FNAME, 4096);

	ret = test(&ring, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "test 0 0 0 failed\n");
		goto err;
	}

	ret = test(&ring, 0, 1, 0);
	if (ret) {
		fprintf(stderr, "test 0 1 0 failed\n");
		goto err;
	}

	ret = test(&ring, 0, 0, 1);
	if (ret) {
		fprintf(stderr, "test 0 0 1 failed\n");
		goto err;
	}

	ret = test(&ring, 0, 1, 1);
	if (ret) {
		fprintf(stderr, "test 0 1 1 failed\n");
		goto err;
	}

	ret = test(&ring, 1, 0, 0);
	if (ret) {
		fprintf(stderr, "test 1 0 0 failed\n");
		goto err;
	}

	ret = test(&ring, 1, 0, 1);
	if (ret) {
		fprintf(stderr, "test 1 0 1 failed\n");
		goto err;
	}

	unlink(FNAME);
	return 0;
err:
	unlink(FNAME);
	return 1;
}
