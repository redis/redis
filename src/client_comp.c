#include "server.h"
#include "client_comp.h"

#ifdef USE_COMPRESSION
#include <zstd.h>

/* Abstraction over compression library */
typedef struct compressionType {
    int (*init_compress)(struct compressionState *st, int level);
    int (*init_decompress)(struct compressionState *st);
    int (*compress)(struct compressionState *st, int flush);
    int (*decompress)(struct compressionState *st);
    void (*end)(struct compressionState *st);
} compressionType;

/* Temporary buffer used by compression library to store compressed/decompressed
 * data. */
typedef struct {
    unsigned char *data;
    int size;
    int written;
    int consumed;
} tempBuf;

/* Main compression state struct */
struct compressionState {
    const compressionType *type;
    tempBuf input;  /* Buffer holding compressed data */
    tempBuf output; /* Buffer holding uncompressed(decompressed) data */
    union {
        ZSTD_CStream *zstdCCtx;  /* Zstd compression ctx */
        ZSTD_DStream *zstdDCtx;  /* Zstd decompression ctx */
    } ctx;
    int write_flush_pending;    /* write flush not yet completed */
    int frame_open;             /* Data has been fed to the compressor with
                                 * ZSTD_e_continue but the frame has not been
                                 * ended/flushed yet, so buffered data is still
                                 * waiting to be sent. */
    int read_flush_pending;    /* read flush not yet completed */
    mstime_t last_write;  /* Time since last write. Used to check if it's time
                           * to flush the buffer */
    compressionDirection dir;
    listNode *pending_data_node;  /* Node in the event loop's pending
                                   * decompression list (el->privdata[2]),
                                   * or NULL if not pending. */
    int handle_pending;  /* When set, clientConnRead only drains already-read
                          * compressed data without touching the socket. */
    size_t alloc_size;  /* Total allocated size of this struct plus the input/output
                         * buffers, captured at allocation time so memory accounting
                         * doesn't have to query the allocator on every call. */
};

/* --- zstd --- */

static int zstdInitCompress(compressionState *st, int level) {
    st->ctx.zstdCCtx = ZSTD_createCStream();
    if (!st->ctx.zstdCCtx) {
        serverLog(LL_NOTICE, "Failed to create ZSTD compression context");
        return -1;
    }
    size_t res = ZSTD_CCtx_setParameter(st->ctx.zstdCCtx, ZSTD_c_compressionLevel, level);
    if (ZSTD_isError(res)) {
        ZSTD_freeCStream(st->ctx.zstdCCtx);
        serverLog(LL_NOTICE, "Failed to set compression level for ZSTD compression context");
        return -1;
    }

    /* temp buf storing compressed data */
    size_t outSize = ZSTD_CStreamOutSize();
    size_t usable = 0;
    st->output.data = zmalloc_usable(outSize, &usable);
    st->output.size = outSize;
    st->output.written = 0;
    st->output.consumed = 0;
    st->alloc_size += usable;

    /* temp buf storing uncompressed data */
    size_t inSize = ZSTD_CStreamInSize();
    st->input.data = zmalloc_usable(inSize, &usable);
    st->input.size = inSize;
    st->input.written = 0;
    st->input.consumed = 0;
    st->alloc_size += usable;

    st->write_flush_pending = 0;

    return 0;
}

static int zstdInitDecompress(compressionState *st) {
    st->ctx.zstdDCtx = ZSTD_createDStream();
    if (!st->ctx.zstdDCtx) {
        serverLog(LL_NOTICE, "Failed to create ZSTD decompression context");
        return -1;
    }

    /* temp buf storing compressed data */
    size_t inSize = ZSTD_DStreamInSize();
    size_t usable = 0;
    st->input.data = zmalloc_usable(inSize, &usable);
    st->input.size = inSize;
    st->input.written = 0;
    st->input.consumed = 0;
    st->alloc_size += usable;

    /* temp buf storing decompressed data */
    size_t outSize = ZSTD_DStreamOutSize();
    st->output.data = zmalloc_usable(outSize, &usable);
    st->output.size = outSize;
    st->output.written = 0;
    st->output.consumed = 0;
    st->alloc_size += usable;

    st->read_flush_pending = 0;

    return 0;
}

