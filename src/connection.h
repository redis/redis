
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

#ifndef __REDIS_CONNECTION_H
#define __REDIS_CONNECTION_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include "ae.h"

#define CONN_INFO_LEN   32
#define CONN_ADDR_STR_LEN 128 /* Similar to INET6_ADDRSTRLEN, hoping to handle other protocols. */
#define MAX_ACCEPTS_PER_CALL 1000

struct aeEventLoop;
typedef struct connection connection;
typedef struct connListener connListener;

typedef enum {
    CONN_STATE_NONE = 0,
    CONN_STATE_CONNECTING,
    CONN_STATE_ACCEPTING,
    CONN_STATE_CONNECTED,
    CONN_STATE_CLOSED,
    CONN_STATE_ERROR
} ConnectionState;

#define CONN_FLAG_CLOSE_SCHEDULED   (1<<0)      /* Closed scheduled by a handler */
#define CONN_FLAG_WRITE_BARRIER     (1<<1)      /* Write barrier requested */

#define CONN_TYPE_SOCKET            "tcp"
#define CONN_TYPE_UNIX              "unix"
#define CONN_TYPE_TLS               "tls"
#define CONN_TYPE_MAX               8           /* 8 is enough to be extendable */

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

typedef struct ConnectionType {
    /* connection type */
    const char *(*get_type)(struct connection *conn);

    /* connection type initialize & finalize & configure */
    void (*init)(void); /* auto-call during register */
    void (*cleanup)(void);
    int (*configure)(void *priv, int reconfigure);

    /* ae & accept & listen & error & address handler */
    void (*ae_handler)(struct aeEventLoop *el, int fd, void *clientData, int mask);
    aeFileProc *accept_handler;
    int (*addr)(connection *conn, char *ip, size_t ip_len, int *port, int remote);
    int (*is_local)(connection *conn);
    int (*listen)(connListener *listener);

    /* create/shutdown/close connection */
    connection* (*conn_create)(void);
    connection* (*conn_create_accepted)(int fd, void *priv);
    void (*shutdown)(struct connection *conn);
    void (*close)(struct connection *conn);

    /* connect & accept */
    int (*connect)(struct connection *conn, const char *addr, int port, const char *source_addr, ConnectionCallbackFunc connect_handler);
    int (*blocking_connect)(struct connection *conn, const char *addr, int port, long long timeout);
    int (*accept)(struct connection *conn, ConnectionCallbackFunc accept_handler);

    /* IO */
    int (*write)(struct connection *conn, const void *data, size_t data_len);
    int (*writev)(struct connection *conn, const struct iovec *iov, int iovcnt);
    int (*read)(struct connection *conn, void *buf, size_t buf_len);
    int (*set_write_handler)(struct connection *conn, ConnectionCallbackFunc handler, int barrier);
    int (*set_read_handler)(struct connection *conn, ConnectionCallbackFunc handler);
    const char *(*get_last_error)(struct connection *conn);
    ssize_t (*sync_write)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_read)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_readline)(struct connection *conn, char *ptr, ssize_t size, long long timeout);

    /* pending data */
    int (*has_pending_data)(void);
    int (*process_pending_data)(void);

    /* TLS specified methods */
    sds (*get_peer_cert)(struct connection *conn);
} ConnectionType;

struct connection {
    ConnectionType *type;
    ConnectionState state;
    int last_errno;
    int fd;
    short int flags;
    short int refs;
    unsigned short int iovcnt;
    void *private_data;
    ConnectionCallbackFunc conn_handler;
    ConnectionCallbackFunc write_handler;
    ConnectionCallbackFunc read_handler;
};

#define CONFIG_BINDADDR_MAX 16

/* Setup a listener by a connection type */
struct connListener {
    int fd[CONFIG_BINDADDR_MAX];
    int count;
    char **bindaddr;
    int bindaddr_count;
    int port;
    ConnectionType *ct;
    void *priv; /* used by connection type specified data */
};

/* The connection module does not deal with listening and accepting sockets,
 * so we assume we have a socket when an incoming connection is created.
 *
 * The fd supplied should therefore be associated with an already accept()ed
 * socket.
 *
 * connAccept() may directly call accept_handler(), or return and call it
 * at a later time. This behavior is a bit awkward but aims to reduce the need
 * to wait for the next event loop, if no additional handshake is required.
 *
 * IMPORTANT: accept_handler may decide to close the connection, calling connClose().
 * To make this safe, the connection is only marked with CONN_FLAG_CLOSE_SCHEDULED
 * in this case, and connAccept() returns with an error.
 *
 * connAccept() callers must always check the return value and on error (C_ERR)
 * a connClose() must be called.
 */

