/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200112L

#include "lib.h"
#include "syscall.h"
#include "liburing.h"
#include "int_flags.h"
#include "liburing/compat.h"
#include "liburing/io_uring.h"

static inline int do_register(struct io_uring *ring, unsigned int opcode,
			      const void *arg, unsigned int nr_args)
{
	int fd;

	if (ring->int_flags & INT_FLAG_REG_REG_RING) {
		opcode |= IORING_REGISTER_USE_REGISTERED_RING;
		fd = ring->enter_ring_fd;
	} else {
		fd = ring->ring_fd;
	}

	return __sys_io_uring_register(fd, opcode, arg, nr_args);
}

int io_uring_register_buffers_update_tag(struct io_uring *ring, unsigned off,
					 const struct iovec *iovecs,
					 const __u64 *tags,
					 unsigned nr)
{
	struct io_uring_rsrc_update2 up = {
		.offset	= off,
		.data = (unsigned long)iovecs,
		.tags = (unsigned long)tags,
		.nr = nr,
	};

	return do_register(ring, IORING_REGISTER_BUFFERS_UPDATE, &up, sizeof(up));
}

int io_uring_register_buffers_tags(struct io_uring *ring,
				   const struct iovec *iovecs,
				   const __u64 *tags,
				   unsigned nr)
{
	struct io_uring_rsrc_register reg = {
		.nr = nr,
		.data = (unsigned long)iovecs,
		.tags = (unsigned long)tags,
	};

	return do_register(ring, IORING_REGISTER_BUFFERS2, &reg, sizeof(reg));
}

int io_uring_register_buffers_sparse(struct io_uring *ring, unsigned nr)
{
	struct io_uring_rsrc_register reg = {
		.flags = IORING_RSRC_REGISTER_SPARSE,
		.nr = nr,
	};

	return do_register(ring, IORING_REGISTER_BUFFERS2, &reg, sizeof(reg));
}

int io_uring_register_buffers(struct io_uring *ring, const struct iovec *iovecs,
			      unsigned nr_iovecs)
{
	return do_register(ring, IORING_REGISTER_BUFFERS, iovecs, nr_iovecs);
}

int io_uring_unregister_buffers(struct io_uring *ring)
{
	return do_register(ring, IORING_UNREGISTER_BUFFERS, NULL, 0);
}

int io_uring_register_files_update_tag(struct io_uring *ring, unsigned off,
					const int *files, const __u64 *tags,
					unsigned nr_files)
{
	struct io_uring_rsrc_update2 up = {
		.offset	= off,
		.data = (unsigned long)files,
		.tags = (unsigned long)tags,
		.nr = nr_files,
	};

	return do_register(ring, IORING_REGISTER_FILES_UPDATE2, &up, sizeof(up));
}

/*
 * Register an update for an existing file set. The updates will start at
 * 'off' in the original array, and 'nr_files' is the number of files we'll
 * update.
 *
 * Returns number of files updated on success, -ERROR on failure.
 */
int io_uring_register_files_update(struct io_uring *ring, unsigned off,
				   const int *files, unsigned nr_files)
{
	struct io_uring_files_update up = {
		.offset	= off,
		.fds	= (unsigned long) files,
	};

	return do_register(ring, IORING_REGISTER_FILES_UPDATE, &up, nr_files);
}

static int increase_rlimit_nofile(unsigned nr)
{
	int ret;
	struct rlimit rlim;

	ret = __sys_getrlimit(RLIMIT_NOFILE, &rlim);
	if (ret < 0)
		return ret;

	if (rlim.rlim_cur < nr) {
		rlim.rlim_cur += nr;
		__sys_setrlimit(RLIMIT_NOFILE, &rlim);
	}

	return 0;
}

int io_uring_register_files_sparse(struct io_uring *ring, unsigned nr)
{
	struct io_uring_rsrc_register reg = {
		.flags = IORING_RSRC_REGISTER_SPARSE,
		.nr = nr,
	};
	int ret, did_increase = 0;

	do {
		ret = do_register(ring, IORING_REGISTER_FILES2, &reg, sizeof(reg));
		if (ret >= 0)
			break;
		if (ret == -EMFILE && !did_increase) {
			did_increase = 1;
			increase_rlimit_nofile(nr);
			continue;
		}
		break;
	} while (1);

	return ret;
}

int io_uring_register_files_tags(struct io_uring *ring, const int *files,
				 const __u64 *tags, unsigned nr)
{
	struct io_uring_rsrc_register reg = {
		.nr = nr,
		.data = (unsigned long)files,
		.tags = (unsigned long)tags,
	};
	int ret, did_increase = 0;

	do {
		ret = do_register(ring, IORING_REGISTER_FILES2, &reg, sizeof(reg));
		if (ret >= 0)
			break;
		if (ret == -EMFILE && !did_increase) {
			did_increase = 1;
			increase_rlimit_nofile(nr);
			continue;
		}
		break;
	} while (1);

	return ret;
}

int io_uring_register_files(struct io_uring *ring, const int *files,
			    unsigned nr_files)
{
	int ret, did_increase = 0;

	do {
		ret = do_register(ring, IORING_REGISTER_FILES, files, nr_files);
		if (ret >= 0)
			break;
		if (ret == -EMFILE && !did_increase) {
			did_increase = 1;
			increase_rlimit_nofile(nr_files);
			continue;
		}
		break;
	} while (1);

	return ret;
}

