/* SPDX-License-Identifier: MIT */
/*
 * Description: link <open file><read from file><close file> with an existing
 * file present in the opened slot, verifying that we get the new file
 * rather than the old one.
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
#define FNAME1		".slot.reuse.1"
#define FNAME2		".slot.reuse.2"
#define PAT1		0xaa
#define PAT2		0x55
#define BSIZE		4096

static int test(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	char buf[BSIZE];
	int ret, i;

	/* open FNAME1 in slot 0 */
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_openat_direct(sqe, AT_FDCWD, FNAME1, O_RDONLY, 0, 0);
	sqe->user_data = 1;

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}
	if (cqe->res != 0) {
		fprintf(stderr, "open res %d\n", ret);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);

	/*
	 * Now open FNAME2 in that same slot, verifying we get data from
	 * FNAME2 and not FNAME1.
	 */
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_openat_direct(sqe, AT_FDCWD, FNAME2, O_RDONLY, 0, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, 0, buf, sizeof(buf), 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 3;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_close_direct(sqe, 0);
	sqe->user_data = 4;

	ret = io_uring_submit(ring);
	if (ret != 3) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		switch (cqe->user_data) {
		case 2:
			if (cqe->res) {
				fprintf(stderr, "bad open %d\n", cqe->res);
				goto err;
			}
			break;
		case 3:
			if (cqe->res != sizeof(buf)) {
				fprintf(stderr, "bad read %d\n", cqe->res);
				goto err;
			}
			break;
		case 4:
			if (cqe->res) {
				fprintf(stderr, "bad close %d\n", cqe->res);
				goto err;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	for (i = 0; i < sizeof(buf); i++) {
		if (buf[i] == PAT2)
			continue;
		fprintf(stderr, "Bad pattern %x at %d\n", buf[i], i);
		goto err;
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
		return T_EXIT_SKIP;

	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return T_EXIT_FAIL;
	}
	if (!(p.features & IORING_FEAT_CQE_SKIP))
		return T_EXIT_SKIP;

	memset(files, -1, sizeof(files));
	ret = io_uring_register_files(&ring, files, ARRAY_SIZE(files));
	if (ret) {
		fprintf(stderr, "Failed registering files\n");
		return T_EXIT_FAIL;
	}

	t_create_file_pattern(FNAME1, 4096, PAT1);
	t_create_file_pattern(FNAME2, 4096, PAT2);

	ret = test(&ring);
	if (ret) {
		fprintf(stderr, "test failed\n");
		goto err;
	}

	unlink(FNAME1);
	unlink(FNAME2);
	return T_EXIT_PASS;
err:
	unlink(FNAME1);
	unlink(FNAME2);
	return T_EXIT_FAIL;
}
