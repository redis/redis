/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <poll.h>
#include <arpa/inet.h>

#include "liburing.h"
#include "helpers.h"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static int recv_thread_ready = 0;
static int recv_thread_done = 0;

static void signal_var(int *var)
{
        pthread_mutex_lock(&mutex);
        *var = 1;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
}

static void wait_for_var(int *var)
{
        pthread_mutex_lock(&mutex);

        while (!*var)
                pthread_cond_wait(&cond, &mutex);

        pthread_mutex_unlock(&mutex);
}

struct data {
	unsigned expected[2];
	unsigned just_positive[2];
	unsigned long timeout;
	unsigned short port;
	unsigned int addr;
	int stop;
};

static void *send_thread(void *arg)
{
	struct data *data = arg;
	int ret;

	wait_for_var(&recv_thread_ready);

	if (data->stop)
		return NULL;

	int s0 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert(s0 != -1);

	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	addr.sin_port = data->port;
	addr.sin_addr.s_addr = data->addr;

	ret = connect(s0, (struct sockaddr*)&addr, sizeof(addr));
	assert(ret != -1);

	wait_for_var(&recv_thread_done);

	close(s0);
	return NULL;
}

static void *recv_thread(void *arg)
{
	struct data *data = arg;
	struct io_uring ring;
	int i, ret;

	ret = io_uring_queue_init(8, &ring, 0);
	assert(ret == 0);

	int s0 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert(s0 != -1);

	int32_t val = 1;
	ret = setsockopt(s0, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
	assert(ret != -1);
	ret = setsockopt(s0, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	assert(ret != -1);

	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	data->addr = inet_addr("127.0.0.1");
	addr.sin_addr.s_addr = data->addr;

	i = 0;
	do {
		data->port = htons(1025 + (rand() % 64510));
		addr.sin_port = data->port;

		if (bind(s0, (struct sockaddr*)&addr, sizeof(addr)) != -1)
			break;
	} while (++i < 100);

	if (i >= 100) {
		printf("Can't find good port, skipped\n");
		data->stop = 1;
		signal_var(&recv_thread_ready);
		goto out;
	}

	ret = listen(s0, 128);
	assert(ret != -1);

	signal_var(&recv_thread_ready);

	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&ring);
	assert(sqe != NULL);

	io_uring_prep_accept(sqe, s0, NULL, NULL, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	sqe = io_uring_get_sqe(&ring);
	assert(sqe != NULL);

	struct __kernel_timespec ts;
	ts.tv_sec = data->timeout / 1000000000;
	ts.tv_nsec = data->timeout % 1000000000;
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 2;

	ret = io_uring_submit(&ring);
	assert(ret == 2);

	for (i = 0; i < 2; i++) {
		struct io_uring_cqe *cqe;
		int idx;

		if (io_uring_wait_cqe(&ring, &cqe)) {
			fprintf(stderr, "wait cqe failed\n");
			goto err;
		}
		idx = cqe->user_data - 1;
		if (cqe->res != data->expected[idx]) {
			if (cqe->res > 0 && data->just_positive[idx])
				goto ok;
			if (cqe->res == -EBADF) {
				fprintf(stdout, "Accept not supported, skipping\n");
				data->stop = 1;
				goto out;
			}
			fprintf(stderr, "cqe %" PRIu64 " got %d, wanted %d\n",
					(uint64_t) cqe->user_data, cqe->res,
					data->expected[idx]);
			goto err;
		}
ok:
		if (cqe->user_data == 1 && cqe->res > 0)
			close(cqe->res);

		io_uring_cqe_seen(&ring, cqe);
	}

	signal_var(&recv_thread_done);

out:
	close(s0);
	return NULL;
err:
	close(s0);
	return (void *) 1;
}

static int test_accept_timeout(int do_connect, unsigned long timeout)
{
	struct io_uring ring;
	struct io_uring_params p = {};
	pthread_t t1, t2;
	struct data d;
	void *tret;
	int ret, fast_poll;

	ret = io_uring_queue_init_params(1, &ring, &p);
	if (ret) {
		fprintf(stderr, "queue_init: %d\n", ret);
		return 1;
	}

	fast_poll = (p.features & IORING_FEAT_FAST_POLL) != 0;
	io_uring_queue_exit(&ring);

	recv_thread_ready = 0;
	recv_thread_done = 0;

	memset(&d, 0, sizeof(d));
	d.timeout = timeout;
	if (!do_connect) {
		if (fast_poll) {
			d.expected[0] = -ECANCELED;
			d.expected[1] = -ETIME;
		} else {
			d.expected[0] = -EINTR;
			d.expected[1] = -EALREADY;
		}
	} else {
		d.expected[0] = -1U;
		d.just_positive[0] = 1;
		d.expected[1] = -ECANCELED;
	}

	pthread_create(&t1, NULL, recv_thread, &d);

	if (do_connect)
		pthread_create(&t2, NULL, send_thread, &d);

	pthread_join(t1, &tret);
	if (tret)
		ret++;

	if (do_connect) {
		pthread_join(t2, &tret);
		if (tret)
			ret++;
	}

	return ret;
}

int main(int argc, char *argv[])
{
	if (argc > 1)
		return T_EXIT_SKIP;
	if (test_accept_timeout(0, 200000000)) {
		fprintf(stderr, "accept timeout 0 failed\n");
		return T_EXIT_FAIL;
	}

	if (test_accept_timeout(1, 1000000000)) {
		fprintf(stderr, "accept and connect timeout 0 failed\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
