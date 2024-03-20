#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "helpers.h"
#include "liburing.h"

static int no_xattr;

/* Define constants. */
#define XATTR_SIZE  255
#define QUEUE_DEPTH 32

#define FILENAME    "xattr.test"
#define KEY1        "user.val1"
#define KEY2        "user.val2"
#define VALUE1      "value1"
#define VALUE2      "value2-a-lot-longer"


/* Call fsetxattr. */
static int io_uring_fsetxattr(struct io_uring *ring, int fd, const char *name,
			      const void *value, size_t size, int flags)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "Error cannot get sqe\n");
		return -1;
	}

	io_uring_prep_fsetxattr(sqe, fd, name, value, flags, size);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "Error io_uring_submit_and_wait: ret=%d\n", ret);
		return -1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "Error io_uring_wait_cqe: ret=%d\n", ret);
		return -1;
	}

	ret = cqe->res;
	if (ret < 0) {
		if (cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP)
			no_xattr = 1;
	}
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

/* Submit fgetxattr request. */
static int io_uring_fgetxattr(struct io_uring *ring, int fd, const char *name,
			      void *value, size_t size)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "Error cannot get sqe\n");
		return -1;
	}

	io_uring_prep_fgetxattr(sqe, fd, name, value, size);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "Error io_uring_submit_and_wait: ret=%d\n", ret);
		return -1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "Error io_uring_wait_cqe: ret=%d\n", ret);
		return -1;
	}

	ret = cqe->res;
	if (ret == -1) {
		fprintf(stderr, "Error couldn'tget value\n");
		return -1;
	}

	io_uring_cqe_seen(ring, cqe);
	return ret;
}

/* Call setxattr. */
static int io_uring_setxattr(struct io_uring *ring, const char *path,
			     const char *name, const void *value, size_t size,
			     int flags)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "Error cannot get sqe\n");
		return -1;
	}

	io_uring_prep_setxattr(sqe, name, value, path, flags, size);

	ret = io_uring_submit_and_wait(ring, 1);
	if (ret != 1) {
		fprintf(stderr, "Error io_uring_submit_and_wait: ret=%d\n", ret);
		return -1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "Error io_uring_wait_cqe: ret=%d\n", ret);
		return -1;
	}

	ret = cqe->res;
	if (ret < 0) {
		if (ret == -EINVAL || ret == -EOPNOTSUPP)
			no_xattr = 1;
	}
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

/* Submit getxattr request. */
static int io_uring_getxattr(struct io_uring *ring, const char *path,
			     const char *name, void *value, size_t size)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "Error cannot get sqe\n");
		return -1;
	}

	io_uring_prep_getxattr(sqe, name, value, path, size);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "Error io_uring_submit_and_wait: ret=%d\n", ret);
		return -1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "Error io_uring_wait_cqe: ret=%d\n", ret);
		return -1;
	}

	ret = cqe->res;
	if (ret == -1) {
		fprintf(stderr, "Error couldn'tget value\n");
		return -1;
	}

	io_uring_cqe_seen(ring, cqe);
	return ret;
}

