/* ==========================================================================
 * unix.c - unix socket connection implementation
 * --------------------------------------------------------------------------
 * Copyright (C) 2022  zhenwei pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */

#include "server.h"
#include "connection.h"

static ConnectionType CT_Unix;

static const char *connUnixGetType(connection *conn) {
    UNUSED(conn);

    return CONN_TYPE_UNIX;
}

static void connUnixEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    connectionTypeTcp()->ae_handler(el, fd, clientData, mask);
}

static int connUnixAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    return connectionTypeTcp()->addr(conn, ip, ip_len, port, remote);
}

static int connUnixIsLocal(connection *conn) {
    UNUSED(conn);

    return 1; /* Unix socket is always local connection */
}

static int connUnixListen(connListener *listener) {
    int fd;
    mode_t *perm = (mode_t *)listener->priv;

    if (listener->bindaddr_count == 0)
        return C_OK;

    /* currently listener->bindaddr_count is always 1, we still use a loop here in case Redis supports multi Unix socket in the future */
    for (int j = 0; j < listener->bindaddr_count; j++) {
        char *addr = listener->bindaddr[j];

        unlink(addr); /* don't care if this fails */
        fd = anetUnixServer(server.neterr, addr, *perm, server.tcp_backlog);
        if (fd == ANET_ERR) {
            serverLog(LL_WARNING, "Failed opening Unix socket: %s", server.neterr);
            exit(1);
        }
        anetNonBlock(NULL, fd);
        anetCloexec(fd);
        listener->fd[listener->count++] = fd;
    }

    return C_OK;
}

static connection *connCreateUnix(void) {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = &CT_Unix;
    conn->fd = -1;
    conn->iovcnt = IOV_MAX;

    return conn;
}

static connection *connCreateAcceptedUnix(int fd, void *priv) {
    UNUSED(priv);
    connection *conn = connCreateUnix();
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    return conn;
}

static void connUnixAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd;
    int max = server.max_new_conns_per_cycle;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted connection to %s", server.unixsocket);
        acceptCommonHandler(connCreateAcceptedUnix(cfd, NULL),CLIENT_UNIX_SOCKET,NULL);
    }
}

static void connUnixShutdown(connection *conn) {
    connectionTypeTcp()->shutdown(conn);
}

static void connUnixClose(connection *conn) {
    connectionTypeTcp()->close(conn);
}

static int connUnixAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    return connectionTypeTcp()->accept(conn, accept_handler);
}

static int connUnixWrite(connection *conn, const void *data, size_t data_len) {
    return connectionTypeTcp()->write(conn, data, data_len);
}

static int connUnixWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    return connectionTypeTcp()->writev(conn, iov, iovcnt);
}

static int connUnixRead(connection *conn, void *buf, size_t buf_len) {
    return connectionTypeTcp()->read(conn, buf, buf_len);
}

static int connUnixSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    return connectionTypeTcp()->set_write_handler(conn, func, barrier);
}

static int connUnixSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    return connectionTypeTcp()->set_read_handler(conn, func);
}

static const char *connUnixGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static ssize_t connUnixSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connUnixSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connUnixSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncReadLine(conn->fd, ptr, size, timeout);
}

static ConnectionType CT_Unix = {
    /* connection type */
    .get_type = connUnixGetType,

    /* connection type initialize & finalize & configure */
    .init = NULL,
    .cleanup = NULL,
    .configure = NULL,

    /* ae & accept & listen & error & address handler */
    .ae_handler = connUnixEventHandler,
    .accept_handler = connUnixAcceptHandler,
    .addr = connUnixAddr,
    .is_local = connUnixIsLocal,
    .listen = connUnixListen,

    /* create/shutdown/close connection */
    .conn_create = connCreateUnix,
    .conn_create_accepted = connCreateAcceptedUnix,
    .shutdown = connUnixShutdown,
    .close = connUnixClose,

    /* connect & accept */
    .connect = NULL,
    .blocking_connect = NULL,
    .accept = connUnixAccept,

    /* IO */
    .write = connUnixWrite,
    .writev = connUnixWritev,
    .read = connUnixRead,
    .set_write_handler = connUnixSetWriteHandler,
    .set_read_handler = connUnixSetReadHandler,
    .get_last_error = connUnixGetLastError,
    .sync_write = connUnixSyncWrite,
    .sync_read = connUnixSyncRead,
    .sync_readline = connUnixSyncReadLine,

    /* pending data */
    .has_pending_data = NULL,
    .process_pending_data = NULL,
};

int RedisRegisterConnectionTypeUnix(void)
{
    return connTypeRegister(&CT_Unix);
}
