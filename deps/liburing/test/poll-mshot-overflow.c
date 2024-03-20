// SPDX-License-Identifier: MIT

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>

#include "liburing.h"
#include "helpers.h"

static int check_final_cqe(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int count = 0;
	bool signalled_no_more = false;

	while (!io_uring_peek_cqe(ring, &cqe)) {
		if (cqe->user_data == 1) {
			count++;
			if (signalled_no_more) {
				fprintf(stderr, "signalled no more!\n");
				return T_EXIT_FAIL;
			}
			if (!(cqe->flags & IORING_CQE_F_MORE))
				signalled_no_more = true;
		} else if (cqe->user_data != 3) {
			fprintf(stderr, "%d: got unexpected %d\n", count, (int)cqe->user_data);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	if (!count) {
		fprintf(stderr, "no cqe\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}

static int test(bool defer_taskrun)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int pipe1[2];
	int ret, i;

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return T_EXIT_FAIL;
	}

	struct io_uring_params params = {
		/* cheat using SINGLE_ISSUER existence to know if this behaviour
		 * is updated
		 */
		.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER,
		.cq_entries = 2
	};

	if (defer_taskrun)
		params.flags |= IORING_SETUP_SINGLE_ISSUER |
				IORING_SETUP_DEFER_TASKRUN;

	ret = io_uring_queue_init_params(2, &ring, &params);
	if (ret)
		return T_EXIT_SKIP;

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return T_EXIT_FAIL;
	}
	io_uring_prep_poll_multishot(sqe, pipe1[0], POLLIN);
	io_uring_sqe_set_data64(sqe, 1);

	if (io_uring_cq_ready(&ring)) {
		fprintf(stderr, "unexpected cqe\n");
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 2; i++) {
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_nop(sqe);
		io_uring_sqe_set_data64(sqe, 2);
		io_uring_submit(&ring);
	}

	do {
		errno = 0;
		ret = write(pipe1[1], "foo", 3);
	} while (ret == -1 && errno == EINTR);

	if (ret <= 0) {
		fprintf(stderr, "write failed: %d\n", errno);
		return T_EXIT_FAIL;
	}

	/* should have 2 cqe + 1 overflow now, so take out two cqes */
	for (i = 0; i < 2; i++) {
		if (io_uring_peek_cqe(&ring, &cqe)) {
			fprintf(stderr, "unexpectedly no cqe\n");
			return T_EXIT_FAIL;
		}
		if (cqe->user_data != 2) {
			fprintf(stderr, "unexpected user_data\n");
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	/* make sure everything is processed */
	io_uring_get_events(&ring);

	/* now remove the poll */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_poll_remove(sqe, 1);
	io_uring_sqe_set_data64(sqe, 3);
	ret = io_uring_submit(&ring);

	if (ret != 1) {
		fprintf(stderr, "bad poll remove\n");
		return T_EXIT_FAIL;
	}

	ret = check_final_cqe(&ring);

	close(pipe1[0]);
	close(pipe1[1]);
	io_uring_queue_exit(&ring);

	return ret;
}

static int test_downgrade(bool support_defer)
{
	struct io_uring_cqe cqes[128];
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	int fds[2];
	int ret, i, cqe_count, tmp = 0, more_cqe_count;

	if (pipe(fds) != 0) {
		perror("pipe");
		return -1;
	}

	struct io_uring_params params = {
		.flags = IORING_SETUP_CQSIZE,
		.cq_entries = 2
	};

	ret = io_uring_queue_init_params(2, &ring, &params);
	if (ret) {
		fprintf(stderr, "queue init: %d\n", ret);
		return -1;
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		return -1;
	}
	io_uring_prep_poll_multishot(sqe, fds[0], POLLIN);
	io_uring_sqe_set_data64(sqe, 1);
	io_uring_submit(&ring);

	for (i = 0; i < 8; i++) {
		ret = write(fds[1], &tmp, sizeof(tmp));
		if (ret != sizeof(tmp)) {
			perror("write");
			return -1;
		}
		ret = read(fds[0], &tmp, sizeof(tmp));
		if (ret != sizeof(tmp)) {
			perror("read");
			return -1;
		}
	}

	cqe_count = 0;
	while (!io_uring_peek_cqe(&ring, &cqe)) {
		cqes[cqe_count++] = *cqe;
		io_uring_cqe_seen(&ring, cqe);
	}

	/* Some kernels might allow overflows to poll,
	 * but if they didn't it should stop the MORE flag
	 */
	if (cqe_count < 3) {
		fprintf(stderr, "too few cqes: %d\n", cqe_count);
		return -1;
	} else if (cqe_count == 8) {
		more_cqe_count = cqe_count;
		/* downgrade only available since support_defer */
		if (support_defer) {
			fprintf(stderr, "did not downgrade on overflow\n");
			return -1;
		}
	} else {
		more_cqe_count = cqe_count - 1;
		cqe = &cqes[cqe_count - 1];
		if (cqe->flags & IORING_CQE_F_MORE) {
			fprintf(stderr, "incorrect MORE flag %x\n", cqe->flags);
			return -1;
		}
	}

	for (i = 0; i < more_cqe_count; i++) {
		cqe = &cqes[i];
		if (!(cqe->flags & IORING_CQE_F_MORE)) {
			fprintf(stderr, "missing MORE flag\n");
			return -1;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "bad res: %d\n", cqe->res);
			return -1;
		}
	}

	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	bool support_defer;

	if (argc > 1)
		return T_EXIT_SKIP;

	support_defer = t_probe_defer_taskrun();
	ret = test_downgrade(support_defer);
	if (ret) {
		fprintf(stderr, "%s: test_downgrade(%d) failed\n", argv[0], support_defer);
		return T_EXIT_FAIL;
	}

	ret = test(false);
	if (ret == T_EXIT_SKIP)
		return ret;
	if (ret != T_EXIT_PASS) {
		fprintf(stderr, "%s: test(false) failed\n", argv[0]);
		return ret;
	}

	if (support_defer) {
		ret = test(true);
		if (ret != T_EXIT_PASS) {
			fprintf(stderr, "%s: test(true) failed\n", argv[0]);
			return ret;
		}
	}

	return ret;
}