/* Test driver for fsetxattr and fgetxattr. */
static int test_fxattr(void)
{
	int rc = 0;
	size_t value_len;
	struct io_uring ring;
	char value[XATTR_SIZE];

	/* Init io-uring queue. */
	int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	if (ret) {
		fprintf(stderr, "child: ring setup failed: %d\n", ret);
		return -1;
	}

	/* Create the test file. */
	int fd = open(FILENAME, O_CREAT | O_RDWR, 0644);
	if (fd < 0) {
		fprintf(stderr, "Error: cannot open file: ret=%d\n", fd);
		return -1;
	}

	/* Test writing attributes. */
	if (io_uring_fsetxattr(&ring, fd, KEY1, VALUE1, strlen(VALUE1), 0) < 0) {
		if (no_xattr) {
			fprintf(stdout, "No xattr support, skipping\n");
			goto Exit;
		}
		fprintf(stderr, "Error fsetxattr cannot write key1\n");
		rc = -1;
		goto Exit;
	}

	if (io_uring_fsetxattr(&ring, fd, KEY2, VALUE2, strlen(VALUE2), 0) < 0) {
		fprintf(stderr, "Error fsetxattr cannot write key1\n");
		rc = -1;
		goto Exit;
	}

	/* Test reading attributes. */
	value_len = io_uring_fgetxattr(&ring, fd, KEY1, value, XATTR_SIZE);
	if (value_len != strlen(VALUE1) || strncmp(value, VALUE1, value_len)) {
		fprintf(stderr, "Error: fgetxattr expected value: %s, returned value: %s\n", VALUE1, value);
		rc = -1;
		goto Exit;
	}

	value_len = io_uring_fgetxattr(&ring, fd, KEY2, value, XATTR_SIZE);
	if (value_len != strlen(VALUE2) || strncmp(value, VALUE2, value_len)) {
		fprintf(stderr, "Error: fgetxattr expected value: %s, returned value: %s\n", VALUE2, value);
		rc = -1;
		goto Exit;
	}

	/* Cleanup. */
Exit:
	close(fd);
	unlink(FILENAME);

	io_uring_queue_exit(&ring);

	return rc;
}

/* Test driver for setxattr and getxattr. */
static int test_xattr(void)
{
	int rc = 0;
	int value_len;
	struct io_uring ring;
	char value[XATTR_SIZE];

	/* Init io-uring queue. */
	int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	if (ret) {
		fprintf(stderr, "child: ring setup failed: %d\n", ret);
		return -1;
	}

	/* Create the test file. */
	t_create_file(FILENAME, 0);

	/* Test writing attributes. */
	if (io_uring_setxattr(&ring, FILENAME, KEY1, VALUE1, strlen(VALUE1), 0) < 0) {
		fprintf(stderr, "Error setxattr cannot write key1\n");
		rc = -1;
		goto Exit;
	}

	if (io_uring_setxattr(&ring, FILENAME, KEY2, VALUE2, strlen(VALUE2), 0) < 0) {
		fprintf(stderr, "Error setxattr cannot write key1\n");
		rc = -1;
		goto Exit;
	}

	/* Test reading attributes. */
	value_len = io_uring_getxattr(&ring, FILENAME, KEY1, value, XATTR_SIZE);
	if (value_len != strlen(VALUE1) || strncmp(value, VALUE1, value_len)) {
		fprintf(stderr, "Error: getxattr expected value: %s, returned value: %s\n", VALUE1, value);
		rc = -1;
		goto Exit;
	}

	value_len = io_uring_getxattr(&ring, FILENAME, KEY2, value, XATTR_SIZE);
	if (value_len != strlen(VALUE2) || strncmp(value, VALUE2, value_len)) {
		fprintf(stderr, "Error: getxattr expected value: %s, returned value: %s\n", VALUE2, value);
		rc = -1;
		goto Exit;
	}

	/* Cleanup. */
Exit:
	io_uring_queue_exit(&ring);
	unlink(FILENAME);

	return rc;
}

/* Test driver for failure cases of fsetxattr and fgetxattr. */
static int test_failure_fxattr(void)
{
	struct io_uring ring;
	char value[XATTR_SIZE];

	/* Init io-uring queue. */
	int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	if (ret) {
		fprintf(stderr, "child: ring setup failed: %d\n", ret);
		return -1;
	}

	/* Create the test file. */
	int fd = open(FILENAME, O_CREAT | O_RDWR, 0644);
	if (fd < 0) {
		fprintf(stderr, "Error: cannot open file: ret=%d\n", fd);
		return -1;
	}

	/* Test writing attributes. */
	if (io_uring_fsetxattr(&ring, -1, KEY1, VALUE1, strlen(VALUE1), 0) >= 0)
		return 1;
	if (io_uring_fsetxattr(&ring, fd, NULL, VALUE1, strlen(VALUE1), 0) >= 0)
		return 1;
	if (io_uring_fsetxattr(&ring, fd, KEY1, NULL,   strlen(VALUE1), 0) >= 0)
		return 1;
	if (io_uring_fsetxattr(&ring, fd, KEY1, VALUE1, 0, 0) != 0)
		return 1;
	if (io_uring_fsetxattr(&ring, fd, KEY1, VALUE1, -1, 0) >= 0)
		return 1;

	/* Test reading attributes. */
	if (io_uring_fgetxattr(&ring, -1, KEY1, value, XATTR_SIZE) >= 0)
		return 1;
	if (io_uring_fgetxattr(&ring, fd, NULL, value, XATTR_SIZE) >= 0)
		return 1;
	if (io_uring_fgetxattr(&ring, fd, KEY1, value, 0) != 0)
		return 1;

	/* Cleanup. */
	close(fd);
	unlink(FILENAME);
	io_uring_queue_exit(&ring);
	return 0;
}


