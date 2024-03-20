// SPDX-License-Identifier: MIT

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <assert.h>

#include "liburing.h"
#include "helpers.h"

#define ENORECVMULTISHOT 9999

enum early_error_t {
	ERROR_NONE  = 0,
	ERROR_NOT_ENOUGH_BUFFERS,
	ERROR_EARLY_CLOSE_SENDER,
	ERROR_EARLY_CLOSE_RECEIVER,
	ERROR_EARLY_OVERFLOW,
	ERROR_EARLY_LAST
};

struct args {
	bool stream;
	bool wait_each;
	bool recvmsg;
	enum early_error_t early_error;
	bool defer;
};

static int check_sockaddr(struct sockaddr_in *in)
{
	struct in_addr expected;

	inet_pton(AF_INET, "127.0.0.1", &expected);
	if (in->sin_family != AF_INET) {
		fprintf(stderr, "bad family %d\n", (int)htons(in->sin_family));
		return -1;
	}
	if (memcmp(&expected, &in->sin_addr, sizeof(in->sin_addr))) {
		char buff[256];
		const char *addr = inet_ntop(AF_INET, &in->sin_addr, buff, sizeof(buff));

		fprintf(stderr, "unexpected address %s\n", addr ? addr : "INVALID");
		return -1;
	}
	return 0;
}