int io_uring_unregister_files(struct io_uring *ring)
{
	return do_register(ring, IORING_UNREGISTER_FILES, NULL, 0);
}

int io_uring_register_eventfd(struct io_uring *ring, int event_fd)
{
	return do_register(ring, IORING_REGISTER_EVENTFD, &event_fd, 1);
}

int io_uring_unregister_eventfd(struct io_uring *ring)
{
	return do_register(ring, IORING_UNREGISTER_EVENTFD, NULL, 0);
}

int io_uring_register_eventfd_async(struct io_uring *ring, int event_fd)
{
	return do_register(ring, IORING_REGISTER_EVENTFD_ASYNC, &event_fd, 1);
}

int io_uring_register_probe(struct io_uring *ring, struct io_uring_probe *p,
			    unsigned int nr_ops)
{
	return do_register(ring, IORING_REGISTER_PROBE, p, nr_ops);
}

int io_uring_register_personality(struct io_uring *ring)
{
	return do_register(ring, IORING_REGISTER_PERSONALITY, NULL, 0);
}

int io_uring_unregister_personality(struct io_uring *ring, int id)
{
	return do_register(ring, IORING_UNREGISTER_PERSONALITY, NULL, id);
}

int io_uring_register_restrictions(struct io_uring *ring,
				   struct io_uring_restriction *res,
				   unsigned int nr_res)
{
	return do_register(ring, IORING_REGISTER_RESTRICTIONS, res, nr_res);
}

int io_uring_enable_rings(struct io_uring *ring)
{
	return do_register(ring, IORING_REGISTER_ENABLE_RINGS, NULL, 0);
}

int io_uring_register_iowq_aff(struct io_uring *ring, size_t cpusz,
			       const cpu_set_t *mask)
{
	if (cpusz >= (1U << 31))
		return -EINVAL;

	return do_register(ring, IORING_REGISTER_IOWQ_AFF, mask, (int) cpusz);
}

int io_uring_unregister_iowq_aff(struct io_uring *ring)
{
	return do_register(ring, IORING_UNREGISTER_IOWQ_AFF, NULL, 0);
}

int io_uring_register_iowq_max_workers(struct io_uring *ring, unsigned int *val)
{
	return do_register(ring, IORING_REGISTER_IOWQ_MAX_WORKERS, val, 2);
}

int io_uring_register_ring_fd(struct io_uring *ring)
{
	struct io_uring_rsrc_update up = {
		.data = ring->ring_fd,
		.offset = -1U,
	};
	int ret;

	if (ring->int_flags & INT_FLAG_REG_RING)
		return -EEXIST;

	ret = do_register(ring, IORING_REGISTER_RING_FDS, &up, 1);
	if (ret == 1) {
		ring->enter_ring_fd = up.offset;
		ring->int_flags |= INT_FLAG_REG_RING;
		if (ring->features & IORING_FEAT_REG_REG_RING) {
			ring->int_flags |= INT_FLAG_REG_REG_RING;
		}
	}
	return ret;
}


int io_uring_unregister_ring_fd(struct io_uring *ring)
{
	struct io_uring_rsrc_update up = {
		.offset = ring->enter_ring_fd,
	};
	int ret;

	if (!(ring->int_flags & INT_FLAG_REG_RING))
		return -EINVAL;

	ret = do_register(ring, IORING_UNREGISTER_RING_FDS, &up, 1);
	if (ret == 1) {
		ring->enter_ring_fd = ring->ring_fd;
		ring->int_flags &= ~(INT_FLAG_REG_RING | INT_FLAG_REG_REG_RING);
	}
	return ret;
}

int io_uring_close_ring_fd(struct io_uring *ring)
{
	if (!(ring->features & IORING_FEAT_REG_REG_RING))
		return -EOPNOTSUPP;
	if (!(ring->int_flags & INT_FLAG_REG_RING))
		return -EINVAL;
	if (ring->ring_fd == -1)
		return -EBADF;

	__sys_close(ring->ring_fd);
	ring->ring_fd = -1;
	return 1;
}

int io_uring_register_buf_ring(struct io_uring *ring,
			       struct io_uring_buf_reg *reg,
			       unsigned int __maybe_unused flags)
{
	return do_register(ring, IORING_REGISTER_PBUF_RING, reg, 1);
}

int io_uring_unregister_buf_ring(struct io_uring *ring, int bgid)
{
	struct io_uring_buf_reg reg = { .bgid = bgid };

	return do_register(ring, IORING_UNREGISTER_PBUF_RING, &reg, 1);
}

int io_uring_register_sync_cancel(struct io_uring *ring,
				  struct io_uring_sync_cancel_reg *reg)
{
	return do_register(ring, IORING_REGISTER_SYNC_CANCEL, reg, 1);
}

int io_uring_register_file_alloc_range(struct io_uring *ring,
					unsigned off, unsigned len)
{
	struct io_uring_file_index_range range = {
		.off = off,
		.len = len
	};

	return do_register(ring, IORING_REGISTER_FILE_ALLOC_RANGE, &range, 0);
}
