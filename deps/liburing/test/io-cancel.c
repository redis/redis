/* SPDX-License-Identifier: MIT */
/*
 * Description: Basic IO cancel test
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <poll.h>

#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE	(128 * 1024)
#define BS		4096
#define BUFFERS		(FILE_SIZE / BS)

static struct iovec *vecs;

static unsigned long long utime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000000;
	return sec + usec;
}

static unsigned long long utime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return utime_since(tv, &end);
}

static int start_io(struct io_uring *ring, int fd, int do_write)
{
	struct io_uring_sqe *sqe;
	int i, ret;

	for (i = 0; i < BUFFERS; i++) {
		off_t offset;

		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "sqe get failed\n");
			goto err;
		}
		offset = BS * (rand() % BUFFERS);
		if (do_write) {
			io_uring_prep_writev(sqe, fd, &vecs[i], 1, offset);
		} else {
			io_uring_prep_readv(sqe, fd, &vecs[i], 1, offset);
		}
		sqe->user_data = i + 1;
	}

	ret = io_uring_submit(ring);
	if (ret != BUFFERS) {
		fprintf(stderr, "submit got %d, wanted %d\n", ret, BUFFERS);
		goto err;
	}

	return 0;
err:
	return 1;
}

static int wait_io(struct io_uring *ring, unsigned nr_io, int do_partial)
{
	struct io_uring_cqe *cqe;
	int i, ret;

	for (i = 0; i < nr_io; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			goto err;
		}
		if (do_partial && cqe->user_data) {
			if (!(cqe->user_data & 1)) {
				if (cqe->res != BS) {
					fprintf(stderr, "IO %d wasn't cancelled but got error %d\n", (unsigned) cqe->user_data, cqe->res);
					goto err;
				}
			}
		}
		io_uring_cqe_seen(ring, cqe);
	}
	return 0;
err:
	return 1;

}

static int do_io(struct io_uring *ring, int fd, int do_write)
{
	if (start_io(ring, fd, do_write))
		return 1;
	if (wait_io(ring, BUFFERS, 0))
		return 1;
	return 0;
}

static int start_cancel(struct io_uring *ring, int do_partial, int async_cancel)
{
	struct io_uring_sqe *sqe;
	int i, ret, submitted = 0;

	for (i = 0; i < BUFFERS; i++) {
		if (do_partial && (i & 1))
			continue;
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "sqe get failed\n");
			goto err;
		}
		io_uring_prep_cancel64(sqe, i + 1, 0);
		if (async_cancel)
			sqe->flags |= IOSQE_ASYNC;
		sqe->user_data = 0;
		submitted++;
	}

	ret = io_uring_submit(ring);
	if (ret != submitted) {
		fprintf(stderr, "submit got %d, wanted %d\n", ret, submitted);
		goto err;
	}
	return 0;
err:
	return 1;
}

/*
 * Test cancels. If 'do_partial' is set, then we only attempt to cancel half of
 * the submitted IO. This is done to verify that cancelling one piece of IO doesn't
 * impact others.
 */
static int test_io_cancel(const char *file, int do_write, int do_partial,
			  int async_cancel)
{
	struct io_uring ring;
	struct timeval start_tv;
	unsigned long usecs;
	unsigned to_wait;
	int fd, ret;

	fd = open(file, O_RDWR | O_DIRECT);
	if (fd < 0) {
		if (errno == EINVAL)
			return T_EXIT_SKIP;
		perror("file open");
		goto err;
	}

	ret = io_uring_queue_init(4 * BUFFERS, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		goto err;
	}

	if (do_io(&ring, fd, do_write))
		goto err;
	gettimeofday(&start_tv, NULL);
	if (do_io(&ring, fd, do_write))
		goto err;
	usecs = utime_since_now(&start_tv);

	if (start_io(&ring, fd, do_write))
		goto err;
	/* sleep for 1/3 of the total time, to allow some to start/complete */
	usleep(usecs / 3);
	if (start_cancel(&ring, do_partial, async_cancel))
		goto err;
	to_wait = BUFFERS;
	if (do_partial)
		to_wait += BUFFERS / 2;
	else
		to_wait += BUFFERS;
	if (wait_io(&ring, to_wait, do_partial))
		goto err;

	io_uring_queue_exit(&ring);
	close(fd);
	return 0;
err:
	if (fd != -1)
		close(fd);
	return 1;
}

