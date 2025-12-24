/* io_uring based async connection implementation
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * This file implements an io_uring-based connection type that provides
 * true asynchronous I/O operations. Unlike the poll-based ae_iouring.c,
 * this implementation uses io_uring for the actual read/write operations,
 * allowing for batched submissions and reduced syscall overhead.
 */

#include "config.h"

#ifdef HAVE_IOURING

#include "server.h"
#include "connhelpers.h"
#include <liburing.h>
#include <sys/uio.h>
#include <limits.h>

/* IOV_MAX might not be defined, use a fallback */
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

/* io_uring operation types */
#define IOURING_OP_READ     1
#define IOURING_OP_WRITE    2
#define IOURING_OP_CONNECT  3
#define IOURING_OP_ACCEPT   4

/* Buffer configuration */
#define IOURING_READ_BUF_SIZE   (16 * 1024)  /* 16KB read buffer */
#define IOURING_WRITE_BATCH_MAX 64           /* Max iovec entries per write */

/* Global io_uring instance for async I/O operations.
 * This is separate from the event loop ring to allow for
 * independent submission and completion handling. */
static struct io_uring g_io_ring;
static int g_io_ring_initialized = 0;

/* Per-connection io_uring state */
typedef struct iouringConnState {
    /* Read state */
    char *read_buf;              /* Pre-allocated read buffer */
    size_t read_buf_size;        /* Size of read buffer */
    size_t read_buf_used;        /* Bytes available in buffer */
    size_t read_buf_pos;         /* Current read position */
    int read_pending;            /* Read request is pending */

    /* Write state */
    int write_pending;           /* Write request is pending */
    size_t write_submitted;      /* Bytes submitted for writing */

    /* Connection state tracking */
    int connect_pending;         /* Connect in progress */
} iouringConnState;

static ConnectionType CT_IoUring;

/* ============================================================================
 * io_uring initialization and cleanup
 * ========================================================================== */

/* Initialize the global io_uring instance for async I/O */
int iouringInit(void) {
    if (g_io_ring_initialized) return C_OK;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    /* Initialize with a reasonable queue depth */
    int ret = io_uring_queue_init_params(4096, &g_io_ring, &params);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to initialize io_uring for async I/O: %s",
                  strerror(-ret));
        return C_ERR;
    }

    g_io_ring_initialized = 1;
    serverLog(LL_NOTICE, "io_uring async I/O initialized");
    return C_OK;
}

/* Cleanup the global io_uring instance */
void iouringCleanup(void) {
    if (g_io_ring_initialized) {
        io_uring_queue_exit(&g_io_ring);
        g_io_ring_initialized = 0;
    }
}

/* ============================================================================
 * Connection creation and destruction
 * ========================================================================== */

static connection *connCreateIoUring(struct aeEventLoop *el) {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = &CT_IoUring;
    conn->fd = -1;
    conn->iovcnt = IOV_MAX;
    conn->el = el;

    /* Allocate io_uring specific state */
    iouringConnState *state = zcalloc(sizeof(iouringConnState));
    state->read_buf = zmalloc(IOURING_READ_BUF_SIZE);
    state->read_buf_size = IOURING_READ_BUF_SIZE;
    state->read_buf_used = 0;
    state->read_buf_pos = 0;
    state->read_pending = 0;
    state->write_pending = 0;
    state->connect_pending = 0;

    /* Store state in connection's private data for now.
     * In a full implementation, we might extend the connection struct. */
    conn->private_data = state;

    return conn;
}

static connection *connCreateAcceptedIoUring(struct aeEventLoop *el, int fd, void *priv) {
    UNUSED(priv);
    connection *conn = connCreateIoUring(el);
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    return conn;
}

static void connIoUringClose(connection *conn) {
    if (conn->fd != -1) {
        if (conn->el) aeDeleteFileEvent(conn->el, conn->fd, AE_READABLE | AE_WRITABLE);
        close(conn->fd);
        conn->fd = -1;
    }

    /* Free io_uring state */
    if (conn->private_data) {
        iouringConnState *state = conn->private_data;
        if (state->read_buf) zfree(state->read_buf);
        zfree(state);
        conn->private_data = NULL;
    }

    if (connHasRefs(conn)) {
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }

    zfree(conn);
}

