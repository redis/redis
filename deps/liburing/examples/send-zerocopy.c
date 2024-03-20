/* SPDX-License-Identifier: MIT */
/* based on linux-kernel/tools/testing/selftests/net/msg_zerocopy.c */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

#include <poll.h>
#include <sched.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/ipv6.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <linux/mman.h>

#include "liburing.h"

#define ZC_TAG 0xfffffffULL
#define MAX_SUBMIT_NR 512
#define MAX_THREADS 100

struct thread_data {
	pthread_t thread;
	void *ret;
	int idx;
	unsigned long long packets;
	unsigned long long bytes;
	struct sockaddr_storage dst_addr;
	int fd;
};

static bool cfg_reg_ringfd = true;
static bool cfg_fixed_files = 1;
static bool cfg_zc = 1;
static int  cfg_nr_reqs = 8;
static bool cfg_fixed_buf = 1;
static bool cfg_hugetlb = 0;
static bool cfg_defer_taskrun = 0;
static int  cfg_cpu = -1;
static bool cfg_rx = 0;
static unsigned  cfg_nr_threads = 1;

static int  cfg_family		= PF_UNSPEC;
static int  cfg_type		= 0;
static int  cfg_payload_len;
static int  cfg_port		= 8000;
static int  cfg_runtime_ms	= 4200;

static socklen_t cfg_alen;
static char *str_addr = NULL;

static char payload_buf[IP_MAXPACKET] __attribute__((aligned(4096)));
static char *payload;
static struct thread_data threads[MAX_THREADS];
static pthread_barrier_t barrier;

/*
 * Implementation of error(3), prints an error message and exits.
 */
static void t_error(int status, int errnum, const char *format, ...)
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

static void set_cpu_affinity(void)
{
	cpu_set_t mask;

	if (cfg_cpu == -1)
		return;

	CPU_ZERO(&mask);
	CPU_SET(cfg_cpu, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask))
		t_error(1, errno, "unable to pin cpu\n");
}

static void set_iowq_affinity(struct io_uring *ring)
{
	cpu_set_t mask;
	int ret;

	if (cfg_cpu == -1)
		return;

	ret = io_uring_register_iowq_aff(ring, 1, &mask);
	if (ret)
		t_error(1, ret, "unabled to set io-wq affinity\n");
}

static unsigned long gettimeofday_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

static void do_setsockopt(int fd, int level, int optname, int val)
{
	if (setsockopt(fd, level, optname, &val, sizeof(val)))
		t_error(1, errno, "setsockopt %d.%d: %d", level, optname, val);
}

static void setup_sockaddr(int domain, const char *str_addr,
			   struct sockaddr_storage *sockaddr)
{
	struct sockaddr_in6 *addr6 = (void *) sockaddr;
	struct sockaddr_in *addr4 = (void *) sockaddr;
	int port = cfg_port;

	switch (domain) {
	case PF_INET:
		memset(addr4, 0, sizeof(*addr4));
		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(port);
		if (str_addr &&
		    inet_pton(AF_INET, str_addr, &(addr4->sin_addr)) != 1)
			t_error(1, 0, "ipv4 parse error: %s", str_addr);
		break;
	case PF_INET6:
		memset(addr6, 0, sizeof(*addr6));
		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(port);
		if (str_addr &&
		    inet_pton(AF_INET6, str_addr, &(addr6->sin6_addr)) != 1)
			t_error(1, 0, "ipv6 parse error: %s", str_addr);
		break;
	default:
		t_error(1, 0, "illegal domain");
	}
}

static int do_poll(int fd, int events)
{
	struct pollfd pfd;
	int ret;

	pfd.events = events;
	pfd.revents = 0;
	pfd.fd = fd;

	ret = poll(&pfd, 1, -1);
	if (ret == -1)
		t_error(1, errno, "poll");

	return ret && (pfd.revents & events);
}

/* Flush all outstanding bytes for the tcp receive queue */
static int do_flush_tcp(struct thread_data *td, int fd)
{
	int ret;

	/* MSG_TRUNC flushes up to len bytes */
	ret = recv(fd, NULL, 1 << 21, MSG_TRUNC | MSG_DONTWAIT);
	if (ret == -1 && errno == EAGAIN)
		return 0;
	if (ret == -1)
		t_error(1, errno, "flush");
	if (!ret)
		return 1;

	td->packets++;
	td->bytes += ret;
	return 0;
}

