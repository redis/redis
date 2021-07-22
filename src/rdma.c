/* ==========================================================================
 * rdma.c - Support RDMA protocol for transport layer. Instead of IB verbs
 *          low-level API, Use rsocket which is implemented by rdma-core to
 *          make this module easy to implement/maintain.
 * --------------------------------------------------------------------------
 * Copyright (C) 2021  zhenwei pi
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
#include "connhelpers.h"

static void serverNetError(char *err, const char *fmt, ...) {
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

#ifdef USE_RDMA
#ifdef __linux__    /* currently RDMA is only supported on Linux */
#include <arpa/inet.h>
#include <netdb.h>
#include <rdma/rsocket.h>
#include <sys/types.h>
#include <fcntl.h>

void closeSocketListeners(socketFds *sfd);

static int rsocketSetBlock(int fd, int block) {
    int flags;

    if ((flags = rfcntl(fd, F_GETFL)) == -1) {
        serverLog(LL_WARNING, "RDMA: rfcntl F_GETFL failed");
        return ANET_ERR;
    }

    if (block)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (rfcntl(fd, F_SETFL, flags) == -1) {
        serverLog(LL_WARNING, "RDMA: rfcntl F_SETFL failed");
        return ANET_ERR;
    }

    return ANET_OK;
}

int rsocketGetSocketError(connection *conn) {
    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);

    if (rgetsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1) {
        sockerr = errno;
    }

    return sockerr;
}

static void connRdmaEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask)
{
    UNUSED(el);
    UNUSED(fd);
    connection *conn = clientData;

    if (conn->state == CONN_STATE_CONNECTING &&
            (mask & AE_WRITABLE) && conn->conn_handler) {

        int conn_error = rsocketGetSocketError(conn);
        if (conn_error) {
            conn->last_errno = conn_error;
            conn->state = CONN_STATE_ERROR;
        } else {
            conn->state = CONN_STATE_CONNECTED;
        }

        if (!conn->write_handler) {
            aeDeleteFileEvent(server.el, conn->fd, AE_WRITABLE);
        }

        if (!callHandler(conn, conn->conn_handler)) {
            return;
        }

        conn->conn_handler = NULL;
    }

    int invert = conn->flags & CONN_FLAG_WRITE_BARRIER;
    int call_write = (mask & AE_WRITABLE) && conn->write_handler;
    int call_read = (mask & AE_READABLE) && conn->read_handler;

    if (!invert && call_read) {
        if (!callHandler(conn, conn->read_handler)) {
            return;
	}
    }

    if (call_write) {
        if (!callHandler(conn, conn->write_handler)) {
           return;
        }
    }

    if (invert && call_read) {
        if (!callHandler(conn, conn->read_handler)) {
            return;
        }
    }
}

static void connRdmaClose(connection *conn) {
    if (conn->fd != -1) {
        aeDeleteFileEvent(server.el,conn->fd, AE_READABLE | AE_WRITABLE);
        rclose(conn->fd);
        conn->fd = -1;
    }

    if (connHasRefs(conn)) {
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }

    zfree(conn);
}

static int connRdmaWrite(connection *conn, const void *data, size_t data_len) {
    int ret = rsend(conn->fd, data, data_len, 0);

    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;

        if (conn->state == CONN_STATE_CONNECTED) {
            conn->state = CONN_STATE_ERROR;
        }
    }

    return ret;
}

static int connRdmaRead(connection *conn, void *buf, size_t buf_len) {
    int ret = rrecv(conn->fd, buf, buf_len, 0);

    if (!ret) {
        conn->state = CONN_STATE_CLOSED;
    } else if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;

        if (conn->state == CONN_STATE_CONNECTED) {
            conn->state = CONN_STATE_ERROR;
        }
    }

    return ret;
}

static int connRdmaAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    int ret = C_OK;

    if (conn->state != CONN_STATE_ACCEPTING) {
        return C_ERR;
    }

    conn->state = CONN_STATE_CONNECTED;
    connIncrRefs(conn);

    if (!callHandler(conn, accept_handler)) {
        ret = C_ERR;
    }
    connDecrRefs(conn);

    return ret;
}

