/* SPDX-License-Identifier: MIT */
#define _DEFAULT_SOURCE

#include "lib.h"
#include "syscall.h"
#include "liburing.h"
#include "int_flags.h"
#include "liburing/compat.h"
#include "liburing/io_uring.h"

#define KERN_MAX_ENTRIES	32768
#define KERN_MAX_CQ_ENTRIES	(2 * KERN_MAX_ENTRIES)

static inline int __fls(int x)
{
	if (!x)
		return 0;
	return 8 * sizeof(x) - __builtin_clz(x);
}

static unsigned roundup_pow2(unsigned depth)
{
	return 1U << __fls(depth - 1);
}

static int get_sq_cq_entries(unsigned entries, struct io_uring_params *p,
			     unsigned *sq, unsigned *cq)
{
	unsigned cq_entries;

	if (!entries)
		return -EINVAL;
	if (entries > KERN_MAX_ENTRIES) {
		if (!(p->flags & IORING_SETUP_CLAMP))
			return -EINVAL;
		entries = KERN_MAX_ENTRIES;
	}

	entries = roundup_pow2(entries);
	if (p->flags & IORING_SETUP_CQSIZE) {
		if (!p->cq_entries)
			return -EINVAL;
		cq_entries = p->cq_entries;
		if (cq_entries > KERN_MAX_CQ_ENTRIES) {
			if (!(p->flags & IORING_SETUP_CLAMP))
				return -EINVAL;
			cq_entries = KERN_MAX_CQ_ENTRIES;
		}
		cq_entries = roundup_pow2(cq_entries);
		if (cq_entries < entries)
			return -EINVAL;
	} else {
		cq_entries = 2 * entries;
	}

	*sq = entries;
	*cq = cq_entries;
	return 0;
}

static void io_uring_unmap_rings(struct io_uring_sq *sq, struct io_uring_cq *cq)
{
	if (sq->ring_sz)
		__sys_munmap(sq->ring_ptr, sq->ring_sz);
	if (cq->ring_ptr && cq->ring_sz && cq->ring_ptr != sq->ring_ptr)
		__sys_munmap(cq->ring_ptr, cq->ring_sz);
}

static void io_uring_setup_ring_pointers(struct io_uring_params *p,
					 struct io_uring_sq *sq,
					 struct io_uring_cq *cq)
{
	sq->khead = sq->ring_ptr + p->sq_off.head;
	sq->ktail = sq->ring_ptr + p->sq_off.tail;
	sq->kring_mask = sq->ring_ptr + p->sq_off.ring_mask;
	sq->kring_entries = sq->ring_ptr + p->sq_off.ring_entries;
	sq->kflags = sq->ring_ptr + p->sq_off.flags;
	sq->kdropped = sq->ring_ptr + p->sq_off.dropped;
	sq->array = sq->ring_ptr + p->sq_off.array;

	cq->khead = cq->ring_ptr + p->cq_off.head;
	cq->ktail = cq->ring_ptr + p->cq_off.tail;
	cq->kring_mask = cq->ring_ptr + p->cq_off.ring_mask;
	cq->kring_entries = cq->ring_ptr + p->cq_off.ring_entries;
	cq->koverflow = cq->ring_ptr + p->cq_off.overflow;
	cq->cqes = cq->ring_ptr + p->cq_off.cqes;
	if (p->cq_off.flags)
		cq->kflags = cq->ring_ptr + p->cq_off.flags;

	sq->ring_mask = *sq->kring_mask;
	sq->ring_entries = *sq->kring_entries;
	cq->ring_mask = *cq->kring_mask;
	cq->ring_entries = *cq->kring_entries;
}

static int io_uring_mmap(int fd, struct io_uring_params *p,
			 struct io_uring_sq *sq, struct io_uring_cq *cq)
{
	size_t size;
	int ret;

	size = sizeof(struct io_uring_cqe);
	if (p->flags & IORING_SETUP_CQE32)
		size += sizeof(struct io_uring_cqe);

	sq->ring_sz = p->sq_off.array + p->sq_entries * sizeof(unsigned);
	cq->ring_sz = p->cq_off.cqes + p->cq_entries * size;

	if (p->features & IORING_FEAT_SINGLE_MMAP) {
		if (cq->ring_sz > sq->ring_sz)
			sq->ring_sz = cq->ring_sz;
		cq->ring_sz = sq->ring_sz;
	}
	sq->ring_ptr = __sys_mmap(0, sq->ring_sz, PROT_READ | PROT_WRITE,
				  MAP_SHARED | MAP_POPULATE, fd,
				  IORING_OFF_SQ_RING);
	if (IS_ERR(sq->ring_ptr))
		return PTR_ERR(sq->ring_ptr);

