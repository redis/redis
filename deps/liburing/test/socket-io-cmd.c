/* SPDX-License-Identifier: MIT */
/*
 * Check that CMD operations on sockets are consistent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>

#include "liburing.h"
#include "helpers.h"

#define USERDATA 0x1234
#define MSG "foobarbaz"

struct fds {
	int tx;
	int rx;
};

/* Create 2 sockets (tx, rx) given the socket type */
static struct fds create_sockets(bool stream)
{
	struct fds retval;
	int fd[2];

	t_create_socket_pair(fd, stream);

	retval.tx = fd[0];
	retval.rx = fd[1];

	return retval;
}

static int create_sqe_and_submit(struct io_uring *ring, int32_t fd, int op)
{
	struct io_uring_sqe *sqe;
	int ret;

	assert(fd > 0);
	sqe = io_uring_get_sqe(ring);
	assert(sqe != NULL);

	io_uring_prep_cmd_sock(sqe, op, fd, 0, 0, NULL, 0);
	sqe->user_data = USERDATA;

	/* Submitting SQE */
	ret = io_uring_submit_and_wait(ring, 1);
	if (ret <= 0)
		return ret;

	return 0;
}

static int receive_cqe(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int err;

	err = io_uring_wait_cqe(ring, &cqe);
	assert(err ==  0);
	assert(cqe->user_data == USERDATA);
	io_uring_cqe_seen(ring, cqe);

	/* Return the result of the operation */
	return cqe->res;
}

static ssize_t send_data(struct fds *s, char *str)
{
	size_t written_bytes;

	written_bytes = write(s->tx, str, strlen(str));
	assert(written_bytes == strlen(MSG));

	return written_bytes;
}

static int run_test(bool stream)
{
	struct fds sockfds;
	size_t bytes_in, bytes_out;
	struct io_uring ring;
	size_t written_bytes;
	int error;

	/* Create three sockets */
	sockfds = create_sockets(stream);
	assert(sockfds.tx > 0);
	assert(sockfds.rx > 0);
	/* Send data sing the sockfds->send */
	written_bytes = send_data(&sockfds, MSG);

	/* Simply io_uring ring creation */
	error = t_create_ring(1, &ring, 0);
	if (error == T_SETUP_SKIP)
		return error;
	else if (error != T_SETUP_OK)
		return T_EXIT_FAIL;

	error = create_sqe_and_submit(&ring, sockfds.rx,
				      SOCKET_URING_OP_SIOCINQ);
	bytes_in = receive_cqe(&ring);
	if (error)
		return T_EXIT_FAIL;

	error = create_sqe_and_submit(&ring, sockfds.tx,
				      SOCKET_URING_OP_SIOCOUTQ);
	if (error)
		return T_EXIT_FAIL;

	bytes_out = receive_cqe(&ring);
	if (bytes_in == -ENOTSUP || bytes_out == -ENOTSUP) {
		fprintf(stderr, "Skipping tests. -ENOTSUP returned\n");
		return T_EXIT_SKIP;
	}

	/*
	 * Assert the number of written bytes are either in the socket buffer
	 * or on the receive side
	 */
	if (bytes_in + bytes_out != written_bytes) {
		fprintf(stderr, "values does not match: %zu+%zu != %zu\n",
			bytes_in, bytes_out, written_bytes);
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&ring);

	return T_EXIT_PASS;
}

/*
 * Make sure that siocoutq and siocinq returns the same value
 * using ioctl(2) and uring commands for raw sockets
 */
static int run_test_raw(void)
{
	int ioctl_siocoutq, ioctl_siocinq;
	int uring_siocoutq, uring_siocinq;
	struct io_uring ring;
	int sock, error;

	sock = socket(PF_INET, SOCK_RAW, IPPROTO_TCP);
	if (sock == -1)  {
		/* You need root to create raw socket */
		perror("Not able to create a raw socket");
		return T_EXIT_SKIP;
	}

	/* Simple SIOCOUTQ using ioctl */
	error = ioctl(sock, SIOCOUTQ, &ioctl_siocoutq);
	if (error < 0) {
		fprintf(stderr, "Failed to run ioctl(SIOCOUTQ): %d\n", error);
		return T_EXIT_FAIL;
	}

	error = ioctl(sock, SIOCINQ, &ioctl_siocinq);
	if (error < 0) {
		fprintf(stderr, "Failed to run ioctl(SIOCINQ): %d\n", error);
		return T_EXIT_FAIL;
	}

	/* Get the same operation using uring cmd */
	error = t_create_ring(1, &ring, 0);
	if (error == T_SETUP_SKIP)
		return error;
	else if (error != T_SETUP_OK)
		return T_EXIT_FAIL;

	create_sqe_and_submit(&ring, sock, SOCKET_URING_OP_SIOCOUTQ);
	uring_siocoutq = receive_cqe(&ring);

	create_sqe_and_submit(&ring, sock, SOCKET_URING_OP_SIOCINQ);
	uring_siocinq = receive_cqe(&ring);

	/* Compare that both values (ioctl and uring CMD) should be similar */
	if (uring_siocoutq != ioctl_siocoutq) {
		fprintf(stderr, "values does not match: %d != %d\n",
			uring_siocoutq, ioctl_siocoutq);
		return T_EXIT_FAIL;
	}
	if (uring_siocinq != ioctl_siocinq) {
		fprintf(stderr, "values does not match: %d != %d\n",
			uring_siocinq, ioctl_siocinq);
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	int err;

	if (argc > 1)
		return 0;

	/* Test SOCK_STREAM */
	err = run_test(true);
	if (err)
		return err;

	/* Test SOCK_DGRAM */
	err = run_test(false);
	if (err)
		return err;

	/* Test raw sockets */
	return run_test_raw();
}
