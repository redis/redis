// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "liburing.h"
#include "test.h"
#include "helpers.h"

#define EXEC_FILENAME ".defer-taskrun"
#define EXEC_FILESIZE (1U<<20)

static bool can_read_t(int fd, int time)
{
	int ret;
	struct pollfd p = {
		.fd = fd,
		.events = POLLIN,
	};

	ret = poll(&p, 1, time);

	return ret == 1;
}

static bool can_read(int fd)
{
	return can_read_t(fd, 0);
}

static void eventfd_clear(int fd)
{
	uint64_t val;
	int ret;

	assert(can_read(fd));
	ret = read(fd, &val, 8);
	assert(ret == 8);
}

static void eventfd_trigger(int fd)
{
	uint64_t val = 1;
	int ret;

	ret = write(fd, &val, sizeof(val));
	assert(ret == sizeof(val));
}

#define CHECK(x)								\
do {										\
	if (!(x)) {								\
		fprintf(stderr, "%s:%d %s failed\n", __FILE__, __LINE__, #x);	\
		return -1;							\
	}									\
} while (0)


static int test_eventfd(void)
{
	struct io_uring ring;
	int ret;
	int fda, fdb;
	struct io_uring_cqe *cqe;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_DEFER_TASKRUN);
	if (ret)
		return ret;

	fda = eventfd(0, EFD_NONBLOCK);
	fdb = eventfd(0, EFD_NONBLOCK);

	CHECK(fda >= 0 && fdb >= 0);

	ret = io_uring_register_eventfd(&ring, fda);
	if (ret)
		return ret;

	CHECK(!can_read(fda));
	CHECK(!can_read(fdb));

	io_uring_prep_poll_add(io_uring_get_sqe(&ring), fdb, POLLIN);
	io_uring_submit(&ring);
	CHECK(!can_read(fda)); /* poll should not have completed */

	io_uring_prep_nop(io_uring_get_sqe(&ring));
	io_uring_submit(&ring);
	CHECK(can_read(fda)); /* nop should have */

	CHECK(io_uring_peek_cqe(&ring, &cqe) == 0);
	CHECK(cqe->res == 0);
	io_uring_cqe_seen(&ring, cqe);
	eventfd_clear(fda);

	eventfd_trigger(fdb);
	/* can take time due to rcu_call */
	CHECK(can_read_t(fda, 1000));

	/* should not have processed the cqe yet */
	CHECK(io_uring_cq_ready(&ring) == 0);

	io_uring_get_events(&ring);
	CHECK(io_uring_cq_ready(&ring) == 1);


	io_uring_queue_exit(&ring);
	return 0;
}

struct thread_data {
	struct io_uring ring;
	int efd;
	char buff[8];
};

static void *thread(void *t)
{
	struct thread_data *td = t;

	io_uring_enable_rings(&td->ring);
	io_uring_prep_read(io_uring_get_sqe(&td->ring), td->efd, td->buff, sizeof(td->buff), 0);
	io_uring_submit(&td->ring);

	return NULL;
}

static int test_thread_shutdown(void)
{
	pthread_t t1;
	int ret;
	struct thread_data td;
	struct io_uring_cqe *cqe;
	uint64_t val = 1;

	ret = io_uring_queue_init(8, &td.ring, IORING_SETUP_SINGLE_ISSUER |
					       IORING_SETUP_DEFER_TASKRUN |
					       IORING_SETUP_R_DISABLED);
	if (ret)
		return ret;

	CHECK(io_uring_get_events(&td.ring) == -EBADFD);

	td.efd = eventfd(0, 0);
	CHECK(td.efd >= 0);

	CHECK(pthread_create(&t1, NULL, thread, &td) == 0);
	CHECK(pthread_join(t1, NULL) == 0);

	CHECK(io_uring_get_events(&td.ring) == -EEXIST);

	CHECK(write(td.efd, &val, sizeof(val)) == sizeof(val));
	CHECK(io_uring_wait_cqe(&td.ring, &cqe) == -EEXIST);

	close(td.efd);
	io_uring_queue_exit(&td.ring);
	return 0;
}

static int test_exec(const char *filename)
{
	int ret;
	int fd;
	struct io_uring ring;
	pid_t fork_pid;
	static char * const new_argv[] = {"1", "2", "3", NULL};
	static char * const new_env[] = {NULL};
	char *buff;

	fork_pid = fork();
	CHECK(fork_pid >= 0);
	if (fork_pid > 0) {
		int wstatus;

		CHECK(waitpid(fork_pid, &wstatus, 0) != (pid_t)-1);
		if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) == T_EXIT_FAIL) {
			fprintf(stderr, "child failed %i\n", WEXITSTATUS(wstatus));
			return -1;
		}
		return T_EXIT_PASS;
	}

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_DEFER_TASKRUN);
	if (ret)
		return ret;

	if (filename) {
		fd = open(filename, O_RDONLY | O_DIRECT);
		if (fd < 0 && errno == EINVAL)
			return T_EXIT_SKIP;
	} else {
		t_create_file(EXEC_FILENAME, EXEC_FILESIZE);
		fd = open(EXEC_FILENAME, O_RDONLY | O_DIRECT);
		if (fd < 0 && errno == EINVAL) {
			unlink(EXEC_FILENAME);
			return T_EXIT_SKIP;
		}
		unlink(EXEC_FILENAME);
	}
	buff = (char*)malloc(EXEC_FILESIZE);
	CHECK(posix_memalign((void **)&buff, 4096, EXEC_FILESIZE) == 0);
	CHECK(buff);

	CHECK(fd >= 0);
	io_uring_prep_read(io_uring_get_sqe(&ring), fd, buff, EXEC_FILESIZE, 0);
	io_uring_submit(&ring);
	ret = execve("/proc/self/exe", new_argv, new_env);
	/* if we get here it failed anyway */
	fprintf(stderr, "execve failed %d\n", ret);
	return T_EXIT_FAIL;
}