	if (p->features & IORING_FEAT_SINGLE_MMAP) {
		cq->ring_ptr = sq->ring_ptr;
	} else {
		cq->ring_ptr = __sys_mmap(0, cq->ring_sz, PROT_READ | PROT_WRITE,
					  MAP_SHARED | MAP_POPULATE, fd,
					  IORING_OFF_CQ_RING);
		if (IS_ERR(cq->ring_ptr)) {
			ret = PTR_ERR(cq->ring_ptr);
			cq->ring_ptr = NULL;
			goto err;
		}
	}

	size = sizeof(struct io_uring_sqe);
	if (p->flags & IORING_SETUP_SQE128)
		size += 64;
	sq->sqes = __sys_mmap(0, size * p->sq_entries, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
	if (IS_ERR(sq->sqes)) {
		ret = PTR_ERR(sq->sqes);
err:
		io_uring_unmap_rings(sq, cq);
		return ret;
	}

	io_uring_setup_ring_pointers(p, sq, cq);
	return 0;
}

/*
 * For users that want to specify sq_thread_cpu or sq_thread_idle, this
 * interface is a convenient helper for mmap()ing the rings.
 * Returns -errno on error, or zero on success.  On success, 'ring'
 * contains the necessary information to read/write to the rings.
 */
__cold int io_uring_queue_mmap(int fd, struct io_uring_params *p,
			       struct io_uring *ring)
{
	memset(ring, 0, sizeof(*ring));
	return io_uring_mmap(fd, p, &ring->sq, &ring->cq);
}

/*
 * Ensure that the mmap'ed rings aren't available to a child after a fork(2).
 * This uses madvise(..., MADV_DONTFORK) on the mmap'ed ranges.
 */
__cold int io_uring_ring_dontfork(struct io_uring *ring)
{
	size_t len;
	int ret;

	if (!ring->sq.ring_ptr || !ring->sq.sqes || !ring->cq.ring_ptr)
		return -EINVAL;

	len = sizeof(struct io_uring_sqe);
	if (ring->flags & IORING_SETUP_SQE128)
		len += 64;
	len *= ring->sq.ring_entries;
	ret = __sys_madvise(ring->sq.sqes, len, MADV_DONTFORK);
	if (ret < 0)
		return ret;

	len = ring->sq.ring_sz;
	ret = __sys_madvise(ring->sq.ring_ptr, len, MADV_DONTFORK);
	if (ret < 0)
		return ret;

	if (ring->cq.ring_ptr != ring->sq.ring_ptr) {
		len = ring->cq.ring_sz;
		ret = __sys_madvise(ring->cq.ring_ptr, len, MADV_DONTFORK);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* FIXME */
static size_t huge_page_size = 2 * 1024 * 1024;

/*
 * Returns negative for error, or number of bytes used in the buffer on success
 */
static int io_uring_alloc_huge(unsigned entries, struct io_uring_params *p,
			       struct io_uring_sq *sq, struct io_uring_cq *cq,
			       void *buf, size_t buf_size)
{
	unsigned long page_size = get_page_size();
	unsigned sq_entries, cq_entries;
	size_t ring_mem, sqes_mem;
	unsigned long mem_used = 0;
	void *ptr;
	int ret;

	ret = get_sq_cq_entries(entries, p, &sq_entries, &cq_entries);
	if (ret)
		return ret;

	sqes_mem = sq_entries * sizeof(struct io_uring_sqe);
	sqes_mem = (sqes_mem + page_size - 1) & ~(page_size - 1);
	ring_mem = cq_entries * sizeof(struct io_uring_cqe);
	if (p->flags & IORING_SETUP_CQE32)
		ring_mem *= 2;
	ring_mem += sq_entries * sizeof(unsigned);
	mem_used = sqes_mem + ring_mem;
	mem_used = (mem_used + page_size - 1) & ~(page_size - 1);

	/*
	 * A maxed-out number of CQ entries with IORING_SETUP_CQE32 fills a 2MB
	 * huge page by itself, so the SQ entries won't fit in the same huge
	 * page. For SQEs, that shouldn't be possible given KERN_MAX_ENTRIES,
	 * but check that too to future-proof (e.g. against different huge page
	 * sizes). Bail out early so we don't overrun.
	 */
	if (!buf && (sqes_mem > huge_page_size || ring_mem > huge_page_size))
		return -ENOMEM;

	if (buf) {
		if (mem_used > buf_size)
			return -ENOMEM;
		ptr = buf;
	} else {
		int map_hugetlb = 0;
		if (sqes_mem <= page_size)
			buf_size = page_size;
		else {
			buf_size = huge_page_size;
			map_hugetlb = MAP_HUGETLB;
		}
		ptr = __sys_mmap(NULL, buf_size, PROT_READ|PROT_WRITE,
					MAP_SHARED|MAP_ANONYMOUS|map_hugetlb,
					-1, 0);
		if (IS_ERR(ptr))
			return PTR_ERR(ptr);
	}

	sq->sqes = ptr;
	if (mem_used <= buf_size) {
		sq->ring_ptr = (void *) sq->sqes + sqes_mem;
		/* clear ring sizes, we have just one mmap() to undo */
		cq->ring_sz = 0;
		sq->ring_sz = 0;
	} else {
		int map_hugetlb = 0;
		if (ring_mem <= page_size)
			buf_size = page_size;
		else {
			buf_size = huge_page_size;
			map_hugetlb = MAP_HUGETLB;
		}
		ptr = __sys_mmap(NULL, buf_size, PROT_READ|PROT_WRITE,
					MAP_SHARED|MAP_ANONYMOUS|map_hugetlb,
					-1, 0);
		if (IS_ERR(ptr)) {
			__sys_munmap(sq->sqes, 1);
			return PTR_ERR(ptr);
		}
		sq->ring_ptr = ptr;
		sq->ring_sz = buf_size;
		cq->ring_sz = 0;
	}

	cq->ring_ptr = (void *) sq->ring_ptr;
	p->sq_off.user_addr = (unsigned long) sq->sqes;
	p->cq_off.user_addr = (unsigned long) sq->ring_ptr;
	return (int) mem_used;
}

static int __io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
					struct io_uring_params *p, void *buf,
					size_t buf_size)
{
	int fd, ret = 0;
	unsigned *sq_array;
	unsigned sq_entries, index;

	memset(ring, 0, sizeof(*ring));

	/*
	 * The kernel does this check already, but checking it here allows us
	 * to avoid handling it below.
	 */
	if (p->flags & IORING_SETUP_REGISTERED_FD_ONLY
	    && !(p->flags & IORING_SETUP_NO_MMAP))
		return -EINVAL;

	if (p->flags & IORING_SETUP_NO_MMAP) {
		ret = io_uring_alloc_huge(entries, p, &ring->sq, &ring->cq,
						buf, buf_size);
		if (ret < 0)
			return ret;
		if (buf)
			ring->int_flags |= INT_FLAG_APP_MEM;
	}

	fd = __sys_io_uring_setup(entries, p);
	if (fd < 0) {
		if ((p->flags & IORING_SETUP_NO_MMAP) &&
		    !(ring->int_flags & INT_FLAG_APP_MEM)) {
			__sys_munmap(ring->sq.sqes, 1);
			io_uring_unmap_rings(&ring->sq, &ring->cq);
		}
		return fd;
	}

	if (!(p->flags & IORING_SETUP_NO_MMAP)) {
		ret = io_uring_queue_mmap(fd, p, ring);
		if (ret) {
			__sys_close(fd);
			return ret;
		}
	} else {
		io_uring_setup_ring_pointers(p, &ring->sq, &ring->cq);
	}

	/*
	 * Directly map SQ slots to SQEs
	 */
	sq_array = ring->sq.array;
	sq_entries = ring->sq.ring_entries;
	for (index = 0; index < sq_entries; index++)
		sq_array[index] = index;

	ring->features = p->features;
	ring->flags = p->flags;
	ring->enter_ring_fd = fd;
	if (p->flags & IORING_SETUP_REGISTERED_FD_ONLY) {
		ring->ring_fd = -1;
		ring->int_flags |= INT_FLAG_REG_RING | INT_FLAG_REG_REG_RING;
	} else {
		ring->ring_fd = fd;
	}

	return ret;
}

/*
 * Like io_uring_queue_init_params(), except it allows the application to pass
 * in a pre-allocated memory range that is used for the shared data between
 * the kernel and the application. This includes the sqes array, and the two
 * rings. The memory must be contigious, the use case here is that the app
 * allocates a huge page and passes it in.
 *
 * Returns the number of bytes used in the buffer, the app can then reuse
 * the buffer with the returned offset to put more rings in the same huge
 * page. Returns -ENOMEM if there's not enough room left in the buffer to
 * host the ring.
 */
int io_uring_queue_init_mem(unsigned entries, struct io_uring *ring,
			    struct io_uring_params *p,
			    void *buf, size_t buf_size)
{
	/* should already be set... */
	p->flags |= IORING_SETUP_NO_MMAP;
	return __io_uring_queue_init_params(entries, ring, p, buf, buf_size);
}

int io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
			       struct io_uring_params *p)
{
	int ret;