static void connIoUringShutdown(connection *conn) {
    if (conn->fd == -1) return;
    shutdown(conn->fd, SHUT_RDWR);
}

/* ============================================================================
 * Synchronous I/O operations (fallback for compatibility)
 * ========================================================================== */

static int connIoUringWrite(connection *conn, const void *data, size_t data_len) {
    /* For now, use synchronous write. In a full implementation,
     * this would submit to io_uring and return immediately. */
    int ret = write(conn->fd, data, data_len);
    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }
    return ret;
}

static int connIoUringWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    int ret = writev(conn->fd, iov, iovcnt);
    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }
    return ret;
}

static int connIoUringRead(connection *conn, void *buf, size_t buf_len) {
    iouringConnState *state = conn->private_data;

    /* Check if we have buffered data from a previous async read */
    if (state && state->read_buf_used > state->read_buf_pos) {
        size_t available = state->read_buf_used - state->read_buf_pos;
        size_t to_copy = (buf_len < available) ? buf_len : available;
        memcpy(buf, state->read_buf + state->read_buf_pos, to_copy);
        state->read_buf_pos += to_copy;

        /* Reset buffer if fully consumed */
        if (state->read_buf_pos >= state->read_buf_used) {
            state->read_buf_pos = 0;
            state->read_buf_used = 0;
        }
        return to_copy;
    }

    /* No buffered data, do synchronous read */
    int ret = read(conn->fd, buf, buf_len);
    if (!ret) {
        conn->state = CONN_STATE_CLOSED;
    } else if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }
    return ret;
}

/* ============================================================================
 * Async I/O operations using io_uring
 * ========================================================================== */

/* Submit an async read request to io_uring */
int connIoUringAsyncRead(connection *conn) {
    if (!g_io_ring_initialized) return C_ERR;

    iouringConnState *state = conn->private_data;
    if (!state || state->read_pending) return C_ERR;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_io_ring);
    if (!sqe) {
        /* Queue full, try to flush */
        io_uring_submit(&g_io_ring);
        sqe = io_uring_get_sqe(&g_io_ring);
        if (!sqe) return C_ERR;
    }

    /* Prepare read request */
    io_uring_prep_recv(sqe, conn->fd, state->read_buf, state->read_buf_size, 0);
    io_uring_sqe_set_data(sqe, conn);

    state->read_pending = 1;
    return C_OK;
}

/* Submit an async write request to io_uring */
int connIoUringAsyncWrite(connection *conn, const void *data, size_t data_len) {
    if (!g_io_ring_initialized) return C_ERR;

    iouringConnState *state = conn->private_data;
    if (!state || state->write_pending) return C_ERR;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_io_ring);
    if (!sqe) {
        io_uring_submit(&g_io_ring);
        sqe = io_uring_get_sqe(&g_io_ring);
        if (!sqe) return C_ERR;
    }

    /* Prepare write request */
    io_uring_prep_send(sqe, conn->fd, data, data_len, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, conn);

    state->write_pending = 1;
    state->write_submitted = data_len;
    return C_OK;
}

/* Submit batched requests */
int connIoUringSubmit(void) {
    if (!g_io_ring_initialized) return 0;
    return io_uring_submit(&g_io_ring);
}