static int zstdCompress(compressionState *st, int flush) {
    ZSTD_inBuffer input = {
        .src  = st->input.data,
        .size = st->input.written,
        .pos  = st->input.consumed
    };
    ZSTD_outBuffer output = {
        .dst  = st->output.data,
        .size = st->output.size,
        .pos  = st->output.written
    };

    size_t consumed_before = input.pos;

    ZSTD_EndDirective directive;
    /* We use ZSTD_e_end instead of ZSTD_e_flush when we want to flush zstd's.
     * This flushes zstd's internal buffers but also ends the current frame.
     * This of course lowers the compression ratio but massively increases speed
     * on the decompression side also, as it doesn't need to wait for more data.
     * The resulting compression ratio is still very good (tested with default
     * compression level). */
    if (flush || st->write_flush_pending)
        directive = ZSTD_e_end;
    else
        directive = ZSTD_e_continue;

    size_t ret;
    do {
        ret = ZSTD_compressStream2(st->ctx.zstdCCtx, &output, &input, directive);
        if (ZSTD_isError(ret)) {
            serverLog(LL_WARNING, "zstd compress error: %s", ZSTD_getErrorName(ret));
            return -1;
        }
    } while (ret > 0 && output.pos < output.size);

    /* If we pass a directive different than ZSTD_e_continue to zstd we want to
     * keep using that directive until compressStream2 returns 0. By keeping
     * this flag raised we know we are in the process of flushing data, i.e we
     * cannot use ZSTD_e_continue before we have flushed it all. */
    st->write_flush_pending = (directive == ZSTD_e_end && ret > 0);

    /* Track whether an un-ended frame still holds buffered data. Feeding input
     * with ZSTD_e_continue opens a frame; a fully completed ZSTD_e_end (ret == 0)
     * ends it. This lets callers cheaply tell when there is buffered data worth
     * force flushing (e.g. once a replica has caught up). */
    if (directive == ZSTD_e_end) {
        if (ret == 0) st->frame_open = 0;
    } else if (input.pos > consumed_before) {
        st->frame_open = 1;
    }

    st->input.consumed = input.pos;
    st->output.written = output.pos;

    return 0;
}

static int zstdDecompress(compressionState *st) {
    ZSTD_inBuffer input = {
        .src  = st->input.data,
        .size = st->input.written,
        .pos  = st->input.consumed
    };
    ZSTD_outBuffer output = {
        .dst  = st->output.data,
        .size = st->output.size,
        .pos  = st->output.written
    };

    size_t ret = ZSTD_decompressStream(st->ctx.zstdDCtx, &output, &input);
    if (ZSTD_isError(ret)) {
        serverLog(LL_NOTICE, "zstd decompress error: %s", ZSTD_getErrorName(ret));
        return -1;
    }

    /* Don't try to flush again if we already tried, no more progress can be
     * made without additional input. */
    st->read_flush_pending = !st->read_flush_pending && (ret > 0);

    st->input.consumed = input.pos;
    st->output.written = output.pos;

    return 0;
}

static void zstdEnd(compressionState *st) {
    if (st->dir == COMPRESS && st->ctx.zstdCCtx) {
        /* Flush any pending data so context is in a good state before closing it */
        if (st->write_flush_pending) {
            size_t sz = ZSTD_CStreamOutSize();
            char *tmp = zmalloc(sz);
            ZSTD_inBuffer input = {
                .src  = NULL,
                .size = 0,
                .pos  = 0
            };
            ZSTD_outBuffer output = {
                .dst  = tmp,
                .size = sz,
                .pos  = 0
            };
            while (ZSTD_compressStream2(st->ctx.zstdCCtx, &output, &input, ZSTD_e_end) > 0) {
                /* Just ignore the output, we are closing the compression state
                 * anyways */
                output.pos = 0;
            }
            zfree(tmp);
        }
        ZSTD_freeCStream(st->ctx.zstdCCtx);
        st->ctx.zstdCCtx = NULL;
    } else if (st->dir == DECOMPRESS && st->ctx.zstdDCtx) {
        ZSTD_freeDStream(st->ctx.zstdDCtx);
        st->ctx.zstdDCtx = NULL;
    }
}