	ret = __io_uring_queue_init_params(entries, ring, p, NULL, 0);
	return ret >= 0 ? 0 : ret;
}

/*
 * Returns -errno on error, or zero on success. On success, 'ring'
 * contains the necessary information to read/write to the rings.
 */
__cold int io_uring_queue_init(unsigned entries, struct io_uring *ring,
			       unsigned flags)
{
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	p.flags = flags;

	return io_uring_queue_init_params(entries, ring, &p);
}

__cold void io_uring_queue_exit(struct io_uring *ring)
{
	struct io_uring_sq *sq = &ring->sq;
	struct io_uring_cq *cq = &ring->cq;
	size_t sqe_size;

	if (!sq->ring_sz) {
		sqe_size = sizeof(struct io_uring_sqe);
		if (ring->flags & IORING_SETUP_SQE128)
			sqe_size += 64;
		__sys_munmap(sq->sqes, sqe_size * sq->ring_entries);
		io_uring_unmap_rings(sq, cq);
	} else {
		if (!(ring->int_flags & INT_FLAG_APP_MEM)) {
			__sys_munmap(sq->sqes,
				*sq->kring_entries * sizeof(struct io_uring_sqe));
			io_uring_unmap_rings(sq, cq);
		}
	}

	/*
	 * Not strictly required, but frees up the slot we used now rather
	 * than at process exit time.
	 */
	if (ring->int_flags & INT_FLAG_REG_RING)
		io_uring_unregister_ring_fd(ring);
	if (ring->ring_fd != -1)
		__sys_close(ring->ring_fd);
}

__cold struct io_uring_probe *io_uring_get_probe_ring(struct io_uring *ring)
{
	struct io_uring_probe *probe;
	size_t len;
	int r;

	len = sizeof(*probe) + 256 * sizeof(struct io_uring_probe_op);
	probe = malloc(len);
	if (!probe)
		return NULL;
	memset(probe, 0, len);

	r = io_uring_register_probe(ring, probe, 256);
	if (r >= 0)
		return probe;

	free(probe);
	return NULL;
}

__cold struct io_uring_probe *io_uring_get_probe(void)
{
	struct io_uring ring;
	struct io_uring_probe *probe;
	int r;

	r = io_uring_queue_init(2, &ring, 0);
	if (r < 0)
		return NULL;

	probe = io_uring_get_probe_ring(&ring);
	io_uring_queue_exit(&ring);
	return probe;
}

__cold void io_uring_free_probe(struct io_uring_probe *probe)
{
	free(probe);
}

static size_t npages(size_t size, long page_size)
{
	size--;
	size /= page_size;
	return __fls((int) size);
}

#define KRING_SIZE	320

static size_t rings_size(struct io_uring_params *p, unsigned entries,
			 unsigned cq_entries, long page_size)
{
	size_t pages, sq_size, cq_size;

	cq_size = sizeof(struct io_uring_cqe);
	if (p->flags & IORING_SETUP_CQE32)
		cq_size += sizeof(struct io_uring_cqe);
	cq_size *= cq_entries;
	cq_size += KRING_SIZE;
	cq_size = (cq_size + 63) & ~63UL;
	pages = (size_t) 1 << npages(cq_size, page_size);

	sq_size = sizeof(struct io_uring_sqe);
	if (p->flags & IORING_SETUP_SQE128)
		sq_size += 64;
	sq_size *= entries;
	pages += (size_t) 1 << npages(sq_size, page_size);
	return pages * page_size;
}

/*
 * Return the required ulimit -l memlock memory required for a given ring
 * setup, in bytes. May return -errno on error. On newer (5.12+) kernels,
 * io_uring no longer requires any memlock memory, and hence this function
 * will return 0 for that case. On older (5.11 and prior) kernels, this will
 * return the required memory so that the caller can ensure that enough space
 * is available before setting up a ring with the specified parameters.
 */
__cold ssize_t io_uring_mlock_size_params(unsigned entries,
					  struct io_uring_params *p)
{
	struct io_uring_params lp;
	struct io_uring ring;
	unsigned cq_entries, sq;
	long page_size;
	ssize_t ret;
	int cret;

