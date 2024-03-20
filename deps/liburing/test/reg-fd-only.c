/* SPDX-License-Identifier: MIT */
/*
 * Test io_uring_setup with IORING_SETUP_REGISTERED_FD_ONLY
 *
 */
#include <stdio.h>

#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring ring;
	unsigned values[2];
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_REGISTERED_FD_ONLY | IORING_SETUP_NO_MMAP);
	if (ret == -EINVAL)
		return T_EXIT_SKIP;
	else if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;
	}

	ret = io_uring_register_ring_fd(&ring);
	if (ret != -EEXIST) {
		fprintf(stderr, "registering already-registered ring fd should fail\n");
		goto err;
	}

	ret = io_uring_close_ring_fd(&ring);
	if (ret != -EBADF) {
		fprintf(stderr, "closing already-closed ring fd should fail\n");
		goto err;
	}

	/* Test a simple io_uring_register operation expected to work.
	 * io_uring_register_iowq_max_workers is arbitrary.
	 */
	values[0] = values[1] = 0;
	ret = io_uring_register_iowq_max_workers(&ring, values);
	if (ret || (values[0] == 0 && values[1] == 0)) {
		fprintf(stderr, "io_uring_register operation failed after closing ring fd\n");
		goto err;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;

err:
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}
