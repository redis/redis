/* SPDX-License-Identifier: MIT */
/*
 * Description: run various reads tests, verifying data
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#include "helpers.h"
#include "liburing.h"

#define FSIZE		128*1024*1024
#define CHUNK_SIZE	131072
#define PUNCH_SIZE	32768

/*
 * 8 because it fits within the on-stack iov, 16 because it's larger than 8
 */
#define MIN_VECS	8
#define MAX_VECS	16

/*
 * Can be anything, let's just do something for a bit of parallellism
 */
#define READ_BATCH	16

static void verify_buf_sync(void *buf, size_t size, bool registered)
{
#if defined(__hppa__)
	if (registered) {
		unsigned long off = (unsigned long) buf & 4095;
		unsigned long p = (unsigned long) buf & ~4095;
		int i;

		size += off;
		for (i = 0; i < size; i += 32)
			asm volatile("fdc 0(%0)" : : "r" (p + i));
	}
#endif
}

/*
 * Each offset in the file has the offset / sizeof(int) stored for every
 * sizeof(int) address.
 */
static int verify_buf(void *buf, size_t size, off_t off, bool registered)
{
	int i, u_in_buf = size / sizeof(unsigned int);
	unsigned int *ptr;

	verify_buf_sync(buf, size, registered);

	off /= sizeof(unsigned int);
	ptr = buf;
	for (i = 0; i < u_in_buf; i++) {
		if (off != *ptr) {
			fprintf(stderr, "Found %u, wanted %llu\n", *ptr,
					(unsigned long long) off);
			return 1;
		}
		ptr++;
		off++;
	}

	return 0;
}

static int test_truncate(struct io_uring *ring, const char *fname, int buffered,
			 int vectored, int provide_buf)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct iovec vec;
	struct stat sb;
	off_t punch_off, off, file_size;
	void *buf = NULL;
	int u_in_buf, i, ret, fd, first_pass = 1;
	unsigned int *ptr;

	if (buffered)
		fd = open(fname, O_RDWR);
	else
		fd = open(fname, O_DIRECT | O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (fstat(fd, &sb) < 0) {
		perror("stat");
		close(fd);
		return 1;
	}

	if (S_ISREG(sb.st_mode)) {
		file_size = sb.st_size;
	} else if (S_ISBLK(sb.st_mode)) {
		unsigned long long bytes;

		if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) {
			perror("ioctl");
			close(fd);
			return 1;
		}
		file_size = bytes;
	} else {
		goto out;
	}

	if (file_size < CHUNK_SIZE)
		goto out;

	t_posix_memalign(&buf, 4096, CHUNK_SIZE);

	off = file_size - (CHUNK_SIZE / 2);
	punch_off = off + CHUNK_SIZE / 4;

	u_in_buf = CHUNK_SIZE / sizeof(unsigned int);
	ptr = buf;
	for (i = 0; i < u_in_buf; i++) {
		*ptr = i;
		ptr++;
	}
	ret = pwrite(fd, buf, CHUNK_SIZE / 2, off);
	if (ret < 0) {
		perror("pwrite");
		goto err;
	} else if (ret != CHUNK_SIZE / 2)
		goto out;