static int connRdmaSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    if (func == conn->write_handler) {
        return C_OK;
    }

    conn->write_handler = func;
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;

    if (!conn->write_handler) {
        aeDeleteFileEvent(server.el, conn->fd, AE_WRITABLE);
    } else {
        if (aeCreateFileEvent(server.el, conn->fd, AE_WRITABLE,
                    conn->type->ae_handler, conn) == AE_ERR) {
            return C_ERR;
        }
    }

    return C_OK;
}

static int connRdmaSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    if (func == conn->read_handler) {
        return C_OK;
    }

    conn->read_handler = func;
    if (!conn->read_handler) {
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE);
    } else {
        if (aeCreateFileEvent(server.el,conn->fd, AE_READABLE,
                              conn->type->ae_handler, conn) == AE_ERR) {
            return C_ERR;
        }
    }

    return C_OK;
}

static const char *connRdmaGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static int _connRdmaConnect(connection *conn, const char *addr, int port, const char *src_addr) {
    struct addrinfo hints, *servinfo = NULL, *cliinfo = NULL, *psrv, *pcli;
    char _port[6];
    int ret;

    /* getaddrinfo for server side */
    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ((ret = getaddrinfo(addr, _port, &hints, &servinfo)) != 0) {
        hints.ai_family = AF_INET6;
        if ((ret = getaddrinfo(addr, _port, &hints, &servinfo)) != 0) {
            serverLog(LL_WARNING, "RDMA: try to connect %s:%s, getaddrinfo failed, %s",
                      addr, _port, gai_strerror(ret));
            goto err;
        }
    }

    /* getaddrinfo for client side */
    if (src_addr) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(src_addr, NULL, &hints, &cliinfo) != 0) {
            hints.ai_family = AF_INET6;
            if (getaddrinfo(src_addr, NULL, &hints, &servinfo) != 0) {
                serverLog(LL_WARNING, "RDMA: try to bind %s, getaddrinfo failed, %s",
                          src_addr, gai_strerror(ret));
                goto err;
            }
        }
    }

    for (psrv = servinfo; psrv != NULL; psrv = psrv->ai_next) {
        conn->fd = rsocket(psrv->ai_family, psrv->ai_socktype, psrv->ai_protocol);
        if (conn->fd == -1) {
            continue;
        }

        for (pcli = cliinfo; pcli != NULL; pcli = pcli->ai_next) {
            if (rbind(conn->fd, pcli->ai_addr, pcli->ai_addrlen) != -1) {
                break;
            }
        }

        if (rsocketSetBlock(conn->fd, 0) != ANET_OK) {
            goto err;
        }

        ret = rconnect(conn->fd, psrv->ai_addr, psrv->ai_addrlen);
        if (ret == -1) {
            if (errno != EINPROGRESS) {
                serverLog(LL_WARNING, "RDMA: connect failed, %s", strerror(errno));
	        goto err;
            }
        }

        break;
    }

    ret = ANET_OK;
    goto out;

err:
    ret = ANET_ERR;
    if (conn->fd != -1) {
        rclose(conn->fd);
        conn->fd = -1;
    }

out:
    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    if(cliinfo) {
        freeaddrinfo(cliinfo);
    }

    return ret;
}

static int connRdmaWait(int fd, int events, long long timeoutms) {
    struct pollfd pfd = {0};
    int retval;

    pfd.fd = fd;
    pfd.events = events;

    retval = rpoll(&pfd, 1, timeoutms);
    if (retval < 0) {
        return C_ERR;
    }

    return pfd.revents;
}

static int connRdmaConnect(connection *conn, const char *addr, int port, const char *src_addr, ConnectionCallbackFunc connect_handler) {
    if (_connRdmaConnect(conn, addr, port, src_addr) != ANET_OK) {
        return C_ERR;
    }

    conn->state = CONN_STATE_CONNECTING;
    conn->conn_handler = connect_handler;
    aeCreateFileEvent(server.el, conn->fd, AE_WRITABLE, conn->type->ae_handler, conn);

    return C_OK;
}