/* Process completed io_uring operations */
int connIoUringProcessCompletions(void) {
    if (!g_io_ring_initialized) return 0;

    struct io_uring_cqe *cqe;
    unsigned head;
    int processed = 0;

    io_uring_for_each_cqe(&g_io_ring, head, cqe) {
        connection *conn = io_uring_cqe_get_data(cqe);
        if (!conn) {
            processed++;
            continue;
        }

        iouringConnState *state = conn->private_data;
        int res = cqe->res;

        if (state->read_pending) {
            state->read_pending = 0;
            if (res > 0) {
                state->read_buf_used = res;
                state->read_buf_pos = 0;
                /* Trigger read handler */
                if (conn->read_handler) {
                    conn->read_handler(conn);
                }
            } else if (res == 0) {
                conn->state = CONN_STATE_CLOSED;
                if (conn->read_handler) conn->read_handler(conn);
            } else {
                conn->last_errno = -res;
                if (-res != EAGAIN && conn->state == CONN_STATE_CONNECTED)
                    conn->state = CONN_STATE_ERROR;
            }
        }

        if (state->write_pending) {
            state->write_pending = 0;
            if (res < 0) {
                conn->last_errno = -res;
                if (-res != EAGAIN && conn->state == CONN_STATE_CONNECTED)
                    conn->state = CONN_STATE_ERROR;
            }
            /* Trigger write handler if registered */
            if (conn->write_handler) {
                conn->write_handler(conn);
            }
        }

        processed++;
    }

    io_uring_cq_advance(&g_io_ring, processed);
    return processed;
}

/* ============================================================================
 * Connection and event handlers
 * ========================================================================== */

static int connIoUringConnect(connection *conn, const char *addr, int port,
                               const char *src_addr, ConnectionCallbackFunc connect_handler) {
    int fd = anetTcpNonBlockBestEffortBindConnect(NULL, addr, port, src_addr);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTING;
    conn->conn_handler = connect_handler;

    aeCreateFileEvent(conn->el, conn->fd, AE_WRITABLE,
                      conn->type->ae_handler, conn);
    return C_OK;
}

static int connIoUringBlockingConnect(connection *conn, const char *addr, int port,
                                       long long timeout) {
    int fd = anetTcpNonBlockConnect(NULL, addr, port);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    if ((aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE) == 0) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = ETIMEDOUT;
        close(fd);
        return C_ERR;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTED;
    return C_OK;
}

static int connIoUringAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    if (conn->state != CONN_STATE_ACCEPTING) return C_ERR;
    conn->state = CONN_STATE_CONNECTED;

    connIncrRefs(conn);
    int ret = C_OK;
    if (!callHandler(conn, accept_handler)) ret = C_ERR;
    connDecrRefs(conn);

    return ret;
}

static void connIoUringEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    UNUSED(el);
    UNUSED(fd);
    connection *conn = clientData;

    if (conn->state == CONN_STATE_CONNECTING && (mask & AE_WRITABLE) && conn->conn_handler) {
        int conn_error = anetGetError(conn->fd);
        if (conn_error) {
            conn->last_errno = conn_error;
            conn->state = CONN_STATE_ERROR;
        } else {
            conn->state = CONN_STATE_CONNECTED;
        }

        if (!conn->write_handler) aeDeleteFileEvent(conn->el, conn->fd, AE_WRITABLE);
        if (!callHandler(conn, conn->conn_handler)) return;
        conn->conn_handler = NULL;
    }

    int invert = conn->flags & CONN_FLAG_WRITE_BARRIER;
    int call_write = (mask & AE_WRITABLE) && conn->write_handler;
    int call_read = (mask & AE_READABLE) && conn->read_handler;

    if (!invert && call_read) {
        if (!callHandler(conn, conn->read_handler)) return;
    }
    if (call_write) {
        if (!callHandler(conn, conn->write_handler)) return;
    }
    if (invert && call_read) {
        if (!callHandler(conn, conn->read_handler)) return;
    }
}

static int connIoUringSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    if (func == conn->write_handler) return C_OK;

    conn->write_handler = func;
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;

    if (!conn->write_handler)
        aeDeleteFileEvent(conn->el, conn->fd, AE_WRITABLE);
    else if (aeCreateFileEvent(conn->el, conn->fd, AE_WRITABLE,
                                conn->type->ae_handler, conn) == AE_ERR)
        return C_ERR;

    return C_OK;
}

static int connIoUringSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    if (func == conn->read_handler) return C_OK;

    conn->read_handler = func;
    if (!conn->read_handler)
        aeDeleteFileEvent(conn->el, conn->fd, AE_READABLE);
    else if (aeCreateFileEvent(conn->el, conn->fd, AE_READABLE,
                                conn->type->ae_handler, conn) == AE_ERR)
        return C_ERR;

    return C_OK;
}