again:
	/*
	 * Read in last bit of file so it's known cached, then remove half of that
	 * last bit so we get a short read that needs retry
	 */
	ret = pread(fd, buf, CHUNK_SIZE / 2, off);
	if (ret < 0) {
		perror("pread");
		goto err;
	} else if (ret != CHUNK_SIZE / 2)
		goto out;

	if (posix_fadvise(fd, punch_off, CHUNK_SIZE / 4, POSIX_FADV_DONTNEED) < 0) {
		perror("posix_fadivse");
		goto err;
	}

	if (provide_buf) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_provide_buffers(sqe, buf, CHUNK_SIZE, 1, 0, 0);
		ret = io_uring_submit(ring);
		if (ret != 1) {
			fprintf(stderr, "submit failed %d\n", ret);
			goto err;
		}
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", ret);
			goto err;
		}
		ret = cqe->res;
		io_uring_cqe_seen(ring, cqe);
		if (ret) {
			fprintf(stderr, "Provide buffer failed %d\n", ret);
			goto err;
		}
	}

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "get sqe failed\n");
		goto err;
	}

	if (vectored) {
		assert(!provide_buf);
		vec.iov_base = buf;
		vec.iov_len = CHUNK_SIZE;
		io_uring_prep_readv(sqe, fd, &vec, 1, off);
	} else {
		if (provide_buf) {
			io_uring_prep_read(sqe, fd, NULL, CHUNK_SIZE, off);
			sqe->flags |= IOSQE_BUFFER_SELECT;
		} else {
			io_uring_prep_read(sqe, fd, buf, CHUNK_SIZE, off);
		}
	}
	memset(buf, 0, CHUNK_SIZE);

	ret = io_uring_submit(ring);
	if (ret != 1) {
		fprintf(stderr, "Submit failed %d\n", ret);
		goto err;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		fprintf(stderr, "wait completion %d\n", ret);
		goto err;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	if (ret != CHUNK_SIZE / 2) {
		fprintf(stderr, "Unexpected truncated read %d\n", ret);
		goto err;
	}

	if (verify_buf(buf, CHUNK_SIZE / 2, 0, false))
		goto err;

	/*
	 * Repeat, but punch first part instead of last
	 */
	if (first_pass) {
		punch_off = file_size - CHUNK_SIZE / 4;
		first_pass = 0;
		goto again;
	}

out:
	free(buf);
	close(fd);
	return 0;
err:
	free(buf);
	close(fd);
	return 1;
}

enum {
	PUNCH_NONE,
	PUNCH_FRONT,
	PUNCH_MIDDLE,
	PUNCH_END,
};

/*
 * For each chunk in file, DONTNEED a start, end, or middle segment of it.
 * We enter here with the file fully cached every time, either freshly
 * written or after other reads. This forces (at least) the buffered reads
 * to be handled incrementally, exercising that path.
 */
static int do_punch(int fd)
{
	off_t offset = 0;
	int punch_type;

	while (offset + CHUNK_SIZE <= FSIZE) {
		off_t punch_off;

		punch_type = rand() % (PUNCH_END + 1);
		switch (punch_type) {
		default:
		case PUNCH_NONE:
			punch_off = -1; /* gcc... */
			break;
		case PUNCH_FRONT:
			punch_off = offset;
			break;
		case PUNCH_MIDDLE:
			punch_off = offset + PUNCH_SIZE;
			break;
		case PUNCH_END:
			punch_off = offset + CHUNK_SIZE - PUNCH_SIZE;
			break;
		}

		offset += CHUNK_SIZE;
		if (punch_type == PUNCH_NONE)
			continue;
		if (posix_fadvise(fd, punch_off, PUNCH_SIZE, POSIX_FADV_DONTNEED) < 0) {
			perror("posix_fadivse");
			return 1;
		}
	}

	return 0;
}

static int provide_buffers(struct io_uring *ring, void **buf)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int i, ret;

	/* real use case would have one buffer chopped up, but... */
	for (i = 0; i < READ_BATCH; i++) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_provide_buffers(sqe, buf[i], CHUNK_SIZE, 1, 0, i);
	}

	ret = io_uring_submit(ring);
	if (ret != READ_BATCH) {
		fprintf(stderr, "Submit failed %d\n", ret);
		return 1;
	}

	for (i = 0; i < READ_BATCH; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait cqe %d\n", ret);
			return 1;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "cqe res provide %d\n", cqe->res);
			return 1;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
}

