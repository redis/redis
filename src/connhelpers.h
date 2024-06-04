
/*
 * Copyright (c) 2019-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef __REDIS_CONNHELPERS_H
#define __REDIS_CONNHELPERS_H

#include "connection.h"

/* These are helper functions that are common to different connection
 * implementations (currently sockets in connection.c and TLS in tls.c).
 *
 * Currently helpers implement the mechanisms for invoking connection
 * handlers and tracking connection references, to allow safe destruction
 * of connections from within a handler.
 */

/* Increment connection references.
 *
 * Inside a connection handler, we guarantee refs >= 1 so it is always
 * safe to connClose().
 *
 * In other cases where we don't want to prematurely lose the connection,
 * it can go beyond 1 as well; currently it is only done by connAccept().
 */
static inline void connIncrRefs(connection *conn) {
    conn->refs++;
}

/* Decrement connection references.
 *
 * Note that this is not intended to provide any automatic free logic!
 * callHandler() takes care of that for the common flows, and anywhere an
 * explicit connIncrRefs() is used, the caller is expected to take care of
 * that.
 */

static inline void connDecrRefs(connection *conn) {
    conn->refs--;
}

static inline int connHasRefs(connection *conn) {
    return conn->refs;
}

/* Helper for connection implementations to call handlers:
 * 1. Increment refs to protect the connection.
 * 2. Execute the handler (if set).
 * 3. Decrement refs and perform deferred close, if refs==0.
 */
static inline int callHandler(connection *conn, ConnectionCallbackFunc handler) {
    connIncrRefs(conn);
    if (handler) handler(conn);
    connDecrRefs(conn);
    if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) {
        if (!connHasRefs(conn)) connClose(conn);
        return 0;
    }
    return 1;
}

#endif  /* __REDIS_CONNHELPERS_H */
