/* io_uring based async connection interface
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __CONN_IOURING_H
#define __CONN_IOURING_H

#include "config.h"

#ifdef HAVE_IOURING

#include "connection.h"

/* Initialize io_uring subsystem */
int iouringInit(void);

/* Cleanup io_uring subsystem */
void iouringCleanup(void);

/* Submit an async read request */
int connIoUringAsyncRead(connection *conn);

/* Submit an async write request */
int connIoUringAsyncWrite(connection *conn, const void *data, size_t data_len);

/* Submit all pending io_uring requests */
int connIoUringSubmit(void);

/* Process completed io_uring operations */
int connIoUringProcessCompletions(void);

/* Register io_uring connection type */
int RedisRegisterConnectionTypeIoUring(void);

/* Get io_uring connection type */
ConnectionType *connectionTypeIoUring(void);

#else /* !HAVE_IOURING */

/* Stub implementations when io_uring is not available */
static inline int iouringInit(void) { return -1; }
static inline void iouringCleanup(void) {}
static inline int connIoUringAsyncRead(connection *conn) { (void)conn; return -1; }
static inline int connIoUringAsyncWrite(connection *conn, const void *data, size_t data_len) {
    (void)conn; (void)data; (void)data_len; return -1;
}
static inline int connIoUringSubmit(void) { return 0; }
static inline int connIoUringProcessCompletions(void) { return 0; }
static inline int RedisRegisterConnectionTypeIoUring(void) { return -1; }
static inline ConnectionType *connectionTypeIoUring(void) { return NULL; }

#endif /* HAVE_IOURING */

#endif /* __CONN_IOURING_H */
