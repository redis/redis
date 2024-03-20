/* SPDX-License-Identifier: MIT */
/*
 * Description: run various openat(2) tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>

#include "helpers.h"
#include "liburing.h"

#define FDS	800

static int no_direct_pick;

static int submit_wait(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int ret;

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return 1;
	}
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		return 1;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static inline int try_close(struct io_uring *ring, int slot)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_close_direct(sqe, slot);
	return submit_wait(ring);
}

static int do_opens(struct io_uring *ring, const char *path, int nr,
		    int expect_enfile)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int i, ret;

	for (i = 0; i < nr; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			goto err;
		}
		io_uring_prep_openat_direct(sqe, -1, path, O_RDONLY, 0, 0);
		sqe->file_index = UINT_MAX;

		ret = io_uring_submit(ring);
		if (ret <= 0) {
			fprintf(stderr, "sqe submit failed: %d\n", ret);
			goto err;
		}
	}

	for (i = 0; i < nr; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		ret = cqe->res;
		if (ret < 0) {
			if (!expect_enfile || ret != -ENFILE) {
				printf("open=%d, %d\n", cqe->res, i);
				goto err;
			}
			if (!i && ret == -EINVAL) {
				no_direct_pick = 1;
				return 0;
			}
		}
		io_uring_cqe_seen(ring, cqe);
	}
	return 0;
err:
	return 1;
}

static int test_openat(struct io_uring *ring, const char *path)
{
	int ret, i;

	/* open all */
	ret = do_opens(ring, path, FDS, 0);
	if (ret)
		goto err;
	if (no_direct_pick)
		return 0;

	/* now close 100 randomly */
	for (i = 0; i < 100; i++) {
		do {
			int slot = rand() % FDS;
			ret = try_close(ring, slot);
			if (ret == -EBADF)
				continue;
			break;
		} while (1);
	}

	/* opening 100 should work, we closed 100 */
	ret = do_opens(ring, path, 100, 0);
	if (ret)
		goto err;

	/* we should be full now, expect -ENFILE */
	ret = do_opens(ring, path, 1, 1);
	if (ret)
		goto err;

	return ret;
err:
	fprintf(stderr,"%s: err=%d\n", __FUNCTION__, ret);
	return -1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	const char *path;
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = io_uring_register_files_sparse(&ring, FDS);
	if (ret ) {
		if (ret != -EINVAL) {
			fprintf(stderr, "Sparse file registration failed\n");
			return 1;
		}
		/* skip, kernel doesn't support sparse file array */
		return 0;
	}

	path = "/tmp/.open.direct.pick";
	t_create_file(path, 4096);

	ret = test_openat(&ring, path);
	if (ret < 0) {
		if (ret == -EINVAL) {
			fprintf(stdout, "Open not supported, skipping\n");
			goto done;
		}
		fprintf(stderr, "test_openat absolute failed: %d\n", ret);
		goto err;
	}

done:
	unlink(path);
	return 0;
err:
	unlink(path);
	return 1;
}