static const compressionType zstdType = {
    .init_compress = zstdInitCompress,
    .init_decompress = zstdInitDecompress,
    .compress = zstdCompress,
    .decompress = zstdDecompress,
    .end = zstdEnd,
};

/* Create compression state for the client */
int compressionStateCreate(client *c) {
    size_t usable = 0;
    compressionState *st = zcalloc_usable(sizeof(compressionState), &usable);
    st->alloc_size = usable;
    st->type = &zstdType;
    st->last_write = 0;
    st->write_flush_pending = 0;
    st->frame_open = 0;
    st->read_flush_pending = 0;
    st->dir = CD_INVALID;
    st->pending_data_node = NULL;
    st->handle_pending = 0;

    c->compression_state = st;

    return 1;
}

void compressionStateDestroy(compressionState *state) {
    if (state == NULL) return;

    state->type->end(state);
    zfree(state->input.data);
    zfree(state->output.data);
    zfree(state);
}

/* Create and initialize a compression state for the client. No-op if already
 * initialized. `dir` indicates the compression direction, i.e if the client
 * will compress or decompress data.
 * Currently only viable for master/replica clients. */
int clientCreateCompressionState(client *c, compressionDirection dir) {
    /* Client compression already initialized */
    if (c->compression_state != NULL)
        return 1;

    serverAssert(compressionStateCreate(c));

    compressionState *st = c->compression_state;

    if (dir == COMPRESS) {
        serverAssert(c->compression_level > 0 && c->flags & CLIENT_SLAVE);

        if (st->type->init_compress(st, c->compression_level) == -1) {
            compressionStateDestroy(c->compression_state);
            c->compression_state = NULL;
            return 0;
        }

        st->dir = COMPRESS;

        serverLog(LL_NOTICE, "Initialized compression at level %d for client #%llu...",
                c->compression_level, (unsigned long long)c->id);
    } else if (dir == DECOMPRESS) {
        serverAssert(server.repl_master_compression_level > 0);

        if (st->type->init_decompress(st) == -1) {
            compressionStateDestroy(c->compression_state);
            c->compression_state = NULL;
            return 0;
        }
 
        st->dir = DECOMPRESS;

        serverLog(LL_NOTICE, "Decompression for master client initialized.");
    } else {
        /* Inaccessible */
        serverAssert(0);
    }

    return 1;
}

void clientDestroyCompressionState(client *c) {
    if (c->compression_state == NULL) return;

    /* Make sure the client is not left referenced in its event loop's pending
     * decompression list before we free the compression state. */
    clientCompressionPendingRemove(c);

    compressionStateDestroy(c->compression_state);
    c->compression_state = NULL;
    c->compression_level = 0;
    c->io_flags &= ~CLIENT_IO_COMPRESSION_ENABLED;

    serverLog(LL_NOTICE, "Compression state for client #%llu%s destroyed...",
              (unsigned long long)c->id,
              c->flags & CLIENT_MASTER ? " (master)" : c->flags & CLIENT_SLAVE ?
              " (slave)" : "");
}

/* Enable client compression and create compression state if not present.
 * Currently only valid for primary/replica clients.
 * Return 0 if compression state was not created and failed to be initialized,
 * 1 if compression was enabled. */
int clientEnableCompression(client *c, compressionDirection dir) {
    serverAssert((c->flags & CLIENT_MASTER) || (c->flags & CLIENT_SLAVE));
    if (!clientCreateCompressionState(c, dir)) {
        return 0;
    }

    c->io_flags |= CLIENT_IO_COMPRESSION_ENABLED;
    return 1;
}

/* Disable compression without destroying compression state. */
void clientDisableCompression(client *c) {
    c->io_flags &= ~CLIENT_IO_COMPRESSION_ENABLED;
}

/* Compress any data sent for compression (see clientConnWrite) and
 * write to socket. Compression library may not return compressed data
 * immediately so this call may not write anything to socket.
 * When `force_flush` is set the compressor's frame is ended and flushed
 * regardless of `compression_max_latency`, otherwise the buffer is only flushed
 * once `compression_max_latency` ms have passed since the last write.
 * Return number of bytes written to socket or -1 on socket write error. */
