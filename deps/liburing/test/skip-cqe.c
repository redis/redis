/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include "liburing.h"
#include "helpers.h"

#define LINK_SIZE 		6
#define TIMEOUT_USER_DATA	(-1)

static int fds[2];

/* should be successfully submitted but fails during execution */
static void prep_exec_fail_req(struct io_uring_sqe *sqe)
{
	io_uring_prep_write(sqe, fds[1], NULL, 100, 0);
}

static int test_link_success(struct io_uring *ring, int nr, bool skip_last)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	for (i = 0; i < nr; ++i) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_nop(sqe);
		if (i != nr - 1 || skip_last)
			sqe->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
		sqe->user_data = i;
	}

	ret = io_uring_submit(ring);
	if (ret != nr) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	if (!skip_last) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret != 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (cqe->res != 0) {
			fprintf(stderr, "nop failed: res %d\n", cqe->res);
			goto err;
		}
		if (cqe->user_data != nr - 1) {
			fprintf(stderr, "invalid user_data %i\n", (int)cqe->user_data);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	if (io_uring_peek_cqe(ring, &cqe) >= 0) {
		fprintf(stderr, "single CQE expected %i\n", (int)cqe->user_data);
		goto err;
	}
	return 0;
err:
	return 1;
}

static int test_link_fail(struct io_uring *ring, int nr, int fail_idx)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	for (i = 0; i < nr; ++i) {
		sqe = io_uring_get_sqe(ring);
		if (i == fail_idx)
			prep_exec_fail_req(sqe);
		else
			io_uring_prep_nop(sqe);

		if (i != nr - 1)
			sqe->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
		sqe->user_data = i;
	}

	ret = io_uring_submit(ring);
	if (ret != nr) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret != 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}
	if (!cqe->res || cqe->user_data != fail_idx) {
		fprintf(stderr, "got: user_data %d res %d, expected data: %d\n",
				(int)cqe->user_data, cqe->res, fail_idx);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);

	if (io_uring_peek_cqe(ring, &cqe) >= 0) {
		fprintf(stderr, "single CQE expected %i\n", (int)cqe->user_data);
		goto err;
	}
	return 0;
err:
	return 1;
}

static int test_ltimeout_cancel(struct io_uring *ring, int nr, int tout_idx,
				bool async, int fail_idx)
{
	struct __kernel_timespec ts = {.tv_sec = 1, .tv_nsec = 0};
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;
	int e_res = 0, e_idx = nr - 1;

	if (fail_idx >= 0) {
		e_res = -EFAULT;
		e_idx = fail_idx;
	}

	for (i = 0; i < nr; ++i) {
		sqe = io_uring_get_sqe(ring);
		if (i == fail_idx)
			prep_exec_fail_req(sqe);
		else
			io_uring_prep_nop(sqe);
		sqe->user_data = i;
		sqe->flags |= IOSQE_IO_LINK;
		if (async)
			sqe->flags |= IOSQE_ASYNC;
		if (i != nr - 1)
			sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;

		if (i == tout_idx) {
			sqe = io_uring_get_sqe(ring);
			io_uring_prep_link_timeout(sqe, &ts, 0);
			sqe->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
			sqe->user_data = TIMEOUT_USER_DATA;
		}
	}

	ret = io_uring_submit(ring);
	if (ret != nr + 1) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}
	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret != 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}
	if (cqe->user_data != e_idx) {
		fprintf(stderr, "invalid user_data %i\n", (int)cqe->user_data);
		goto err;
	}
	if (cqe->res != e_res) {
		fprintf(stderr, "unexpected res: %d\n", cqe->res);
		goto err;
	}
	io_uring_cqe_seen(ring, cqe);

	if (io_uring_peek_cqe(ring, &cqe) >= 0) {
		fprintf(stderr, "single CQE expected %i\n", (int)cqe->user_data);
		goto err;
	}
	return 0;
err:
	return 1;
}

static int test_ltimeout_fire(struct io_uring *ring, bool async,
			      bool skip_main, bool skip_tout)
{
	char buf[1];
	struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;
	int nr = 1 + !skip_tout;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, fds[0], buf, sizeof(buf), 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->flags |= async ? IOSQE_ASYNC : 0;
	sqe->flags |= skip_main ? IOSQE_CQE_SKIP_SUCCESS : 0;
	sqe->user_data = 0;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->flags |= skip_tout ? IOSQE_CQE_SKIP_SUCCESS : 0;
	sqe->user_data = 1;

	ret = io_uring_submit(ring);
	if (ret != 2) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		return 1;
	}

	for (i = 0; i < nr; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret != 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			return 1;
		}
		switch (cqe->user_data) {
		case 0:
			if (cqe->res != -ECANCELED && cqe->res != -EINTR) {
				fprintf(stderr, "unexpected read return: %d\n", cqe->res);
				return 1;
			}
			break;
		case 1:
			if (skip_tout) {
				fprintf(stderr, "extra timeout cqe, %d\n", cqe->res);
				return 1;
			}
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}


	if (io_uring_peek_cqe(ring, &cqe) >= 0) {
		fprintf(stderr, "single CQE expected: got data: %i res: %i\n",
				(int)cqe->user_data, cqe->res);
		return 1;
	}
	return 0;
}