static int connRdmaBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    if (_connRdmaConnect(conn, addr, port, NULL) != ANET_OK) {
        return C_ERR;
    }

    if (connRdmaWait(conn->fd, POLLOUT, timeout) == C_ERR) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = ETIMEDOUT;
    }

    conn->state = CONN_STATE_CONNECTED;
    return C_OK;
}

static ssize_t connRdmaSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    ssize_t nwritten, towrite = size;
    long long startms = mstime(), elapsedms, waitms;

    while(1) {
        nwritten = rsend(conn->fd, ptr, towrite, 0);
        if (nwritten == -1) {
            if (errno != EAGAIN) {
                return -1;
            }
        } else {
            ptr += nwritten;
            towrite -= nwritten;
            if (towrite == 0) {
                break;
            }
        }

        elapsedms = mstime() - startms;
        waitms = timeout - elapsedms;
        if (connRdmaWait(conn->fd, POLLOUT, waitms) == C_ERR) {
            return -1;
        }

        if (elapsedms >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }

    return size;
}

static ssize_t connRdmaSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    ssize_t nwritten, toread = size;
    long long startms = mstime(), elapsedms, waitms;

    while(1) {
        nwritten = rrecv(conn->fd, ptr, toread, 0);
        if (nwritten == -1) {
            if (errno != EAGAIN) {
                return -1;
            }
        } else {
            ptr += nwritten;
            toread -= nwritten;
            if (toread == 0) {
                break;
            }
        }

        elapsedms = mstime() - startms;
        waitms = timeout - elapsedms;
        if (connRdmaWait(conn->fd, POLLIN, waitms) == C_ERR) {
            return -1;
        }

        if (elapsedms >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }

    return size;
}

static ssize_t connRdmaSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    ssize_t nread = 0;
    char c;

    size--;
    while(size) {
        if (connRdmaSyncRead(conn, &c, 1, timeout) == -1) {
            return -1;
        }

        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr - 1) == '\r') {
                *(ptr-1) = '\0';
            }

            return nread;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }

        size--;
    }

    return nread;
}

static int connRdmaGetType(connection *conn) {
    UNUSED(conn);

    return CONN_TYPE_RDMA;
}

ConnectionType CT_RDMA = {
    .ae_handler = connRdmaEventHandler,
    .accept = connRdmaAccept,
    .set_read_handler = connRdmaSetReadHandler,
    .set_write_handler = connRdmaSetWriteHandler,
    .get_last_error = connRdmaGetLastError,
    .read = connRdmaRead,
    .write = connRdmaWrite,
    .close = connRdmaClose,
    .connect = connRdmaConnect,
    .blocking_connect = connRdmaBlockingConnect,
    .sync_read = connRdmaSyncRead,
    .sync_write = connRdmaSyncWrite,
    .sync_readline = connRdmaSyncReadLine,
    .get_type = connRdmaGetType
};

static int rdmaServer(char *err, int port, char *bindaddr, int af)
{
    int fd = -1, retval, val;
    char _port[6];  /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;

    snprintf(_port,6,"%d",port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (bindaddr && !strcmp("*", bindaddr))
        bindaddr = NULL;

    if (af == AF_INET6 && bindaddr && !strcmp("::*", bindaddr)) {
        bindaddr = NULL;
    }

    if ((retval = getaddrinfo(bindaddr, _port, &hints, &servinfo)) != 0) {
        serverNetError(err, "RDMA: getaddrinfo %s", gai_strerror(retval));
        return ANET_ERR;
    } else if (!servinfo) {
        serverNetError(err, "RDMA: get empty addr info");
        return ANET_ERR;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        fd = rsocket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }

        val = 1;
        retval = rsetsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
        if (retval < 0) {
            goto error;
        }

        retval = rbind(fd, p->ai_addr, p->ai_addrlen);
        if (retval < 0) {
            goto error;
        }

        retval = rlisten(fd, 16);
        if (retval < 0) {
            goto error;
        }

        goto end;
    }

error:
    if (fd >= 0) {
        rclose(fd);
        fd = -1;
    }

end:
    freeaddrinfo(servinfo);
    return fd;
}


