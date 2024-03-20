/* SPDX-License-Identifier: MIT */
/*
 * io_uring_setup.c
 *
 * Description: Unit tests for the io_uring_setup system call.
 *
 * Copyright 2019, Red Hat, Inc.
 * Author: Jeff Moyer <jmoyer@redhat.com>
 */
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/sysinfo.h>
#include "liburing.h"
#include "helpers.h"

#include "../src/syscall.h"

/* bogus: setup returns a valid fd on success... expect can't predict the
   fd we'll get, so this really only takes 1 parameter: error */
static int try_io_uring_setup(unsigned entries, struct io_uring_params *p,
			      int expect)
{
	int ret;

	ret = io_uring_setup(entries, p);
	if (ret != expect) {
		fprintf(stderr, "expected %d, got %d\n", expect, ret);
		/* if we got a valid uring, close it */
		if (ret > 0)
			close(ret);
		return 1;
	}

	if (expect < 0 && expect != ret) {
		if (ret == -EPERM && geteuid() != 0) {
			printf("Needs root, not flagging as an error\n");
			return 0;
		}
		fprintf(stderr, "expected errno %d, got %d\n", expect, ret);
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int fd;
	unsigned int status = 0;
	struct io_uring_params p;

	if (argc > 1)
		return T_EXIT_SKIP;

	memset(&p, 0, sizeof(p));
	status |= try_io_uring_setup(0, &p, -EINVAL);
	status |= try_io_uring_setup(1, NULL, -EFAULT);

	/* resv array is non-zero */
	memset(&p, 0, sizeof(p));
	p.resv[0] = p.resv[1] = p.resv[2] = 1;
	status |= try_io_uring_setup(1, &p, -EINVAL);

	/* invalid flags */
	memset(&p, 0, sizeof(p));
	p.flags = ~0U;
	status |= try_io_uring_setup(1, &p, -EINVAL);

	/* IORING_SETUP_SQ_AFF set but not IORING_SETUP_SQPOLL */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SQ_AFF;
	status |= try_io_uring_setup(1, &p, -EINVAL);

	/* attempt to bind to invalid cpu */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
	p.sq_thread_cpu = get_nprocs_conf();
	status |= try_io_uring_setup(1, &p, -EINVAL);

	/* I think we can limit a process to a set of cpus.  I assume
	 * we shouldn't be able to setup a kernel thread outside of that.
	 * try to do that. (task->cpus_allowed) */

	/* read/write on io_uring_fd */
	memset(&p, 0, sizeof(p));
	fd = io_uring_setup(1, &p);
	if (fd < 0) {
		fprintf(stderr, "io_uring_setup failed with %d, expected success\n",
		       -fd);
		status = 1;
	} else {
		char buf[4096];
		int ret;
		ret = read(fd, buf, 4096);
		if (ret >= 0) {
			fprintf(stderr, "read from io_uring fd succeeded.  expected fail\n");
			status = 1;
		}
	}

	if (!status)
		return T_EXIT_PASS;

	fprintf(stderr, "FAIL\n");
	return T_EXIT_FAIL;
}
