/* SPDX-License-Identifier: MIT */
/*
 * Check that writev on a socket that has been shutdown(2) fails
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "liburing.h"
#include "helpers.h"

static void sig_pipe(int sig)
{
}

int main(int argc, char *argv[])
{
	int p_fd[2], ret;
	int32_t recv_s0;
	int32_t val = 1;
	struct sockaddr_in addr = { };

	if (argc > 1)
		return 0;

	srand(getpid());

	recv_s0 = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);

	ret = setsockopt(recv_s0, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
	assert(ret != -1);
	ret = setsockopt(recv_s0, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	assert(ret != -1);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	ret = t_bind_ephemeral_port(recv_s0, &addr);
	assert(!ret);
	ret = listen(recv_s0, 128);
	assert(ret != -1);

	p_fd[1] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);

	val = 1;
	ret = setsockopt(p_fd[1], IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
	assert(ret != -1);

	int32_t flags = fcntl(p_fd[1], F_GETFL, 0);
	assert(flags != -1);

	flags |= O_NONBLOCK;
	ret = fcntl(p_fd[1], F_SETFL, flags);
	assert(ret != -1);

	ret = connect(p_fd[1], (struct sockaddr*)&addr, sizeof(addr));
	assert(ret == -1);

	flags = fcntl(p_fd[1], F_GETFL, 0);
	assert(flags != -1);

	flags &= ~O_NONBLOCK;
	ret = fcntl(p_fd[1], F_SETFL, flags);
	assert(ret != -1);

	p_fd[0] = accept(recv_s0, NULL, NULL);
	assert(p_fd[0] != -1);

	signal(SIGPIPE, sig_pipe);

	while (1) {
		int32_t code;
		socklen_t code_len = sizeof(code);

		ret = getsockopt(p_fd[1], SOL_SOCKET, SO_ERROR, &code, &code_len);
		assert(ret != -1);

		if (!code)
			break;
	}

	struct io_uring m_io_uring;

	ret = io_uring_queue_init(32, &m_io_uring, 0);
	assert(ret >= 0);

	{
		struct io_uring_cqe *cqe;
		struct io_uring_sqe *sqe;
		int res;

		sqe = io_uring_get_sqe(&m_io_uring);
		io_uring_prep_shutdown(sqe, p_fd[1], SHUT_WR);
		sqe->user_data = 1;

		res = io_uring_submit_and_wait(&m_io_uring, 1);
		assert(res != -1);

		res = io_uring_wait_cqe(&m_io_uring, &cqe);
		if (res < 0) {
			fprintf(stderr, "wait: %s\n", strerror(-ret));
			goto err;
		}

		if (cqe->res) {
			if (cqe->res == -EINVAL) {
				fprintf(stdout, "Shutdown not supported, skipping\n");
				goto done;
			}
			fprintf(stderr, "writev: %d\n", cqe->res);
			goto err;
		}

		io_uring_cqe_seen(&m_io_uring, cqe);
	}

	{
		struct io_uring_cqe *cqe;
		struct io_uring_sqe *sqe;
		struct iovec iov[1];
		char send_buff[128];
		int res;

		iov[0].iov_base = send_buff;
		iov[0].iov_len = sizeof(send_buff);

		sqe = io_uring_get_sqe(&m_io_uring);
		assert(sqe != NULL);

		io_uring_prep_writev(sqe, p_fd[1], iov, 1, 0);
		res = io_uring_submit_and_wait(&m_io_uring, 1);
		assert(res != -1);

		res = io_uring_wait_cqe(&m_io_uring, &cqe);
		if (res < 0) {
			fprintf(stderr, "wait: %s\n", strerror(-ret));
			goto err;
		}

		if (cqe->res != -EPIPE) {
			fprintf(stderr, "writev: %d\n", cqe->res);
			goto err;
		}
		io_uring_cqe_seen(&m_io_uring, cqe);
	}

done:
	io_uring_queue_exit(&m_io_uring);
	return 0;
err:
	io_uring_queue_exit(&m_io_uring);
	return 1;
}
