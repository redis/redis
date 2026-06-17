/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __CLIENT_COMP_H
#define __CLIENT_COMP_H

#include <sys/types.h>

/* Opaque handle to client's compression state used internally by client_comp */
typedef struct compressionState compressionState;

struct client;
struct aeEventLoop;

typedef enum {
    CD_INVALID,
    COMPRESS,
    DECOMPRESS,
} compressionDirection;

int clientCreateCompressionState(struct client *c, compressionDirection dir);
void clientDestroyCompressionState(struct client *c);

int clientEnableCompression(struct client *c, compressionDirection dir);

void clientDisableCompression(struct client *c);

int compressAndWrite(struct client *c, int *tot_written);

int readFromBufAndDecompress(struct client *c, char *input_buf, size_t input_len,
                             char *output_buf, size_t output_len,
                             size_t *consumed);

int clientHasPendingCompressionFlush(struct client *c);
int clientHasPendingCompressedData(struct client *c);

/* Client-level codec API. These are the networking-facing wrappers that apply
 * the client's compressor (codec) on top of the *real* connection, i.e the
 * compression/decompression happens outside of the connection layer.
 *
 * clientConnWrite compresses `len` bytes from `data` and writes them to the
 * client's connection. It returns the number of *uncompressed* bytes consumed
 * (so the caller can advance its source buffer) and stores the number of bytes
 * actually written to the socket in *nwritten. Returns -1 on a connection error.
 *
 * clientConnRead reads from the client's connection and decompresses into `buf`.
 * It returns the number of *decompressed* bytes placed in `buf` (read(2)-like
 * semantics: 0 means EOF, -1 means error/no-data) and stores the number of bytes
 * actually read from the socket in *nread.
 *
 * When the client has no active compressor for the relevant direction both
 * simply forward to connWrite/connRead and set the out-param equal to the
 * return value. */
ssize_t clientConnWrite(struct client *c, const void *data, size_t len, size_t *nwritten);
ssize_t clientConnRead(struct client *c, void *buf, size_t buf_len, size_t *nread);

/* Whether the client currently has an active compressor for the given direction. */
int clientIsCompressing(struct client *c);
int clientIsDecompressing(struct client *c);

/* Per-event-loop pending decompressed data handling. A decompressor may hold
 * data in its internal buffers that hasn't been delivered to the query buffer
 * yet, with no socket event to wake us up. These drive that drain from the
 * event loop's beforeSleep, alongside connTypeProcessPendingData. */
int clientCompressionHasPendingData(struct aeEventLoop *el);
int clientCompressionProcessPendingData(struct aeEventLoop *el);

/* Remove a client from its event loop's pending decompression list. Must be
 * called before the client's connection is unbound from its event loop. */
void clientCompressionPendingRemove(struct client *c);

#endif /* __CLIENT_COMP_H */