static int connIoUringRebindEventLoop(connection *conn, aeEventLoop *el) {
    serverAssert(!conn->el && !conn->read_handler && !conn->write_handler);
    conn->el = el;
    return C_OK;
}

static const char *connIoUringGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static int connIoUringAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    if (anetFdToString(conn->fd, ip, ip_len, port, remote) == 0)
        return C_OK;
    conn->last_errno = errno;
    return C_ERR;
}

static int connIoUringIsLocal(connection *conn) {
    char cip[NET_IP_STR_LEN + 1] = {0};
    if (connIoUringAddr(conn, cip, sizeof(cip) - 1, NULL, 1) == C_ERR)
        return -1;
    return !strncmp(cip, "127.", 4) || !strcmp(cip, "::1");
}

static int connIoUringListen(connListener *listener) {
    return listenToPort(listener);
}

static void connIoUringAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = server.max_new_conns_per_cycle;
    char cip[NET_IP_STR_LEN];
    UNUSED(mask);
    UNUSED(privdata);

    while (max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (anetAcceptFailureNeedsRetry(errno))
                continue;
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING, "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE, "Accepted %s:%d (io_uring)", cip, cport);
        acceptCommonHandler(connCreateAcceptedIoUring(el, cfd, NULL), 0, cip);
    }
}

static ssize_t connIoUringSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connIoUringSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connIoUringSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncReadLine(conn->fd, ptr, size, timeout);
}

static const char *connIoUringGetType(connection *conn) {
    UNUSED(conn);
    return "io_uring_tcp";
}

/* ============================================================================
 * Connection type definition
 * ========================================================================== */

static ConnectionType CT_IoUring = {
    /* connection type */
    .get_type = connIoUringGetType,

    /* connection type initialize & finalize & configure */
    .init = NULL,
    .cleanup = iouringCleanup,
    .configure = NULL,

    /* ae & accept & listen & error & address handler */
    .ae_handler = connIoUringEventHandler,
    .accept_handler = connIoUringAcceptHandler,
    .addr = connIoUringAddr,
    .is_local = connIoUringIsLocal,
    .listen = connIoUringListen,

    /* create/shutdown/close connection */
    .conn_create = connCreateIoUring,
    .conn_create_accepted = connCreateAcceptedIoUring,
    .shutdown = connIoUringShutdown,
    .close = connIoUringClose,

    /* connect & accept */
    .connect = connIoUringConnect,
    .blocking_connect = connIoUringBlockingConnect,
    .accept = connIoUringAccept,

    /* event loop */
    .unbind_event_loop = NULL,
    .rebind_event_loop = connIoUringRebindEventLoop,

    /* IO */
    .write = connIoUringWrite,
    .writev = connIoUringWritev,
    .read = connIoUringRead,
    .set_write_handler = connIoUringSetWriteHandler,
    .set_read_handler = connIoUringSetReadHandler,
    .get_last_error = connIoUringGetLastError,
    .sync_write = connIoUringSyncWrite,
    .sync_read = connIoUringSyncRead,
    .sync_readline = connIoUringSyncReadLine,

    /* pending data */
    .has_pending_data = NULL,
    .process_pending_data = NULL,
};

/* Register io_uring connection type */
int RedisRegisterConnectionTypeIoUring(void) {
    if (iouringInit() == C_ERR) {
        return C_ERR;
    }
    return connTypeRegister(&CT_IoUring);
}

/* Get io_uring connection type */
ConnectionType *connectionTypeIoUring(void) {
    static ConnectionType *ct_iouring = NULL;
    static int cached = 0;

    if (!cached) {
        cached = 1;
        ct_iouring = connectionByType("io_uring_tcp");
    }
    return ct_iouring;
}

#else /* !HAVE_IOURING */

/* Empty compilation unit when io_uring is not available */
typedef int make_iso_compilers_happy;

#endif /* HAVE_IOURING */
