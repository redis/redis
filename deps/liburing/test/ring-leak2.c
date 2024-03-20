/* SPDX-License-Identifier: MIT */
/*
 * Description: Test two ring deadlock. A buggy kernel will end up
 * 		having io_wq_* workers pending, as the circular reference
 * 		will prevent full exit.
 *
 * Based on a test case from Josef <josef.grieb@gmail.com>
 *
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <pthread.h>

#include "liburing.h"
#include "../src/syscall.h"

enum {
	ACCEPT,
	READ,
	WRITE,
	POLLING_IN,
	POLLING_RDHUP,
	CLOSE,
	EVENTFD_READ,
};

typedef struct conn_info {
	__u32 fd;
	__u16 type;
	__u16 bid;
} conn_info;

static char read_eventfd_buffer[8];

static pthread_mutex_t lock;
static struct io_uring *client_ring;

static int client_eventfd = -1;

static int setup_io_uring(struct io_uring *ring)
{
	struct io_uring_params p = { };
	int ret;

	ret = io_uring_queue_init_params(8, ring, &p);
	if (ret) {
		fprintf(stderr, "Unable to setup io_uring: %s\n",
			strerror(-ret));
		return 1;
	}
	return 0;
}

static void add_socket_eventfd_read(struct io_uring *ring, int fd)
{
	struct io_uring_sqe *sqe;
	conn_info conn_i = {
		.fd = fd,
		.type = EVENTFD_READ,
	};

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, fd, &read_eventfd_buffer, 8, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);

	memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

static void add_socket_pollin(struct io_uring *ring, int fd)
{
	struct io_uring_sqe *sqe;
	conn_info conn_i = {
		.fd = fd,
		.type = POLLING_IN,
	};

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_poll_add(sqe, fd, POLL_IN);

	memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

static void *server_thread(void *arg)
{
	struct sockaddr_in serv_addr;
	int port = 0;
	int sock_listen_fd, evfd;
	const int val = 1;
	struct io_uring ring;
       
	sock_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	serv_addr.sin_addr.s_addr = INADDR_ANY;

	evfd = eventfd(0, EFD_CLOEXEC);

	// bind and listen
	if (bind(sock_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		perror("Error binding socket...\n");
		exit(1);
	}
	if (listen(sock_listen_fd, 1) < 0) {
		perror("Error listening on socket...\n");
		exit(1);
	}

	setup_io_uring(&ring);
	add_socket_eventfd_read(&ring, evfd);
	add_socket_pollin(&ring, sock_listen_fd);

	while (1) {
		struct io_uring_cqe *cqe;
		unsigned head;
		unsigned count = 0;

		io_uring_submit_and_wait(&ring, 1);

		io_uring_for_each_cqe(&ring, head, cqe) {
			struct conn_info conn_i;

			count++;
			memcpy(&conn_i, &cqe->user_data, sizeof(conn_i));

			if (conn_i.type == ACCEPT) {
				int sock_conn_fd = cqe->res;
				// only read when there is no error, >= 0
				if (sock_conn_fd > 0) {
					add_socket_pollin(&ring, sock_listen_fd);

					pthread_mutex_lock(&lock);
					io_uring_submit(client_ring);
					pthread_mutex_unlock(&lock);

				}
			} else if (conn_i.type == POLLING_IN) {
				break;
			}
		}
		io_uring_cq_advance(&ring, count);
	}
}

static void *client_thread(void *arg)
{
	struct io_uring ring;
	int ret;

	setup_io_uring(&ring);
	client_ring = &ring;

	client_eventfd = eventfd(0, EFD_CLOEXEC);
	pthread_mutex_lock(&lock);
	add_socket_eventfd_read(&ring, client_eventfd);
	pthread_mutex_unlock(&lock);

	while (1) {
		struct io_uring_cqe *cqe;
		unsigned head;
		unsigned count = 0;

		pthread_mutex_lock(&lock);
		io_uring_submit(&ring);
		pthread_mutex_unlock(&lock);

		ret = __sys_io_uring_enter(ring.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
		if (ret < 0) {
			perror("Error io_uring_enter...\n");
			exit(1);
		}

		// go through all CQEs
		io_uring_for_each_cqe(&ring, head, cqe) {
			struct conn_info conn_i;
			int type;

			count++;
			memcpy(&conn_i, &cqe->user_data, sizeof(conn_i));

			type = conn_i.type;
			if (type == READ) {
				pthread_mutex_lock(&lock);

				if (cqe->res <= 0) {
					// connection closed or error
					shutdown(conn_i.fd, SHUT_RDWR);
				} else {
					pthread_mutex_unlock(&lock);
					break;
				}
				add_socket_pollin(&ring, conn_i.fd);
				pthread_mutex_unlock(&lock);
			} else if (type == WRITE) {
			} else if (type == POLLING_IN) {
				break;
			} else if (type == POLLING_RDHUP) {
				break;
			} else if (type == CLOSE) {
			} else if (type == EVENTFD_READ) {
				add_socket_eventfd_read(&ring, client_eventfd);
			}
		}

		io_uring_cq_advance(&ring, count);
	}
}

static void sig_alrm(int sig)
{
	exit(0);
}

int main(int argc, char *argv[])
{
	pthread_t server_thread_t, client_thread_t;
	struct sigaction act;

	if (argc > 1)
		return 0;

	if (pthread_mutex_init(&lock, NULL) != 0) {
		printf("\n mutex init failed\n");
		return 1;
	}

	pthread_create(&server_thread_t, NULL, &server_thread, NULL);
	pthread_create(&client_thread_t, NULL, &client_thread, NULL);

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_alrm;
	act.sa_flags = SA_RESTART;
	sigaction(SIGALRM, &act, NULL);
	alarm(1);

	pthread_join(server_thread_t, NULL);
	return 0;
}