int clientCompressAndWrite(client *c, int *tot_written, int force_flush) {
    if (c->compression_level <= 0)
        return 0;

    compressionState *state = c->compression_state;
    serverAssert(state);

    /* All available uncompressed data was consumed so we need to reset the
     * uncompressed buffer */
    if (state->input.written == state->input.size &&
        state->input.consumed == state->input.size)
    {
        state->input.written = 0;
        state->input.consumed = 0;
    }

    if (state->output.written < state->output.size) {
        /* Force flush when requested or after `compression_max_latency` ms have
         * passed. Note, this only makes sense when we have enough space for
         * compressing data. */
        int flush = force_flush ||
                    mstime() - state->last_write >= server.compression_max_latency;
        if (state->type->compress(state, flush) == -1) {
            clientDestroyCompressionState(c);
            /* Set error connection state in order to disconnect the client.
             * Compression write error is non-recoverable. */
            c->conn->state = CONN_STATE_ERROR;
            return 1;
        }
    }

    /* Try to write all the data available in the compressed buffer. */
    *tot_written = 0;
    int towrite = state->output.written - state->output.consumed;
    do {
        int written = connWrite(
            c->conn, state->output.data + state->output.consumed, towrite);
        if (written < 0) {
            return 1;
        }

        state->output.consumed += written;
        *tot_written += written;
        towrite -= written;

        /* All of the compressed data was send to the socket so we need to reset
         * the compression buffer. */
        if (state->output.consumed == state->output.size) {
            serverAssert(towrite == 0 && state->output.written == state->output.size);

            state->output.written = 0;
            state->output.consumed = 0;
        }
    } while (towrite > 0);

    if (*tot_written > 0)
        state->last_write = mstime();

    return 0;
}

/* Force flush the compressor's currently buffered frame to the socket. This is
 * used to promptly deliver data to a replica that has caught up with the
 * replication stream, so it doesn't have to wait for the periodic
 * compression_max_latency flush. It's a no-op when there's nothing buffered.
 * Returns the number of bytes written to the socket, or -1 on write error (the
 * connection state is set so the caller disconnects the client). */
int clientFlushCompressedData(client *c) {
    compressionState *state = c->compression_state;
    if (state == NULL || state->dir != COMPRESS) return 0;

    /* Nothing is buffered in the compressor, so there is nothing to flush. */
    if (!state->frame_open && !state->write_flush_pending) return 0;

    int written = 0;
    if (clientCompressAndWrite(c, &written, 1) != 0) return -1;
    return written;
}

/* Decompress input compressed data and put it in `buf`. If decompressed data
 * is more than buflen this function must be called again so output data can
 * be consumed. If buflen is sufficiently large this function will decompress
 * as much data as possible. */
int decompressInto(compressionState *state, char *buf, size_t buflen) {
    if (buflen == 0)
      return 0;

    int consumed = 0;

    /* Decompress as much data as possible */
    while ((size_t)consumed < buflen &&
           (state->read_flush_pending ||
            state->input.written > state->input.consumed ||
            state->output.written > state->output.consumed))
    {
        /* Reset the decompressed buffer if all the available data is consumed */
        if (state->output.consumed == state->output.size) {
            state->output.written = 0;
            state->output.consumed = 0;
        }

        if ((state->read_flush_pending && state->output.size > state->output.written) ||
             state->input.written > state->input.consumed)
        {
            if (state->type->decompress(state) == -1) {
                return -1;
            }
        }

        /* Copy the decompressed data to the output buffer */
        if (state->output.written > state->output.consumed) {
            size_t nonconsumed_decompressed = state->output.written - state->output.consumed;
            int to_consume = min(buflen - consumed, nonconsumed_decompressed);
            memcpy(buf + consumed, state->output.data + state->output.consumed, to_consume);

            state->output.consumed += to_consume;
            consumed += to_consume;
        }
    }

    if (state->output.consumed == state->output.size)
    {
        state->output.written = 0;
        state->output.consumed = 0;
    }

    /* Reset the compressed buffer if we decompressed all the available data */
    if (state->input.consumed == state->input.size) {
        state->input.written = 0;
        state->input.consumed = 0;
    }

    serverAssert((size_t)consumed <= buflen);
    return consumed;
}

