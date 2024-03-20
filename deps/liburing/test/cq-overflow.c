/* SPDX-License-Identifier: MIT */
/*
 * Description: run various CQ ring overflow tests
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include "helpers.h"
#include "liburing.h"

#define FILE_SIZE	(256 * 1024)
#define BS		4096
#define BUFFERS		(FILE_SIZE / BS)

static struct iovec *vecs;

#define ENTRIES	8

/*
 * io_uring has rare cases where CQEs are lost.
 * This happens when there is no space in the CQ ring, and also there is no
 * GFP_ATOMIC memory available. In reality this probably means that the process
 * is about to be killed as many other things might start failing, but we still
 * want to test that liburing and the kernel deal with this properly. The fault
 * injection framework allows us to test this scenario. Unfortunately this
 * requires some system wide changes and so we do not enable this by default.
 * The tests in this file should work in both cases (where overflows are queued
 * and where they are dropped) on recent kernels.
 *
 * In order to test dropped CQEs you should enable fault injection in the kernel
 * config:
 *
 * CONFIG_FAULT_INJECTION=y
 * CONFIG_FAILSLAB=y
 * CONFIG_FAULT_INJECTION_DEBUG_FS=y
 *
 * and then run the test as follows:
 * echo Y > /sys/kernel/debug/failslab/task-filter
 * echo 100 > /sys/kernel/debug/failslab/probability
 * echo 0 > /sys/kernel/debug/failslab/verbose
 * echo 100000 > /sys/kernel/debug/failslab/times
 * bash -c "echo 1 > /proc/self/make-it-fail && exec ./cq-overflow.t"
 */

static int test_io(const char *file, unsigned long usecs, unsigned *drops,
		   int fault)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring_params p;
	unsigned reaped, total;
	struct io_uring ring;
	int nodrop, i, fd, ret;
	bool cqe_dropped = false;

	fd = open(file, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		if (errno == EINVAL)
			return T_EXIT_SKIP;
		perror("file open");
		return T_EXIT_FAIL;
	}

	memset(&p, 0, sizeof(p));
	ret = io_uring_queue_init_params(ENTRIES, &ring, &p);
	if (ret) {
		close(fd);
		fprintf(stderr, "ring create failed: %d\n", ret);
		return T_EXIT_FAIL;
	}
	nodrop = 0;
	if (p.features & IORING_FEAT_NODROP)
		nodrop = 1;

	total = 0;
	for (i = 0; i < BUFFERS / 2; i++) {
		off_t offset;

		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "sqe get failed\n");
			goto err;
		}
		offset = BS * (rand() % BUFFERS);
		if (fault && i == ENTRIES + 4)
			vecs[i].iov_base = NULL;
		io_uring_prep_readv(sqe, fd, &vecs[i], 1, offset);

		ret = io_uring_submit(&ring);
		if (nodrop && ret == -EBUSY) {
			*drops = 1;
			total = i;
			break;
		} else if (ret != 1) {
			fprintf(stderr, "submit got %d, wanted %d\n", ret, 1);
			total = i;
			break;
		}
		total++;
	}

	if (*drops)
		goto reap_it;

	usleep(usecs);

	for (i = total; i < BUFFERS; i++) {
		off_t offset;

		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "sqe get failed\n");
			goto err;
		}
		offset = BS * (rand() % BUFFERS);
		io_uring_prep_readv(sqe, fd, &vecs[i], 1, offset);

		ret = io_uring_submit(&ring);
		if (nodrop && ret == -EBUSY) {
			*drops = 1;
			break;
		} else if (ret != 1) {
			fprintf(stderr, "submit got %d, wanted %d\n", ret, 1);
			break;
		}
		total++;
	}

reap_it:
	reaped = 0;
	do {
		if (nodrop && !cqe_dropped) {
			/* nodrop should never lose events unless cqe_dropped */
			if (reaped == total)
				break;
		} else {
			if (reaped + *ring.cq.koverflow == total)
				break;
		}
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (nodrop && ret == -EBADR) {
			cqe_dropped = true;
			continue;
		} else if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			goto err;
		}
		if (cqe->res != BS) {
			if (!(fault && cqe->res == -EFAULT)) {
				fprintf(stderr, "cqe res %d, wanted %d\n",
						cqe->res, BS);
				goto err;
			}
		}
		io_uring_cqe_seen(&ring, cqe);
		reaped++;
	} while (1);

	if (!io_uring_peek_cqe(&ring, &cqe)) {
		fprintf(stderr, "found unexpected completion\n");
		goto err;
	}

	if (!nodrop || cqe_dropped) {
		*drops = *ring.cq.koverflow;
	} else if (*ring.cq.koverflow) {
		fprintf(stderr, "Found %u overflows\n", *ring.cq.koverflow);
		goto err;
	}

	io_uring_queue_exit(&ring);
	close(fd);
	return T_EXIT_PASS;
