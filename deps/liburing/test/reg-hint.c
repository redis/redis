/* SPDX-License-Identifier: MIT */
/*
 * Test alloc hint sanity after unregistering the file table
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	io_uring_queue_init(1, &ring, 0);

	ret = io_uring_register_files_sparse(&ring, 16);
	if (ret) {
		if (ret == -EINVAL)
			return T_EXIT_SKIP;

		fprintf(stderr, "Failed to register file table: %d\n", ret);
		return T_EXIT_FAIL;
	}
	io_uring_unregister_files(&ring);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_socket_direct_alloc(sqe, AF_UNIX, SOCK_DGRAM, 0, 0);

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait cqe: %d\n", ret);
		return T_EXIT_FAIL;
	}

	if (cqe->res != -ENFILE) {
		fprintf(stderr, "Bad CQE res: %d\n", cqe->res);
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(&ring, cqe);
	return T_EXIT_PASS;
}