	memset(&lp, 0, sizeof(lp));

	/*
	 * We only really use this inited ring to see if the kernel is newer
	 * or not. Newer kernels don't require memlocked memory. If we fail,
	 * it's most likely because it's an older kernel and we have no
	 * available memlock space. Just continue on, lp.features will still
	 * be zeroed at this point and we'll do the right thing.
	 */
	ret = io_uring_queue_init_params(entries, &ring, &lp);
	if (!ret)
		io_uring_queue_exit(&ring);

	/*
	 * Native workers imply using cgroup memory accounting, and hence no
	 * memlock memory is needed for the ring allocations.
	 */
	if (lp.features & IORING_FEAT_NATIVE_WORKERS)
		return 0;

	if (!entries)
		return -EINVAL;
	if (entries > KERN_MAX_ENTRIES) {
		if (!(p->flags & IORING_SETUP_CLAMP))
			return -EINVAL;
		entries = KERN_MAX_ENTRIES;
	}

	cret = get_sq_cq_entries(entries, p, &sq, &cq_entries);
	if (cret)
		return cret;

	page_size = get_page_size();
	return rings_size(p, sq, cq_entries, page_size);
}

/*
 * Return required ulimit -l memory space for a given ring setup. See
 * @io_uring_mlock_size_params().
 */
__cold ssize_t io_uring_mlock_size(unsigned entries, unsigned flags)
{
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	p.flags = flags;
	return io_uring_mlock_size_params(entries, &p);
}

#if defined(__hppa__)
static struct io_uring_buf_ring *br_setup(struct io_uring *ring,
					  unsigned int nentries, int bgid,
					  unsigned int flags, int *ret)
{
	struct io_uring_buf_ring *br;
	struct io_uring_buf_reg reg;
	size_t ring_size;
	off_t off;
	int lret;

	memset(&reg, 0, sizeof(reg));
	reg.ring_entries = nentries;
	reg.bgid = bgid;
	reg.flags = IOU_PBUF_RING_MMAP;

	*ret = 0;
	lret = io_uring_register_buf_ring(ring, &reg, flags);
	if (lret) {
		*ret = lret;
		return NULL;
	}

	off = IORING_OFF_PBUF_RING | (unsigned long long) bgid << IORING_OFF_PBUF_SHIFT;
	ring_size = nentries * sizeof(struct io_uring_buf);
	br = __sys_mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ring->ring_fd, off);
	if (IS_ERR(br)) {
		*ret = PTR_ERR(br);
		return NULL;
	}