/* Read data from input_buf and decompress it immediately. The result is written
 * into output_buf.
 * Return number of bytes decompressed. *consumed stores number of bytes consumed
 * from input_buf.
 * Note, that we may have enough compressed data inside input buf so that decompressing
 * it will exceed output_len. The function must be ran in a loop until input_buf
 * is fully consumed - so make sure to have free space in output_buf on each call. */
int clientReadBufAndDecompress(client *c, char *input_buf, size_t input_len,
                             char *output_buf, size_t output_len, size_t *consumed)
{
    compressionState *state = c->compression_state;
    if (!state) {
        return -1;
    }

    int tot_decompressed = 0;
    *consumed = 0;
    while (*consumed <= input_len && (size_t)tot_decompressed < output_len) {
        int to_consume =
            min(state->input.size - state->input.written,
                (int)(input_len - *consumed));

        if (to_consume)
            memcpy(state->input.data + state->input.written,
                   input_buf + *consumed, to_consume);

        *consumed += to_consume;

        state->input.written += to_consume;

        int decompressed = decompressInto(state, output_buf + tot_decompressed,
                                          output_len - tot_decompressed);
        if (decompressed <= 0) {
            /* Compression library failure, we should close the connection */
            if (decompressed < 0)
                c->conn->state = CONN_STATE_CLOSED;
            break;
        }
        tot_decompressed += decompressed;
    }

    return tot_decompressed;
}

/* Check if we need to flush compressed data. Compression library may wait for
 * a lot of compressed data before it finishes a frame and gives it back to user.
 * While this gives the best compression ratio it introduces a lot of latency so
 * we make a compromise and flush periodically. */
int clientHasPendingCompressionFlush(client *c) {
    compressionState *state = c->compression_state;
    if (!state) return 0;
    if (state->dir != COMPRESS) return 0;

    return state->write_flush_pending || (mstime() - state->last_write >= server.compression_max_latency);
}

/* Check if we still have pending compressed data. This may mean that either the
 * compression library still has data in it's internal buffers, we still have
 * compressed data that needs to be consumed by the library or we have stored
 * decompressed data that we still have not consumed. */
int clientHasPendingCompressedData(client *c) {
    compressionState *state = c->compression_state;
    if (!state) return 0;
    if (state->dir != DECOMPRESS) return 0;

    return (state->read_flush_pending && state->output.size > state->output.written) ||
           state->input.written > state->input.consumed ||
           state->output.written > state->output.consumed;
}

/* Return the number of bytes used by the client's compression state, i.e the
 * compressionState struct itself plus its input/output buffers. The size is
 * captured at allocation time (see compressionStateCreate/zstdInitCompress/
 * zstdInitDecompress) so this call is just a field read instead of querying
 * the allocator. Note this does not include the zstd compression/decompression
 * context (ctx.zstdCCtx/zstdDCtx), which is allocated by libzstd via libc
 * malloc and therefore tracked neither here nor in used_memory. */
size_t clientCompressionMemoryUsage(client *c) {
    compressionState *st = c->compression_state;
    if (!st) return 0;

    return st->alloc_size;
}

/* Add the client to its event loop's pending decompression list so its buffered
 * compressed/decompressed data can be drained from beforeSleep even when no
 * socket read event fires. No-op if already present. */
static void clientCompressionPendingAdd(client *c) {
    compressionState *st = c->compression_state;
    if (!st || st->pending_data_node) return;

    aeEventLoop *el = c->conn ? c->conn->el : NULL;
    if (!el) return;

    if (!el->privdata[2]) el->privdata[2] = listCreate();

    list *l = el->privdata[2];
    listAddNodeTail(l, c);
    st->pending_data_node = listLast(l);
}

void clientCompressionPendingRemove(client *c) {
    compressionState *st = c->compression_state;
    if (!st || !st->pending_data_node) return;

    aeEventLoop *el = c->conn ? c->conn->el : NULL;
    list *l = el ? el->privdata[2] : NULL;
    if (l && listLength(l) > 0 && listSearchKey(l, c) == st->pending_data_node) {
        listDelNode(l, st->pending_data_node);
    } else {
        zfree(st->pending_data_node);
    }
    st->pending_data_node = NULL;
}

int clientCompressionHasPendingData(struct aeEventLoop *el) {
    list *l = el->privdata[2];
    if (!l) return 0;
    return listLength(l) > 0;
}

