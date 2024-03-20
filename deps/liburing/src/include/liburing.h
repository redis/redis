/* SPDX-License-Identifier: MIT */
#ifndef LIB_URING_H
#define LIB_URING_H

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500 /* Required for glibc to expose sigset_t */
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* Required for musl to expose cpu_set_t */
#endif

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>
#include <fcntl.h>
#include <sched.h>
#include <linux/swab.h>
#include "liburing/compat.h"
#include "liburing/io_uring.h"
#include "liburing/io_uring_version.h"
#include "liburing/barrier.h"

#ifndef uring_unlikely
#define uring_unlikely(cond)	__builtin_expect(!!(cond), 0)
#endif

#ifndef uring_likely
#define uring_likely(cond)	__builtin_expect(!!(cond), 1)
#endif

#ifndef IOURINGINLINE
#define IOURINGINLINE static inline
#endif

#ifdef __alpha__
/*
 * alpha and mips are the exceptions, all other architectures have
 * common numbers for new system calls.
 */
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup		535
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter		536
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register		537
#endif
#elif defined __mips__
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup		(__NR_Linux + 425)
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter		(__NR_Linux + 426)
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register		(__NR_Linux + 427)
#endif
#else /* !__alpha__ and !__mips__ */
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup		425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter		426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register		427
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Library interface to io_uring
 */
struct io_uring_sq {
	unsigned *khead;
	unsigned *ktail;
	// Deprecated: use `ring_mask` instead of `*kring_mask`
	unsigned *kring_mask;
	// Deprecated: use `ring_entries` instead of `*kring_entries`
	unsigned *kring_entries;
	unsigned *kflags;
	unsigned *kdropped;
	unsigned *array;
	struct io_uring_sqe *sqes;

	unsigned sqe_head;
	unsigned sqe_tail;

	size_t ring_sz;
	void *ring_ptr;

	unsigned ring_mask;
	unsigned ring_entries;

	unsigned pad[2];
};

struct io_uring_cq {
	unsigned *khead;
	unsigned *ktail;
	// Deprecated: use `ring_mask` instead of `*kring_mask`
	unsigned *kring_mask;
	// Deprecated: use `ring_entries` instead of `*kring_entries`
	unsigned *kring_entries;
	unsigned *kflags;
	unsigned *koverflow;
	struct io_uring_cqe *cqes;

	size_t ring_sz;
	void *ring_ptr;

	unsigned ring_mask;
	unsigned ring_entries;

	unsigned pad[2];
};

struct io_uring {
	struct io_uring_sq sq;
	struct io_uring_cq cq;
	unsigned flags;
	int ring_fd;

	unsigned features;
	int enter_ring_fd;
	__u8 int_flags;
	__u8 pad[3];
	unsigned pad2;
};

/*
 * Library interface
 */

/*
 * return an allocated io_uring_probe structure, or NULL if probe fails (for
 * example, if it is not available). The caller is responsible for freeing it
 */
struct io_uring_probe *io_uring_get_probe_ring(struct io_uring *ring);
/* same as io_uring_get_probe_ring, but takes care of ring init and teardown */
struct io_uring_probe *io_uring_get_probe(void);

/*
 * frees a probe allocated through io_uring_get_probe() or
 * io_uring_get_probe_ring()
 */
void io_uring_free_probe(struct io_uring_probe *probe);

IOURINGINLINE int io_uring_opcode_supported(const struct io_uring_probe *p,
					    int op)
{
	if (op > p->last_op)
		return 0;
	return (p->ops[op].flags & IO_URING_OP_SUPPORTED) != 0;
}

int io_uring_queue_init_mem(unsigned entries, struct io_uring *ring,
				struct io_uring_params *p,
				void *buf, size_t buf_size);
int io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
				struct io_uring_params *p);
int io_uring_queue_init(unsigned entries, struct io_uring *ring,
			unsigned flags);
int io_uring_queue_mmap(int fd, struct io_uring_params *p,
			struct io_uring *ring);
int io_uring_ring_dontfork(struct io_uring *ring);
void io_uring_queue_exit(struct io_uring *ring);
unsigned io_uring_peek_batch_cqe(struct io_uring *ring,
	struct io_uring_cqe **cqes, unsigned count);
int io_uring_wait_cqes(struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
		       unsigned wait_nr, struct __kernel_timespec *ts,
		       sigset_t *sigmask);
int io_uring_wait_cqe_timeout(struct io_uring *ring,
			      struct io_uring_cqe **cqe_ptr,
			      struct __kernel_timespec *ts);
int io_uring_submit(struct io_uring *ring);
int io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr);
int io_uring_submit_and_wait_timeout(struct io_uring *ring,
				     struct io_uring_cqe **cqe_ptr,
				     unsigned wait_nr,
				     struct __kernel_timespec *ts,
				     sigset_t *sigmask);

int io_uring_register_buffers(struct io_uring *ring, const struct iovec *iovecs,
			      unsigned nr_iovecs);
int io_uring_register_buffers_tags(struct io_uring *ring,
				   const struct iovec *iovecs,
				   const __u64 *tags, unsigned nr);
int io_uring_register_buffers_sparse(struct io_uring *ring, unsigned nr);
int io_uring_register_buffers_update_tag(struct io_uring *ring,
					 unsigned off,
					 const struct iovec *iovecs,
					 const __u64 *tags, unsigned nr);
int io_uring_unregister_buffers(struct io_uring *ring);

int io_uring_register_files(struct io_uring *ring, const int *files,
			    unsigned nr_files);
int io_uring_register_files_tags(struct io_uring *ring, const int *files,
				 const __u64 *tags, unsigned nr);
int io_uring_register_files_sparse(struct io_uring *ring, unsigned nr);
int io_uring_register_files_update_tag(struct io_uring *ring, unsigned off,
				       const int *files, const __u64 *tags,
				       unsigned nr_files);

int io_uring_unregister_files(struct io_uring *ring);
int io_uring_register_files_update(struct io_uring *ring, unsigned off,
				   const int *files, unsigned nr_files);
int io_uring_register_eventfd(struct io_uring *ring, int fd);
int io_uring_register_eventfd_async(struct io_uring *ring, int fd);
int io_uring_unregister_eventfd(struct io_uring *ring);
int io_uring_register_probe(struct io_uring *ring, struct io_uring_probe *p,
			    unsigned nr);
int io_uring_register_personality(struct io_uring *ring);
int io_uring_unregister_personality(struct io_uring *ring, int id);
int io_uring_register_restrictions(struct io_uring *ring,
				   struct io_uring_restriction *res,
				   unsigned int nr_res);
