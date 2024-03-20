/* SPDX-License-Identifier: MIT */
/*
 * Description: trigger segfault. A recent 6.4-rc kernel introduced a bug
 *		via vhost where segfaults for applications using io_uring
 *		would hang in D state forever upon trying to generate the
 *		core file. Perform a trivial test where a child process
 *		generates a NULL pointer dereference and ensure that we don't
 *		hang.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#include "liburing.h"
#include "helpers.h"

static void test(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int *ptr = NULL;
	int fds[2];
	char r1;

	if (pipe(fds) < 0) {
		perror("pipe");
		exit(0);
	}

	io_uring_queue_init(8, &ring, 0);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, fds[0], &r1, sizeof(r1), 0);
	sqe->flags = IOSQE_ASYNC;
	sqe->user_data = 1;

	io_uring_submit(&ring);
	*ptr = 0;
	exit(0);
}

int main(int argc, char *argv[])
{
	pid_t pid;
	int wstat;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return T_EXIT_SKIP;
	} else if (!pid) {
		test();
	}

	wait(&wstat);
	return T_EXIT_PASS;
}