/* Flush all outstanding datagrams. Verify first few bytes of each. */
static int do_flush_datagram(struct thread_data *td, int fd)
{
	long ret, off = 0;
	char buf[64];

	/* MSG_TRUNC will return full datagram length */
	ret = recv(fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC);
	if (ret == -1 && errno == EAGAIN)
		return 0;

	if (ret == -1)
		t_error(1, errno, "recv");
	if (ret != cfg_payload_len)
		t_error(1, 0, "recv: ret=%u != %u", ret, cfg_payload_len);
	if ((unsigned long) ret > sizeof(buf) - off)
		ret = sizeof(buf) - off;
	if (memcmp(buf + off, payload, ret))
		t_error(1, 0, "recv: data mismatch");

	td->packets++;
	td->bytes += cfg_payload_len;
	return 0;
}

static void do_setup_rx(int domain, int type, int protocol)
{
	struct sockaddr_storage addr = {};
	struct thread_data *td;
	int listen_fd, fd;
	unsigned int i;

	fd = socket(domain, type, protocol);
	if (fd == -1)
		t_error(1, errno, "socket r");

	do_setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, 1);

	setup_sockaddr(cfg_family, str_addr, &addr);

	if (bind(fd, (void *)&addr, cfg_alen))
		t_error(1, errno, "bind");

	if (type != SOCK_STREAM) {
		if (cfg_nr_threads != 1)
			t_error(1, 0, "udp rx cant multithread");
		threads[0].fd = fd;
		return;
	}

	listen_fd = fd;
	if (listen(listen_fd, cfg_nr_threads))
		t_error(1, errno, "listen");

	for (i = 0; i < cfg_nr_threads; i++) {
		td = &threads[i];

		fd = accept(listen_fd, NULL, NULL);
		if (fd == -1)
			t_error(1, errno, "accept");
		td->fd = fd;
	}

	if (close(listen_fd))
		t_error(1, errno, "close listen sock");
}

static void *do_rx(void *arg)
{
	struct thread_data *td = arg;
	const int cfg_receiver_wait_ms = 400;
	uint64_t tstop;
	int ret, fd = td->fd;

	tstop = gettimeofday_ms() + cfg_runtime_ms + cfg_receiver_wait_ms;
	do {
		if (cfg_type == SOCK_STREAM)
			ret = do_flush_tcp(td, fd);
		else
			ret = do_flush_datagram(td, fd);

		if (ret)
			break;

		do_poll(fd, POLLIN);
	} while (gettimeofday_ms() < tstop);

	if (close(fd))
		t_error(1, errno, "close");
	pthread_exit(&td->ret);
	return NULL;
}

static inline struct io_uring_cqe *wait_cqe_fast(struct io_uring *ring)
{
	struct io_uring_cqe *cqe;
	unsigned head;
	int ret;

	io_uring_for_each_cqe(ring, head, cqe)
		return cqe;

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret)
		t_error(1, ret, "wait cqe");
	return cqe;
}

