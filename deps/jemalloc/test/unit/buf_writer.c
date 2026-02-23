#include "test/jemalloc_test.h"

#include "jemalloc/internal/buf_writer.h"

#define TEST_BUF_SIZE 16
#define UNIT_MAX (TEST_BUF_SIZE * 3)

static size_t test_write_len;
static char test_buf[TEST_BUF_SIZE];
static uint64_t arg;
static uint64_t arg_store;

static void
test_write_cb(void *cbopaque, const char *s) {
	size_t prev_test_write_len = test_write_len;
	test_write_len += strlen(s); /* only increase the length */
	arg_store = *(uint64_t *)cbopaque; /* only pass along the argument */
	assert_zu_le(prev_test_write_len, test_write_len,
	    "Test write overflowed");
}

static void
test_buf_writer_body(tsdn_t *tsdn, buf_writer_t *buf_writer) {
	char s[UNIT_MAX + 1];
	size_t n_unit, remain, i;
	ssize_t unit;

	assert(buf_writer->buf != NULL);
	memset(s, 'a', UNIT_MAX);
	arg = 4; /* Starting value of random argument. */
	arg_store = arg;
	for (unit = UNIT_MAX; unit >= 0; --unit) {
		/* unit keeps decreasing, so strlen(s) is always unit. */
		s[unit] = '\0';
		for (n_unit = 1; n_unit <= 3; ++n_unit) {
			test_write_len = 0;
			remain = 0;
			for (i = 1; i <= n_unit; ++i) {
				arg = prng_lg_range_u64(&arg, 64);
				buf_writer_cb(buf_writer, s);
				remain += unit;
				if (remain > buf_writer->buf_size) {
					/* Flushes should have happened. */
					assert_u64_eq(arg_store, arg, "Call "
					    "back argument didn't get through");
					remain %= buf_writer->buf_size;
					if (remain == 0) {
						/* Last flush should be lazy. */
						remain += buf_writer->buf_size;
					}
				}
				assert_zu_eq(test_write_len + remain, i * unit,
				    "Incorrect length after writing %zu strings"
				    " of length %zu", i, unit);
			}
			buf_writer_flush(buf_writer);
			expect_zu_eq(test_write_len, n_unit * unit,
			    "Incorrect length after flushing at the end of"
			    " writing %zu strings of length %zu", n_unit, unit);
		}
	}
	buf_writer_terminate(tsdn, buf_writer);
}

TEST_BEGIN(test_buf_write_static) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	assert_false(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    test_buf, TEST_BUF_SIZE),
	    "buf_writer_init() should not encounter error on static buffer");
	test_buf_writer_body(tsdn, &buf_writer);
}
TEST_END

TEST_BEGIN(test_buf_write_dynamic) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	assert_false(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    NULL, TEST_BUF_SIZE), "buf_writer_init() should not OOM");
	test_buf_writer_body(tsdn, &buf_writer);
}
TEST_END

TEST_BEGIN(test_buf_write_oom) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	assert_true(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    NULL, SC_LARGE_MAXCLASS + 1), "buf_writer_init() should OOM");
	assert(buf_writer.buf == NULL);

	char s[UNIT_MAX + 1];
	size_t n_unit, i;
	ssize_t unit;

	memset(s, 'a', UNIT_MAX);
	arg = 4; /* Starting value of random argument. */
	arg_store = arg;
	for (unit = UNIT_MAX; unit >= 0; unit -= UNIT_MAX / 4) {
		/* unit keeps decreasing, so strlen(s) is always unit. */
		s[unit] = '\0';
		for (n_unit = 1; n_unit <= 3; ++n_unit) {
			test_write_len = 0;
			for (i = 1; i <= n_unit; ++i) {
				arg = prng_lg_range_u64(&arg, 64);
				buf_writer_cb(&buf_writer, s);
				assert_u64_eq(arg_store, arg,
				    "Call back argument didn't get through");
				assert_zu_eq(test_write_len, i * unit,
				    "Incorrect length after writing %zu strings"
				    " of length %zu", i, unit);
			}
			buf_writer_flush(&buf_writer);
			expect_zu_eq(test_write_len, n_unit * unit,
			    "Incorrect length after flushing at the end of"
			    " writing %zu strings of length %zu", n_unit, unit);
		}
	}
	buf_writer_terminate(tsdn, &buf_writer);
}
TEST_END

static int test_read_count;
static size_t test_read_len;
static uint64_t arg_sum;

ssize_t
test_read_cb(void *cbopaque, void *buf, size_t limit) {
	static uint64_t rand = 4;

	arg_sum += *(uint64_t *)cbopaque;
	assert_zu_gt(limit, 0, "Limit for read_cb must be positive");
	--test_read_count;
	if (test_read_count == 0) {
		return -1;
	} else {
		size_t read_len = limit;
		if (limit > 1) {
			rand = prng_range_u64(&rand, (uint64_t)limit);
			read_len -= (size_t)rand;
		}
		assert(read_len > 0);
		memset(buf, 'a', read_len);
		size_t prev_test_read_len = test_read_len;
		test_read_len += read_len;
		assert_zu_le(prev_test_read_len, test_read_len,
		    "Test read overflowed");
		return read_len;
	}
}

static void
test_buf_writer_pipe_body(tsdn_t *tsdn, buf_writer_t *buf_writer) {
	arg = 4; /* Starting value of random argument. */
	for (int count = 5; count > 0; --count) {
		arg = prng_lg_range_u64(&arg, 64);
		arg_sum = 0;
		test_read_count = count;
		test_read_len = 0;
		test_write_len = 0;
		buf_writer_pipe(buf_writer, test_read_cb, &arg);
		assert(test_read_count == 0);
		expect_u64_eq(arg_sum, arg * count, "");
		expect_zu_eq(test_write_len, test_read_len,
		    "Write length should be equal to read length");
	}
	buf_writer_terminate(tsdn, buf_writer);
}

TEST_BEGIN(test_buf_write_pipe) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	assert_false(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    test_buf, TEST_BUF_SIZE),
	    "buf_writer_init() should not encounter error on static buffer");
	test_buf_writer_pipe_body(tsdn, &buf_writer);
}
TEST_END

TEST_BEGIN(test_buf_write_pipe_oom) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	assert_true(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    NULL, SC_LARGE_MAXCLASS + 1), "buf_writer_init() should OOM");
	test_buf_writer_pipe_body(tsdn, &buf_writer);
}
TEST_END

int
main(void) {
	return test(
	    test_buf_write_static,
	    test_buf_write_dynamic,
	    test_buf_write_oom,
	    test_buf_write_pipe,
	    test_buf_write_pipe_oom);
}
