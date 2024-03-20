/* SPDX-License-Identifier: MIT */
/*
 * Description: test that io-wq affinity is correctly set for SQPOLL
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "liburing.h"
#include "helpers.h"

#define IOWQ_CPU	0
#define SQPOLL_CPU	1

static int verify_comm(pid_t pid, const char *name, int cpu)
{
	char comm[64], buf[64];
	cpu_set_t set;
	int fd, ret;

	sprintf(comm, "/proc/%d/comm", pid);
	fd = open(comm, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return T_EXIT_SKIP;
	}

	ret = read(fd, buf, sizeof(buf));
	if (ret < 0) {
		close(fd);
		return T_EXIT_SKIP;
	}

	if (strncmp(buf, name, strlen(name) - 1)) {
		close(fd);
		return T_EXIT_SKIP;
	}

	close(fd);

	ret = sched_getaffinity(pid, sizeof(set), &set);
	if (ret < 0) {
		perror("sched_getaffinity");
		return T_EXIT_SKIP;
	}

	if (CPU_COUNT(&set) != 1) {
		fprintf(stderr, "More than one CPU set in mask\n");
		return T_EXIT_FAIL;
	}
	if (!CPU_ISSET(cpu, &set)) {
		fprintf(stderr, "Wrong CPU set in mask\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}

static int verify_affinity(pid_t pid, int sqpoll)
{
	pid_t wq_pid, sqpoll_pid = -1;
	char name[64];
	int ret;

	wq_pid = pid + 2;
	if (sqpoll)
		sqpoll_pid = pid + 1;

	/* verify we had the pids right */
	sprintf(name, "iou-wrk-%d", pid);
	ret = verify_comm(wq_pid, name, IOWQ_CPU);
	if (ret != T_EXIT_PASS)
		return ret;

	if (sqpoll_pid != -1) {
		sprintf(name, "iou-sqp-%d", pid);
		ret = verify_comm(sqpoll_pid, name, SQPOLL_CPU);
		if (ret != T_EXIT_PASS)
			return ret;
	}

	return T_EXIT_PASS;
}

static int test(int sqpoll)
{
	struct io_uring_params p = { };
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	char buf[64];
	int fds[2], ret;
	cpu_set_t set;

	if (sqpoll) {
		p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
		p.sq_thread_cpu = SQPOLL_CPU;
	}

	io_uring_queue_init_params(8, &ring, &p);

	CPU_ZERO(&set);
	CPU_SET(IOWQ_CPU, &set);

	ret = io_uring_register_iowq_aff(&ring, sizeof(set), &set);
	if (ret) {
		fprintf(stderr, "register aff: %d\n", ret);
		return T_EXIT_FAIL;
	}

	if (pipe(fds) < 0) {
		perror("pipe");
		return T_EXIT_FAIL;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, fds[0], buf, sizeof(buf), 0);
	sqe->flags |= IOSQE_ASYNC;

	io_uring_submit(&ring);

	usleep(10000);

	ret = verify_affinity(getpid(), sqpoll);
	io_uring_queue_exit(&ring);
	return ret;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = test(1);
	if (ret == T_EXIT_SKIP) {
		return T_EXIT_SKIP;
	} else if (ret != T_EXIT_PASS) {
		fprintf(stderr, "test sqpoll failed\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