static void do_tx(struct thread_data *td, int domain, int type, int protocol)
{
	const int notif_slack = 128;
	struct io_uring ring;
	struct iovec iov;
	uint64_t tstop;
	int i, fd, ret;
	int compl_cqes = 0;
	int ring_flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

	if (cfg_defer_taskrun)
		ring_flags |= IORING_SETUP_DEFER_TASKRUN;

	fd = socket(domain, type, protocol);
	if (fd == -1)
		t_error(1, errno, "socket t");

	if (connect(fd, (void *)&td->dst_addr, cfg_alen))
		t_error(1, errno, "connect, idx %i", td->idx);

	ret = io_uring_queue_init(512, &ring, ring_flags);
	if (ret)
		t_error(1, ret, "io_uring: queue init");

	set_cpu_affinity();
	set_iowq_affinity(&ring);

	if (cfg_fixed_files) {
		ret = io_uring_register_files(&ring, &fd, 1);
		if (ret < 0)
			t_error(1, ret, "io_uring: files registration");
	}
	if (cfg_reg_ringfd) {
		ret = io_uring_register_ring_fd(&ring);
		if (ret < 0)
			t_error(1, ret, "io_uring: io_uring_register_ring_fd");
	}

	iov.iov_base = payload;
	iov.iov_len = cfg_payload_len;

	ret = io_uring_register_buffers(&ring, &iov, 1);
	if (ret)
		t_error(1, ret, "io_uring: buffer registration");

	pthread_barrier_wait(&barrier);

	tstop = gettimeofday_ms() + cfg_runtime_ms;
	do {
		struct io_uring_sqe *sqe;
		struct io_uring_cqe *cqe;
		unsigned buf_idx = 0;
		unsigned msg_flags = MSG_WAITALL;

		for (i = 0; i < cfg_nr_reqs; i++) {
			sqe = io_uring_get_sqe(&ring);

			if (!cfg_zc)
				io_uring_prep_send(sqe, fd, payload,
						   cfg_payload_len, 0);
			else {
				io_uring_prep_send_zc(sqe, fd, payload,
						     cfg_payload_len, msg_flags, 0);
				if (cfg_fixed_buf) {
					sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
					sqe->buf_index = buf_idx;
				}
			}
			sqe->user_data = 1;
			if (cfg_fixed_files) {
				sqe->fd = 0;
				sqe->flags |= IOSQE_FIXED_FILE;
			}
		}

		if (cfg_defer_taskrun && compl_cqes >= notif_slack)
			ret = io_uring_submit_and_get_events(&ring);
		else
			ret = io_uring_submit(&ring);

		if (ret != cfg_nr_reqs)
			t_error(1, ret, "submit");

		for (i = 0; i < cfg_nr_reqs; i++) {
			cqe = wait_cqe_fast(&ring);

			if (cqe->flags & IORING_CQE_F_NOTIF) {
				if (cqe->flags & IORING_CQE_F_MORE)
					t_error(1, -EINVAL, "F_MORE notif");
				compl_cqes--;
				i--;
				io_uring_cqe_seen(&ring, cqe);
				continue;
			}
			if (cqe->flags & IORING_CQE_F_MORE)
				compl_cqes++;

			if (cqe->res >= 0) {
				td->packets++;
				td->bytes += cqe->res;
			} else if (cqe->res == -ECONNREFUSED || cqe->res == -EPIPE ||
				   cqe->res == -ECONNRESET) {
				fprintf(stderr, "Connection failure\n");
				goto out_fail;
			} else if (cqe->res != -EAGAIN) {
				t_error(1, cqe->res, "send failed");
			}
			io_uring_cqe_seen(&ring, cqe);
		}
	} while (gettimeofday_ms() < tstop);

out_fail:
	shutdown(fd, SHUT_RDWR);
	if (close(fd))
		t_error(1, errno, "close");

	while (compl_cqes) {
		struct io_uring_cqe *cqe = wait_cqe_fast(&ring);

		io_uring_cqe_seen(&ring, cqe);
		compl_cqes--;
	}
	io_uring_queue_exit(&ring);
}


static void *do_test(void *arg)
{
	struct thread_data *td = arg;
	int protocol = 0;

	setup_sockaddr(cfg_family, str_addr, &td->dst_addr);

	do_tx(td, cfg_family, cfg_type, protocol);
	pthread_exit(&td->ret);
	return NULL;
}

static void usage(const char *filepath)
{
	t_error(1, 0, "Usage: %s [-n<N>] [-z<val>] [-s<payload size>] "
		    "(-4|-6) [-t<time s>] -D<dst_ip> udp", filepath);
}