/* Test driver for failure cases for setxattr and getxattr. */
static int test_failure_xattr(void)
{
	struct io_uring ring;
	char value[XATTR_SIZE];

	/* Init io-uring queue. */
	int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	if (ret) {
		fprintf(stderr, "child: ring setup failed: %d\n", ret);
		return -1;
	}

	/* Create the test file. */
	t_create_file(FILENAME, 0);

	/* Test writing attributes. */
	if (io_uring_setxattr(&ring, "complete garbage", KEY1, VALUE1, strlen(VALUE1), 0) >= 0)
		return 1;
	if (io_uring_setxattr(&ring, NULL,     KEY1, VALUE1, strlen(VALUE1), 0) >= 0)
		return 1;
	if (io_uring_setxattr(&ring, FILENAME, NULL, VALUE1, strlen(VALUE1), 0) >= 0)
		return 1;
	if (io_uring_setxattr(&ring, FILENAME, KEY1, NULL,   strlen(VALUE1), 0) >= 0)
		return 1;
	if (io_uring_setxattr(&ring, FILENAME, KEY1, VALUE1, 0, 0) != 0)
		return 1;

	/* Test reading attributes. */
	if (io_uring_getxattr(&ring, "complete garbage", KEY1, value, XATTR_SIZE) >= 0)
		return 1;
	if (io_uring_getxattr(&ring, NULL,     KEY1, value, XATTR_SIZE) >= 0)
		return 1;
	if (io_uring_getxattr(&ring, FILENAME, NULL, value, XATTR_SIZE) >= 0)
		return 1;
	if (io_uring_getxattr(&ring, FILENAME, KEY1, NULL,  XATTR_SIZE) != 0)
		return 1;
	if (io_uring_getxattr(&ring, FILENAME, KEY1, value, 0) != 0)
		return 1;

	/* Cleanup. */
	io_uring_queue_exit(&ring);
	unlink(FILENAME);
	return 0;
}

/* Test for invalid SQE, this will cause a segmentation fault if enabled. */
static int test_invalid_sqe(void)
{
#ifdef DESTRUCTIVE_TEST
	struct io_uring_sqe *sqe = NULL;
	struct io_uring_cqe *cqe = NULL;
	struct io_uring ring;

	/* Init io-uring queue. */
	int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	if (ret) {
		fprintf(stderr, "child: ring setup failed: %d\n", ret);
		return -1;
	}

	/* Pass invalid SQE. */
	io_uring_prep_setxattr(sqe, FILENAME, KEY1, VALUE1, strlen(VALUE1), 0);

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "Error io_uring_submit_and_wait: ret=%d\n", ret);
		return -1;
	}

	ret = io_uring_wait_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "Error io_uring_wait_cqe: ret=%d\n", ret);
		return -1;
	}

	ret = cqe->res;
	io_uring_cqe_seen(&ring, cqe);

	return ret;
#else
	return 0;
#endif
}

/* Test driver. */
int main(int argc, char *argv[])
{
	if (argc > 1)
		return 0;

	if (test_fxattr())
		return EXIT_FAILURE;
	if (no_xattr)
		return EXIT_SUCCESS;
	if (test_xattr() || test_failure_fxattr() || test_failure_xattr() ||
	    test_invalid_sqe())
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}
