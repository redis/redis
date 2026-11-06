#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/malloc_io.h"

static void *
buf_writer_allocate_internal_buf(tsdn_t *tsdn, size_t buf_len) {
#ifdef JEMALLOC_JET
	if (buf_len > SC_LARGE_MAXCLASS) {
		return NULL;
	}
#else
	assert(buf_len <= SC_LARGE_MAXCLASS);
#endif
	return iallocztm(tsdn, buf_len, sz_size2index(buf_len), false, NULL,
	    true, arena_get(tsdn, 0, false), true);
}

static void
buf_writer_free_internal_buf(tsdn_t *tsdn, void *buf) {
	if (buf != NULL) {
		idalloctm(tsdn, buf, NULL, NULL, true, true);
	}
}

static void
buf_writer_assert(buf_writer_t *buf_writer) {
	assert(buf_writer != NULL);
	assert(buf_writer->write_cb != NULL);
	if (buf_writer->buf != NULL) {
		assert(buf_writer->buf_size > 0);
	} else {
		assert(buf_writer->buf_size == 0);
		assert(buf_writer->internal_buf);
	}
	assert(buf_writer->buf_end <= buf_writer->buf_size);
}

bool
buf_writer_init(tsdn_t *tsdn, buf_writer_t *buf_writer, write_cb_t *write_cb,
    void *cbopaque, char *buf, size_t buf_len) {
	if (write_cb != NULL) {
		buf_writer->write_cb = write_cb;
	} else {
		buf_writer->write_cb = je_malloc_message != NULL ?
		    je_malloc_message : wrtmessage;
	}
	buf_writer->cbopaque = cbopaque;
	assert(buf_len >= 2);
	if (buf != NULL) {
		buf_writer->buf = buf;
		buf_writer->internal_buf = false;
	} else {
		buf_writer->buf = buf_writer_allocate_internal_buf(tsdn,
		    buf_len);
		buf_writer->internal_buf = true;
	}
	if (buf_writer->buf != NULL) {
		buf_writer->buf_size = buf_len - 1; /* Allowing for '\0'. */
	} else {
		buf_writer->buf_size = 0;
	}
	buf_writer->buf_end = 0;
	buf_writer_assert(buf_writer);
	return buf_writer->buf == NULL;
}

void
buf_writer_flush(buf_writer_t *buf_writer) {
	buf_writer_assert(buf_writer);
	if (buf_writer->buf == NULL) {
		return;
	}
	buf_writer->buf[buf_writer->buf_end] = '\0';
	buf_writer->write_cb(buf_writer->cbopaque, buf_writer->buf);
	buf_writer->buf_end = 0;
	buf_writer_assert(buf_writer);
}

void
buf_writer_cb(void *buf_writer_arg, const char *s) {
	buf_writer_t *buf_writer = (buf_writer_t *)buf_writer_arg;
	buf_writer_assert(buf_writer);
	if (buf_writer->buf == NULL) {
		buf_writer->write_cb(buf_writer->cbopaque, s);
		return;
	}
	size_t i, slen, n;
	for (i = 0, slen = strlen(s); i < slen; i += n) {
		if (buf_writer->buf_end == buf_writer->buf_size) {
			buf_writer_flush(buf_writer);
		}
		size_t s_remain = slen - i;
		size_t buf_remain = buf_writer->buf_size - buf_writer->buf_end;
		n = s_remain < buf_remain ? s_remain : buf_remain;
		memcpy(buf_writer->buf + buf_writer->buf_end, s + i, n);
		buf_writer->buf_end += n;
		buf_writer_assert(buf_writer);
	}
	assert(i == slen);
}

void
buf_writer_terminate(tsdn_t *tsdn, buf_writer_t *buf_writer) {
	buf_writer_assert(buf_writer);
	buf_writer_flush(buf_writer);
	if (buf_writer->internal_buf) {
		buf_writer_free_internal_buf(tsdn, buf_writer->buf);
	}
}

void
buf_writer_pipe(buf_writer_t *buf_writer, read_cb_t *read_cb,
    void *read_cbopaque) {
	/*
	 * A tiny local buffer in case the buffered writer failed to allocate
	 * at init.
	 */
	static char backup_buf[16];
	static buf_writer_t backup_buf_writer;

	buf_writer_assert(buf_writer);
	assert(read_cb != NULL);
	if (buf_writer->buf == NULL) {
		buf_writer_init(TSDN_NULL, &backup_buf_writer,
		    buf_writer->write_cb, buf_writer->cbopaque, backup_buf,
		    sizeof(backup_buf));
		buf_writer = &backup_buf_writer;
	}
	assert(buf_writer->buf != NULL);
	ssize_t nread = 0;
	do {
		buf_writer->buf_end += nread;
		buf_writer_assert(buf_writer);
		if (buf_writer->buf_end == buf_writer->buf_size) {
			buf_writer_flush(buf_writer);
		}
		nread = read_cb(read_cbopaque,
		    buf_writer->buf + buf_writer->buf_end,
		    buf_writer->buf_size - buf_writer->buf_end);
	} while (nread > 0);
	buf_writer_flush(buf_writer);
}