static int test_dont_cancel_another_ring(void)
{
	struct io_uring ring1, ring2;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	char buffer[128];
	int ret, fds[2];
	struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000, };

	ret = io_uring_queue_init(8, &ring1, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}
	ret = io_uring_queue_init(8, &ring2, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}
	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}

	sqe = io_uring_get_sqe(&ring1);
	if (!sqe) {
		fprintf(stderr, "%s: failed to get sqe\n", __FUNCTION__);
		return 1;
	}
	io_uring_prep_read(sqe, fds[0], buffer, 10, 0);
	sqe->flags |= IOSQE_ASYNC;
	sqe->user_data = 1;

	ret = io_uring_submit(&ring1);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}

	/* make sure it doesn't cancel requests of the other ctx */
	sqe = io_uring_get_sqe(&ring2);
	if (!sqe) {
		fprintf(stderr, "%s: failed to get sqe\n", __FUNCTION__);
		return 1;
	}
	io_uring_prep_cancel64(sqe, 1, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(&ring2);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}

	ret = io_uring_wait_cqe(&ring2, &cqe);
	if (ret) {
		fprintf(stderr, "wait_cqe=%d\n", ret);
		return 1;
	}
	if (cqe->user_data != 2 || cqe->res != -ENOENT) {
		fprintf(stderr, "error: cqe %i: res=%i, but expected -ENOENT\n",
			(int)cqe->user_data, (int)cqe->res);
		return 1;
	}
	io_uring_cqe_seen(&ring2, cqe);

	ret = io_uring_wait_cqe_timeout(&ring1, &cqe, &ts);
	if (ret != -ETIME) {
		fprintf(stderr, "read got cancelled or wait failed\n");
		return 1;
	}
	io_uring_cqe_seen(&ring1, cqe);

	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring1);
	io_uring_queue_exit(&ring2);
	return 0;
}

static int test_cancel_req_across_fork(void)
{
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	char buffer[128];
	int ret, i, fds[2];
	pid_t p;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}
	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}
	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "%s: failed to get sqe\n", __FUNCTION__);
		return 1;
	}
	io_uring_prep_read(sqe, fds[0], buffer, 10, 0);
	sqe->flags |= IOSQE_ASYNC;
	sqe->user_data = 1;

	ret = io_uring_submit(&ring);
	if (ret != 1) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}

	p = fork();
	if (p == -1) {
		fprintf(stderr, "fork() failed\n");
		return 1;
	}

	if (p == 0) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "%s: failed to get sqe\n", __FUNCTION__);
			return 1;
		}
		io_uring_prep_cancel64(sqe, 1, 0);
		sqe->user_data = 2;

		ret = io_uring_submit(&ring);
		if (ret != 1) {
			fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
			return 1;
		}

		for (i = 0; i < 2; ++i) {
			ret = io_uring_wait_cqe(&ring, &cqe);
			if (ret) {
				fprintf(stderr, "wait_cqe=%d\n", ret);
				return 1;
			}
			switch (cqe->user_data) {
			case 1:
				if (cqe->res != -EINTR &&
				    cqe->res != -ECANCELED) {
					fprintf(stderr, "%i %i\n", (int)cqe->user_data, cqe->res);
					exit(1);
				}
				break;
			case 2:
				if (cqe->res != -EALREADY && cqe->res) {
					fprintf(stderr, "%i %i\n", (int)cqe->user_data, cqe->res);
					exit(1);
				}
				break;
			default:
				fprintf(stderr, "%i %i\n", (int)cqe->user_data, cqe->res);
				exit(1);
			}

			io_uring_cqe_seen(&ring, cqe);
		}
		exit(0);
	} else {
		int wstatus;
		pid_t childpid;

		do {
			childpid = waitpid(p, &wstatus, 0);
		} while (childpid == (pid_t)-1 && errno == EINTR);

		if (childpid == (pid_t)-1) {
			perror("waitpid()");
			return 1;
		}
		if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus)) {
			fprintf(stderr, "child failed %i\n", WEXITSTATUS(wstatus));
			return 1;
		}
	}

	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