/* Drain pending decompressed data for all clients registered on this event
 * loop. We invoke the client's read handler (readQueryFromClient) with the
 * handle_pending flag set so it only processes already-buffered data without
 * issuing a socket read. */
int clientCompressionProcessPendingData(struct aeEventLoop *el) {
    list *l = el->privdata[2];
    if (!l || listLength(l) == 0) return 0;

    listIter li;
    listNode *ln;
    int processed = 0;
    listRewind(l, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        /* Drop entries we can no longer drain: the client is gone, compression
         * was disabled, the connection is detached, or there's no read handler
         * installed (e.g. it was unbound from this event loop). Leaving them in
         * the list would keep clientCompressionHasPendingData() true and prevent
         * beforeSleep from ever sleeping while doing no useful work. If the
         * client still has buffered data it will be re-added by clientConnRead
         * on its next read. */
        if (!c || !c->conn || !c->compression_state || !connHasReadHandler(c->conn)) {
            if (c && c->compression_state &&
                c->compression_state->pending_data_node == ln)
            {
                c->compression_state->pending_data_node = NULL;
            }
            listDelNode(l, ln);
            continue;
        }

        c->compression_state->handle_pending = 1;
        c->conn->read_handler(c->conn);
        c->compression_state->handle_pending = 0;

        ++processed;
    }
    return processed;
}

/* Compress `len` bytes from `data` and write them to the client's connection.
 * See client_comp.h for the contract. */
int clientConnWrite(client *c, const void *data, size_t len, int *nwritten) {
    compressionState *state = c->compression_state;
    /* No active compressor for this direction: behave like a plain write. */
    if (!state || state->dir != COMPRESS) {
        int w = connWrite(c->conn, data, len);
        if (nwritten) *nwritten = (w > 0) ? w : 0;
        return w;
    }

    int consumed = 0;
    int sock_written = 0;
    while ((size_t)consumed != len) {
        int to_consume =
            min(state->input.size - state->input.written, (int)(len - consumed));
        serverAssert(to_consume >= 0);

        memcpy(state->input.data + state->input.written,
               (char*)data + consumed, to_consume);

        state->input.written += to_consume;
        consumed += to_consume;

        /* Write whatever we have available in the compressed buffer. We don't
         * force a flush here so data keeps batching into the current frame;
         * the frame is flushed either once the replica catches up (see
         * clientFlushCompressedData) or after compression_max_latency ms. */
        int written = 0;
        int err = clientCompressAndWrite(c, &written, 0);
        if (err) {
            if (connGetState(c->conn) != CONN_STATE_CONNECTED) {
                if (nwritten) *nwritten = sock_written;
                return -1;
            }
            /* Connection is still healthy, return what we managed to consume. */
            break;
        }
        sock_written += written;

        if (written == 0 && state->output.written == state->output.consumed)
            break;
    }

    if (nwritten) *nwritten = sock_written;
    /* `consumed` counts the uncompressed bytes taken from the caller while the
     * socket received `sock_written` compressed bytes. Track the uncompressed
     * size here so callers don't have to be compression-aware. */
    if (consumed > 0)
        atomicIncr(server.stat_net_repl_output_uncompressed_bytes, consumed);
    return consumed;
}

/* Read from the client's connection and decompress into `buf`.
 * See client_comp.h for the contract. */
