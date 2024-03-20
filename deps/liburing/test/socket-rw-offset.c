/* SPDX-License-Identifier: MIT */
/*
 * Check that a readv on a socket queued before a writev doesn't hang
 * the processing.
 *
 * From Hrvoje Zeba <zeba.hrvoje@gmail.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

int main(int argc, char *argv[])
{
	int p_fd[2], ret;
	int32_t recv_s0;
	int32_t val = 1;
	struct sockaddr_in addr;
	struct iovec iov_r[1], iov_w[1];

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

	while (1) {
		int32_t code;
		socklen_t code_len = sizeof(code);

		ret = getsockopt(p_fd[1], SOL_SOCKET, SO_ERROR, &code, &code_len);
		assert(ret != -1);

		if (!code)
			break;
	}

	struct io_uring m_io_uring;
	struct io_uring_params p = { };

	ret = io_uring_queue_init_params(32, &m_io_uring, &p);
	assert(ret >= 0);

	/* skip for kernels without cur position read/write */
	if (!(p.features & IORING_FEAT_RW_CUR_POS))
		return 0;

	char recv_buff[128];
	char send_buff[128];

	{
		iov_r[0].iov_base = recv_buff;
		iov_r[0].iov_len = sizeof(recv_buff);

		struct io_uring_sqe* sqe = io_uring_get_sqe(&m_io_uring);
		assert(sqe != NULL);

		io_uring_prep_readv(sqe, p_fd[0], iov_r, 1, -1);
	}

	{
		iov_w[0].iov_base = send_buff;
		iov_w[0].iov_len = sizeof(send_buff);

		struct io_uring_sqe* sqe = io_uring_get_sqe(&m_io_uring);
		assert(sqe != NULL);

		io_uring_prep_writev(sqe, p_fd[1], iov_w, 1, 0);
	}

	ret = io_uring_submit_and_wait(&m_io_uring, 2);
	assert(ret != -1);

	struct io_uring_cqe* cqe;
	uint32_t head;
	uint32_t count = 0;

	ret = 0;
	while (count != 2) {
		io_uring_for_each_cqe(&m_io_uring, head, cqe) {
			if (cqe->res != 128) {
				fprintf(stderr, "Got %d, expected 128\n", cqe->res);
				ret = 1;
				goto err;
			}
			assert(cqe->res == 128);
			count++;
		}

		assert(count <= 2);
		io_uring_cq_advance(&m_io_uring, count);
	}

err:
	io_uring_queue_exit(&m_io_uring);
	return ret;
}
