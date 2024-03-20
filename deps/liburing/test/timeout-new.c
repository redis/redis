/* SPDX-License-Identifier: MIT */
/*
 * Description: tests for getevents timeout
 *
 */
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include "liburing.h"

#define TIMEOUT_MSEC	200
#define TIMEOUT_SEC	10

static int thread_ret0, thread_ret1;
static int cnt = 0;
static pthread_mutex_t mutex;

static void msec_to_ts(struct __kernel_timespec *ts, unsigned int msec)
{
	ts->tv_sec = msec / 1000;
	ts->tv_nsec = (msec % 1000) * 1000000;
}

static unsigned long long mtime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000;
	usec /= 1000;
	return sec + usec;
}

static unsigned long long mtime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return mtime_since(tv, &end);
}


static int test_return_before_timeout(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret;
	bool retried = false;
	struct __kernel_timespec ts;

	msec_to_ts(&ts, TIMEOUT_MSEC);

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		return 1;
	}

again:
	ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
	if (ret == -ETIME && (ring->flags & IORING_SETUP_SQPOLL) && !retried) {
		/*
		 * there is a small chance SQPOLL hasn't been waked up yet,
		 * give it one more try.
		 */
		printf("warning: funky SQPOLL timing\n");
		sleep(1);
		retried = true;
		goto again;
	} else if (ret < 0) {
		fprintf(stderr, "%s: timeout error: %d\n", __FUNCTION__, ret);
		return 1;
	}
	io_uring_cqe_seen(ring, cqe);
	return 0;
}

static int test_return_after_timeout(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	int ret;
	struct __kernel_timespec ts;
	struct timeval tv;
	unsigned long long exp;

	msec_to_ts(&ts, TIMEOUT_MSEC);
	gettimeofday(&tv, NULL);
	ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
	exp = mtime_since_now(&tv);
	if (ret != -ETIME) {
		fprintf(stderr, "%s: timeout error: %d\n", __FUNCTION__, ret);
		return 1;
	}

	if (exp < TIMEOUT_MSEC / 2 || exp > (TIMEOUT_MSEC  * 3) / 2) {
		fprintf(stderr, "%s: Timeout seems wonky (got %llu)\n", __FUNCTION__, exp);
		return 1;
	}

	return 0;
}

static int __reap_thread_fn(void *data)
{
	struct io_uring *ring = (struct io_uring *)data;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;

	msec_to_ts(&ts, TIMEOUT_SEC);
	pthread_mutex_lock(&mutex);
	cnt++;
	pthread_mutex_unlock(&mutex);
	return io_uring_wait_cqe_timeout(ring, &cqe, &ts);
}

static void *reap_thread_fn0(void *data)
{
	thread_ret0 = __reap_thread_fn(data);
	return NULL;
}

static void *reap_thread_fn1(void *data)
{
	thread_ret1 = __reap_thread_fn(data);
	return NULL;
}

/*
 * This is to test issuing a sqe in main thread and reaping it in two child-thread
 * at the same time. To see if timeout feature works or not.
 */
static int test_multi_threads_timeout(void)
{
	struct io_uring ring;
	int ret;
	bool both_wait = false;
	pthread_t reap_thread0, reap_thread1;
	struct io_uring_sqe *sqe;

	ret = io_uring_queue_init(8, &ring, 0);
	if (ret) {
		fprintf(stderr, "%s: ring setup failed: %d\n", __FUNCTION__, ret);
		return 1;
	}

	pthread_create(&reap_thread0, NULL, reap_thread_fn0, &ring);
	pthread_create(&reap_thread1, NULL, reap_thread_fn1, &ring);

	/*
	 * make two threads both enter io_uring_wait_cqe_timeout() before issuing the sqe
	 * as possible as we can. So that there are two threads in the ctx->wait queue.
	 * In this way, we can test if a cqe wakes up two threads at the same time.
	 */
	while(!both_wait) {
		pthread_mutex_lock(&mutex);
		if (cnt == 2)
			both_wait = true;
		pthread_mutex_unlock(&mutex);
		sleep(1);
	}

	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {
		fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
		goto err;
	}

	io_uring_prep_nop(sqe);

	ret = io_uring_submit(&ring);
	if (ret <= 0) {
		fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		goto err;
	}

	pthread_join(reap_thread0, NULL);
	pthread_join(reap_thread1, NULL);

	if ((thread_ret0 && thread_ret0 != -ETIME) || (thread_ret1 && thread_ret1 != -ETIME)) {
		fprintf(stderr, "%s: thread wait cqe timeout failed: %d %d\n",
				__FUNCTION__, thread_ret0, thread_ret1);
		goto err;
	}

	return 0;
err:
	return 1;
}

int main(int argc, char *argv[])
{
	struct io_uring ring_normal, ring_sq;
	int ret;

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init(8, &ring_normal, 0);
	if (ret) {
		fprintf(stderr, "ring_normal setup failed: %d\n", ret);
		return 1;
	}
	if (!(ring_normal.features & IORING_FEAT_EXT_ARG)) {
		fprintf(stderr, "feature IORING_FEAT_EXT_ARG not supported, skipping.\n");
		return 0;
	}

	ret = test_return_before_timeout(&ring_normal);
	if (ret) {
		fprintf(stderr, "ring_normal: test_return_before_timeout failed\n");
		return ret;
	}

	ret = test_return_after_timeout(&ring_normal);
	if (ret) {
		fprintf(stderr, "ring_normal: test_return_after_timeout failed\n");
		return ret;
	}

	ret = io_uring_queue_init(8, &ring_sq, IORING_SETUP_SQPOLL);
	if (ret) {
		fprintf(stderr, "ring_sq setup failed: %d\n", ret);
		return 1;
	}

	ret = test_return_before_timeout(&ring_sq);
	if (ret) {
		fprintf(stderr, "ring_sq: test_return_before_timeout failed\n");
		return ret;
	}

	ret = test_return_after_timeout(&ring_sq);
	if (ret) {
		fprintf(stderr, "ring_sq: test_return_after_timeout failed\n");
		return ret;
	}

	ret = test_multi_threads_timeout();
	if (ret) {
		fprintf(stderr, "test_multi_threads_timeout failed\n");
		return ret;
	}

	return 0;
}
