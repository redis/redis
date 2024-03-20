/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include "liburing.h"
#include "helpers.h"

static void sig_alrm(int sig)
{
	fprintf(stderr, "Timed out!\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring_params p;
	struct io_uring ring;
	int ret, data;

	if (argc > 1)
		return T_EXIT_SKIP;

	signal(SIGALRM, sig_alrm);

	memset(&p, 0, sizeof(p));
	p.sq_thread_idle = 100;
	p.flags = IORING_SETUP_SQPOLL;
	ret = t_create_ring_params(4, &ring, &p);
	if (ret == T_SETUP_SKIP)
		return T_EXIT_SKIP;
	else if (ret < 0)
		return T_EXIT_FAIL;

	/* make sure sq thread is sleeping at this point */
	usleep(150000);
	alarm(1);

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "sqe get failed\n");
		return T_EXIT_FAIL;
	}

	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, (void *) (unsigned long) 42);
	io_uring_submit_and_wait(&ring, 1);

	ret = io_uring_peek_cqe(&ring, &cqe);
	if (ret) {
		fprintf(stderr, "cqe get failed\n");
		return 1;
	}

	data = (unsigned long) io_uring_cqe_get_data(cqe);
	if (data != 42) {
		fprintf(stderr, "invalid data: %d\n", data);
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
