/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200112L

#include "lib.h"
#include "syscall.h"
#include "liburing.h"
#include "int_flags.h"
#include "liburing/compat.h"
#include "liburing/io_uring.h"

/*
 * Returns true if we're not using SQ thread (thus nobody submits but us)
 * or if IORING_SQ_NEED_WAKEUP is set, so submit thread must be explicitly
 * awakened. For the latter case, we set the thread wakeup flag.
 * If no SQEs are ready for submission, returns false.
 */
static inline bool sq_ring_needs_enter(struct io_uring *ring,
				       unsigned submit,
				       unsigned *flags)
{
	if (!submit)
		return false;

	if (!(ring->flags & IORING_SETUP_SQPOLL))
		return true;

	/*
	 * Ensure the kernel can see the store to the SQ tail before we read
	 * the flags.
	 */
	io_uring_smp_mb();

	if (uring_unlikely(IO_URING_READ_ONCE(*ring->sq.kflags) &
			   IORING_SQ_NEED_WAKEUP)) {
		*flags |= IORING_ENTER_SQ_WAKEUP;
		return true;
	}

	return false;
}

static inline bool cq_ring_needs_flush(struct io_uring *ring)
{
	return IO_URING_READ_ONCE(*ring->sq.kflags) &
				 (IORING_SQ_CQ_OVERFLOW | IORING_SQ_TASKRUN);
}

static inline bool cq_ring_needs_enter(struct io_uring *ring)
{
	return (ring->flags & IORING_SETUP_IOPOLL) || cq_ring_needs_flush(ring);
}

struct get_data {
	unsigned submit;
	unsigned wait_nr;
	unsigned get_flags;
	int sz;
	int has_ts;
	void *arg;
};

static int _io_uring_get_cqe(struct io_uring *ring,
			     struct io_uring_cqe **cqe_ptr,
			     struct get_data *data)
{
	struct io_uring_cqe *cqe = NULL;
	bool looped = false;
	int err = 0;

	do {
		bool need_enter = false;
		unsigned flags = 0;
		unsigned nr_available;
		int ret;

		ret = __io_uring_peek_cqe(ring, &cqe, &nr_available);
		if (ret) {
			if (!err)
				err = ret;
			break;
		}
		if (!cqe && !data->wait_nr && !data->submit) {
			/*
			 * If we already looped once, we already entered
			 * the kernel. Since there's nothing to submit or
			 * wait for, don't keep retrying.
			 */
			if (looped || !cq_ring_needs_enter(ring)) {
				if (!err)
					err = -EAGAIN;
				break;
			}
			need_enter = true;
		}
		if (data->wait_nr > nr_available || need_enter) {
			flags = IORING_ENTER_GETEVENTS | data->get_flags;
			need_enter = true;
		}
		if (sq_ring_needs_enter(ring, data->submit, &flags))
			need_enter = true;
		if (!need_enter)
			break;
		if (looped && data->has_ts) {
			struct io_uring_getevents_arg *arg = data->arg;

			if (!cqe && arg->ts && !err)
				err = -ETIME;
			break;
		}

		if (ring->int_flags & INT_FLAG_REG_RING)
			flags |= IORING_ENTER_REGISTERED_RING;
		ret = __sys_io_uring_enter2(ring->enter_ring_fd, data->submit,
					    data->wait_nr, flags, data->arg,
					    data->sz);
		if (ret < 0) {
			if (!err)
				err = ret;
			break;
		}

		data->submit -= ret;
		if (cqe)
			break;
		if (!looped) {
			looped = true;
			err = ret;
		}
	} while (1);

	*cqe_ptr = cqe;
	return err;
}

int __io_uring_get_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
		       unsigned submit, unsigned wait_nr, sigset_t *sigmask)
{
	struct get_data data = {
		.submit		= submit,
		.wait_nr 	= wait_nr,
		.get_flags	= 0,
		.sz		= _NSIG / 8,
		.arg		= sigmask,
	};

	return _io_uring_get_cqe(ring, cqe_ptr, &data);
}

int io_uring_get_events(struct io_uring *ring)
{
	int flags = IORING_ENTER_GETEVENTS;

	if (ring->int_flags & INT_FLAG_REG_RING)
		flags |= IORING_ENTER_REGISTERED_RING;
	return __sys_io_uring_enter(ring->enter_ring_fd, 0, 0, flags, NULL);
}

/*
 * Fill in an array of IO completions up to count, if any are available.
 * Returns the amount of IO completions filled.
 */