static int test(struct io_uring *ring, const char *fname, int buffered,
		int vectored, int small_vecs, int registered, int provide)
{
	struct iovec vecs[READ_BATCH][MAX_VECS];
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	void *buf[READ_BATCH];
	int ret, fd, flags;
	int i, j, nr_vecs;
	off_t off, voff;
	size_t left;

	if (registered) {
		assert(!provide);
		assert(!vectored && !small_vecs);
	}
	if (provide) {
		assert(!registered);
		assert(!vectored && !small_vecs);
	}

	flags = O_RDONLY;
	if (!buffered)
		flags |= O_DIRECT;
	fd = open(fname, flags);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (do_punch(fd))
		return 1;

	if (vectored) {
		if (small_vecs)
			nr_vecs = MIN_VECS;
		else
			nr_vecs = MAX_VECS;

		for (j = 0; j < READ_BATCH; j++) {
			for (i = 0; i < nr_vecs; i++) {
				void *ptr;

				t_posix_memalign(&ptr, 4096, CHUNK_SIZE / nr_vecs);
				vecs[j][i].iov_base = ptr;
				vecs[j][i].iov_len = CHUNK_SIZE / nr_vecs;
			}
		}
	} else {
		for (j = 0; j < READ_BATCH; j++)
			t_posix_memalign(&buf[j], 4096, CHUNK_SIZE);
		nr_vecs = 0;
	}

	if (registered) {
		struct iovec v[READ_BATCH];

		for (i = 0; i < READ_BATCH; i++) {
			v[i].iov_base = buf[i];
			v[i].iov_len = CHUNK_SIZE;
		}
		ret = t_register_buffers(ring, v, READ_BATCH);
		if (ret) {
			if (ret == T_SETUP_SKIP) {
				ret = 0;
				goto free_bufs;
			}
			goto err;
		}
	}

	i = 0;
	left = FSIZE;
	off = 0;
	while (left) {
		int pending = 0;

		if (provide && provide_buffers(ring, buf))
			goto err;

		for (i = 0; i < READ_BATCH; i++) {
			size_t this = left;

			if (this > CHUNK_SIZE)
				this = CHUNK_SIZE;

			sqe = io_uring_get_sqe(ring);
			if (!sqe) {
				fprintf(stderr, "get sqe failed\n");
				goto err;
			}

			if (vectored) {
				io_uring_prep_readv(sqe, fd, vecs[i], nr_vecs, off);
			} else {
				if (registered) {
					io_uring_prep_read_fixed(sqe, fd, buf[i], this, off, i);
				} else if (provide) {
					io_uring_prep_read(sqe, fd, NULL, this, off);
					sqe->flags |= IOSQE_BUFFER_SELECT;
				} else {
					io_uring_prep_read(sqe, fd, buf[i], this, off);
				}
			}
			sqe->user_data = ((uint64_t)off << 32) | i;
			off += this;
			left -= this;
			pending++;
			if (!left)
				break;
		}

		ret = io_uring_submit(ring);
		if (ret != pending) {
			fprintf(stderr, "sqe submit failed: %d\n", ret);
			goto err;
		}

		for (i = 0; i < pending; i++) {
			int index;

			ret = io_uring_wait_cqe(ring, &cqe);
			if (ret < 0) {
				fprintf(stderr, "wait completion %d\n", ret);
				goto err;
			}
			if (cqe->res < 0) {
				fprintf(stderr, "bad read %d, read %d\n", cqe->res, i);
				goto err;
			}
			if (cqe->res < CHUNK_SIZE) {
				fprintf(stderr, "short read %d, read %d\n", cqe->res, i);
				goto err;
			}
			if (cqe->flags & IORING_CQE_F_BUFFER)
				index = cqe->flags >> 16;
			else
				index = cqe->user_data & 0xffffffff;
			voff = cqe->user_data >> 32;
			io_uring_cqe_seen(ring, cqe);
			if (vectored) {
				for (j = 0; j < nr_vecs; j++) {
					void *buf = vecs[index][j].iov_base;
					size_t len = vecs[index][j].iov_len;

					if (verify_buf(buf, len, voff, registered))
						goto err;
					voff += len;
				}
			} else {
				if (verify_buf(buf[index], CHUNK_SIZE, voff, registered))
					goto err;
			}
		}
	}

	ret = 0;
done:
	if (registered)
		io_uring_unregister_buffers(ring);
free_bufs:
	if (vectored) {
		for (j = 0; j < READ_BATCH; j++)
			for (i = 0; i < nr_vecs; i++)
				free(vecs[j][i].iov_base);
	} else {
		for (j = 0; j < READ_BATCH; j++)
			free(buf[j]);
	}
	close(fd);
	return ret;
err:
	ret = 1;
	goto done;
}