int io_uring_enable_rings(struct io_uring *ring);
int __io_uring_sqring_wait(struct io_uring *ring);
int io_uring_register_iowq_aff(struct io_uring *ring, size_t cpusz,
				const cpu_set_t *mask);
int io_uring_unregister_iowq_aff(struct io_uring *ring);
int io_uring_register_iowq_max_workers(struct io_uring *ring,
				       unsigned int *values);
int io_uring_register_ring_fd(struct io_uring *ring);
int io_uring_unregister_ring_fd(struct io_uring *ring);
int io_uring_close_ring_fd(struct io_uring *ring);
int io_uring_register_buf_ring(struct io_uring *ring,
			       struct io_uring_buf_reg *reg, unsigned int flags);
int io_uring_unregister_buf_ring(struct io_uring *ring, int bgid);
int io_uring_register_sync_cancel(struct io_uring *ring,
				 struct io_uring_sync_cancel_reg *reg);

int io_uring_register_file_alloc_range(struct io_uring *ring,
					unsigned off, unsigned len);

int io_uring_get_events(struct io_uring *ring);
int io_uring_submit_and_get_events(struct io_uring *ring);

/*
 * io_uring syscalls.
 */
int io_uring_enter(unsigned int fd, unsigned int to_submit,
		   unsigned int min_complete, unsigned int flags, sigset_t *sig);
int io_uring_enter2(unsigned int fd, unsigned int to_submit,
		    unsigned int min_complete, unsigned int flags,
		    sigset_t *sig, size_t sz);
int io_uring_setup(unsigned int entries, struct io_uring_params *p);
int io_uring_register(unsigned int fd, unsigned int opcode, const void *arg,
		      unsigned int nr_args);

/*
 * Mapped buffer ring alloc/register + unregister/free helpers
 */
struct io_uring_buf_ring *io_uring_setup_buf_ring(struct io_uring *ring,
						  unsigned int nentries,
						  int bgid, unsigned int flags,
						  int *ret);
int io_uring_free_buf_ring(struct io_uring *ring, struct io_uring_buf_ring *br,
			   unsigned int nentries, int bgid);

/*
 * Helper for the peek/wait single cqe functions. Exported because of that,
 * but probably shouldn't be used directly in an application.
 */
int __io_uring_get_cqe(struct io_uring *ring,
			struct io_uring_cqe **cqe_ptr, unsigned submit,
			unsigned wait_nr, sigset_t *sigmask);

#define LIBURING_UDATA_TIMEOUT	((__u64) -1)

/*
 * Calculates the step size for CQE iteration.
 * 	For standard CQE's its 1, for big CQE's its two.
 */
#define io_uring_cqe_shift(ring)					\
	(!!((ring)->flags & IORING_SETUP_CQE32))

#define io_uring_cqe_index(ring,ptr,mask)				\
	(((ptr) & (mask)) << io_uring_cqe_shift(ring))

#define io_uring_for_each_cqe(ring, head, cqe)				\
	/*								\
	 * io_uring_smp_load_acquire() enforces the order of tail	\
	 * and CQE reads.						\
	 */								\
	for (head = *(ring)->cq.khead;					\
	     (cqe = (head != io_uring_smp_load_acquire((ring)->cq.ktail) ? \
		&(ring)->cq.cqes[io_uring_cqe_index(ring, head, (ring)->cq.ring_mask)] : NULL)); \
	     head++)							\

/*
 * Must be called after io_uring_for_each_cqe()
 */
IOURINGINLINE void io_uring_cq_advance(struct io_uring *ring, unsigned nr)
{
	if (nr) {
		struct io_uring_cq *cq = &ring->cq;

		/*
		 * Ensure that the kernel only sees the new value of the head
		 * index after the CQEs have been read.
		 */
		io_uring_smp_store_release(cq->khead, *cq->khead + nr);
	}
}

/*
 * Must be called after io_uring_{peek,wait}_cqe() after the cqe has
 * been processed by the application.
 */
IOURINGINLINE void io_uring_cqe_seen(struct io_uring *ring,
				     struct io_uring_cqe *cqe)
{
	if (cqe)
		io_uring_cq_advance(ring, 1);
}

/*
 * Command prep helpers
 */

/*
 * Associate pointer @data with the sqe, for later retrieval from the cqe
 * at command completion time with io_uring_cqe_get_data().
 */
IOURINGINLINE void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data)
{
	sqe->user_data = (unsigned long) data;
}

IOURINGINLINE void *io_uring_cqe_get_data(const struct io_uring_cqe *cqe)
{
	return (void *) (uintptr_t) cqe->user_data;
}

/*
 * Assign a 64-bit value to this sqe, which can get retrieved at completion
 * time with io_uring_cqe_get_data64. Just like the non-64 variants, except
 * these store a 64-bit type rather than a data pointer.
 */
IOURINGINLINE void io_uring_sqe_set_data64(struct io_uring_sqe *sqe,
					   __u64 data)
{
	sqe->user_data = data;
}

IOURINGINLINE __u64 io_uring_cqe_get_data64(const struct io_uring_cqe *cqe)
{
	return cqe->user_data;
}

/*
 * Tell the app the have the 64-bit variants of the get/set userdata
 */
#define LIBURING_HAVE_DATA64

IOURINGINLINE void io_uring_sqe_set_flags(struct io_uring_sqe *sqe,
					  unsigned flags)
{
	sqe->flags = (__u8) flags;
}

IOURINGINLINE void __io_uring_set_target_fixed_file(struct io_uring_sqe *sqe,
						    unsigned int file_index)
{
	/* 0 means no fixed files, indexes should be encoded as "index + 1" */
	sqe->file_index = file_index + 1;
}

IOURINGINLINE void io_uring_prep_rw(int op, struct io_uring_sqe *sqe, int fd,
				    const void *addr, unsigned len,
				    __u64 offset)
{
	sqe->opcode = (__u8) op;
	sqe->flags = 0;
	sqe->ioprio = 0;
	sqe->fd = fd;
	sqe->off = offset;
	sqe->addr = (unsigned long) addr;
	sqe->len = len;
	sqe->rw_flags = 0;
	sqe->buf_index = 0;
	sqe->personality = 0;
	sqe->file_index = 0;
	sqe->addr3 = 0;
	sqe->__pad2[0] = 0;
}

/*
 * io_uring_prep_splice() - Either @fd_in or @fd_out must be a pipe.
 *
 * - If @fd_in refers to a pipe, @off_in is ignored and must be set to -1.
 *
 * - If @fd_in does not refer to a pipe and @off_in is -1, then @nbytes are read
 *   from @fd_in starting from the file offset, which is incremented by the
 *   number of bytes read.
 *
 * - If @fd_in does not refer to a pipe and @off_in is not -1, then the starting
 *   offset of @fd_in will be @off_in.
 *
 * This splice operation can be used to implement sendfile by splicing to an
 * intermediate pipe first, then splice to the final destination.
 * In fact, the implementation of sendfile in kernel uses splice internally.
 *
 * NOTE that even if fd_in or fd_out refers to a pipe, the splice operation
 * can still fail with EINVAL if one of the fd doesn't explicitly support splice
 * operation, e.g. reading from terminal is unsupported from kernel 5.7 to 5.11.
 * Check issue #291 for more information.
 */