static int test_cancel_inflight_exit(void)
{
	struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0, };
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;
	pid_t p;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}
	p = fork();
	if (p == -1) {
		fprintf(stderr, "fork() failed\n");
		return 1;
	}

	if (p == 0) {
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_poll_add(sqe, ring.ring_fd, POLLIN);
		sqe->user_data = 1;
		sqe->flags |= IOSQE_IO_LINK;

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_timeout(sqe, &ts, 0, 0);
		sqe->user_data = 2;

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_timeout(sqe, &ts, 0, 0);
		sqe->user_data = 3;

		ret = io_uring_submit(&ring);
		if (ret != 3) {
			fprintf(stderr, "io_uring_submit() failed %s, ret %i\n", __FUNCTION__, ret);
			exit(1);
		}
		exit(0);
	} else {
		int wstatus;

		if (waitpid(p, &wstatus, 0) == (pid_t)-1) {
			perror("waitpid()");
			return 1;
		}
		if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus)) {
			fprintf(stderr, "child failed %i\n", WEXITSTATUS(wstatus));
			return 1;
		}
	}

	for (i = 0; i < 3; ++i) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return 1;
		}
		if ((cqe->user_data == 1 && cqe->res != -ECANCELED) ||
		    (cqe->user_data == 2 && cqe->res != -ECANCELED) ||
		    (cqe->user_data == 3 && cqe->res != -ETIME)) {
			fprintf(stderr, "%i %i\n", (int)cqe->user_data, cqe->res);
			return 1;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	io_uring_queue_exit(&ring);
	return 0;
}

static int test_sqpoll_cancel_iowq_requests(void)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	int ret, fds[2];
	char buffer[16];

	ret = io_uring_queue_init(8, &ring, IORING_SETUP_SQPOLL);
	if (ret) {
		fprintf(stderr, "ring create failed: %d\n", ret);
		return 1;
	}
	if (pipe(fds)) {
		perror("pipe");
		return 1;
	}
	/* pin both pipe ends via io-wq */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_read(sqe, fds[0], buffer, 10, 0);
	sqe->flags |= IOSQE_ASYNC | IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_write(sqe, fds[1], buffer, 10, 0);
	sqe->flags |= IOSQE_ASYNC;
	sqe->user_data = 2;
	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "%s: got %d, wanted 1\n", __FUNCTION__, ret);
		return 1;
	}

	/* wait for sqpoll to kick in and submit before exit */
	sleep(1);
	io_uring_queue_exit(&ring);

	/* close the write end, so if ring is cancelled properly read() fails*/
	close(fds[1]);
	ret = read(fds[0], buffer, 10);
	close(fds[0]);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *fname = ".io-cancel-test";
	int i, ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	if (test_dont_cancel_another_ring()) {
		fprintf(stderr, "test_dont_cancel_another_ring() failed\n");
		return T_EXIT_FAIL;
	}

	if (test_cancel_req_across_fork()) {
		fprintf(stderr, "test_cancel_req_across_fork() failed\n");
		return T_EXIT_FAIL;
	}

	if (test_cancel_inflight_exit()) {
		fprintf(stderr, "test_cancel_inflight_exit() failed\n");
		return T_EXIT_FAIL;
	}

	if (test_sqpoll_cancel_iowq_requests()) {
		fprintf(stderr, "test_sqpoll_cancel_iowq_requests() failed\n");
		return T_EXIT_FAIL;
	}

	t_create_file(fname, FILE_SIZE);

	vecs = t_create_buffers(BUFFERS, BS);

	for (i = 0; i < 8; i++) {
		int write = (i & 1) != 0;
		int partial = (i & 2) != 0;
		int async = (i & 4) != 0;

		ret = test_io_cancel(fname, write, partial, async);
		if (ret == T_EXIT_FAIL) {
			fprintf(stderr, "test_io_cancel %d %d %d failed\n",
				write, partial, async);
			goto err;
		}
	}

	unlink(fname);
	return T_EXIT_PASS;
err:
	unlink(fname);
	return T_EXIT_FAIL;
}