static int fill_pattern(const char *fname)
{
	size_t left = FSIZE;
	unsigned int val, *ptr;
	void *buf;
	int fd, i;

	fd = open(fname, O_WRONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	val = 0;
	buf = t_malloc(4096);
	while (left) {
		int u_in_buf = 4096 / sizeof(val);
		size_t this = left;

		if (this > 4096)
			this = 4096;
		ptr = buf;
		for (i = 0; i < u_in_buf; i++) {
			*ptr = val;
			val++;
			ptr++;
		}
		if (write(fd, buf, 4096) != 4096)
			return 1;
		left -= 4096;
	}

	fsync(fd);
	close(fd);
	free(buf);
	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	const char *fname;
	char buf[32];
	int ret;

	srand(getpid());

	if (argc > 1) {
		fname = argv[1];
	} else {
		sprintf(buf, ".file-verify.%d", getpid());
		fname = buf;
		t_create_file(fname, FSIZE);
	}

	ret = io_uring_queue_init(READ_BATCH, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		goto err;
	}

	if (fill_pattern(fname))
		goto err;

	ret = test(&ring, fname, 1, 0, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "Buffered novec test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 1, 0, 0, 1, 0);
	if (ret) {
		fprintf(stderr, "Buffered novec reg test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 1, 0, 0, 0, 1);
	if (ret) {
		fprintf(stderr, "Buffered novec provide test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 1, 1, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "Buffered vec test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 1, 1, 1, 0, 0);
	if (ret) {
		fprintf(stderr, "Buffered small vec test failed\n");
		goto err;
	}

	ret = test(&ring, fname, 0, 0, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "O_DIRECT novec test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 0, 0, 0, 1, 0);
	if (ret) {
		fprintf(stderr, "O_DIRECT novec reg test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 0, 0, 0, 0, 1);
	if (ret) {
		fprintf(stderr, "O_DIRECT novec provide test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 0, 1, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "O_DIRECT vec test failed\n");
		goto err;
	}
	ret = test(&ring, fname, 0, 1, 1, 0, 0);
	if (ret) {
		fprintf(stderr, "O_DIRECT small vec test failed\n");
		goto err;
	}

	ret = test_truncate(&ring, fname, 1, 0, 0);
	if (ret) {
		fprintf(stderr, "Buffered end truncate read failed\n");
		goto err;
	}
	ret = test_truncate(&ring, fname, 1, 1, 0);
	if (ret) {
		fprintf(stderr, "Buffered end truncate vec read failed\n");
		goto err;
	}
	ret = test_truncate(&ring, fname, 1, 0, 1);
	if (ret) {
		fprintf(stderr, "Buffered end truncate pbuf read failed\n");
		goto err;
	}

	ret = test_truncate(&ring, fname, 0, 0, 0);
	if (ret) {
		fprintf(stderr, "O_DIRECT end truncate read failed\n");
		goto err;
	}
	ret = test_truncate(&ring, fname, 0, 1, 0);
	if (ret) {
		fprintf(stderr, "O_DIRECT end truncate vec read failed\n");
		goto err;
	}
	ret = test_truncate(&ring, fname, 0, 0, 1);
	if (ret) {
		fprintf(stderr, "O_DIRECT end truncate pbuf read failed\n");
		goto err;
	}

	if (buf == fname)
		unlink(fname);
	return T_EXIT_PASS;
err:
	if (buf == fname)
		unlink(fname);
	return T_EXIT_FAIL;
}
