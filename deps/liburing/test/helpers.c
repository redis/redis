/* SPDX-License-Identifier: MIT */
/*
 * Description: Helpers for tests.
 */
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "helpers.h"
#include "liburing.h"

/*
 * Helper for allocating memory in tests.
 */
void *t_malloc(size_t size)
{
	void *ret;
	ret = malloc(size);
	assert(ret);
	return ret;
}

/*
 * Helper for binding socket to an ephemeral port.
 * The port number to be bound is returned in @addr->sin_port.
 */
int t_bind_ephemeral_port(int fd, struct sockaddr_in *addr)
{
	socklen_t addrlen;
	int ret;

	addr->sin_port = 0;
	if (bind(fd, (struct sockaddr *)addr, sizeof(*addr)))
		return -errno;

	addrlen = sizeof(*addr);
	ret = getsockname(fd, (struct sockaddr *)addr, &addrlen);
	assert(!ret);
	assert(addr->sin_port != 0);
	return 0;
}

/*
 * Helper for allocating size bytes aligned on a boundary.
 */
void t_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	int ret;
	ret = posix_memalign(memptr, alignment, size);
	assert(!ret);
}

/*
 * Helper for allocating space for an array of nmemb elements
 * with size bytes for each element.
 */
void *t_calloc(size_t nmemb, size_t size)
{
	void *ret;
	ret = calloc(nmemb, size);
	assert(ret);
	return ret;
}

/*
 * Helper for creating file and write @size byte buf with 0xaa value in the file.
 */
static void __t_create_file(const char *file, size_t size, char pattern)
{
	ssize_t ret;
	char *buf;
	int fd;

	buf = t_malloc(size);
	memset(buf, pattern, size);

	fd = open(file, O_WRONLY | O_CREAT, 0644);
	assert(fd >= 0);

	ret = write(fd, buf, size);
	fsync(fd);
	close(fd);
	free(buf);
	assert(ret == size);
}

void t_create_file(const char *file, size_t size)
{
	__t_create_file(file, size, 0xaa);
}

void t_create_file_pattern(const char *file, size_t size, char pattern)
{
	__t_create_file(file, size, pattern);
}

/*
 * Helper for creating @buf_num number of iovec
 * with @buf_size bytes buffer of each iovec.
 */
struct iovec *t_create_buffers(size_t buf_num, size_t buf_size)
{
	struct iovec *vecs;
	int i;

	vecs = t_malloc(buf_num * sizeof(struct iovec));
	for (i = 0; i < buf_num; i++) {
		t_posix_memalign(&vecs[i].iov_base, buf_size, buf_size);
		vecs[i].iov_len = buf_size;
	}
	return vecs;
}

/*
 * Helper for setting up an io_uring instance, skipping if the given user isn't
 * allowed to.
 */
enum t_setup_ret t_create_ring_params(int depth, struct io_uring *ring,
				      struct io_uring_params *p)
{
	int ret;

	ret = io_uring_queue_init_params(depth, ring, p);
	if (!ret)
		return T_SETUP_OK;
	if ((p->flags & IORING_SETUP_SQPOLL) && ret == -EPERM && geteuid()) {
		fprintf(stdout, "SQPOLL skipped for regular user\n");
		return T_SETUP_SKIP;
	}

	if (ret != -EINVAL)
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
	return ret;
}

enum t_setup_ret t_create_ring(int depth, struct io_uring *ring,
			       unsigned int flags)
{
	struct io_uring_params p = { };

	p.flags = flags;
	return t_create_ring_params(depth, ring, &p);
}

enum t_setup_ret t_register_buffers(struct io_uring *ring,
				    const struct iovec *iovecs,
				    unsigned nr_iovecs)
{
	int ret;

	ret = io_uring_register_buffers(ring, iovecs, nr_iovecs);
	if (!ret)
		return T_SETUP_OK;

	if ((ret == -EPERM || ret == -ENOMEM) && geteuid()) {
		fprintf(stdout, "too large non-root buffer registration, skip\n");
		return T_SETUP_SKIP;
	}

	fprintf(stderr, "buffer register failed: %s\n", strerror(-ret));
	return ret;
}

int t_create_socket_pair(int fd[2], bool stream)
{
	int ret;
	int type = stream ? SOCK_STREAM : SOCK_DGRAM;
	int val;
	struct sockaddr_in serv_addr;
	struct sockaddr *paddr;
	socklen_t paddrlen;

	type |= SOCK_CLOEXEC;
	fd[0] = socket(AF_INET, type, 0);
	if (fd[0] < 0)
		return errno;
	fd[1] = socket(AF_INET, type, 0);
	if (fd[1] < 0) {
		ret = errno;
		close(fd[0]);
		return ret;
	}

	val = 1;
	if (setsockopt(fd[0], SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)))
		goto errno_cleanup;

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = 0;
	inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

	paddr = (struct sockaddr *)&serv_addr;
	paddrlen = sizeof(serv_addr);

	if (bind(fd[0], paddr, paddrlen)) {
		fprintf(stderr, "bind failed\n");
		goto errno_cleanup;
	}

	if (stream && listen(fd[0], 16)) {
		fprintf(stderr, "listen failed\n");
		goto errno_cleanup;
	}

	if (getsockname(fd[0], (struct sockaddr *)&serv_addr,
			(socklen_t *)&paddrlen)) {
		fprintf(stderr, "getsockname failed\n");
		goto errno_cleanup;
	}
	inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

	if (connect(fd[1], (struct sockaddr *)&serv_addr, paddrlen)) {
		fprintf(stderr, "connect failed\n");
		goto errno_cleanup;
	}

	if (!stream) {
		/* connect the other udp side */
		if (getsockname(fd[1], (struct sockaddr *)&serv_addr,
				(socklen_t *)&paddrlen)) {
			fprintf(stderr, "getsockname failed\n");
			goto errno_cleanup;
		}
		inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

		if (connect(fd[0], (struct sockaddr *)&serv_addr, paddrlen)) {
			fprintf(stderr, "connect failed\n");
			goto errno_cleanup;
		}
		return 0;
	}

	/* for stream case we must accept and cleanup the listen socket */

	ret = accept(fd[0], NULL, NULL);
	if (ret < 0)
		goto errno_cleanup;

	close(fd[0]);
	fd[0] = ret;

	return 0;

errno_cleanup:
	ret = errno;
	close(fd[0]);
	close(fd[1]);
	return ret;
}

bool t_probe_defer_taskrun(void)
{
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init(1, &ring, IORING_SETUP_SINGLE_ISSUER |
					    IORING_SETUP_DEFER_TASKRUN);
	if (ret < 0)
		return false;
	io_uring_queue_exit(&ring);
	return true;
}

/*
 * Sync internal state with kernel ring state on the SQ side. Returns the
 * number of pending items in the SQ ring, for the shared ring.
 */
unsigned __io_uring_flush_sq(struct io_uring *ring)
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
 * Implementation of error(3), prints an error message and exits.
 */
void t_error(int status, int errnum, const char *format, ...)
{
	va_list args;
    	va_start(args, format);

	vfprintf(stderr, format, args);
    	if (errnum)
        	fprintf(stderr, ": %s", strerror(errnum));

	fprintf(stderr, "\n");
	va_end(args);
    	exit(status);
}