err:
	if (fd != -1)
		close(fd);
	io_uring_queue_exit(&ring);
	return T_EXIT_SKIP;
}

static int reap_events(struct io_uring *ring, unsigned nr_events, int do_wait)
{
	struct io_uring_cqe *cqe;
	int i, ret = 0, seq = 0;
	unsigned int start_overflow = *ring->cq.koverflow;
	bool dropped = false;

	for (i = 0; i < nr_events; i++) {
		if (do_wait)
			ret = io_uring_wait_cqe(ring, &cqe);
		else
			ret = io_uring_peek_cqe(ring, &cqe);
		if (do_wait && ret == -EBADR) {
			unsigned int this_drop = *ring->cq.koverflow -
				start_overflow;

			dropped = true;
			start_overflow = *ring->cq.koverflow;
			assert(this_drop > 0);
			i += (this_drop - 1);
			continue;
		} else if (ret) {
			if (ret != -EAGAIN)
				fprintf(stderr, "cqe peek failed: %d\n", ret);
			break;
		}
		if (!dropped && cqe->user_data != seq) {
			fprintf(stderr, "cqe sequence out-of-order\n");
			fprintf(stderr, "got %d, wanted %d\n", (int) cqe->user_data,
					seq);
			return -EINVAL;
		}
		seq++;
		io_uring_cqe_seen(ring, cqe);
	}

	return i ? i : ret;
}

/*
 * Submit some NOPs and watch if the overflow is correct
 */
static int test_overflow(void)
{
	struct io_uring ring;
	struct io_uring_params p;
	struct io_uring_sqe *sqe;
	unsigned pending;
	int ret, i, j;

	memset(&p, 0, sizeof(p));
	ret = io_uring_queue_init_params(4, &ring, &p);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init failed %d\n", ret);
		return 1;
	}

	/* submit 4x4 SQEs, should overflow the ring by 8 */
	pending = 0;
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			sqe = io_uring_get_sqe(&ring);
			if (!sqe) {
				fprintf(stderr, "get sqe failed\n");
				goto err;
			}

			io_uring_prep_nop(sqe);
			sqe->user_data = (i * 4) + j;
		}

		ret = io_uring_submit(&ring);
		if (ret == 4) {
			pending += 4;
			continue;
		}
		if (p.features & IORING_FEAT_NODROP) {
			if (ret == -EBUSY)
				break;
		}
		fprintf(stderr, "sqe submit failed: %d\n", ret);
		goto err;
	}

	/* we should now have 8 completions ready */
	ret = reap_events(&ring, pending, 0);
	if (ret < 0)
		goto err;

	if (!(p.features & IORING_FEAT_NODROP)) {
		if (*ring.cq.koverflow != 8) {
			fprintf(stderr, "cq ring overflow %d, expected 8\n",
					*ring.cq.koverflow);
			goto err;
		}
	}
	io_uring_queue_exit(&ring);
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;
}


static void submit_one_nop(struct io_uring *ring, int ud)
{
	struct io_uring_sqe *sqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	assert(sqe);
	io_uring_prep_nop(sqe);
	sqe->user_data = ud;
	ret = io_uring_submit(ring);
	assert(ret == 1);
}

/*
 * Create an overflow condition and ensure that SQEs are still processed
 */
