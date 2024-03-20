/* SPDX-License-Identifier: MIT */
/*
 * Description: generic tests for  io_uring drain io
 *
 * The main idea is to randomly generate different type of sqe to
 * challenge the drain logic. There are some restrictions for the
 * generated sqes, details in io_uring maillist:
 * https://lore.kernel.org/io-uring/39a49b4c-27c2-1035-b250-51daeccaab9b@linux.alibaba.com/
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>

#include "liburing.h"
#include "helpers.h"

enum {
	multi,
	single,
	nop,
	cancel,
	op_last,
};

struct sqe_info {
	__u8 op;
	unsigned flags;
};

#define max_entry 50

/*
 * sqe_flags: combination of sqe flags
 * multi_sqes: record the user_data/index of all the multishot sqes
 * cnt: how many entries there are in multi_sqes
 * we can leverage multi_sqes array for cancellation: we randomly pick
 * up an entry in multi_sqes when form a cancellation sqe.
 * multi_cap: limitation of number of multishot sqes
 */
static const unsigned sqe_flags[4] = {
	0,
	IOSQE_IO_LINK,
	IOSQE_IO_DRAIN,
	IOSQE_IO_LINK | IOSQE_IO_DRAIN
};
static int multi_sqes[max_entry], cnt = 0;
static int multi_cap = max_entry / 5;

