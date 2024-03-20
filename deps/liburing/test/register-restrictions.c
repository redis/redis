/* SPDX-License-Identifier: MIT */
/*
 * Description: test restrictions
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>

#include "liburing.h"

enum {
	TEST_OK,
	TEST_SKIPPED,
	TEST_FAILED
};

static int test_restrictions_sqe_op(void)
{
	struct io_uring_restriction res[2];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, pipe1[2];

	uint64_t ptr;
	struct iovec vec = {
		.iov_base = &ptr,
		.iov_len = sizeof(ptr)
	};

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return TEST_FAILED;
	}

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_R_DISABLED);
	if (ret) {
		if (ret == -EINVAL)
			return TEST_SKIPPED;
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return TEST_FAILED;
	}

	res[0].opcode = IORING_RESTRICTION_SQE_OP;
	res[0].sqe_op = IORING_OP_WRITEV;

	res[1].opcode = IORING_RESTRICTION_SQE_OP;
	res[1].sqe_op = IORING_OP_WRITE;

	ret = io_uring_register_restrictions(&ring, res, 2);
	if (ret) {
		if (ret == -EINVAL)
			return TEST_SKIPPED;

		fprintf(stderr, "failed to register restrictions: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_enable_rings(&ring);
	if (ret) {
		fprintf(stderr, "ring enabling failed: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, pipe1[1], &vec, 1, 0);
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_readv(sqe, pipe1[0], &vec, 1, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	for (int i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait: %d\n", ret);
			return TEST_FAILED;
		}

		switch (cqe->user_data) {
		case 1: /* writev */
			if (cqe->res != sizeof(ptr)) {
				fprintf(stderr, "write res: %d\n", cqe->res);
				return TEST_FAILED;
			}

			break;
		case 2: /* readv should be denied */
			if (cqe->res != -EACCES) {
				fprintf(stderr, "read res: %d\n", cqe->res);
				return TEST_FAILED;
			}
			break;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	return TEST_OK;
}