static int test_overflow_handling(bool batch, int cqe_multiple, bool poll,
				  bool defer)
{
	struct io_uring ring;
	struct io_uring_params p;
	int ret, i, j, ud, cqe_count;
	unsigned int count;
	int const N = 8;
	int const LOOPS = 128;
	int const QUEUE_LENGTH = 1024;
	int completions[N];
	int queue[QUEUE_LENGTH];
	int queued = 0;
	int outstanding = 0;
	bool cqe_dropped = false;

	memset(&completions, 0, sizeof(int) * N);
	memset(&p, 0, sizeof(p));
	p.cq_entries = 2 * cqe_multiple;
	p.flags |= IORING_SETUP_CQSIZE;

	if (poll)
		p.flags |= IORING_SETUP_IOPOLL;

	if (defer)
		p.flags |= IORING_SETUP_SINGLE_ISSUER |
			   IORING_SETUP_DEFER_TASKRUN;

	ret = io_uring_queue_init_params(2, &ring, &p);
	if (ret) {
		fprintf(stderr, "io_uring_queue_init failed %d\n", ret);
		return 1;
	}

	assert(p.cq_entries < N);
	/* submit N SQEs, some should overflow */
	for (i = 0; i < N; i++) {
		submit_one_nop(&ring, i);
		outstanding++;
	}

	for (i = 0; i < LOOPS; i++) {
		struct io_uring_cqe *cqes[N];

		if (io_uring_cq_has_overflow(&ring)) {
			/*
			 * Flush any overflowed CQEs and process those. Actively
			 * flush these to make sure CQEs arrive in vague order
			 * of being sent.
			 */
			ret = io_uring_get_events(&ring);
			if (ret != 0) {
				fprintf(stderr,
					"io_uring_get_events returned %d\n",
					ret);
				goto err;
			}
		} else if (!cqe_dropped) {
			for (j = 0; j < queued; j++) {
				submit_one_nop(&ring, queue[j]);
				outstanding++;
			}
			queued = 0;
		}

		/* We have lost some random cqes, stop if no remaining. */
		if (cqe_dropped && outstanding == *ring.cq.koverflow)
			break;

		ret = io_uring_wait_cqe(&ring, &cqes[0]);
		if (ret == -EBADR) {
			cqe_dropped = true;
			fprintf(stderr, "CQE dropped\n");
			continue;
		} else if (ret != 0) {
			fprintf(stderr, "io_uring_wait_cqes failed %d\n", ret);
			goto err;
		}
		cqe_count = 1;
		if (batch) {
			ret = io_uring_peek_batch_cqe(&ring, &cqes[0], 2);
			if (ret < 0) {
				fprintf(stderr,
					"io_uring_peek_batch_cqe failed %d\n",
					ret);
				goto err;
			}
			cqe_count = ret;
		}
		for (j = 0; j < cqe_count; j++) {
			assert(cqes[j]->user_data < N);
			ud = cqes[j]->user_data;
			completions[ud]++;
			assert(queued < QUEUE_LENGTH);
			queue[queued++] = (int)ud;
		}
		io_uring_cq_advance(&ring, cqe_count);
		outstanding -= cqe_count;
	}

	/* See if there were any drops by flushing the CQ ring *and* overflow */
	do {
		struct io_uring_cqe *cqe;

		ret = io_uring_get_events(&ring);
		if (ret < 0) {
			if (ret == -EBADR) {
				fprintf(stderr, "CQE dropped\n");
				cqe_dropped = true;
				break;
			}
			goto err;
		}
		if (outstanding && !io_uring_cq_ready(&ring))
			ret = io_uring_wait_cqe_timeout(&ring, &cqe, NULL);

		if (ret && ret != -ETIME) {
			if (ret == -EBADR) {
				fprintf(stderr, "CQE dropped\n");
				cqe_dropped = true;
				break;
			}
			fprintf(stderr, "wait_cqe_timeout = %d\n", ret);
			goto err;
		}
		count = io_uring_cq_ready(&ring);
		io_uring_cq_advance(&ring, count);
		outstanding -= count;
	} while (count);

	io_uring_queue_exit(&ring);

	/* Make sure that completions come back in the same order they were
	 * sent. If they come back unfairly then this will concentrate on a
	 * couple of indices.
	 */
	for (i = 1; !cqe_dropped && i < N; i++) {
		if (abs(completions[i] - completions[i - 1]) > 1) {
			fprintf(stderr, "bad completion size %d %d\n",
				completions[i], completions[i - 1]);
			goto err;
		}
	}
	return 0;
err:
	io_uring_queue_exit(&ring);
	return 1;
}

int main(int argc, char *argv[])
{
	const char *fname = ".cq-overflow";
	unsigned iters, drops;
	unsigned long usecs;
	int ret;
	int i;
	bool can_defer;

	if (argc > 1)
		return T_EXIT_SKIP;

	can_defer = t_probe_defer_taskrun();
	for (i = 0; i < 16; i++) {
		bool batch = i & 1;
		int mult = (i & 2) ? 1 : 2;
		bool poll = i & 4;
		bool defer = i & 8;

		if (defer && !can_defer)
			continue;

		ret = test_overflow_handling(batch, mult, poll, defer);
		if (ret) {
			fprintf(stderr, "test_overflow_handling("
				"batch=%d, mult=%d, poll=%d, defer=%d) failed\n",
				batch, mult, poll, defer);
			goto err;
		}
	}

	ret = test_overflow();
	if (ret) {
		fprintf(stderr, "test_overflow failed\n");
		return ret;
	}

	t_create_file(fname, FILE_SIZE);

	vecs = t_create_buffers(BUFFERS, BS);

	iters = 0;
	usecs = 1000;
	do {
		drops = 0;

		ret = test_io(fname, usecs, &drops, 0);
		if (ret == T_EXIT_SKIP)
			break;
		else if (ret != T_EXIT_PASS) {
			fprintf(stderr, "test_io nofault failed\n");
			goto err;
		}
		if (drops)
			break;
		usecs = (usecs * 12) / 10;
		iters++;
	} while (iters < 40);

	if (test_io(fname, usecs, &drops, 0) == T_EXIT_FAIL) {
		fprintf(stderr, "test_io nofault failed\n");
		goto err;
	}

	if (test_io(fname, usecs, &drops, 1) == T_EXIT_FAIL) {
		fprintf(stderr, "test_io fault failed\n");
		goto err;
	}

	unlink(fname);
	return T_EXIT_PASS;
err:
	unlink(fname);
	return T_EXIT_FAIL;
}
