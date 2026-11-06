#ifndef JEMALLOC_INTERNAL_BUF_WRITER_H
#define JEMALLOC_INTERNAL_BUF_WRITER_H

/*
 * Note: when using the buffered writer, cbopaque is passed to write_cb only
 * when the buffer is flushed.  It would make a difference if cbopaque points
 * to something that's changing for each write_cb call, or something that
 * affects write_cb in a way dependent on the content of the output string.
 * However, the most typical usage case in practice is that cbopaque points to
 * some "option like" content for the write_cb, so it doesn't matter.
 */

typedef struct {
	write_cb_t *write_cb;
	void *cbopaque;
	char *buf;
	size_t buf_size;
	size_t buf_end;
	bool internal_buf;
} buf_writer_t;

bool buf_writer_init(tsdn_t *tsdn, buf_writer_t *buf_writer,
    write_cb_t *write_cb, void *cbopaque, char *buf, size_t buf_len);
void buf_writer_flush(buf_writer_t *buf_writer);
write_cb_t buf_writer_cb;
void buf_writer_terminate(tsdn_t *tsdn, buf_writer_t *buf_writer);

typedef ssize_t (read_cb_t)(void *read_cbopaque, void *buf, size_t limit);
void buf_writer_pipe(buf_writer_t *buf_writer, read_cb_t *read_cb,
    void *read_cbopaque);

#endif /* JEMALLOC_INTERNAL_BUF_WRITER_H */
