/* SPDX-License-Identifier: MIT */
/*
 * Description: test IORING_REGISTER_PROBE
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "helpers.h"
#include "liburing.h"

static int no_probe;

static int verify_probe(struct io_uring_probe *p, int full)
{
	if (!full && p->ops_len) {
		fprintf(stderr, "Got ops_len=%u\n", p->ops_len);
		return 1;
	}
	if (!p->last_op) {
		fprintf(stderr, "Got last_op=%u\n", p->last_op);
		return 1;
	}
	if (!full)
		return 0;
	/* check a few ops that must be supported */
	if (!(p->ops[IORING_OP_NOP].flags & IO_URING_OP_SUPPORTED)) {
		fprintf(stderr, "NOP not supported!?\n");
		return 1;
	}
	if (!(p->ops[IORING_OP_READV].flags & IO_URING_OP_SUPPORTED)) {
		fprintf(stderr, "READV not supported!?\n");
		return 1;
	}
	if (!(p->ops[IORING_OP_WRITE].flags & IO_URING_OP_SUPPORTED)) {
		fprintf(stderr, "WRITE not supported!?\n");
		return 1;
	}

	return 0;
}

static int test_probe_helper(struct io_uring *ring)
{
	int ret;
	struct io_uring_probe *p;

	p = io_uring_get_probe_ring(ring);
	if (!p) {
		fprintf(stderr, "Failed getting probe data\n");
		return 1;
	}

	ret = verify_probe(p, 1);
	io_uring_free_probe(p);
	return ret;
}

static int test_probe(struct io_uring *ring)
{
	struct io_uring_probe *p;
	size_t len;
	int ret;

	len = sizeof(*p) + 256 * sizeof(struct io_uring_probe_op);
	p = t_calloc(1, len);
	ret = io_uring_register_probe(ring, p, 0);
	if (ret == -EINVAL) {
		fprintf(stdout, "Probe not supported, skipping\n");
		no_probe = 1;
		goto out;
	} else if (ret) {
		fprintf(stdout, "Probe returned %d\n", ret);
		goto err;
	}

	if (verify_probe(p, 0))
		goto err;

	/* now grab for all entries */
	memset(p, 0, len);
	ret = io_uring_register_probe(ring, p, 256);
	if (ret == -EINVAL) {
		fprintf(stdout, "Probe not supported, skipping\n");
		goto err;
	} else if (ret) {
		fprintf(stdout, "Probe returned %d\n", ret);
		goto err;
	}

	if (verify_probe(p, 1))
		goto err;

out:
	free(p);
	return 0;
err:
	free(p);
	return 1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}

	ret = test_probe(&ring);
	if (ret) {
		fprintf(stderr, "test_probe failed\n");
		return ret;
	}
	if (no_probe)
		return 0;

	ret = test_probe_helper(&ring);
	if (ret) {
		fprintf(stderr, "test_probe failed\n");
		return ret;
	}


	return 0;
}