IOURINGINLINE void io_uring_prep_splice(struct io_uring_sqe *sqe,
					int fd_in, int64_t off_in,
					int fd_out, int64_t off_out,
					unsigned int nbytes,
					unsigned int splice_flags)
{
	io_uring_prep_rw(IORING_OP_SPLICE, sqe, fd_out, NULL, nbytes,
				(__u64) off_out);
	sqe->splice_off_in = (__u64) off_in;
	sqe->splice_fd_in = fd_in;
	sqe->splice_flags = splice_flags;
}

IOURINGINLINE void io_uring_prep_tee(struct io_uring_sqe *sqe,
				     int fd_in, int fd_out,
				     unsigned int nbytes,
				     unsigned int splice_flags)
{
	io_uring_prep_rw(IORING_OP_TEE, sqe, fd_out, NULL, nbytes, 0);
	sqe->splice_off_in = 0;
	sqe->splice_fd_in = fd_in;
	sqe->splice_flags = splice_flags;
}

IOURINGINLINE void io_uring_prep_readv(struct io_uring_sqe *sqe, int fd,
				       const struct iovec *iovecs,
				       unsigned nr_vecs, __u64 offset)
{
	io_uring_prep_rw(IORING_OP_READV, sqe, fd, iovecs, nr_vecs, offset);
}

IOURINGINLINE void io_uring_prep_readv2(struct io_uring_sqe *sqe, int fd,
				       const struct iovec *iovecs,
				       unsigned nr_vecs, __u64 offset,
				       int flags)
{
	io_uring_prep_readv(sqe, fd, iovecs, nr_vecs, offset);
	sqe->rw_flags = flags;
}

IOURINGINLINE void io_uring_prep_read_fixed(struct io_uring_sqe *sqe, int fd,
					    void *buf, unsigned nbytes,
					    __u64 offset, int buf_index)
{
	io_uring_prep_rw(IORING_OP_READ_FIXED, sqe, fd, buf, nbytes, offset);
	sqe->buf_index = (__u16) buf_index;
}

IOURINGINLINE void io_uring_prep_writev(struct io_uring_sqe *sqe, int fd,
					const struct iovec *iovecs,
					unsigned nr_vecs, __u64 offset)
{
	io_uring_prep_rw(IORING_OP_WRITEV, sqe, fd, iovecs, nr_vecs, offset);
}

IOURINGINLINE void io_uring_prep_writev2(struct io_uring_sqe *sqe, int fd,
				       const struct iovec *iovecs,
				       unsigned nr_vecs, __u64 offset,
				       int flags)
{
	io_uring_prep_writev(sqe, fd, iovecs, nr_vecs, offset);
	sqe->rw_flags = flags;
}

IOURINGINLINE void io_uring_prep_write_fixed(struct io_uring_sqe *sqe, int fd,
					     const void *buf, unsigned nbytes,
					     __u64 offset, int buf_index)
{
	io_uring_prep_rw(IORING_OP_WRITE_FIXED, sqe, fd, buf, nbytes, offset);
	sqe->buf_index = (__u16) buf_index;
}

IOURINGINLINE void io_uring_prep_recvmsg(struct io_uring_sqe *sqe, int fd,
					 struct msghdr *msg, unsigned flags)
{
	io_uring_prep_rw(IORING_OP_RECVMSG, sqe, fd, msg, 1, 0);
	sqe->msg_flags = flags;
}

IOURINGINLINE void io_uring_prep_recvmsg_multishot(struct io_uring_sqe *sqe,
						   int fd, struct msghdr *msg,
						   unsigned flags)
{
	io_uring_prep_recvmsg(sqe, fd, msg, flags);
	sqe->ioprio |= IORING_RECV_MULTISHOT;
}

IOURINGINLINE void io_uring_prep_sendmsg(struct io_uring_sqe *sqe, int fd,
					 const struct msghdr *msg,
					 unsigned flags)
{
	io_uring_prep_rw(IORING_OP_SENDMSG, sqe, fd, msg, 1, 0);
	sqe->msg_flags = flags;
}

IOURINGINLINE unsigned __io_uring_prep_poll_mask(unsigned poll_mask)
{
#if __BYTE_ORDER == __BIG_ENDIAN
	poll_mask = __swahw32(poll_mask);
#endif
	return poll_mask;
}

IOURINGINLINE void io_uring_prep_poll_add(struct io_uring_sqe *sqe, int fd,
					  unsigned poll_mask)
{
	io_uring_prep_rw(IORING_OP_POLL_ADD, sqe, fd, NULL, 0, 0);
	sqe->poll32_events = __io_uring_prep_poll_mask(poll_mask);
}

IOURINGINLINE void io_uring_prep_poll_multishot(struct io_uring_sqe *sqe,
						int fd, unsigned poll_mask)
{
	io_uring_prep_poll_add(sqe, fd, poll_mask);
	sqe->len = IORING_POLL_ADD_MULTI;
}

IOURINGINLINE void io_uring_prep_poll_remove(struct io_uring_sqe *sqe,
					     __u64 user_data)
{
	io_uring_prep_rw(IORING_OP_POLL_REMOVE, sqe, -1, NULL, 0, 0);
	sqe->addr = user_data;
}

IOURINGINLINE void io_uring_prep_poll_update(struct io_uring_sqe *sqe,
					     __u64 old_user_data,
					     __u64 new_user_data,
					     unsigned poll_mask, unsigned flags)
{
	io_uring_prep_rw(IORING_OP_POLL_REMOVE, sqe, -1, NULL, flags,
			 new_user_data);
	sqe->addr = old_user_data;
	sqe->poll32_events = __io_uring_prep_poll_mask(poll_mask);
}

IOURINGINLINE void io_uring_prep_fsync(struct io_uring_sqe *sqe, int fd,
				       unsigned fsync_flags)
{
	io_uring_prep_rw(IORING_OP_FSYNC, sqe, fd, NULL, 0, 0);
	sqe->fsync_flags = fsync_flags;
}

IOURINGINLINE void io_uring_prep_nop(struct io_uring_sqe *sqe)
{
	io_uring_prep_rw(IORING_OP_NOP, sqe, -1, NULL, 0, 0);
}