int clientConnRead(client *c, void *buf, size_t buf_len, int *nread_out) {
    compressionState *state = c->compression_state;
    /* No active decompressor: behave like a plain read. */
    if (!state || state->dir != DECOMPRESS) {
        int r = connRead(c->conn, buf, buf_len);
        if (nread_out) *nread_out = (r > 0) ? r : 0;
        return r;
    }

    int sock_read = 0;
    int decompressed = 0;
    do {
        int curr = decompressInto(state, (char*)buf + decompressed, buf_len - decompressed);
        /* Decompression error, we should close the connection */
        if (curr < 0) {
            c->conn->state = CONN_STATE_CLOSED;
            break;
        }
        decompressed += curr;

        int nread = 0;
        /* When draining pending data we only decompress what we have already
         * read from the socket without issuing another socket read. The actual
         * socket read happens when the event loop handles a read event, in which
         * case handle_pending is not set. */
        if (!state->handle_pending) {
            nread = connRead(c->conn,
                             state->input.data + state->input.written,
                             state->input.size - state->input.written);

            if (nread < 0 && connGetState(c->conn) == CONN_STATE_ERROR) {
                if (nread_out) *nread_out = sock_read;
                return -1;
            }
            /* Even if nread == 0 we continue the loop until decompressInto has
             * nothing more it can do. */
            if (nread > 0) {
                sock_read += nread;
                state->input.written += nread;
            }
        }

        if (curr <= 0 && nread <= 0) break;
    } while ((size_t)decompressed < buf_len);

    if (nread_out) *nread_out = sock_read;

    if (decompressed == 0 && connGetState(c->conn) == CONN_STATE_CONNECTED)
        return -1;

    /* No need to give pending work to the main thread as the client must be
     * sent to an IO-thread soon enough. */
    if (c->running_tid != IOTHREAD_MAIN_THREAD_ID) {
        if (connGetState(c->conn) == CONN_STATE_CONNECTED && clientHasPendingCompressedData(c)) {
            clientCompressionPendingAdd(c);
        } else if (state->pending_data_node) {
            clientCompressionPendingRemove(c);
        }
    }

    /* `decompressed` bytes were produced from `sock_read` compressed bytes read
     * off the socket. Track the decompressed size here so callers don't have to
     * be compression-aware. */
    if (decompressed > 0)
        atomicIncr(server.stat_net_repl_input_decompressed_bytes, decompressed);

    return decompressed;
}

#else /* !USE_COMPRESSION */

/* Redis was built without compression support (BUILD_COMPRESSION=yes enables it,
 * pulling in libzstd). We still provide the full client_comp API as no-op
 * fallbacks so the rest of the server doesn't need any #ifdefs. Replication
 * compression can never be enabled at runtime in this build because the
 * `repl-compression` config is capped to 0 (see config.c), so no compression
 * state is ever created and the read/write helpers simply forward to the
 * connection layer. */

int clientCreateCompressionState(client *c, compressionDirection dir) {
    UNUSED(c);
    UNUSED(dir);
    return 0;
}

void clientDestroyCompressionState(client *c) {
    UNUSED(c);
}

int clientEnableCompression(client *c, compressionDirection dir) {
    UNUSED(c);
    UNUSED(dir);
    return 0;
}

void clientDisableCompression(client *c) {
    c->io_flags &= ~CLIENT_IO_COMPRESSION_ENABLED;
}

int clientCompressAndWrite(client *c, int *tot_written, int force_flush) {
    UNUSED(c);
    UNUSED(force_flush);
    if (tot_written) *tot_written = 0;
    return 0;
}

int clientFlushCompressedData(client *c) {
    UNUSED(c);
    return 0;
}

int clientReadBufAndDecompress(client *c, char *input_buf, size_t input_len,
                             char *output_buf, size_t output_len, size_t *consumed)
{
    UNUSED(c);
    UNUSED(input_buf);
    UNUSED(input_len);
    UNUSED(output_buf);
    UNUSED(output_len);
    if (consumed) *consumed = 0;
    return 0;
}

int clientHasPendingCompressionFlush(client *c) {
    UNUSED(c);
    return 0;
}

int clientHasPendingCompressedData(client *c) {
    UNUSED(c);
    return 0;
}

size_t clientCompressionMemoryUsage(client *c) {
    UNUSED(c);
    return 0;
}

int clientConnWrite(client *c, const void *data, size_t len, int *nwritten) {
    int w = connWrite(c->conn, data, len);
    if (nwritten) *nwritten = (w > 0) ? w : 0;
    return w;
}

int clientConnRead(client *c, void *buf, size_t buf_len, int *nread_out) {
    int r = connRead(c->conn, buf, buf_len);
    if (nread_out) *nread_out = (r > 0) ? r : 0;
    return r;
}

int clientCompressionHasPendingData(struct aeEventLoop *el) {
    UNUSED(el);
    return 0;
}

int clientCompressionProcessPendingData(struct aeEventLoop *el) {
    UNUSED(el);
    return 0;
}

void clientCompressionPendingRemove(client *c) {
    UNUSED(c);
}

#endif /* USE_COMPRESSION */