static inline int connAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    return conn->type->accept(conn, accept_handler);
}

/* Establish a connection.  The connect_handler will be called when the connection
 * is established, or if an error has occurred.
 *
 * The connection handler will be responsible to set up any read/write handlers
 * as needed.
 *
 * If C_ERR is returned, the operation failed and the connection handler shall
 * not be expected.
 */
static inline int connConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler) {
    return conn->type->connect(conn, addr, port, src_addr, connect_handler);
}

/* Blocking connect.
 *
 * NOTE: This is implemented in order to simplify the transition to the abstract
 * connections, but should probably be refactored out of cluster.c and replication.c,
 * in favor of a pure async implementation.
 */
static inline int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    return conn->type->blocking_connect(conn, addr, port, timeout);
}

/* Write to connection, behaves the same as write(2).
 *
 * Like write(2), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connWrite(connection *conn, const void *data, size_t data_len) {
    return conn->type->write(conn, data, data_len);
}

/* Gather output data from the iovcnt buffers specified by the members of the iov
 * array: iov[0], iov[1], ..., iov[iovcnt-1] and write to connection, behaves the same as writev(3).
 *
 * Like writev(3), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    return conn->type->writev(conn, iov, iovcnt);
}

/* Read from the connection, behaves the same as read(2).
 * 
 * Like read(2), a short read is possible.  A return value of 0 will indicate the
 * connection was closed, and -1 will indicate an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connRead(connection *conn, void *buf, size_t buf_len) {
    int ret = conn->type->read(conn, buf, buf_len);
    return ret;
}

/* Register a write handler, to be called when the connection is writable.
 * If NULL, the existing handler is removed.
 */
static inline int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_write_handler(conn, func, 0);
}

/* Register a read handler, to be called when the connection is readable.
 * If NULL, the existing handler is removed.
 */
static inline int connSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_read_handler(conn, func);
}

/* Set a write handler, and possibly enable a write barrier, this flag is
 * cleared when write handler is changed or removed.
 * With barrier enabled, we never fire the event if the read handler already
 * fired in the same event loop iteration. Useful when you want to persist
 * things to disk before sending replies, and want to do that in a group fashion. */
static inline int connSetWriteHandlerWithBarrier(connection *conn, ConnectionCallbackFunc func, int barrier) {
    return conn->type->set_write_handler(conn, func, barrier);
}

static inline void connShutdown(connection *conn) {
    conn->type->shutdown(conn);
}

static inline void connClose(connection *conn) {
    conn->type->close(conn);
}

/* Returns the last error encountered by the connection, as a string.  If no error,
 * a NULL is returned.
 */
static inline const char *connGetLastError(connection *conn) {
    return conn->type->get_last_error(conn);
}

static inline ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_write(conn, ptr, size, timeout);
}

static inline ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_read(conn, ptr, size, timeout);
}

static inline ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_readline(conn, ptr, size, timeout);
}

/* Return CONN_TYPE_* for the specified connection */
static inline const char *connGetType(connection *conn) {
    return conn->type->get_type(conn);
}

static inline int connLastErrorRetryable(connection *conn) {
    return conn->last_errno == EINTR;
}

/* Get address information of a connection.
 * remote works as boolean type to get local/remote address */
static inline int connAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    if (conn && conn->type->addr) {
        return conn->type->addr(conn, ip, ip_len, port, remote);
    }

    return -1;
}

/* Format an IP,port pair into something easy to parse. If IP is IPv6
 * (matches for ":"), the ip is surrounded by []. IP and port are just
 * separated by colons. This the standard to display addresses within Redis. */
static inline int formatAddr(char *buf, size_t buf_len, char *ip, int port) {
    return snprintf(buf, buf_len, strchr(ip,':') ?
           "[%s]:%d" : "%s:%d", ip, port);
}

static inline int connFormatAddr(connection *conn, char *buf, size_t buf_len, int remote)
{
    char ip[CONN_ADDR_STR_LEN];
    int port;

    if (connAddr(conn, ip, sizeof(ip), &port, remote) < 0) {
        return -1;
    }

    return formatAddr(buf, buf_len, ip, port);
}

static inline int connAddrPeerName(connection *conn, char *ip, size_t ip_len, int *port) {
    return connAddr(conn, ip, ip_len, port, 1);
}