IOURINGINLINE void io_uring_prep_timeout(struct io_uring_sqe *sqe,
					 struct __kernel_timespec *ts,
					 unsigned count, unsigned flags)
{
	io_uring_prep_rw(IORING_OP_TIMEOUT, sqe, -1, ts, 1, count);
	sqe->timeout_flags = flags;
}

IOURINGINLINE void io_uring_prep_timeout_remove(struct io_uring_sqe *sqe,
						__u64 user_data, unsigned flags)
{
	io_uring_prep_rw(IORING_OP_TIMEOUT_REMOVE, sqe, -1, NULL, 0, 0);
	sqe->addr = user_data;
	sqe->timeout_flags = flags;
}

IOURINGINLINE void io_uring_prep_timeout_update(struct io_uring_sqe *sqe,
						struct __kernel_timespec *ts,
						__u64 user_data, unsigned flags)
{
	io_uring_prep_rw(IORING_OP_TIMEOUT_REMOVE, sqe, -1, NULL, 0,
				(uintptr_t) ts);
	sqe->addr = user_data;
	sqe->timeout_flags = flags | IORING_TIMEOUT_UPDATE;
}

IOURINGINLINE void io_uring_prep_accept(struct io_uring_sqe *sqe, int fd,
					struct sockaddr *addr,
					socklen_t *addrlen, int flags)
{
	io_uring_prep_rw(IORING_OP_ACCEPT, sqe, fd, addr, 0,
				(__u64) (unsigned long) addrlen);
	sqe->accept_flags = (__u32) flags;
}

/* accept directly into the fixed file table */
IOURINGINLINE void io_uring_prep_accept_direct(struct io_uring_sqe *sqe, int fd,
					       struct sockaddr *addr,
					       socklen_t *addrlen, int flags,
					       unsigned int file_index)
{
	io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
	/* offset by 1 for allocation */
	if (file_index == IORING_FILE_INDEX_ALLOC)
		file_index--;
	__io_uring_set_target_fixed_file(sqe, file_index);
}

IOURINGINLINE void io_uring_prep_multishot_accept(struct io_uring_sqe *sqe,
						  int fd, struct sockaddr *addr,
						  socklen_t *addrlen, int flags)
{
	io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
	sqe->ioprio |= IORING_ACCEPT_MULTISHOT;
}

/* multishot accept directly into the fixed file table */
IOURINGINLINE void io_uring_prep_multishot_accept_direct(struct io_uring_sqe *sqe,
							 int fd,
							 struct sockaddr *addr,
							 socklen_t *addrlen,
							 int flags)
{
	io_uring_prep_multishot_accept(sqe, fd, addr, addrlen, flags);
	__io_uring_set_target_fixed_file(sqe, IORING_FILE_INDEX_ALLOC - 1);
}