int listenToRdma(int port, socketFds *sfd) {
    int j, fd;
    char **bindaddr = server.rdma_bindaddr;
    int bindaddr_count = server.rdma_bindaddr_count;
    char *default_bindaddr[2] = {"*", "-::*"};

    if (ibv_fork_init()) {
        serverLog(LL_WARNING, "RDMA: FATAL error, ibv_fork_init failed");
        return ANET_ERR;
    }

    /* Force binding of 0.0.0.0 if no bind address is specified. */
    if (server.bindaddr_count == 0) {
        bindaddr_count = 2;
        bindaddr = default_bindaddr;
    }

    for (j = 0; j < bindaddr_count; j++) {
        char* addr = bindaddr[j];
        int optional = *addr == '-';

        if (optional)
            addr++;

        if (strchr(addr,':')) {
            fd = rdmaServer(server.neterr, port, addr, AF_INET6);
        } else {
            fd = rdmaServer(server.neterr, port, addr, AF_INET);
        }

        if (fd == ANET_ERR) {
            int net_errno = errno;
            serverLog(LL_WARNING, "RDMA: Could not create server for %s:%d: %s",
                      addr, port, server.neterr);

            if (net_errno == EADDRNOTAVAIL && optional)
                continue;

            if (net_errno == ENOPROTOOPT || net_errno == EPROTONOSUPPORT ||
                    net_errno == ESOCKTNOSUPPORT || net_errno == EPFNOSUPPORT ||
                    net_errno == EAFNOSUPPORT)
                continue;

            closeSocketListeners(sfd);
            return C_ERR;
        }

        rsocketSetBlock(fd, 0);
        anetCloexec(fd);
        sfd->fd[sfd->count] = fd;
        sfd->count++;
    }

    return C_OK;
}

int rdmaAccept(char *err, int s, char *ip, size_t ip_len, int *port) {
    struct sockaddr sa;
    struct sockaddr_in *sin;
    socklen_t addrlen = sizeof(struct sockaddr);
    int fd;

    fd = raccept(s, &sa, &addrlen);
    if (fd < 0) {
        serverNetError(err, "RDMA: raccept failed, %s", gai_strerror(errno));
        return C_ERR;
    }

    if (sa.sa_family != AF_INET) {
        serverNetError(err, "RDMA: only support AF_INET");
        rclose(fd);
        fd = -1;
        return C_ERR;
    }

    sin = (struct sockaddr_in *)&sa;
    if (ip) {
        inet_ntop(AF_INET, (void*)&(sin->sin_addr), ip, ip_len);
    }

    if (port) {
        *port = ntohs(sin->sin_port);
    }

    return fd;
}

connection *connCreateRdma() {
    connection *c = zcalloc(sizeof(connection));

    c->type = &CT_RDMA;
    c->fd = -1;

    return c;
}

connection *connCreateAcceptedRdma(int fd) {
    connection *c = connCreateRdma();

    c->fd = fd;
    c->state = CONN_STATE_ACCEPTING;

    return c;
}
#else    /* __linux__ */

"BUILD ERROR: RDMA is only supported on linux"

#endif   /* __linux__ */
#else    /* USE_RDMA */
int listenToRdma(int port, socketFds *sfd) {
    UNUSED(port);
    UNUSED(sfd);
    serverNetError(server.neterr, "RDMA: disabled, need rebuild with BUILD_RDMA=yes");

    return C_ERR;
}

int rdmaAccept(char *err, int s, char *ip, size_t ip_len, int *port) {
    UNUSED(err);
    UNUSED(s);
    UNUSED(ip);
    UNUSED(ip_len);
    UNUSED(port);

    serverNetError(server.neterr, "RDMA: disabled, need rebuild with BUILD_RDMA=yes");
    errno = EOPNOTSUPP;

    return C_ERR;
}

connection *connCreateRdma() {
    serverNetError(server.neterr, "RDMA: disabled, need rebuild with BUILD_RDMA=yes");
    errno = EOPNOTSUPP;

    return NULL;
}

connection *connCreateAcceptedRdma(int fd) {
    UNUSED(fd);
    serverNetError(server.neterr, "RDMA: disabled, need rebuild with BUILD_RDMA=yes");
    errno = EOPNOTSUPP;

    return NULL;
}

#endif
