
/*
 * Copyright (c) 2019, Redis Labs
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __REDIS_CONNHELPERS_H
#define __REDIS_CONNHELPERS_H

#include "connection.h"

/* These are helper functions that are common to different connection
 * implementations (currently sockets in connection.c and TLS in tls.c).
 *
 * Currently helpers implement the mechanisms for invoking connection
 * handlers, tracking in-handler states and dealing with deferred
 * destruction (if invoked by a handler).
 */

/* Called whenever a handler is invoked on a connection and sets the
 * CONN_FLAG_IN_HANDLER flag to indicate we're in a handler context.
 *
 * An attempt to close a connection while CONN_FLAG_IN_HANDLER is
 * set will result with deferred close, i.e. setting the CONN_FLAG_CLOSE_SCHEDULED
 * instead of destructing it.
 */
static inline void enterHandler(connection *conn) {
    conn->flags |= CONN_FLAG_IN_HANDLER;
}

/* Called whenever a handler returns. This unsets the CONN_FLAG_IN_HANDLER
 * flag and performs actual close/destruction if a deferred close was
 * scheduled by the handler.
 */
static inline int exitHandler(connection *conn) {
    conn->flags &= ~CONN_FLAG_IN_HANDLER;
    if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) {
        connClose(conn);
        return 0;
    }
    return 1;
}

/* Helper for connection implementations to call handlers:
 * 1. Mark the handler in use.
 * 2. Execute the handler (if set).
 * 3. Mark the handler as NOT in use and perform deferred close if was
 * requested by the handler at any time.
 */
static inline int callHandler(connection *conn, ConnectionCallbackFunc handler) {
    conn->flags |= CONN_FLAG_IN_HANDLER;
    if (handler) handler(conn);
    conn->flags &= ~CONN_FLAG_IN_HANDLER;
    if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) {
        connClose(conn);
        return 0;
    }
    return 1;
}

#endif  /* __REDIS_CONNHELPERS_H */