static int test_flag(void)
{
	struct io_uring ring;
	int ret;
	int fd;
	struct io_uring_cqe *cqe;

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_DEFER_TASKRUN |
					    IORING_SETUP_TASKRUN_FLAG);
	CHECK(!ret);

	fd = eventfd(0, EFD_NONBLOCK);
	CHECK(fd >= 0);

	io_uring_prep_poll_add(io_uring_get_sqe(&ring), fd, POLLIN);
	io_uring_submit(&ring);
	CHECK(!can_read(fd)); /* poll should not have completed */

	eventfd_trigger(fd);
	CHECK(can_read(fd));

	/* should not have processed the poll cqe yet */
	CHECK(io_uring_cq_ready(&ring) == 0);

	/* flag should be set */
	CHECK(IO_URING_READ_ONCE(*ring.sq.kflags) & IORING_SQ_TASKRUN);

	/* Specifically peek, knowing we have only no cqe
	 * but because the flag is set, liburing should try and get more
	 */
	ret = io_uring_peek_cqe(&ring, &cqe);

	CHECK(ret == 0 && cqe);
	CHECK(!(IO_URING_READ_ONCE(*ring.sq.kflags) & IORING_SQ_TASKRUN));

	close(fd);
	io_uring_queue_exit(&ring);
	return 0;
}

static int test_ring_shutdown(void)
{
	struct io_uring ring;
	int ret;
	int fd[2];
	char buff = '\0';
	char send = 'X';

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_DEFER_TASKRUN |
					    IORING_SETUP_TASKRUN_FLAG);
	CHECK(!ret);

	ret = t_create_socket_pair(fd, true);
	CHECK(!ret);

	io_uring_prep_recv(io_uring_get_sqe(&ring), fd[0], &buff, 1, 0);
	io_uring_submit(&ring);

	ret = write(fd[1], &send, 1);
	CHECK(ret == 1);

	/* should not have processed the poll cqe yet */
	CHECK(io_uring_cq_ready(&ring) == 0);
	io_uring_queue_exit(&ring);

	/* task work should have been processed by now */
	CHECK(buff = 'X');

	return 0;
}

static int test_drain(void)
{
	struct io_uring ring;
	int ret, i, fd[2];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct iovec iovecs[128];
	char buff[ARRAY_SIZE(iovecs)];

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_DEFER_TASKRUN |
					    IORING_SETUP_TASKRUN_FLAG);
	CHECK(!ret);

	for (i = 0; i < ARRAY_SIZE(iovecs); i++) {
		iovecs[i].iov_base = &buff[i];
		iovecs[i].iov_len = 1;
	}

	ret = t_create_socket_pair(fd, true);
	CHECK(!ret);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_writev(sqe, fd[1], &iovecs[0], ARRAY_SIZE(iovecs), 0);
	sqe->flags |= IOSQE_IO_DRAIN;
	io_uring_submit(&ring);

	for (i = 0; i < ARRAY_SIZE(iovecs); i++)
		iovecs[i].iov_base = NULL;

	CHECK(io_uring_wait_cqe(&ring, &cqe) == 0);
	CHECK(cqe->res == 128);

	close(fd[0]);
	close(fd[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	const char *filename = NULL;

	if (argc > 2)
		return T_EXIT_SKIP;
	if (argc == 2) {
		/* This test exposes interesting behaviour with a null-blk
		 * device configured like:
		 * $ modprobe null-blk completion_nsec=100000000 irqmode=2
		 * and then run with $ defer-taskrun.t /dev/nullb0
		 */
		filename = argv[1];
	}

	if (!t_probe_defer_taskrun())
		return T_EXIT_SKIP;

	ret = test_thread_shutdown();
	if (ret) {
		fprintf(stderr, "test_thread_shutdown failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_exec(filename);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_exec failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_eventfd();
	if (ret) {
		fprintf(stderr, "eventfd failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_flag();
	if (ret) {
		fprintf(stderr, "flag failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_ring_shutdown();
	if (ret) {
		fprintf(stderr, "test_ring_shutdown failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_drain();
	if (ret) {
		fprintf(stderr, "test_drain failed\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