static void parse_opts(int argc, char **argv)
{
	const int max_payload_len = IP_MAXPACKET -
				    sizeof(struct ipv6hdr) -
				    sizeof(struct tcphdr) -
				    40 /* max tcp options */;
	int c;
	char *daddr = NULL;

	if (argc <= 1)
		usage(argv[0]);

	cfg_payload_len = max_payload_len;

	while ((c = getopt(argc, argv, "46D:p:s:t:n:z:b:l:dC:T:R")) != -1) {
		switch (c) {
		case '4':
			if (cfg_family != PF_UNSPEC)
				t_error(1, 0, "Pass one of -4 or -6");
			cfg_family = PF_INET;
			cfg_alen = sizeof(struct sockaddr_in);
			break;
		case '6':
			if (cfg_family != PF_UNSPEC)
				t_error(1, 0, "Pass one of -4 or -6");
			cfg_family = PF_INET6;
			cfg_alen = sizeof(struct sockaddr_in6);
			break;
		case 'D':
			daddr = optarg;
			break;
		case 'p':
			cfg_port = strtoul(optarg, NULL, 0);
			break;
		case 's':
			cfg_payload_len = strtoul(optarg, NULL, 0);
			break;
		case 't':
			cfg_runtime_ms = 200 + strtoul(optarg, NULL, 10) * 1000;
			break;
		case 'n':
			cfg_nr_reqs = strtoul(optarg, NULL, 0);
			break;
		case 'z':
			cfg_zc = strtoul(optarg, NULL, 0);
			break;
		case 'b':
			cfg_fixed_buf = strtoul(optarg, NULL, 0);
			break;
		case 'l':
			cfg_hugetlb = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			cfg_defer_taskrun = 1;
			break;
		case 'C':
			cfg_cpu = strtol(optarg, NULL, 0);
			break;
		case 'T':
			cfg_nr_threads = strtol(optarg, NULL, 0);
			if (cfg_nr_threads > MAX_THREADS)
				t_error(1, 0, "too many threads\n");
			break;
		case 'R':
			cfg_rx = 1;
			break;
		}
	}

	if (cfg_nr_reqs > MAX_SUBMIT_NR)
		t_error(1, 0, "-n: submit batch nr exceeds max (%d)", MAX_SUBMIT_NR);
	if (cfg_payload_len > max_payload_len)
		t_error(1, 0, "-s: payload exceeds max (%d)", max_payload_len);

	str_addr = daddr;

	if (optind != argc - 1)
		usage(argv[0]);
}

int main(int argc, char **argv)
{
	unsigned long long packets = 0, bytes = 0;
	struct thread_data *td;
	const char *cfg_test;
	unsigned int i;
	void *res;

	parse_opts(argc, argv);
	set_cpu_affinity();

	payload = payload_buf;
	if (cfg_hugetlb) {
		payload = mmap(NULL, 2*1024*1024, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_HUGETLB | MAP_HUGE_2MB | MAP_ANONYMOUS,
				-1, 0);
		if (payload == MAP_FAILED) {
			fprintf(stderr, "hugetlb alloc failed\n");
			return 1;
		}
	}

	cfg_test = argv[argc - 1];
	if (!strcmp(cfg_test, "tcp"))
		cfg_type = SOCK_STREAM;
	else if (!strcmp(cfg_test, "udp"))
		cfg_type = SOCK_DGRAM;
	else
		t_error(1, 0, "unknown cfg_test %s", cfg_test);

	pthread_barrier_init(&barrier, NULL, cfg_nr_threads);

	for (i = 0; i < IP_MAXPACKET; i++)
		payload[i] = 'a' + (i % 26);

	for (i = 0; i < cfg_nr_threads; i++) {
		td = &threads[i];
		td->idx = i;
	}

	if (cfg_rx)
		do_setup_rx(cfg_family, cfg_type, 0);

	for (i = 0; i < cfg_nr_threads; i++)
		pthread_create(&threads[i].thread, NULL,
				!cfg_rx ? do_test : do_rx, &threads[i]);

	for (i = 0; i < cfg_nr_threads; i++) {
		td = &threads[i];
		pthread_join(td->thread, &res);
		packets += td->packets;
		bytes += td->bytes;
	}

	fprintf(stderr, "packets=%llu (MB=%llu), rps=%llu (MB/s=%llu)\n",
		packets, bytes >> 20,
		packets / (cfg_runtime_ms / 1000),
		(bytes >> 20) / (cfg_runtime_ms / 1000));

	pthread_barrier_destroy(&barrier);
	return 0;
}