	return br;
}
#else
static struct io_uring_buf_ring *br_setup(struct io_uring *ring,
					  unsigned int nentries, int bgid,
					  unsigned int flags, int *ret)
{
	struct io_uring_buf_ring *br;
	struct io_uring_buf_reg reg;
	size_t ring_size;
	int lret;

	memset(&reg, 0, sizeof(reg));
	ring_size = nentries * sizeof(struct io_uring_buf);
	br = __sys_mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (IS_ERR(br)) {
		*ret = PTR_ERR(br);
		return NULL;
	}

	reg.ring_addr = (unsigned long) (uintptr_t) br;
	reg.ring_entries = nentries;
	reg.bgid = bgid;

	*ret = 0;
	lret = io_uring_register_buf_ring(ring, &reg, flags);
	if (lret) {
		__sys_munmap(br, ring_size);
		*ret = lret;
		br = NULL;
	}

	return br;
}
#endif

struct io_uring_buf_ring *io_uring_setup_buf_ring(struct io_uring *ring,
						  unsigned int nentries,
						  int bgid, unsigned int flags,
						  int *ret)
{
	struct io_uring_buf_ring *br;

	br = br_setup(ring, nentries, bgid, flags, ret);
	if (br)
		io_uring_buf_ring_init(br);

	return br;
}

int io_uring_free_buf_ring(struct io_uring *ring, struct io_uring_buf_ring *br,
			   unsigned int nentries, int bgid)
{
	int ret;

	ret = io_uring_unregister_buf_ring(ring, bgid);
	if (ret)
		return ret;

	__sys_munmap(br, nentries * sizeof(struct io_uring_buf));
	return 0;
}