static int test(struct args *args)
{
	int const N = 8;
	int const N_BUFFS = N * 64;
	int const N_CQE_OVERFLOW = 4;
	int const min_cqes = 2;
	int const NAME_LEN = sizeof(struct sockaddr_storage);
	int const CONTROL_LEN = CMSG_ALIGN(sizeof(struct sockaddr_storage))
					+ sizeof(struct cmsghdr);
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int fds[2], ret, i, j;
	int total_sent_bytes = 0, total_recv_bytes = 0, total_dropped_bytes = 0;
	int send_buff[256];
	int *sent_buffs[N_BUFFS];
	int *recv_buffs[N_BUFFS];
	int *at;
	struct io_uring_cqe recv_cqe[N_BUFFS];
	int recv_cqes = 0;
	bool early_error = false;
	bool early_error_started = false;
	struct __kernel_timespec timeout = {
		.tv_sec = 1,
	};
	struct msghdr msg;
	struct io_uring_params params = { };
	int n_sqe = 32;

	memset(recv_buffs, 0, sizeof(recv_buffs));

	if (args->defer)
		params.flags |= IORING_SETUP_SINGLE_ISSUER |
				IORING_SETUP_DEFER_TASKRUN;

	if (args->early_error == ERROR_EARLY_OVERFLOW) {
		params.flags |= IORING_SETUP_CQSIZE;
		params.cq_entries = N_CQE_OVERFLOW;
		n_sqe = N_CQE_OVERFLOW;
	}

	ret = io_uring_queue_init_params(n_sqe, &ring, &params);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		return ret;
	}

	ret = t_create_socket_pair(fds, args->stream);
	if (ret) {
		fprintf(stderr, "t_create_socket_pair failed: %d\n", ret);
		return ret;
	}

	if (!args->stream) {
		bool val = true;

		/* force some cmsgs to come back to us */
		ret = setsockopt(fds[0], IPPROTO_IP, IP_RECVORIGDSTADDR, &val,
				 sizeof(val));
		if (ret) {
			fprintf(stderr, "setsockopt failed %d\n", errno);
			goto cleanup;
		}
	}

	for (i = 0; i < ARRAY_SIZE(send_buff); i++)
		send_buff[i] = i;

	for (i = 0; i < ARRAY_SIZE(recv_buffs); i++) {
		/* prepare some different sized buffers */
		int buffer_size = (i % 2 == 0 && (args->stream || args->recvmsg)) ? 1 : N;

		buffer_size *= sizeof(int);
		if (args->recvmsg) {
			buffer_size +=
				sizeof(struct io_uring_recvmsg_out) +
				NAME_LEN +
				CONTROL_LEN;
		}

		recv_buffs[i] = malloc(buffer_size);

		if (i > 2 && args->early_error == ERROR_NOT_ENOUGH_BUFFERS)
			continue;

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_provide_buffers(sqe, recv_buffs[i],
					buffer_size, 1, 7, i);
		io_uring_sqe_set_data64(sqe, 0x999);
		memset(recv_buffs[i], 0xcc, buffer_size);
		if (io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &timeout, NULL) < 0) {
			fprintf(stderr, "provide buffers failed: %d\n", ret);
			ret = -1;
			goto cleanup;
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	sqe = io_uring_get_sqe(&ring);
	if (args->recvmsg) {
		unsigned int flags = 0;

		if (!args->stream)
			flags |= MSG_TRUNC;

		memset(&msg, 0, sizeof(msg));
		msg.msg_namelen = NAME_LEN;
		msg.msg_controllen = CONTROL_LEN;
		io_uring_prep_recvmsg_multishot(sqe, fds[0], &msg, flags);
	} else {
		io_uring_prep_recv_multishot(sqe, fds[0], NULL, 0, 0);
	}
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = 7;
	io_uring_sqe_set_data64(sqe, 1234);
	io_uring_submit(&ring);

	at = &send_buff[0];
	total_sent_bytes = 0;
	for (i = 0; i < N; i++) {
		int to_send = sizeof(*at) * (i+1);

		total_sent_bytes += to_send;
		sent_buffs[i] = at;
		if (send(fds[1], at, to_send, 0) != to_send) {
			if (early_error_started)
				break;
			fprintf(stderr, "send failed %d\n", errno);
			ret = -1;
			goto cleanup;
		}

		if (i == 2) {
			if (args->early_error == ERROR_EARLY_CLOSE_RECEIVER) {
				/* allow previous sends to complete */
				usleep(1000);
				io_uring_get_events(&ring);

				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_recv(sqe, fds[0], NULL, 0, 0);
				io_uring_prep_cancel64(sqe, 1234, 0);
				io_uring_sqe_set_data64(sqe, 0x888);
				sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
				io_uring_submit(&ring);
				early_error_started = true;

				/* allow the cancel to complete */
				usleep(1000);
				io_uring_get_events(&ring);
			}
			if (args->early_error == ERROR_EARLY_CLOSE_SENDER) {
				early_error_started = true;
				shutdown(fds[1], SHUT_RDWR);
				close(fds[1]);
			}
		}
		at += (i+1);

		if (args->wait_each) {
			ret = io_uring_wait_cqes(&ring, &cqe, 1, &timeout, NULL);
			if (ret) {
				fprintf(stderr, "wait_each failed: %d\n", ret);
				ret = -1;
				goto cleanup;
			}
			while (io_uring_peek_cqe(&ring, &cqe) == 0) {
				recv_cqe[recv_cqes++] = *cqe;
				if (cqe->flags & IORING_CQE_F_MORE) {
					io_uring_cqe_seen(&ring, cqe);
				} else {
					early_error = true;
					io_uring_cqe_seen(&ring, cqe);
				}
			}
			if (early_error)
				break;
		}
	}

	close(fds[1]);

	/* allow sends to finish */
	usleep(1000);

	if ((args->stream && !early_error) || recv_cqes < min_cqes) {
		ret = io_uring_wait_cqes(&ring, &cqe, 1, &timeout, NULL);
		if (ret && ret != -ETIME) {
			fprintf(stderr, "wait final failed: %d\n", ret);
			ret = -1;
			goto cleanup;
		}
	}

	while (io_uring_peek_cqe(&ring, &cqe) == 0) {
		recv_cqe[recv_cqes++] = *cqe;
		io_uring_cqe_seen(&ring, cqe);
	}

	ret = -1;
	at = &send_buff[0];
	if (recv_cqes < min_cqes) {
		if (recv_cqes > 0 && recv_cqe[0].res == -EINVAL) {
			return -ENORECVMULTISHOT;
		}
		/* some kernels apparently don't check ->ioprio, skip */
		ret = -ENORECVMULTISHOT;
		goto cleanup;
	}
	for (i = 0; i < recv_cqes; i++) {
		cqe = &recv_cqe[i];

		bool const is_last = i == recv_cqes - 1;

		/*
		 * Older kernels could terminate multishot early due to overflow,
		 * but later ones will not. So discriminate based on the MORE flag.
		 */
		bool const early_last = args->early_error == ERROR_EARLY_OVERFLOW &&
					!args->wait_each &&
					i >= N_CQE_OVERFLOW &&
					!(cqe->flags & IORING_CQE_F_MORE);

		bool const should_be_last =
			(cqe->res <= 0) ||
			(args->stream && is_last) ||
			early_last;
		int *this_recv;
		int orig_payload_size = cqe->res;


		if (should_be_last) {
			int used_res = cqe->res;

			if (!is_last) {
				fprintf(stderr, "not last cqe had error %d\n", i);
				goto cleanup;
			}

			switch (args->early_error) {
			case ERROR_NOT_ENOUGH_BUFFERS:
				if (cqe->res != -ENOBUFS) {
					fprintf(stderr,
						"ERROR_NOT_ENOUGH_BUFFERS: res %d\n", cqe->res);
					goto cleanup;
				}
				break;
			case ERROR_EARLY_OVERFLOW:
				if (cqe->res < 0) {
					fprintf(stderr,
						"ERROR_EARLY_OVERFLOW: res %d\n", cqe->res);
					goto cleanup;
				}
				break;
			case ERROR_EARLY_CLOSE_RECEIVER:
				if (cqe->res != -ECANCELED) {
					fprintf(stderr,
						"ERROR_EARLY_CLOSE_RECEIVER: res %d\n", cqe->res);
					goto cleanup;
				}
				break;
			case ERROR_NONE:
			case ERROR_EARLY_CLOSE_SENDER:
				if (args->recvmsg && (cqe->flags & IORING_CQE_F_BUFFER)) {
					void *buff = recv_buffs[cqe->flags >> 16];
					struct io_uring_recvmsg_out *o =
						io_uring_recvmsg_validate(buff, cqe->res, &msg);

					if (!o) {
						fprintf(stderr, "invalid buff\n");
						goto cleanup;
					}
					if (o->payloadlen != 0) {
						fprintf(stderr, "expected 0 payloadlen, got %u\n",
							o->payloadlen);
						goto cleanup;
					}
					used_res = 0;
				} else if (cqe->res != 0) {
					fprintf(stderr, "early error: res %d\n", cqe->res);
					goto cleanup;
				}
				break;
			case ERROR_EARLY_LAST:
				fprintf(stderr, "bad error_early\n");
				goto cleanup;
			}

			if (cqe->res <= 0 && cqe->flags & IORING_CQE_F_BUFFER) {
				fprintf(stderr, "final BUFFER flag set\n");
				goto cleanup;
			}

			if (cqe->flags & IORING_CQE_F_MORE) {
				fprintf(stderr, "final MORE flag set\n");
				goto cleanup;
			}

			if (used_res <= 0)
				continue;
		} else {
			if (!(cqe->flags & IORING_CQE_F_MORE)) {
				fprintf(stderr, "MORE flag not set\n");
				goto cleanup;
			}
		}

		if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
			fprintf(stderr, "BUFFER flag not set\n");
			goto cleanup;
		}

		this_recv = recv_buffs[cqe->flags >> 16];

		if (args->recvmsg) {
			struct io_uring_recvmsg_out *o = io_uring_recvmsg_validate(
				this_recv, cqe->res, &msg);

			if (!o) {
				fprintf(stderr, "bad recvmsg\n");
				goto cleanup;
			}
			orig_payload_size = o->payloadlen;

			if (!args->stream) {
				orig_payload_size = o->payloadlen;

				struct cmsghdr *cmsg;

				if (o->namelen < sizeof(struct sockaddr_in)) {
					fprintf(stderr, "bad addr len %d",
						o->namelen);
					goto cleanup;
				}
				if (check_sockaddr((struct sockaddr_in *)io_uring_recvmsg_name(o)))
					goto cleanup;

				cmsg = io_uring_recvmsg_cmsg_firsthdr(o, &msg);
				if (!cmsg ||
				    cmsg->cmsg_level != IPPROTO_IP ||
				    cmsg->cmsg_type != IP_RECVORIGDSTADDR) {
					fprintf(stderr, "bad cmsg");
					goto cleanup;
				}
				if (check_sockaddr((struct sockaddr_in *)CMSG_DATA(cmsg)))
					goto cleanup;
				cmsg = io_uring_recvmsg_cmsg_nexthdr(o, &msg, cmsg);
				if (cmsg) {
					fprintf(stderr, "unexpected extra cmsg\n");
					goto cleanup;
				}

			}

			this_recv = (int *)io_uring_recvmsg_payload(o, &msg);
			cqe->res = io_uring_recvmsg_payload_length(o, cqe->res, &msg);
			if (o->payloadlen != cqe->res) {
				if (!(o->flags & MSG_TRUNC)) {
					fprintf(stderr, "expected truncated flag\n");
					goto cleanup;
				}
				total_dropped_bytes += (o->payloadlen - cqe->res);
			}
		}

		total_recv_bytes += cqe->res;

		if (cqe->res % 4 != 0) {
			/*
			 * doesn't seem to happen in practice, would need some
			 * work to remove this requirement
			 */
			fprintf(stderr, "unexpectedly aligned buffer cqe->res=%d\n", cqe->res);
			goto cleanup;
		}

		/*
		 * for tcp: check buffer arrived in order
		 * for udp: based on size validate data based on size
		 */
		if (!args->stream) {
			int sent_idx = orig_payload_size / sizeof(*at) - 1;

			if (sent_idx < 0 || sent_idx > N) {
				fprintf(stderr, "Bad sent idx: %d\n", sent_idx);
				goto cleanup;
			}
			at = sent_buffs[sent_idx];
		}
		for (j = 0; j < cqe->res / 4; j++) {
			int sent = *at++;
			int recv = *this_recv++;

			if (sent != recv) {
				fprintf(stderr, "recv=%d sent=%d\n", recv, sent);
				goto cleanup;
			}
		}
	}

	if (args->early_error == ERROR_NONE &&
	    total_recv_bytes + total_dropped_bytes < total_sent_bytes) {
		fprintf(stderr,
			"missing recv: recv=%d dropped=%d sent=%d\n",
			total_recv_bytes, total_sent_bytes, total_dropped_bytes);
		goto cleanup;
	}

	ret = 0;