static int test_restrictions_register_op(void)
{
	struct io_uring_restriction res[1];
	struct io_uring ring;
	int ret, pipe1[2];

	uint64_t ptr;
	struct iovec vec = {
		.iov_base = &ptr,
		.iov_len = sizeof(ptr)
	};

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return TEST_FAILED;
	}

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_R_DISABLED);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return TEST_FAILED;
	}

	res[0].opcode = IORING_RESTRICTION_REGISTER_OP;
	res[0].register_op = IORING_REGISTER_BUFFERS;

	ret = io_uring_register_restrictions(&ring, res, 1);
	if (ret) {
		if (ret == -EINVAL)
			return TEST_SKIPPED;

		fprintf(stderr, "failed to register restrictions: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_enable_rings(&ring);
	if (ret) {
		fprintf(stderr, "ring enabling failed: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_register_buffers(&ring, &vec, 1);
	if (ret) {
		fprintf(stderr, "io_uring_register_buffers failed: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_register_files(&ring, pipe1, 2);
	if (ret != -EACCES) {
		fprintf(stderr, "io_uring_register_files ret: %d\n", ret);
		return TEST_FAILED;
	}

	io_uring_queue_exit(&ring);
	return TEST_OK;
}

static int test_restrictions_fixed_file(void)
{
	struct io_uring_restriction res[4];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, pipe1[2];

	uint64_t ptr;
	struct iovec vec = {
		.iov_base = &ptr,
		.iov_len = sizeof(ptr)
	};

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return TEST_FAILED;
	}

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_R_DISABLED);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return TEST_FAILED;
	}

	res[0].opcode = IORING_RESTRICTION_SQE_OP;
	res[0].sqe_op = IORING_OP_WRITEV;

	res[1].opcode = IORING_RESTRICTION_SQE_OP;
	res[1].sqe_op = IORING_OP_READV;

	res[2].opcode = IORING_RESTRICTION_SQE_FLAGS_REQUIRED;
	res[2].sqe_flags = IOSQE_FIXED_FILE;

	res[3].opcode = IORING_RESTRICTION_REGISTER_OP;
	res[3].register_op = IORING_REGISTER_FILES;

	ret = io_uring_register_restrictions(&ring, res, 4);
	if (ret) {
		if (ret == -EINVAL)
			return TEST_SKIPPED;

		fprintf(stderr, "failed to register restrictions: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_enable_rings(&ring);
	if (ret) {
		fprintf(stderr, "ring enabling failed: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_register_files(&ring, pipe1, 2);
	if (ret) {
		fprintf(stderr, "io_uring_register_files ret: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, 1, &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_readv(sqe, 0, &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, pipe1[1], &vec, 1, 0);
	sqe->user_data = 3;

	ret = io_uring_submit(&ring);
	if (ret != 3) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	for (int i = 0; i < 3; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait: %d\n", ret);
			return TEST_FAILED;
		}

		switch (cqe->user_data) {
		case 1: /* writev */
			if (cqe->res != sizeof(ptr)) {
				fprintf(stderr, "write res: %d\n", cqe->res);
				return TEST_FAILED;
			}

			break;
		case 2: /* readv */
			if (cqe->res != sizeof(ptr)) {
				fprintf(stderr, "read res: %d\n", cqe->res);
				return TEST_FAILED;
			}
			break;
		case 3: /* writev without fixed_file should be denied */
			if (cqe->res != -EACCES) {
				fprintf(stderr, "write res: %d\n", cqe->res);
				return TEST_FAILED;
			}
			break;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	return TEST_OK;
}

static int test_restrictions_flags(void)
{
	struct io_uring_restriction res[3];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, pipe1[2];

	uint64_t ptr;
	struct iovec vec = {
		.iov_base = &ptr,
		.iov_len = sizeof(ptr)
	};

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return TEST_FAILED;
	}

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_R_DISABLED);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return TEST_FAILED;
	}

	res[0].opcode = IORING_RESTRICTION_SQE_OP;
	res[0].sqe_op = IORING_OP_WRITEV;

	res[1].opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED;
	res[1].sqe_flags = IOSQE_ASYNC | IOSQE_IO_LINK;

	res[2].opcode = IORING_RESTRICTION_SQE_FLAGS_REQUIRED;
	res[2].sqe_flags = IOSQE_FIXED_FILE;

	ret = io_uring_register_restrictions(&ring, res, 3);
	if (ret) {
		if (ret == -EINVAL)
			return TEST_SKIPPED;

		fprintf(stderr, "failed to register restrictions: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_register_files(&ring, pipe1, 2);
	if (ret) {
		fprintf(stderr, "io_uring_register_files ret: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_enable_rings(&ring);
	if (ret) {
		fprintf(stderr, "ring enabling failed: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, 1, &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, 1, &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE | IOSQE_ASYNC);
	sqe->user_data = 2;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, 1, &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE | IOSQE_IO_LINK);
	sqe->user_data = 3;

	ret = io_uring_submit(&ring);
	if (ret != 3) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, 1, &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE | IOSQE_IO_DRAIN);
	sqe->user_data = 4;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, pipe1[1], &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_IO_DRAIN);
	sqe->user_data = 5;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, pipe1[1], &vec, 1, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
	sqe->user_data = 6;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, pipe1[1], &vec, 1, 0);
	sqe->user_data = 7;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	for (int i = 0; i < 7; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait: %d\n", ret);
			return TEST_FAILED;
		}

		switch (cqe->user_data) {
		case 1: /* writev - flags = IOSQE_FIXED_FILE */
		case 2: /* writev - flags = IOSQE_FIXED_FILE | IOSQE_ASYNC */
		case 3: /* writev - flags = IOSQE_FIXED_FILE | IOSQE_IO_LINK */
			if (cqe->res != sizeof(ptr)) {
				fprintf(stderr, "write res: %d user_data %" PRIu64 "\n",
					cqe->res, (uint64_t) cqe->user_data);
				return TEST_FAILED;
			}

			break;
		case 4: /* writev - flags = IOSQE_FIXED_FILE | IOSQE_IO_DRAIN */
		case 5: /* writev - flags = IOSQE_IO_DRAIN */
		case 6: /* writev - flags = IOSQE_ASYNC */
		case 7: /* writev - flags = 0 */
			if (cqe->res != -EACCES) {
				fprintf(stderr, "write res: %d user_data %" PRIu64 "\n",
					cqe->res, (uint64_t) cqe->user_data);
				return TEST_FAILED;
			}
			break;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	return TEST_OK;
}

static int test_restrictions_empty(void)
{
	struct io_uring_restriction res[0];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret, pipe1[2];

	uint64_t ptr;
	struct iovec vec = {
		.iov_base = &ptr,
		.iov_len = sizeof(ptr)
	};

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return TEST_FAILED;
	}

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_R_DISABLED);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_register_restrictions(&ring, res, 0);
	if (ret) {
		if (ret == -EINVAL)
			return TEST_SKIPPED;

		fprintf(stderr, "failed to register restrictions: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_enable_rings(&ring);
	if (ret) {
		fprintf(stderr, "ring enabling failed: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_register_buffers(&ring, &vec, 1);
	if (ret != -EACCES) {
		fprintf(stderr, "io_uring_register_buffers ret: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_register_files(&ring, pipe1, 2);
	if (ret != -EACCES) {
		fprintf(stderr, "io_uring_register_files ret: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, pipe1[1], &vec, 1, 0);

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait: %d\n", ret);
		return TEST_FAILED;
	}

	if (cqe->res != -EACCES) {
		fprintf(stderr, "write res: %d\n", cqe->res);
		return TEST_FAILED;
	}

	io_uring_cqe_seen(&ring, cqe);

	io_uring_queue_exit(&ring);
	return TEST_OK;
}

static int test_restrictions_rings_not_disabled(void)
{
	struct io_uring_restriction res[1];
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return TEST_FAILED;
	}

	res[0].opcode = IORING_RESTRICTION_SQE_OP;
	res[0].sqe_op = IORING_OP_WRITEV;

	ret = io_uring_register_restrictions(&ring, res, 1);
	if (ret != -EBADFD) {
		fprintf(stderr, "io_uring_register_restrictions ret: %d\n",
			ret);
		return TEST_FAILED;
	}

	io_uring_queue_exit(&ring);
	return TEST_OK;
}

static int test_restrictions_rings_disabled(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_R_DISABLED);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return TEST_FAILED;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);

	ret = io_uring_submit(&ring);
	if (ret != -EBADFD) {
		fprintf(stderr, "submit: %d\n", ret);
		return TEST_FAILED;
	}

	io_uring_queue_exit(&ring);
	return TEST_OK;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_restrictions_sqe_op();
	if (ret == TEST_SKIPPED) {
		printf("test_restrictions_sqe_op: skipped\n");
		return 0;
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_restrictions_sqe_op failed\n");
		return ret;
	}

	ret = test_restrictions_register_op();
	if (ret == TEST_SKIPPED) {
		printf("test_restrictions_register_op: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_restrictions_register_op failed\n");
		return ret;
	}

	ret = test_restrictions_fixed_file();
	if (ret == TEST_SKIPPED) {
		printf("test_restrictions_fixed_file: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_restrictions_fixed_file failed\n");
		return ret;
	}

	ret = test_restrictions_flags();
	if (ret == TEST_SKIPPED) {
		printf("test_restrictions_flags: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_restrictions_flags failed\n");
		return ret;
	}

	ret = test_restrictions_empty();
	if (ret == TEST_SKIPPED) {
		printf("test_restrictions_empty: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_restrictions_empty failed\n");
		return ret;
	}

	ret = test_restrictions_rings_not_disabled();
	if (ret == TEST_SKIPPED) {
		printf("test_restrictions_rings_not_disabled: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_restrictions_rings_not_disabled failed\n");
		return ret;
	}

	ret = test_restrictions_rings_disabled();
	if (ret == TEST_SKIPPED) {
		printf("test_restrictions_rings_disabled: skipped\n");
	} else if (ret == TEST_FAILED) {
		fprintf(stderr, "test_restrictions_rings_disabled failed\n");
		return ret;
	}

	return 0;
}
