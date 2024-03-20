/* SPDX-License-Identifier: MIT */
/*
 * Check that IORING_OP_CONNECT works, with and without other side
 * being open.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include "liburing.h"
#include "helpers.h"

static int no_connect;
static unsigned short use_port;
static unsigned int use_addr;

static int create_socket(void)
{
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == -1) {
		perror("socket()");
		return -1;
	}

	return fd;
}

static int submit_and_wait(struct io_uring *ring, int *res)
{
	struct io_uring_cqe *cqe;
	int ret;

	ret = io_uring_submit_and_wait(ring, 1);
	if (ret != 1) {
		fprintf(stderr, "io_using_submit: got %d\n", ret);
		return 1;
	}

	ret = io_uring_peek_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "io_uring_peek_cqe(): no cqe returned");
		return 1;
	}

	*res = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return 0;
}

static int wait_for(struct io_uring *ring, int fd, int mask)
{
	struct io_uring_sqe *sqe;
	int ret, res;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "unable to get sqe\n");
		return -1;
	}

	io_uring_prep_poll_add(sqe, fd, mask);
	sqe->user_data = 2;

	ret = submit_and_wait(ring, &res);
	if (ret)
		return -1;

	if (res < 0) {
		fprintf(stderr, "poll(): failed with %d\n", res);
		return -1;
	}

	return res;
}

static int listen_on_socket(int fd)
{
	struct sockaddr_in addr;
	int ret;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = use_port;
	addr.sin_addr.s_addr = use_addr;

	ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret == -1) {
		perror("bind()");
		return -1;
	}

	ret = listen(fd, 128);
	if (ret == -1) {
		perror("listen()");
		return -1;
	}

	return 0;
}

static int configure_connect(int fd, struct sockaddr_in* addr)
{
	int ret, val = 1;

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
	if (ret == -1) {
		perror("setsockopt()");
		return -1;
	}

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	if (ret == -1) {
		perror("setsockopt()");
		return -1;
	}

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = use_port;
	ret = inet_aton("127.0.0.1", &addr->sin_addr);
	return ret;
}

static int connect_socket(struct io_uring *ring, int fd, int *code)
{
	struct sockaddr_in addr;
	int ret, res;
	socklen_t code_len = sizeof(*code);
	struct io_uring_sqe *sqe;

	if (configure_connect(fd, &addr) == -1)
		return -1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "unable to get sqe\n");
		return -1;
	}

	io_uring_prep_connect(sqe, fd, (struct sockaddr*)&addr, sizeof(addr));
	sqe->user_data = 1;

	ret = submit_and_wait(ring, &res);
	if (ret)
		return -1;

	if (res == -EINPROGRESS) {
		ret = wait_for(ring, fd, POLLOUT | POLLHUP | POLLERR);
		if (ret == -1)
			return -1;

		int ev = (ret & POLLOUT) || (ret & POLLHUP) || (ret & POLLERR);
		if (!ev) {
			fprintf(stderr, "poll(): returned invalid value %#x\n", ret);
			return -1;
		}

		ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, code, &code_len);
		if (ret == -1) {
			perror("getsockopt()");
			return -1;
		}
	} else
		*code = res;
	return 0;
}

static int test_connect_with_no_peer(struct io_uring *ring)
{
	int connect_fd;
	int ret, code;

	connect_fd = create_socket();
	if (connect_fd == -1)
		return -1;

	ret = connect_socket(ring, connect_fd, &code);
	if (ret == -1)
		goto err;

	if (code != -ECONNREFUSED) {
		if (code == -EINVAL || code == -EBADF || code == -EOPNOTSUPP) {
			fprintf(stdout, "No connect support, skipping\n");
			no_connect = 1;
			goto out;
		}
		fprintf(stderr, "connect failed with %d\n", code);
		goto err;
	}

out:
	close(connect_fd);
	return 0;

err:
	close(connect_fd);
	return -1;
}

static int test_connect(struct io_uring *ring)
{
	int accept_fd;
	int connect_fd;
	int ret, code;

	accept_fd = create_socket();
	if (accept_fd == -1)
		return -1;

	ret = listen_on_socket(accept_fd);
	if (ret == -1)
		goto err1;

	connect_fd = create_socket();
	if (connect_fd == -1)
		goto err1;

	ret = connect_socket(ring, connect_fd, &code);
	if (ret == -1)
		goto err2;

	if (code != 0) {
		fprintf(stderr, "connect failed with %d\n", code);
		goto err2;
	}

	close(connect_fd);
	close(accept_fd);

	return 0;

err2:
	close(connect_fd);

err1:
	close(accept_fd);
	return -1;
}

static int test_connect_timeout(struct io_uring *ring)
{
	int connect_fd[2] = {-1, -1};
	int accept_fd = -1;
	int ret, code;
	struct sockaddr_in addr;
	struct io_uring_sqe *sqe;
	struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 100000};
	struct stat sb;

	/*
	 * Test reliably fails if syncookies isn't enabled
	 */
	if (stat("/proc/sys/net/ipv4/tcp_syncookies", &sb) < 0)
		return T_EXIT_SKIP;

	connect_fd[0] = create_socket();
	if (connect_fd[0] == -1)
		return -1;

	connect_fd[1] = create_socket();
	if (connect_fd[1] == -1)
		goto err;

	accept_fd = create_socket();
	if (accept_fd == -1)
		goto err;

	if (configure_connect(connect_fd[0], &addr) == -1)
		goto err;

	if (configure_connect(connect_fd[1], &addr) == -1)
		goto err;

	ret = bind(accept_fd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret == -1) {
		perror("bind()");
		goto err;
	}

	ret = listen(accept_fd, 0);  // no backlog in order to block connect_fd[1]
	if (ret == -1) {
		perror("listen()");
		goto err;
	}

	// We first connect with one client socket in order to fill the accept queue.
	ret = connect_socket(ring, connect_fd[0], &code);
	if (ret == -1 || code != 0) {
		fprintf(stderr, "unable to connect\n");
		goto err;
	}

	// We do not offload completion events from listening socket on purpose. 
	// This way we create a state where the second connect request being stalled by OS.  
	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "unable to get sqe\n");
		goto err;
	}

	io_uring_prep_connect(sqe, connect_fd[1], (struct sockaddr*)&addr, sizeof(addr));
	sqe->user_data = 1;
	sqe->flags |= IOSQE_IO_LINK;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "unable to get sqe\n");
		goto err;
	}
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(ring);
	if (ret != 2) {
		fprintf(stderr, "submitted %d\n", ret);
		return -1;
	}

	for (int i = 0; i < 2; i++) {
		int expected;
		struct io_uring_cqe *cqe;

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe=%d\n", ret);
			return -1;
		}

		expected = (cqe->user_data == 1) ? -ECANCELED : -ETIME;
		if (expected != cqe->res) {
			fprintf(stderr, "cqe %d, res %d, wanted %d\n", 
					(int)cqe->user_data, cqe->res, expected);
			goto err;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	close(connect_fd[0]);
	close(connect_fd[1]);
	close(accept_fd);
	return 0;

err:
	if (connect_fd[0] != -1)
		close(connect_fd[0]);
	if (connect_fd[1] != -1)
		close(connect_fd[1]);
		
	if (accept_fd != -1)
		close(accept_fd);
	return -1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "io_uring_queue_setup() = %d\n", ret);
		return T_EXIT_FAIL;
	}

	srand(getpid());
	use_port = (rand() % 61440) + 4096;
	use_port = htons(use_port);
	use_addr = inet_addr("127.0.0.1");

	ret = test_connect_with_no_peer(&ring);
	if (ret == -1) {
		fprintf(stderr, "test_connect_with_no_peer(): failed\n");
		return T_EXIT_FAIL;
	}
	if (no_connect)
		return T_EXIT_SKIP;

	ret = test_connect(&ring);
	if (ret == -1) {
		fprintf(stderr, "test_connect(): failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_connect_timeout(&ring);
	if (ret == -1) {
		fprintf(stderr, "test_connect_timeout(): failed\n");
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
}