cleanup:
	for (i = 0; i < ARRAY_SIZE(recv_buffs); i++)
		free(recv_buffs[i]);
	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);

	return ret;
}

static int test_enobuf(void)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqes[16];
	char buffs[256];
	int ret, i, fds[2];

	if (t_create_ring(8, &ring, 0) != T_SETUP_OK) {
		fprintf(stderr, "ring create\n");
		return -1;
	}

	ret = t_create_socket_pair(fds, false);
	if (ret) {
		fprintf(stderr, "t_create_socket_pair\n");
		return ret;
	}

	sqe = io_uring_get_sqe(&ring);
	assert(sqe);
	/* deliberately only 2 provided buffers */
	io_uring_prep_provide_buffers(sqe, &buffs[0], 1, 2, 0, 0);
	io_uring_sqe_set_data64(sqe, 0);

	sqe = io_uring_get_sqe(&ring);
	assert(sqe);
	io_uring_prep_recv_multishot(sqe, fds[0], NULL, 0, 0);
	io_uring_sqe_set_data64(sqe, 1);
	sqe->buf_group = 0;
	sqe->flags |= IOSQE_BUFFER_SELECT;

	ret = io_uring_submit(&ring);
	if (ret != 2) {
		fprintf(stderr, "bad submit %d\n", ret);
		return -1;
	}
	for (i = 0; i < 3; i++) {
		do {
			ret = write(fds[1], "?", 1);
		} while (ret == -1 && errno == EINTR);
	}

	ret = io_uring_wait_cqes(&ring, &cqes[0], 4, NULL, NULL);
	if (ret) {
		fprintf(stderr, "wait cqes\n");
		return ret;
	}

	ret = io_uring_peek_batch_cqe(&ring, &cqes[0], 4);
	if (ret != 4) {
		fprintf(stderr, "peek batch cqes\n");
		return -1;
	}

	/* provide buffers */
	assert(cqes[0]->user_data == 0);
	assert(cqes[0]->res == 0);

	/* valid recv */
	assert(cqes[1]->user_data == 1);
	assert(cqes[2]->user_data == 1);
	assert(cqes[1]->res == 1);
	assert(cqes[2]->res == 1);
	assert(cqes[1]->flags & (IORING_CQE_F_BUFFER | IORING_CQE_F_MORE));
	assert(cqes[2]->flags & (IORING_CQE_F_BUFFER | IORING_CQE_F_MORE));

	/* missing buffer */
	assert(cqes[3]->user_data == 1);
	assert(cqes[3]->res == -ENOBUFS);
	assert(!(cqes[3]->flags & (IORING_CQE_F_BUFFER | IORING_CQE_F_MORE)));

	close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	int loop;
	int early_error = 0;
	bool has_defer;

	if (argc > 1)
		return T_EXIT_SKIP;

	has_defer = t_probe_defer_taskrun();

	for (loop = 0; loop < 16; loop++) {
		struct args a = {
			.stream = loop & 0x01,
			.wait_each = loop & 0x2,
			.recvmsg = loop & 0x04,
			.defer = loop & 0x08,
		};
		if (a.defer && !has_defer)
			continue;
		for (early_error = 0; early_error < ERROR_EARLY_LAST; early_error++) {
			a.early_error = (enum early_error_t)early_error;
			ret = test(&a);
			if (ret) {
				if (ret == -ENORECVMULTISHOT) {
					if (loop == 0)
						return T_EXIT_SKIP;
					fprintf(stderr,
						"ENORECVMULTISHOT received but loop>0\n");
				}
				fprintf(stderr,
					"test stream=%d wait_each=%d recvmsg=%d early_error=%d "
					" defer=%d failed\n",
					a.stream, a.wait_each, a.recvmsg, a.early_error, a.defer);
				return T_EXIT_FAIL;
			}
		}
	}

	ret = test_enobuf();
	if (ret) {
		fprintf(stderr, "test_enobuf() failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