unsigned io_uring_peek_batch_cqe(struct io_uring *ring,
				 struct io_uring_cqe **cqes, unsigned count)
{
	unsigned ready;
	bool overflow_checked = false;
	int shift = 0;

	if (ring->flags & IORING_SETUP_CQE32)
		shift = 1;

again:
	ready = io_uring_cq_ready(ring);
	if (ready) {
		unsigned head = *ring->cq.khead;
		unsigned mask = ring->cq.ring_mask;
		unsigned last;
		int i = 0;

		count = count > ready ? ready : count;
		last = head + count;
		for (;head != last; head++, i++)
			cqes[i] = &ring->cq.cqes[(head & mask) << shift];

		return count;
	}

	if (overflow_checked)
		return 0;

	if (cq_ring_needs_flush(ring)) {
		io_uring_get_events(ring);
		overflow_checked = true;
		goto again;
	}

	return 0;
}

/*
 * Sync internal state with kernel ring state on the SQ side. Returns the
 * number of pending items in the SQ ring, for the shared ring.
 */
static unsigned __io_uring_flush_sq(struct io_uring *ring)
{
	struct io_uring_sq *sq = &ring->sq;
	unsigned tail = sq->sqe_tail;

	if (sq->sqe_head != tail) {
		sq->sqe_head = tail;
		/*
		 * Ensure kernel sees the SQE updates before the tail update.
		 */
		if (!(ring->flags & IORING_SETUP_SQPOLL))
			IO_URING_WRITE_ONCE(*sq->ktail, tail);
		else
			io_uring_smp_store_release(sq->ktail, tail);
	}
	/*
	 * This _may_ look problematic, as we're not supposed to be reading
	 * SQ->head without acquire semantics. When we're in SQPOLL mode, the
	 * kernel submitter could be updating this right now. For non-SQPOLL,
	 * task itself does it, and there's no potential race. But even for
	 * SQPOLL, the load is going to be potentially out-of-date the very
	 * instant it's done, regardless or whether or not it's done
	 * atomically. Worst case, we're going to be over-estimating what
	 * we can submit. The point is, we need to be able to deal with this
	 * situation regardless of any perceived atomicity.
	 */
	return tail - *sq->khead;
}

/*
 * If we have kernel support for IORING_ENTER_EXT_ARG, then we can use that
 * more efficiently than queueing an internal timeout command.
 */
static int io_uring_wait_cqes_new(struct io_uring *ring,
				  struct io_uring_cqe **cqe_ptr,
				  unsigned wait_nr,
				  struct __kernel_timespec *ts,
				  sigset_t *sigmask)
{
	struct io_uring_getevents_arg arg = {
		.sigmask	= (unsigned long) sigmask,
		.sigmask_sz	= _NSIG / 8,
		.ts		= (unsigned long) ts
	};
	struct get_data data = {
		.wait_nr	= wait_nr,
		.get_flags	= IORING_ENTER_EXT_ARG,
		.sz		= sizeof(arg),
		.has_ts		= ts != NULL,
		.arg		= &arg
	};

	return _io_uring_get_cqe(ring, cqe_ptr, &data);
}

/*
 * Like io_uring_wait_cqe(), except it accepts a timeout value as well. Note
 * that an sqe is used internally to handle the timeout. For kernel doesn't
 * support IORING_FEAT_EXT_ARG, applications using this function must never
 * set sqe->user_data to LIBURING_UDATA_TIMEOUT!
 *
 * For kernels without IORING_FEAT_EXT_ARG (5.10 and older), if 'ts' is
 * specified, the application need not call io_uring_submit() before
 * calling this function, as we will do that on its behalf. From this it also
 * follows that this function isn't safe to use for applications that split SQ
 * and CQ handling between two threads and expect that to work without
 * synchronization, as this function manipulates both the SQ and CQ side.
 *
 * For kernels with IORING_FEAT_EXT_ARG, no implicit submission is done and
 * hence this function is safe to use for applications that split SQ and CQ
 * handling between two threads.
 */
static int __io_uring_submit_timeout(struct io_uring *ring, unsigned wait_nr,
				     struct __kernel_timespec *ts)
{
	struct io_uring_sqe *sqe;
	int ret;

	/*
	 * If the SQ ring is full, we may need to submit IO first
	 */
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		ret = io_uring_submit(ring);
		if (ret < 0)
			return ret;
		sqe = io_uring_get_sqe(ring);
		if (!sqe)
			return -EAGAIN;
	}
	io_uring_prep_timeout(sqe, ts, wait_nr, 0);
	sqe->user_data = LIBURING_UDATA_TIMEOUT;
	return __io_uring_flush_sq(ring);
}