static int write_pipe(int pipe, char *str)
{
	int ret;
	do {
		errno = 0;
		ret = write(pipe, str, 3);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

static void read_pipe(int pipe)
{
	char str[4] = {0};
	int ret;

	ret = read(pipe, &str, 3);
	if (ret < 0)
		perror("read");
}

static int trigger_event(struct io_uring *ring, int p[])
{
	int ret;
	if ((ret = write_pipe(p[1], "foo")) != 3) {
		fprintf(stderr, "bad write return %d\n", ret);
		return 1;
	}
	usleep(1000);
	io_uring_get_events(ring);
	read_pipe(p[0]);
	return 0;
}

static void io_uring_sqe_prep(int op, struct io_uring_sqe *sqe,
			      unsigned sqe_flags, int arg)
{
	switch (op) {
		case multi:
			io_uring_prep_poll_add(sqe, arg, POLLIN);
			sqe->len |= IORING_POLL_ADD_MULTI;
			break;
		case single:
			io_uring_prep_poll_add(sqe, arg, POLLIN);
			break;
		case nop:
			io_uring_prep_nop(sqe);
			break;
		case cancel:
			io_uring_prep_poll_remove(sqe, arg);
			break;
	}
	sqe->flags = sqe_flags;
}

static __u8 generate_flags(int sqe_op)
{
	__u8 flags = 0;
	/*
	 * drain sqe must be put after multishot sqes cancelled
	 */
	do {
		flags = sqe_flags[rand() % 4];
	} while ((flags & IOSQE_IO_DRAIN) && cnt);

	/*
	 * cancel req cannot have drain or link flag
	 */
	if (sqe_op == cancel) {
		flags &= ~(IOSQE_IO_DRAIN | IOSQE_IO_LINK);
	}
	/*
	 * avoid below case:
	 * sqe0(multishot, link)->sqe1(nop, link)->sqe2(nop)->sqe3(cancel_sqe0)
	 * sqe3 may execute before sqe0 so that sqe0 isn't cancelled
	 */
	if (sqe_op == multi)
		flags &= ~IOSQE_IO_LINK;

	return flags;

}

/*
 * function to generate opcode of a sqe
 * several restrictions here:
 * - cancel all the previous multishot sqes as soon as possible when
 *   we reach high watermark.
 * - ensure there is some multishot sqe when generating a cancel sqe
 * - ensure a cancel/multshot sqe is not in a linkchain
 * - ensure number of multishot sqes doesn't exceed multi_cap
 * - don't generate multishot sqes after high watermark
 */
static int generate_opcode(int i, int pre_flags)
{
	int sqe_op;
	int high_watermark = max_entry - max_entry / 5;
	bool retry0 = false, retry1 = false, retry2 = false;

	if ((i >= high_watermark) && cnt) {
		sqe_op = cancel;
	} else {
		do {
			sqe_op = rand() % op_last;
			retry0 = (sqe_op == cancel) && (!cnt || (pre_flags & IOSQE_IO_LINK));
			retry1 = (sqe_op == multi) && ((multi_cap - 1 < 0) || i >= high_watermark);
			retry2 = (sqe_op == multi) && (pre_flags & IOSQE_IO_LINK);
		} while (retry0 || retry1 || retry2);
	}

	if (sqe_op == multi)
		multi_cap--;
	return sqe_op;
}

static inline void add_multishot_sqe(int index)
{
	multi_sqes[cnt++] = index;
}

static int remove_multishot_sqe(void)
{
	int ret;

	int rem_index = rand() % cnt;
	ret = multi_sqes[rem_index];
	multi_sqes[rem_index] = multi_sqes[cnt - 1];
	cnt--;

	return ret;
}

static int test_generic_drain(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe[max_entry];
	struct sqe_info si[max_entry];
	int cqe_data[max_entry << 1], cqe_res[max_entry << 1];
	int i, j, ret, arg = 0;
	int pipes[max_entry][2];
	int pre_flags = 0;

	for (i = 0; i < max_entry; i++) {
		if (pipe(pipes[i]) != 0) {
			perror("pipe");
			return 1;
		}
	}

	srand((unsigned)time(NULL));
	for (i = 0; i < max_entry; i++) {
		sqe[i] = io_uring_get_sqe(ring);
		if (!sqe[i]) {
			printf("get sqe failed\n");
			goto err;
		}

		int sqe_op = generate_opcode(i, pre_flags);
		__u8 flags = generate_flags(sqe_op);

		if (sqe_op == cancel)
			arg = remove_multishot_sqe();
		if (sqe_op == multi || sqe_op == single)
			arg = pipes[i][0];
		io_uring_sqe_prep(sqe_op, sqe[i], flags, arg);
		sqe[i]->user_data = i;
		si[i].op = sqe_op;
		si[i].flags = flags;
		pre_flags = flags;
		if (sqe_op == multi)
			add_multishot_sqe(i);
	}

	ret = io_uring_submit(ring);
	if (ret < 0) {
		printf("sqe submit failed\n");
		goto err;
	} else if (ret < max_entry) {
		printf("Submitted only %d\n", ret);
		goto err;
	}

	sleep(1);
	// TODO: randomize event triggerring order
	for (i = 0; i < max_entry; i++) {
		if (si[i].op != multi && si[i].op != single)
			continue;

		if (trigger_event(ring, pipes[i]))
			goto err;
	}
	sleep(1);
	i = 0;
	while (!io_uring_peek_cqe(ring, &cqe)) {
		cqe_data[i] = cqe->user_data;
		cqe_res[i++] = cqe->res;
		io_uring_cqe_seen(ring, cqe);
	}

	/*
	 * compl_bits is a bit map to record completions.
	 * eg. sqe[0], sqe[1], sqe[2] fully completed
	 * then compl_bits is 000...00111b
	 *
	 */
	unsigned long long compl_bits = 0;
	for (j = 0; j < i; j++) {
		int index = cqe_data[j];
		if ((si[index].flags & IOSQE_IO_DRAIN) && index) {
			if ((~compl_bits) & ((1ULL << index) - 1)) {
				printf("drain failed\n");
				goto err;
			}
		}
		/*
		 * for multishot sqes, record them only when it is cancelled
		 */
		if ((si[index].op != multi) || (cqe_res[j] == -ECANCELED))
			compl_bits |= (1ULL << index);
	}

	return 0;
err:
	return 1;
}

static int test_simple_drain(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe[2];
	int i, ret;
	int pipe1[2], pipe2[2];

	if (pipe(pipe1) != 0 || pipe(pipe2) != 0) {
		perror("pipe");
		return 1;
	}

	for (i = 0; i < 2; i++) {
		sqe[i] = io_uring_get_sqe(ring);
		if (!sqe[i]) {
			printf("get sqe failed\n");
			goto err;
		}
	}

	io_uring_prep_poll_multishot(sqe[0], pipe1[0], POLLIN);
	sqe[0]->user_data = 0;

	io_uring_prep_poll_add(sqe[1], pipe2[0], POLLIN);
	sqe[1]->user_data = 1;

	/* This test relies on multishot poll to trigger events continually.
	 * however with IORING_SETUP_DEFER_TASKRUN this will only happen when
	 * triggered with a get_events. Hence we sprinkle get_events whenever
	 * there might be work to process in order to get the same result
	 */
	ret = io_uring_submit_and_get_events(ring);
	if (ret < 0) {
		printf("sqe submit failed\n");
		goto err;
	} else if (ret < 2) {
		printf("Submitted only %d\n", ret);
		goto err;
	}

	for (i = 0; i < 2; i++) {
		if (trigger_event(ring, pipe1))
			goto err;
	}
	if (trigger_event(ring, pipe2))
		goto err;

	for (i = 0; i < 2; i++) {
		sqe[i] = io_uring_get_sqe(ring);
		if (!sqe[i]) {
			printf("get sqe failed\n");
			goto err;
		}
	}

	io_uring_prep_poll_remove(sqe[0], 0);
	sqe[0]->user_data = 2;

	io_uring_prep_nop(sqe[1]);
	sqe[1]->flags |= IOSQE_IO_DRAIN;
	sqe[1]->user_data = 3;

	ret = io_uring_submit(ring);
	if (ret < 0) {
		printf("sqe submit failed\n");
		goto err;
	} else if (ret < 2) {
		printf("Submitted only %d\n", ret);
		goto err;
	}

	for (i = 0; i < 6; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			printf("wait completion %d\n", ret);
			goto err;
		}
		if ((i == 5) && (cqe->user_data != 3))
			goto err;
		io_uring_cqe_seen(ring, cqe);
	}

	close(pipe1[0]);
	close(pipe1[1]);
	close(pipe2[0]);
	close(pipe2[1]);
	return 0;
err:
	return 1;
}

static int test(bool defer_taskrun)
{
	struct io_uring ring;
	int i, ret;
	unsigned int flags = 0;

	if (defer_taskrun)
		flags = IORING_SETUP_SINGLE_ISSUER |
			IORING_SETUP_DEFER_TASKRUN;

	ret = io_uring_queue_init(1024, &ring, flags);
	if (ret) {
		printf("ring setup failed\n");
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 5; i++) {
		ret = test_simple_drain(&ring);
		if (ret) {
			fprintf(stderr, "test_simple_drain failed\n");
			return T_EXIT_FAIL;
		}
	}

	for (i = 0; i < 5; i++) {
		ret = test_generic_drain(&ring);
		if (ret) {
			fprintf(stderr, "test_generic_drain failed\n");
			return T_EXIT_FAIL;
		}
	}

	io_uring_queue_exit(&ring);

	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = test(false);
	if (ret != T_EXIT_PASS) {
		fprintf(stderr, "%s: test(false) failed\n", argv[0]);
		return ret;
	}

	if (t_probe_defer_taskrun()) {
		ret = test(true);
		if (ret != T_EXIT_PASS) {
			fprintf(stderr, "%s: test(true) failed\n", argv[0]);
			return ret;
		}
	}

	return ret;
}
