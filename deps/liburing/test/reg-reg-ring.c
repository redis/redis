/* SPDX-License-Identifier: MIT */
/*
 * Test io_uring_register with a registered ring (IORING_REGISTER_USE_REGISTERED_RING)
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

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return T_EXIT_FAIL;
	}

	if (!(ring.features & IORING_FEAT_REG_REG_RING)) {
		fprintf(stderr, "IORING_FEAT_REG_REG_RING not available in kernel\n");
		io_uring_queue_exit(&ring);
		return T_EXIT_SKIP;
	}

	ret = io_uring_close_ring_fd(&ring);
	if (ret != -EINVAL) {
		fprintf(stderr, "closing ring fd should EINVAL before register\n");
		goto err;
	}

	ret = io_uring_unregister_ring_fd(&ring);
	if (ret != -EINVAL) {
		fprintf(stderr, "unregistering not-registered ring fd should fail\n");
		goto err;
	}

	ret = io_uring_register_ring_fd(&ring);
	if (ret != 1) {
		fprintf(stderr, "registering ring fd failed\n");
		goto err;
	}

	ret = io_uring_register_ring_fd(&ring);
	if (ret != -EEXIST) {
		fprintf(stderr, "registering already-registered ring fd should fail\n");
		goto err;
	}

	/* Test a simple io_uring_register operation expected to work.
	 * io_uring_register_iowq_max_workers is arbitrary.
	 */
	values[0] = values[1] = 0;
	ret = io_uring_register_iowq_max_workers(&ring, values);
	if (ret || (values[0] == 0 && values[1] == 0)) {
		fprintf(stderr, "io_uring_register operation failed before closing ring fd\n");
		goto err;
	}

	ret = io_uring_close_ring_fd(&ring);
	if (ret != 1) {
		fprintf(stderr, "closing ring fd failed\n");
		goto err;
	}

	values[0] = values[1] = 0;
	ret = io_uring_register_iowq_max_workers(&ring, values);
	if (ret || (values[0] == 0 && values[1] == 0)) {
		fprintf(stderr, "io_uring_register operation failed after closing ring fd\n");
		goto err;
	}

	ret = io_uring_close_ring_fd(&ring);
	if (ret != -EBADF) {
		fprintf(stderr, "closing already-closed ring fd should fail\n");
		goto err;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;

err:
	io_uring_queue_exit(&ring);
	return T_EXIT_FAIL;
}