static int test_hardlink(struct io_uring *ring, int nr, int fail_idx,
			int skip_idx, bool hardlink_last)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret, i;

	assert(fail_idx < nr);
	assert(skip_idx < nr);

	for (i = 0; i < nr; i++) {
		sqe = io_uring_get_sqe(ring);
		if (i == fail_idx)
			prep_exec_fail_req(sqe);
		else
			io_uring_prep_nop(sqe);
		if (i != nr - 1 || hardlink_last)
			sqe->flags |= IOSQE_IO_HARDLINK;
		if (i == skip_idx)
			sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
		sqe->user_data = i;
	}

	ret = io_uring_submit(ring);
	if (ret != nr) {
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	for (i = 0; i < nr; i++) {
		if (i == skip_idx && fail_idx != skip_idx)
			continue;

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret != 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		if (cqe->user_data != i) {
			fprintf(stderr, "invalid user_data %d (%i)\n",
				(int)cqe->user_data, i);
			goto err;
		}
		if (i == fail_idx) {
			if (cqe->res >= 0) {
				fprintf(stderr, "req should've failed %d %d\n",
					(int)cqe->user_data, cqe->res);
				goto err;
			}
		} else {
			if (cqe->res) {
				fprintf(stderr, "req error %d %d\n",
					(int)cqe->user_data, cqe->res);
				goto err;
			}
		}

		io_uring_cqe_seen(ring, cqe);
	}

	if (io_uring_peek_cqe(ring, &cqe) >= 0) {
		fprintf(stderr, "single CQE expected %i\n", (int)cqe->user_data);
		goto err;
	}
	return 0;
err:
	return 1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret, i, j, k;
	int mid_idx = LINK_SIZE / 2;
	int last_idx = LINK_SIZE - 1;

	if (argc > 1)
		return 0;

	if (pipe(fds)) {
		fprintf(stderr, "pipe() failed\n");
		return 1;
	}
	ret = io_uring_queue_init(16, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	if (!(ring.features & IORING_FEAT_CQE_SKIP))
		return T_EXIT_SKIP;

	for (i = 0; i < 4; i++) {
		bool skip_last = i & 1;
		int sz = (i & 2) ? LINK_SIZE : 1;

		ret = test_link_success(&ring, sz, skip_last);
		if (ret) {
			fprintf(stderr, "test_link_success sz %d, %d last\n",
					skip_last, sz);
			return ret;
		}
	}

	ret = test_link_fail(&ring, LINK_SIZE, mid_idx);
	if (ret) {
		fprintf(stderr, "test_link_fail mid failed\n");
		return ret;
	}

	ret = test_link_fail(&ring, LINK_SIZE, last_idx);
	if (ret) {
		fprintf(stderr, "test_link_fail last failed\n");
		return ret;
	}

	for (i = 0; i < 2; i++) {
		bool async = i & 1;

		ret = test_ltimeout_cancel(&ring, 1, 0, async, -1);
		if (ret) {
			fprintf(stderr, "test_ltimeout_cancel 1 failed, %i\n",
					async);
			return ret;
		}
		ret = test_ltimeout_cancel(&ring, LINK_SIZE, mid_idx, async, -1);
		if (ret) {
			fprintf(stderr, "test_ltimeout_cancel mid failed, %i\n",
					async);
			return ret;
		}
		ret = test_ltimeout_cancel(&ring, LINK_SIZE, last_idx, async, -1);
		if (ret) {
			fprintf(stderr, "test_ltimeout_cancel last failed, %i\n",
					async);
			return ret;
		}
		ret = test_ltimeout_cancel(&ring, LINK_SIZE, mid_idx, async, mid_idx);
		if (ret) {
			fprintf(stderr, "test_ltimeout_cancel fail mid failed, %i\n",
					async);
			return ret;
		}
		ret = test_ltimeout_cancel(&ring, LINK_SIZE, mid_idx, async, mid_idx - 1);
		if (ret) {
			fprintf(stderr, "test_ltimeout_cancel fail2 mid failed, %i\n",
					async);
			return ret;
		}
		ret = test_ltimeout_cancel(&ring, LINK_SIZE, mid_idx, async, mid_idx + 1);
		if (ret) {
			fprintf(stderr, "test_ltimeout_cancel fail3 mid failed, %i\n",
					async);
			return ret;
		}
	}

	for (i = 0; i < 8; i++) {
		bool async = i & 1;
		bool skip1 = i & 2;
		bool skip2 = i & 4;

		ret = test_ltimeout_fire(&ring, async, skip1, skip2);
		if (ret) {
			fprintf(stderr, "test_ltimeout_fire failed\n");
			return ret;
		}
	}

	/* test 3 positions, start/middle/end of the link, i.e. indexes 0, 3, 6 */
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			for (k = 0; k < 2; k++) {
				bool mark_last = k & 1;

				ret = test_hardlink(&ring, 7, i * 3, j * 3, mark_last);
				if (ret) {
					fprintf(stderr, "test_hardlink failed"
							"fail %i skip %i mark last %i\n",
						i * 3, j * 3, k);
					return 1;
				}
			}
		}
	}

	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}