IOURINGINLINE void io_uring_prep_cancel64(struct io_uring_sqe *sqe,
					  __u64 user_data, int flags)
{
	io_uring_prep_rw(IORING_OP_ASYNC_CANCEL, sqe, -1, NULL, 0, 0);
	sqe->addr = user_data;
	sqe->cancel_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_cancel(struct io_uring_sqe *sqe,
					void *user_data, int flags)
{
	io_uring_prep_cancel64(sqe, (__u64) (uintptr_t) user_data, flags);
}

IOURINGINLINE void io_uring_prep_cancel_fd(struct io_uring_sqe *sqe, int fd,
					   unsigned int flags)
{
	io_uring_prep_rw(IORING_OP_ASYNC_CANCEL, sqe, fd, NULL, 0, 0);
	sqe->cancel_flags = (__u32) flags | IORING_ASYNC_CANCEL_FD;
}

IOURINGINLINE void io_uring_prep_link_timeout(struct io_uring_sqe *sqe,
					      struct __kernel_timespec *ts,
					      unsigned flags)
{
	io_uring_prep_rw(IORING_OP_LINK_TIMEOUT, sqe, -1, ts, 1, 0);
	sqe->timeout_flags = flags;
}

IOURINGINLINE void io_uring_prep_connect(struct io_uring_sqe *sqe, int fd,
					 const struct sockaddr *addr,
					 socklen_t addrlen)
{
	io_uring_prep_rw(IORING_OP_CONNECT, sqe, fd, addr, 0, addrlen);
}

IOURINGINLINE void io_uring_prep_files_update(struct io_uring_sqe *sqe,
					      int *fds, unsigned nr_fds,
					      int offset)
{
	io_uring_prep_rw(IORING_OP_FILES_UPDATE, sqe, -1, fds, nr_fds,
				(__u64) offset);
}

IOURINGINLINE void io_uring_prep_fallocate(struct io_uring_sqe *sqe, int fd,
					   int mode, __u64 offset, __u64 len)
{
	io_uring_prep_rw(IORING_OP_FALLOCATE, sqe, fd,
			0, (unsigned int) mode, (__u64) offset);
	sqe->addr = (__u64) len;
}

IOURINGINLINE void io_uring_prep_openat(struct io_uring_sqe *sqe, int dfd,
					const char *path, int flags,
					mode_t mode)
{
	io_uring_prep_rw(IORING_OP_OPENAT, sqe, dfd, path, mode, 0);
	sqe->open_flags = (__u32) flags;
}

/* open directly into the fixed file table */
IOURINGINLINE void io_uring_prep_openat_direct(struct io_uring_sqe *sqe,
					       int dfd, const char *path,
					       int flags, mode_t mode,
					       unsigned file_index)
{
	io_uring_prep_openat(sqe, dfd, path, flags, mode);
	/* offset by 1 for allocation */
	if (file_index == IORING_FILE_INDEX_ALLOC)
		file_index--;
	__io_uring_set_target_fixed_file(sqe, file_index);
}

IOURINGINLINE void io_uring_prep_close(struct io_uring_sqe *sqe, int fd)
{
	io_uring_prep_rw(IORING_OP_CLOSE, sqe, fd, NULL, 0, 0);
}

IOURINGINLINE void io_uring_prep_close_direct(struct io_uring_sqe *sqe,
					      unsigned file_index)
{
	io_uring_prep_close(sqe, 0);
	__io_uring_set_target_fixed_file(sqe, file_index);
}

IOURINGINLINE void io_uring_prep_read(struct io_uring_sqe *sqe, int fd,
				      void *buf, unsigned nbytes, __u64 offset)
{
	io_uring_prep_rw(IORING_OP_READ, sqe, fd, buf, nbytes, offset);
}

IOURINGINLINE void io_uring_prep_write(struct io_uring_sqe *sqe, int fd,
				       const void *buf, unsigned nbytes,
				       __u64 offset)
{
	io_uring_prep_rw(IORING_OP_WRITE, sqe, fd, buf, nbytes, offset);
}

struct statx;
IOURINGINLINE void io_uring_prep_statx(struct io_uring_sqe *sqe, int dfd,
				       const char *path, int flags,
				       unsigned mask, struct statx *statxbuf)
{
	io_uring_prep_rw(IORING_OP_STATX, sqe, dfd, path, mask,
				(__u64) (unsigned long) statxbuf);
	sqe->statx_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_fadvise(struct io_uring_sqe *sqe, int fd,
					 __u64 offset, off_t len, int advice)
{
	io_uring_prep_rw(IORING_OP_FADVISE, sqe, fd, NULL, (__u32) len, offset);
	sqe->fadvise_advice = (__u32) advice;
}

IOURINGINLINE void io_uring_prep_madvise(struct io_uring_sqe *sqe, void *addr,
					 off_t length, int advice)
{
	io_uring_prep_rw(IORING_OP_MADVISE, sqe, -1, addr, (__u32) length, 0);
	sqe->fadvise_advice = (__u32) advice;
}

IOURINGINLINE void io_uring_prep_send(struct io_uring_sqe *sqe, int sockfd,
				      const void *buf, size_t len, int flags)
{
	io_uring_prep_rw(IORING_OP_SEND, sqe, sockfd, buf, (__u32) len, 0);
	sqe->msg_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_send_set_addr(struct io_uring_sqe *sqe,
						const struct sockaddr *dest_addr,
						__u16 addr_len)
{
	sqe->addr2 = (unsigned long)(const void *)dest_addr;
	sqe->addr_len = addr_len;
}

IOURINGINLINE void io_uring_prep_sendto(struct io_uring_sqe *sqe, int sockfd,
					const void *buf, size_t len, int flags,
					const struct sockaddr *addr,
					socklen_t addrlen)
{
	io_uring_prep_send(sqe, sockfd, buf, len, flags);
	io_uring_prep_send_set_addr(sqe, addr, addrlen);
}

IOURINGINLINE void io_uring_prep_send_zc(struct io_uring_sqe *sqe, int sockfd,
					 const void *buf, size_t len, int flags,
					 unsigned zc_flags)
{
	io_uring_prep_rw(IORING_OP_SEND_ZC, sqe, sockfd, buf, (__u32) len, 0);
	sqe->msg_flags = (__u32) flags;
	sqe->ioprio = zc_flags;
}

IOURINGINLINE void io_uring_prep_send_zc_fixed(struct io_uring_sqe *sqe,
						int sockfd, const void *buf,
						size_t len, int flags,
						unsigned zc_flags,
						unsigned buf_index)
{
	io_uring_prep_send_zc(sqe, sockfd, buf, len, flags, zc_flags);
	sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
	sqe->buf_index = buf_index;
}

IOURINGINLINE void io_uring_prep_sendmsg_zc(struct io_uring_sqe *sqe, int fd,
					    const struct msghdr *msg,
					    unsigned flags)
{
	io_uring_prep_sendmsg(sqe, fd, msg, flags);
	sqe->opcode = IORING_OP_SENDMSG_ZC;
}

IOURINGINLINE void io_uring_prep_recv(struct io_uring_sqe *sqe, int sockfd,
				      void *buf, size_t len, int flags)
{
	io_uring_prep_rw(IORING_OP_RECV, sqe, sockfd, buf, (__u32) len, 0);
	sqe->msg_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_recv_multishot(struct io_uring_sqe *sqe,
						int sockfd, void *buf,
						size_t len, int flags)
{
	io_uring_prep_recv(sqe, sockfd, buf, len, flags);
	sqe->ioprio |= IORING_RECV_MULTISHOT;
}

IOURINGINLINE struct io_uring_recvmsg_out *
io_uring_recvmsg_validate(void *buf, int buf_len, struct msghdr *msgh)
{
	unsigned long header = msgh->msg_controllen + msgh->msg_namelen +
				sizeof(struct io_uring_recvmsg_out);
	if (buf_len < 0 || (unsigned long)buf_len < header)
		return NULL;
	return (struct io_uring_recvmsg_out *)buf;
}

IOURINGINLINE void *io_uring_recvmsg_name(struct io_uring_recvmsg_out *o)
{
	return (void *) &o[1];
}

IOURINGINLINE struct cmsghdr *
io_uring_recvmsg_cmsg_firsthdr(struct io_uring_recvmsg_out *o,
			       struct msghdr *msgh)
{
	if (o->controllen < sizeof(struct cmsghdr))
		return NULL;

	return (struct cmsghdr *)((unsigned char *) io_uring_recvmsg_name(o) +
			msgh->msg_namelen);
}

IOURINGINLINE struct cmsghdr *
io_uring_recvmsg_cmsg_nexthdr(struct io_uring_recvmsg_out *o, struct msghdr *msgh,
			      struct cmsghdr *cmsg)
{
	unsigned char *end;

	if (cmsg->cmsg_len < sizeof(struct cmsghdr))
		return NULL;
	end = (unsigned char *) io_uring_recvmsg_cmsg_firsthdr(o, msgh) +
		o->controllen;
	cmsg = (struct cmsghdr *)((unsigned char *) cmsg +
			CMSG_ALIGN(cmsg->cmsg_len));

	if ((unsigned char *) (cmsg + 1) > end)
		return NULL;
	if (((unsigned char *) cmsg) + CMSG_ALIGN(cmsg->cmsg_len) > end)
		return NULL;

	return cmsg;
}

IOURINGINLINE void *io_uring_recvmsg_payload(struct io_uring_recvmsg_out *o,
					     struct msghdr *msgh)
{
	return (void *)((unsigned char *)io_uring_recvmsg_name(o) +
			msgh->msg_namelen + msgh->msg_controllen);
}

IOURINGINLINE unsigned int
io_uring_recvmsg_payload_length(struct io_uring_recvmsg_out *o,
				int buf_len, struct msghdr *msgh)
{
	unsigned long payload_start, payload_end;

	payload_start = (unsigned long) io_uring_recvmsg_payload(o, msgh);
	payload_end = (unsigned long) o + buf_len;
	return (unsigned int) (payload_end - payload_start);
}

IOURINGINLINE void io_uring_prep_openat2(struct io_uring_sqe *sqe, int dfd,
					const char *path, struct open_how *how)
{
	io_uring_prep_rw(IORING_OP_OPENAT2, sqe, dfd, path, sizeof(*how),
				(uint64_t) (uintptr_t) how);
}

/* open directly into the fixed file table */
IOURINGINLINE void io_uring_prep_openat2_direct(struct io_uring_sqe *sqe,
						int dfd, const char *path,
						struct open_how *how,
						unsigned file_index)
{
	io_uring_prep_openat2(sqe, dfd, path, how);
	/* offset by 1 for allocation */
	if (file_index == IORING_FILE_INDEX_ALLOC)
		file_index--;
	__io_uring_set_target_fixed_file(sqe, file_index);
}

struct epoll_event;
IOURINGINLINE void io_uring_prep_epoll_ctl(struct io_uring_sqe *sqe, int epfd,
					   int fd, int op,
					   struct epoll_event *ev)
{
	io_uring_prep_rw(IORING_OP_EPOLL_CTL, sqe, epfd, ev,
				(__u32) op, (__u32) fd);
}

IOURINGINLINE void io_uring_prep_provide_buffers(struct io_uring_sqe *sqe,
						 void *addr, int len, int nr,
						 int bgid, int bid)
{
	io_uring_prep_rw(IORING_OP_PROVIDE_BUFFERS, sqe, nr, addr, (__u32) len,
				(__u64) bid);
	sqe->buf_group = (__u16) bgid;
}

IOURINGINLINE void io_uring_prep_remove_buffers(struct io_uring_sqe *sqe,
						int nr, int bgid)
{
	io_uring_prep_rw(IORING_OP_REMOVE_BUFFERS, sqe, nr, NULL, 0, 0);
	sqe->buf_group = (__u16) bgid;
}

IOURINGINLINE void io_uring_prep_shutdown(struct io_uring_sqe *sqe, int fd,
					  int how)
{
	io_uring_prep_rw(IORING_OP_SHUTDOWN, sqe, fd, NULL, (__u32) how, 0);
}

IOURINGINLINE void io_uring_prep_unlinkat(struct io_uring_sqe *sqe, int dfd,
					  const char *path, int flags)
{
	io_uring_prep_rw(IORING_OP_UNLINKAT, sqe, dfd, path, 0, 0);
	sqe->unlink_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_unlink(struct io_uring_sqe *sqe,
					  const char *path, int flags)
{
	io_uring_prep_unlinkat(sqe, AT_FDCWD, path, flags);
}

IOURINGINLINE void io_uring_prep_renameat(struct io_uring_sqe *sqe, int olddfd,
					  const char *oldpath, int newdfd,
					  const char *newpath, unsigned int flags)
{
	io_uring_prep_rw(IORING_OP_RENAMEAT, sqe, olddfd, oldpath,
				(__u32) newdfd,
				(uint64_t) (uintptr_t) newpath);
	sqe->rename_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_rename(struct io_uring_sqe *sqe,
					const char *oldpath,
					const char *newpath)
{
	io_uring_prep_renameat(sqe, AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

IOURINGINLINE void io_uring_prep_sync_file_range(struct io_uring_sqe *sqe,
						 int fd, unsigned len,
						 __u64 offset, int flags)
{
	io_uring_prep_rw(IORING_OP_SYNC_FILE_RANGE, sqe, fd, NULL, len, offset);
	sqe->sync_range_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_mkdirat(struct io_uring_sqe *sqe, int dfd,
					const char *path, mode_t mode)
{
	io_uring_prep_rw(IORING_OP_MKDIRAT, sqe, dfd, path, mode, 0);
}

IOURINGINLINE void io_uring_prep_mkdir(struct io_uring_sqe *sqe,
					const char *path, mode_t mode)
{
	io_uring_prep_mkdirat(sqe, AT_FDCWD, path, mode);
}

IOURINGINLINE void io_uring_prep_symlinkat(struct io_uring_sqe *sqe,
					   const char *target, int newdirfd,
					   const char *linkpath)
{
	io_uring_prep_rw(IORING_OP_SYMLINKAT, sqe, newdirfd, target, 0,
				(uint64_t) (uintptr_t) linkpath);
}

IOURINGINLINE void io_uring_prep_symlink(struct io_uring_sqe *sqe,
					 const char *target,
					 const char *linkpath)
{
	io_uring_prep_symlinkat(sqe, target, AT_FDCWD, linkpath);
}

IOURINGINLINE void io_uring_prep_linkat(struct io_uring_sqe *sqe, int olddfd,
					const char *oldpath, int newdfd,
					const char *newpath, int flags)
{
	io_uring_prep_rw(IORING_OP_LINKAT, sqe, olddfd, oldpath, (__u32) newdfd,
				(uint64_t) (uintptr_t) newpath);
	sqe->hardlink_flags = (__u32) flags;
}

IOURINGINLINE void io_uring_prep_link(struct io_uring_sqe *sqe,
				      const char *oldpath, const char *newpath,
				      int flags)
{
	io_uring_prep_linkat(sqe, AT_FDCWD, oldpath, AT_FDCWD, newpath, flags);
}

IOURINGINLINE void io_uring_prep_msg_ring_cqe_flags(struct io_uring_sqe *sqe,
					  int fd, unsigned int len, __u64 data,
					  unsigned int flags, unsigned int cqe_flags)
{
	io_uring_prep_rw(IORING_OP_MSG_RING, sqe, fd, NULL, len, data);
	sqe->msg_ring_flags = IORING_MSG_RING_FLAGS_PASS | flags;
	sqe->file_index = cqe_flags;
}

IOURINGINLINE void io_uring_prep_msg_ring(struct io_uring_sqe *sqe, int fd,
					  unsigned int len, __u64 data,
					  unsigned int flags)
{
	io_uring_prep_rw(IORING_OP_MSG_RING, sqe, fd, NULL, len, data);
	sqe->msg_ring_flags = flags;
}

IOURINGINLINE void io_uring_prep_msg_ring_fd(struct io_uring_sqe *sqe, int fd,
					     int source_fd, int target_fd,
					     __u64 data, unsigned int flags)
{
	io_uring_prep_rw(IORING_OP_MSG_RING, sqe, fd,
			 (void *) (uintptr_t) IORING_MSG_SEND_FD, 0, data);
	sqe->addr3 = source_fd;
	/* offset by 1 for allocation */
	if ((unsigned int) target_fd == IORING_FILE_INDEX_ALLOC)
		target_fd--;
	__io_uring_set_target_fixed_file(sqe, target_fd);
	sqe->msg_ring_flags = flags;
}

IOURINGINLINE void io_uring_prep_msg_ring_fd_alloc(struct io_uring_sqe *sqe,
						   int fd, int source_fd,
						   __u64 data, unsigned int flags)
{
	io_uring_prep_msg_ring_fd(sqe, fd, source_fd, IORING_FILE_INDEX_ALLOC,
				  data, flags);
}

IOURINGINLINE void io_uring_prep_getxattr(struct io_uring_sqe *sqe,
					  const char *name, char *value,
					  const char *path, unsigned int len)
{
	io_uring_prep_rw(IORING_OP_GETXATTR, sqe, 0, name, len,
				(__u64) (uintptr_t) value);
	sqe->addr3 = (__u64) (uintptr_t) path;
	sqe->xattr_flags = 0;
}

IOURINGINLINE void io_uring_prep_setxattr(struct io_uring_sqe *sqe,
					  const char *name, const char *value,
					  const char *path, int flags,
					  unsigned int len)
{
	io_uring_prep_rw(IORING_OP_SETXATTR, sqe, 0, name, len,
				(__u64) (uintptr_t) value);
	sqe->addr3 = (__u64) (uintptr_t) path;
	sqe->xattr_flags = flags;
}

IOURINGINLINE void io_uring_prep_fgetxattr(struct io_uring_sqe *sqe,
					   int fd, const char *name,
					   char *value, unsigned int len)
{
	io_uring_prep_rw(IORING_OP_FGETXATTR, sqe, fd, name, len,
				(__u64) (uintptr_t) value);
	sqe->xattr_flags = 0;
}

IOURINGINLINE void io_uring_prep_fsetxattr(struct io_uring_sqe *sqe, int fd,
					   const char *name, const char	*value,
					   int flags, unsigned int len)
{
	io_uring_prep_rw(IORING_OP_FSETXATTR, sqe, fd, name, len,
				(__u64) (uintptr_t) value);
	sqe->xattr_flags = flags;
}

IOURINGINLINE void io_uring_prep_socket(struct io_uring_sqe *sqe, int domain,
					int type, int protocol,
					unsigned int flags)
{
	io_uring_prep_rw(IORING_OP_SOCKET, sqe, domain, NULL, protocol, type);
	sqe->rw_flags = flags;
}

IOURINGINLINE void io_uring_prep_socket_direct(struct io_uring_sqe *sqe,
					       int domain, int type,
					       int protocol,
					       unsigned file_index,
					       unsigned int flags)
{
	io_uring_prep_rw(IORING_OP_SOCKET, sqe, domain, NULL, protocol, type);
	sqe->rw_flags = flags;
	/* offset by 1 for allocation */
	if (file_index == IORING_FILE_INDEX_ALLOC)
		file_index--;
	__io_uring_set_target_fixed_file(sqe, file_index);
}

IOURINGINLINE void io_uring_prep_socket_direct_alloc(struct io_uring_sqe *sqe,
						     int domain, int type,
						     int protocol,
						     unsigned int flags)
{
	io_uring_prep_rw(IORING_OP_SOCKET, sqe, domain, NULL, protocol, type);
	sqe->rw_flags = flags;
	__io_uring_set_target_fixed_file(sqe, IORING_FILE_INDEX_ALLOC - 1);
}


#define UNUSED(x) (void)(x)

/*
 * Prepare commands for sockets
 */
IOURINGINLINE void io_uring_prep_cmd_sock(struct io_uring_sqe *sqe,
					  int cmd_op,
					  int fd,
					  int level,
					  int optname,
					  void *optval,
					  int optlen)
{
	/* This will be removed once the get/setsockopt() patches land */
	UNUSED(optlen);
	UNUSED(optval);
	UNUSED(level);
	UNUSED(optname);
	io_uring_prep_rw(IORING_OP_URING_CMD, sqe, fd, NULL, 0, 0);
	sqe->cmd_op = cmd_op;
}

/*
 * Returns number of unconsumed (if SQPOLL) or unsubmitted entries exist in
 * the SQ ring
 */
IOURINGINLINE unsigned io_uring_sq_ready(const struct io_uring *ring)
{
	unsigned khead = *ring->sq.khead;

	/*
	 * Without a barrier, we could miss an update and think the SQ wasn't
	 * ready. We don't need the load acquire for non-SQPOLL since then we
	 * drive updates.
	 */
	if (ring->flags & IORING_SETUP_SQPOLL)
		khead = io_uring_smp_load_acquire(ring->sq.khead);

	/* always use real head, to avoid losing sync for short submit */
	return ring->sq.sqe_tail - khead;
}

/*
 * Returns how much space is left in the SQ ring.
 */
IOURINGINLINE unsigned io_uring_sq_space_left(const struct io_uring *ring)
{
	return ring->sq.ring_entries - io_uring_sq_ready(ring);
}

/*
 * Only applicable when using SQPOLL - allows the caller to wait for space
 * to free up in the SQ ring, which happens when the kernel side thread has
 * consumed one or more entries. If the SQ ring is currently non-full, no
 * action is taken. Note: may return -EINVAL if the kernel doesn't support
 * this feature.
 */
IOURINGINLINE int io_uring_sqring_wait(struct io_uring *ring)
{
	if (!(ring->flags & IORING_SETUP_SQPOLL))
		return 0;
	if (io_uring_sq_space_left(ring))
		return 0;

	return __io_uring_sqring_wait(ring);
}

/*
 * Returns how many unconsumed entries are ready in the CQ ring
 */
IOURINGINLINE unsigned io_uring_cq_ready(const struct io_uring *ring)
{
	return io_uring_smp_load_acquire(ring->cq.ktail) - *ring->cq.khead;
}

/*
 * Returns true if there are overflow entries waiting to be flushed onto
 * the CQ ring
 */
IOURINGINLINE bool io_uring_cq_has_overflow(const struct io_uring *ring)
{
	return IO_URING_READ_ONCE(*ring->sq.kflags) & IORING_SQ_CQ_OVERFLOW;
}

/*
 * Returns true if the eventfd notification is currently enabled
 */
IOURINGINLINE bool io_uring_cq_eventfd_enabled(const struct io_uring *ring)
{
	if (!ring->cq.kflags)
		return true;

	return !(*ring->cq.kflags & IORING_CQ_EVENTFD_DISABLED);
}

/*
 * Toggle eventfd notification on or off, if an eventfd is registered with
 * the ring.
 */
IOURINGINLINE int io_uring_cq_eventfd_toggle(struct io_uring *ring,
					     bool enabled)
{
	uint32_t flags;

	if (!!enabled == io_uring_cq_eventfd_enabled(ring))
		return 0;

	if (!ring->cq.kflags)
		return -EOPNOTSUPP;

	flags = *ring->cq.kflags;

	if (enabled)
		flags &= ~IORING_CQ_EVENTFD_DISABLED;
	else
		flags |= IORING_CQ_EVENTFD_DISABLED;

	IO_URING_WRITE_ONCE(*ring->cq.kflags, flags);

	return 0;
}

/*
 * Return an IO completion, waiting for 'wait_nr' completions if one isn't
 * readily available. Returns 0 with cqe_ptr filled in on success, -errno on
 * failure.
 */
IOURINGINLINE int io_uring_wait_cqe_nr(struct io_uring *ring,
				      struct io_uring_cqe **cqe_ptr,
				      unsigned wait_nr)
{
	return __io_uring_get_cqe(ring, cqe_ptr, 0, wait_nr, NULL);
}

/*
 * Internal helper, don't use directly in applications. Use one of the
 * "official" versions of this, io_uring_peek_cqe(), io_uring_wait_cqe(),
 * or io_uring_wait_cqes*().
 */
IOURINGINLINE int __io_uring_peek_cqe(struct io_uring *ring,
				      struct io_uring_cqe **cqe_ptr,
				      unsigned *nr_available)
{
	struct io_uring_cqe *cqe;
	int err = 0;
	unsigned available;
	unsigned mask = ring->cq.ring_mask;
	int shift = 0;

	if (ring->flags & IORING_SETUP_CQE32)
		shift = 1;

	do {
		unsigned tail = io_uring_smp_load_acquire(ring->cq.ktail);
		unsigned head = *ring->cq.khead;

		cqe = NULL;
		available = tail - head;
		if (!available)
			break;

		cqe = &ring->cq.cqes[(head & mask) << shift];
		if (!(ring->features & IORING_FEAT_EXT_ARG) &&
				cqe->user_data == LIBURING_UDATA_TIMEOUT) {
			if (cqe->res < 0)
				err = cqe->res;
			io_uring_cq_advance(ring, 1);
			if (!err)
				continue;
			cqe = NULL;
		}

		break;
	} while (1);

	*cqe_ptr = cqe;
	if (nr_available)
		*nr_available = available;
	return err;
}

/*
 * Return an IO completion, if one is readily available. Returns 0 with
 * cqe_ptr filled in on success, -errno on failure.
 */
IOURINGINLINE int io_uring_peek_cqe(struct io_uring *ring,
				    struct io_uring_cqe **cqe_ptr)
{
	if (!__io_uring_peek_cqe(ring, cqe_ptr, NULL) && *cqe_ptr)
		return 0;

	return io_uring_wait_cqe_nr(ring, cqe_ptr, 0);
}

/*
 * Return an IO completion, waiting for it if necessary. Returns 0 with
 * cqe_ptr filled in on success, -errno on failure.
 */
IOURINGINLINE int io_uring_wait_cqe(struct io_uring *ring,
				    struct io_uring_cqe **cqe_ptr)
{
	if (!__io_uring_peek_cqe(ring, cqe_ptr, NULL) && *cqe_ptr)
		return 0;

	return io_uring_wait_cqe_nr(ring, cqe_ptr, 1);
}

/*
 * Return an sqe to fill. Application must later call io_uring_submit()
 * when it's ready to tell the kernel about it. The caller may call this
 * function multiple times before calling io_uring_submit().
 *
 * Returns a vacant sqe, or NULL if we're full.
 */
IOURINGINLINE struct io_uring_sqe *_io_uring_get_sqe(struct io_uring *ring)
{
	struct io_uring_sq *sq = &ring->sq;
	unsigned int head, next = sq->sqe_tail + 1;
	int shift = 0;

	if (ring->flags & IORING_SETUP_SQE128)
		shift = 1;
	if (!(ring->flags & IORING_SETUP_SQPOLL))
		head = IO_URING_READ_ONCE(*sq->khead);
	else
		head = io_uring_smp_load_acquire(sq->khead);

	if (next - head <= sq->ring_entries) {
		struct io_uring_sqe *sqe;

		sqe = &sq->sqes[(sq->sqe_tail & sq->ring_mask) << shift];
		sq->sqe_tail = next;
		return sqe;
	}

	return NULL;
}

/*
 * Return the appropriate mask for a buffer ring of size 'ring_entries'
 */
IOURINGINLINE int io_uring_buf_ring_mask(__u32 ring_entries)
{
	return ring_entries - 1;
}

IOURINGINLINE void io_uring_buf_ring_init(struct io_uring_buf_ring *br)
{
	br->tail = 0;
}

/*
 * Assign 'buf' with the addr/len/buffer ID supplied
 */
IOURINGINLINE void io_uring_buf_ring_add(struct io_uring_buf_ring *br,
					 void *addr, unsigned int len,
					 unsigned short bid, int mask,
					 int buf_offset)
{
	struct io_uring_buf *buf = &br->bufs[(br->tail + buf_offset) & mask];

	buf->addr = (unsigned long) (uintptr_t) addr;
	buf->len = len;
	buf->bid = bid;
}

/*
 * Make 'count' new buffers visible to the kernel. Called after
 * io_uring_buf_ring_add() has been called 'count' times to fill in new
 * buffers.
 */
IOURINGINLINE void io_uring_buf_ring_advance(struct io_uring_buf_ring *br,
					     int count)
{
	unsigned short new_tail = br->tail + count;

	io_uring_smp_store_release(&br->tail, new_tail);
}

IOURINGINLINE void __io_uring_buf_ring_cq_advance(struct io_uring *ring,
						  struct io_uring_buf_ring *br,
						  int cq_count, int buf_count)
{
	br->tail += buf_count;
	io_uring_cq_advance(ring, cq_count);
}

/*
 * Make 'count' new buffers visible to the kernel while at the same time
 * advancing the CQ ring seen entries. This can be used when the application
 * is using ring provided buffers and returns buffers while processing CQEs,
 * avoiding an extra atomic when needing to increment both the CQ ring and
 * the ring buffer index at the same time.
 */
IOURINGINLINE void io_uring_buf_ring_cq_advance(struct io_uring *ring,
						struct io_uring_buf_ring *br,
						int count)
{
	__io_uring_buf_ring_cq_advance(ring, br, count, count);
}

#ifndef LIBURING_INTERNAL
IOURINGINLINE struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring)
{
	return _io_uring_get_sqe(ring);
}
#else
struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring);
#endif

ssize_t io_uring_mlock_size(unsigned entries, unsigned flags);
ssize_t io_uring_mlock_size_params(unsigned entries, struct io_uring_params *p);

/*
 * Versioning information for liburing.
 *
 * Use IO_URING_CHECK_VERSION() for compile time checks including from
 * preprocessor directives.
 *
 * Use io_uring_check_version() for runtime checks of the version of
 * liburing that was loaded by the dynamic linker.
 */
int io_uring_major_version(void);
int io_uring_minor_version(void);
bool io_uring_check_version(int major, int minor);

#define IO_URING_CHECK_VERSION(major,minor) \
  (major > IO_URING_VERSION_MAJOR ||        \
   (major == IO_URING_VERSION_MAJOR &&      \
    minor >= IO_URING_VERSION_MINOR))

#ifdef __cplusplus
}
#endif

#ifdef IOURINGINLINE
#undef IOURINGINLINE
#endif

#endif