static inline int connAddrSockName(connection *conn, char *ip, size_t ip_len, int *port) {
    return connAddr(conn, ip, ip_len, port, 0);
}

/* Test a connection is local or loopback.
 * Return -1 on failure, 0 is not a local connection, 1 is a local connection */
static inline int connIsLocal(connection *conn) {
    if (conn && conn->type->is_local) {
        return conn->type->is_local(conn);
    }

    return -1;
}

static inline int connGetState(connection *conn) {
    return conn->state;
}

/* Returns true if a write handler is registered */
static inline int connHasWriteHandler(connection *conn) {
    return conn->write_handler != NULL;
}

/* Returns true if a read handler is registered */
static inline int connHasReadHandler(connection *conn) {
    return conn->read_handler != NULL;
}

/* Associate a private data pointer with the connection */
static inline void connSetPrivateData(connection *conn, void *data) {
    conn->private_data = data;
}

/* Get the associated private data pointer */
static inline void *connGetPrivateData(connection *conn) {
    return conn->private_data;
}

/* Return a text that describes the connection, suitable for inclusion
 * in CLIENT LIST and similar outputs.
 *
 * For sockets, we always return "fd=<fdnum>" to maintain compatibility.
 */
static inline const char *connGetInfo(connection *conn, char *buf, size_t buf_len) {
    snprintf(buf, buf_len-1, "fd=%i", conn == NULL ? -1 : conn->fd);
    return buf;
}

/* anet-style wrappers to conns */
int connBlock(connection *conn);
int connNonBlock(connection *conn);
int connEnableTcpNoDelay(connection *conn);
int connDisableTcpNoDelay(connection *conn);
int connKeepAlive(connection *conn, int interval);
int connSendTimeout(connection *conn, long long ms);
int connRecvTimeout(connection *conn, long long ms);

/* Get cert for the secure connection */
static inline sds connGetPeerCert(connection *conn) {
    if (conn->type->get_peer_cert) {
        return conn->type->get_peer_cert(conn);
    }

    return NULL;
}

/* Initialize the redis connection framework */
int connTypeInitialize(void);

/* Register a connection type into redis connection framework */
int connTypeRegister(ConnectionType *ct);

/* Lookup a connection type by type name */
ConnectionType *connectionByType(const char *typename);

/* Fast path to get TCP connection type */
ConnectionType *connectionTypeTcp(void);

/* Fast path to get TLS connection type */
ConnectionType *connectionTypeTls(void);

/* Fast path to get Unix connection type */
ConnectionType *connectionTypeUnix(void);

/* Lookup the index of a connection type by type name, return -1 if not found */
int connectionIndexByType(const char *typename);

/* Create a connection of specified type */
static inline connection *connCreate(ConnectionType *ct) {
    return ct->conn_create();
}

/* Create an accepted connection of specified type.
 * priv is connection type specified argument */
static inline connection *connCreateAccepted(ConnectionType *ct, int fd, void *priv) {
    return ct->conn_create_accepted(fd, priv);
}

/* Configure a connection type. A typical case is to configure TLS.
 * priv is connection type specified,
 * reconfigure is boolean type to specify if overwrite the original config */
static inline int connTypeConfigure(ConnectionType *ct, void *priv, int reconfigure) {
    return ct->configure(priv, reconfigure);
}

/* Walk all the connection types and cleanup them all if possible */
void connTypeCleanupAll(void);

/* Test all the connection type has pending data or not. */
int connTypeHasPendingData(void);

/* walk all the connection types and process pending data for each connection type */
int connTypeProcessPendingData(void);

/* Listen on an initialized listener */
static inline int connListen(connListener *listener) {
    return listener->ct->listen(listener);
}

/* Get accept_handler of a connection type */
static inline aeFileProc *connAcceptHandler(ConnectionType *ct) {
    if (ct)
        return ct->accept_handler;
    return NULL;
}

/* Get Listeners information, note that caller should free the non-empty string */
sds getListensInfoString(sds info);

int RedisRegisterConnectionTypeSocket(void);
int RedisRegisterConnectionTypeUnix(void);
int RedisRegisterConnectionTypeTLS(void);

/* Return 1 if connection is using TLS protocol, 0 if otherwise. */
static inline int connIsTLS(connection *conn) {
    return conn && conn->type == connectionTypeTls();
}

#endif  /* __REDIS_CONNECTION_H */