int io_uring_wait_cqes(struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
		       unsigned wait_nr, struct __kernel_timespec *ts,
		       sigset_t *sigmask)
{
	int to_submit = 0;

	if (ts) {
		if (ring->features & IORING_FEAT_EXT_ARG)
			return io_uring_wait_cqes_new(ring, cqe_ptr, wait_nr,
							ts, sigmask);
		to_submit = __io_uring_submit_timeout(ring, wait_nr, ts);
		if (to_submit < 0)
			return to_submit;
	}

	return __io_uring_get_cqe(ring, cqe_ptr, to_submit, wait_nr, sigmask);
}

int io_uring_submit_and_wait_timeout(struct io_uring *ring,
				     struct io_uring_cqe **cqe_ptr,
				     unsigned wait_nr,
				     struct __kernel_timespec *ts,
				     sigset_t *sigmask)
{
	int to_submit;

	if (ts) {
		if (ring->features & IORING_FEAT_EXT_ARG) {
			struct io_uring_getevents_arg arg = {
				.sigmask	= (unsigned long) sigmask,
				.sigmask_sz	= _NSIG / 8,
				.ts		= (unsigned long) ts
			};
			struct get_data data = {
				.submit		= __io_uring_flush_sq(ring),
				.wait_nr	= wait_nr,
				.get_flags	= IORING_ENTER_EXT_ARG,
				.sz		= sizeof(arg),
				.has_ts		= ts != NULL,
				.arg		= &arg
			};

			return _io_uring_get_cqe(ring, cqe_ptr, &data);
		}
		to_submit = __io_uring_submit_timeout(ring, wait_nr, ts);
		if (to_submit < 0)
			return to_submit;
	} else
		to_submit = __io_uring_flush_sq(ring);

	return __io_uring_get_cqe(ring, cqe_ptr, to_submit, wait_nr, sigmask);
}

/*
 * See io_uring_wait_cqes() - this function is the same, it just always uses
 * '1' as the wait_nr.
 */
int io_uring_wait_cqe_timeout(struct io_uring *ring,
			      struct io_uring_cqe **cqe_ptr,
			      struct __kernel_timespec *ts)
{
	return io_uring_wait_cqes(ring, cqe_ptr, 1, ts, NULL);
}

/*
 * Submit sqes acquired from io_uring_get_sqe() to the kernel.
 *
 * Returns number of sqes submitted
 */
static int __io_uring_submit(struct io_uring *ring, unsigned submitted,
			     unsigned wait_nr, bool getevents)
{
	bool cq_needs_enter = getevents || wait_nr || cq_ring_needs_enter(ring);
	unsigned flags;
	int ret;

	flags = 0;
	if (sq_ring_needs_enter(ring, submitted, &flags) || cq_needs_enter) {
		if (cq_needs_enter)
			flags |= IORING_ENTER_GETEVENTS;
		if (ring->int_flags & INT_FLAG_REG_RING)
			flags |= IORING_ENTER_REGISTERED_RING;

		ret = __sys_io_uring_enter(ring->enter_ring_fd, submitted,
					   wait_nr, flags, NULL);
	} else
		ret = submitted;

	return ret;
}

static int __io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr)
{
	return __io_uring_submit(ring, __io_uring_flush_sq(ring), wait_nr, false);
}

/*
 * Submit sqes acquired from io_uring_get_sqe() to the kernel.
 *
 * Returns number of sqes submitted
 */
int io_uring_submit(struct io_uring *ring)
{
	return __io_uring_submit_and_wait(ring, 0);
}

/*
 * Like io_uring_submit(), but allows waiting for events as well.
 *
 * Returns number of sqes submitted
 */
int io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr)
{
	return __io_uring_submit_and_wait(ring, wait_nr);
}

int io_uring_submit_and_get_events(struct io_uring *ring)
{
	return __io_uring_submit(ring, __io_uring_flush_sq(ring), 0, true);
}

#ifdef LIBURING_INTERNAL
struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring)
{
	return _io_uring_get_sqe(ring);
}
#endif

int __io_uring_sqring_wait(struct io_uring *ring)
{
	int flags = IORING_ENTER_SQ_WAIT;

	if (ring->int_flags & INT_FLAG_REG_RING)
		flags |= IORING_ENTER_REGISTERED_RING;

	return __sys_io_uring_enter(ring->enter_ring_fd, 0, 0, flags, NULL);
}
