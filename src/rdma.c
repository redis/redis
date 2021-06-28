/* ==========================================================================
 * rdma.c - support RDMA protocol for transport layer.
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
#include <assert.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <sys/types.h>
#include <sys/socket.h>

typedef enum RedisRdmaOpcode {
    RegisterLocalAddr,
} RedisRdmaOpcode;

typedef struct RedisRdmaCmd {
    uint8_t magic;
    uint8_t version;
    uint8_t opcode;
    uint8_t rsvd[13];
    uint64_t addr;
    uint32_t length;
    uint32_t key;
} RedisRdmaCmd;

#define MIN(a, b) (a) < (b) ? a : b
#define REDIS_MAX_SGE 1024
#define REDIS_RDMA_DEFAULT_RX_LEN  (1024*1024)
#define REDID_RDMA_CMD_MAGIC 'R'
#define REDIS_SYNCIO_RES 10

typedef struct rdma_connection {
    connection c;
    struct rdma_cm_id *cm_id;
    int last_errno;
} rdma_connection;

typedef struct RdmaContext {
    connection *conn;
    char *ip;
    int port;
    struct ibv_pd *pd;
    struct rdma_event_channel *cm_channel;
    struct ibv_comp_channel *comp_channel;
    struct ibv_cq *cq;
    long long timeEvent;

    /* TX */
    char *tx_addr;
    uint32_t tx_length;
    uint32_t tx_offset;
    uint32_t tx_key;
    char *send_buf;
    uint32_t send_length;
    uint32_t send_offset;
    uint32_t send_ops;
    struct ibv_mr *send_mr;

    /* RX */
    uint32_t rx_offset;
    char *recv_buf;
    unsigned int recv_length;
    unsigned int recv_offset;
    struct ibv_mr *recv_mr;

    /* CMD 0 ~ REDIS_MAX_SGE for recv buffer
     * REDIS_MAX_SGE ~ 2 * REDIS_MAX_SGE -1 for send buffer */
    RedisRdmaCmd *cmd_buf;
    struct ibv_mr *cmd_mr;
} RdmaContext;

static struct rdma_event_channel *listen_channel;
static struct rdma_cm_id *listen_cmids[CONFIG_BINDADDR_MAX];

static int rdmaPostRecv(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd) {
    struct ibv_sge sge;
    size_t length = sizeof(RedisRdmaCmd);
    struct ibv_recv_wr recv_wr, *bad_wr;
    int ret;

    sge.addr = (uint64_t)cmd;
    sge.length = length;
    sge.lkey = ctx->cmd_mr->lkey;

    recv_wr.wr_id = (uint64_t)cmd;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    recv_wr.next = NULL;

    ret = ibv_post_recv(cm_id->qp, &recv_wr, &bad_wr);
    if (ret && (ret != EAGAIN)) {
        serverLog(LL_WARNING, "RDMA: post recv failed: %d", ret);
        return C_ERR;
    }

    return C_OK;
}

static void rdmaDestroyIoBuf(RdmaContext *ctx)
{
    if (ctx->recv_mr) {
        ibv_dereg_mr(ctx->recv_mr);
        ctx->recv_mr = NULL;
    }

    zfree(ctx->recv_buf);
    ctx->recv_buf = NULL;

    if (ctx->send_mr) {
        ibv_dereg_mr(ctx->send_mr);
        ctx->send_mr = NULL;
    }

    zfree(ctx->send_buf);
    ctx->send_buf = NULL;

    if (ctx->cmd_mr) {
        ibv_dereg_mr(ctx->cmd_mr);
        ctx->cmd_mr = NULL;
    }

    zfree(ctx->cmd_buf);
    ctx->cmd_buf = NULL;
}

static int rdmaSetupIoBuf(RdmaContext *ctx, struct rdma_cm_id *cm_id)
{
    int access = IBV_ACCESS_LOCAL_WRITE;
    size_t length = sizeof(RedisRdmaCmd) * REDIS_MAX_SGE * 2;
    RedisRdmaCmd *cmd;
    int i;

    /* setup CMD buf & MR */
    ctx->cmd_buf = zcalloc(length);
    ctx->cmd_mr = ibv_reg_mr(ctx->pd, ctx->cmd_buf, length, access);
    if (!ctx->cmd_mr) {
        serverLog(LL_WARNING, "RDMA: reg mr for CMD failed");
	goto destroy_iobuf;
    }

    for (i = 0; i < REDIS_MAX_SGE; i++) {
        cmd = ctx->cmd_buf + i;

        if (rdmaPostRecv(ctx, cm_id, cmd) == C_ERR) {
            serverLog(LL_WARNING, "RDMA: post recv failed");
            goto destroy_iobuf;
        }
    }

    /* setup recv buf & MR */
    access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    length = REDIS_RDMA_DEFAULT_RX_LEN;
    ctx->recv_buf = zcalloc(length);
    ctx->recv_length = length;
    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, length, access);
    if (!ctx->recv_mr) {
        serverLog(LL_WARNING, "RDMA: reg mr for recv buffer failed");
	goto destroy_iobuf;
    }

    return C_OK;

destroy_iobuf:
    rdmaDestroyIoBuf(ctx);
    return C_ERR;
}

static int rdmaAdjustSendbuf(RdmaContext *ctx, unsigned int length) {
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (length == ctx->send_length) {
        return C_OK;
    }

    /* try to free old MR & buffer */
    if (ctx->send_length) {
        ibv_dereg_mr(ctx->send_mr);
        zfree(ctx->send_buf);
        ctx->send_length = 0;
    }

    /* create a new buffer & MR */
    ctx->send_buf = zcalloc(length);
    ctx->send_length = length;
    ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, length, access);
    if (!ctx->send_mr) {
        serverNetError(server.neterr, "RDMA: reg send mr failed");
        serverLog(LL_WARNING, "RDMA: FATAL error, recv corrupted cmd");
        zfree(ctx->send_buf);
        ctx->send_buf = NULL;
        ctx->send_length = 0;
        return C_ERR;
    }

    return C_OK;
}

static int rdmaSendCommand(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd) {
    struct ibv_send_wr send_wr, *bad_wr;
    struct ibv_sge sge;
    RedisRdmaCmd *_cmd;
    int i, ret;

    /* find an unused cmd buffer */
    for (i = REDIS_MAX_SGE; i < 2 * REDIS_MAX_SGE; i++) {
        _cmd = ctx->cmd_buf + i;
        if (!_cmd->magic) {
            break;
        }
    }

    assert(i < 2 * REDIS_MAX_SGE);

    _cmd->addr = htonu64(cmd->addr);
    _cmd->length = htonl(cmd->length);
    _cmd->key = htonl(cmd->key);
    _cmd->opcode = cmd->opcode;
    _cmd->magic = REDID_RDMA_CMD_MAGIC;

    sge.addr = (uint64_t)_cmd;
    sge.length = sizeof(RedisRdmaCmd);
    sge.lkey = ctx->cmd_mr->lkey;

    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr_id = (uint64_t)_cmd;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.next = NULL;
    ret = ibv_post_send(cm_id->qp, &send_wr, &bad_wr);
    if (ret) {
        serverLog(LL_WARNING, "RDMA: post send failed: %d", ret);
        return C_ERR;
    }

    return C_OK;
}

static int connRdmaRegisterRx(RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    RedisRdmaCmd cmd;

    cmd.addr = (uint64_t)ctx->recv_buf;
    cmd.length = ctx->recv_length;
    cmd.key = ctx->recv_mr->rkey;
    cmd.opcode = RegisterLocalAddr;

    ctx->rx_offset = 0;
    ctx->recv_offset = 0;

    return rdmaSendCommand(ctx, cm_id, &cmd);
}

static int rdmaHandleEstablished(struct rdma_cm_event *ev)
{
    struct rdma_cm_id *cm_id = ev->id;
    RdmaContext *ctx = cm_id->context;

    connRdmaRegisterRx(ctx, cm_id);

    return C_OK;
}

static int rdmaHandleDisconnect(struct rdma_cm_event *ev)
{
    struct rdma_cm_id *cm_id = ev->id;
    RdmaContext *ctx = cm_id->context;
    connection *conn = ctx->conn;

    conn->state = CONN_STATE_CLOSED;

    /* kick connection read/write handler to avoid resource leak */
    if (conn->read_handler) {
        callHandler(conn, conn->read_handler);
    } else if (conn->write_handler) {
        callHandler(conn, conn->write_handler);
    }

    return C_OK;
}

static int connRdmaHandleRecv(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd, uint32_t byte_len) {
    RedisRdmaCmd _cmd;

    if (unlikely(byte_len != sizeof(RedisRdmaCmd))) {
        serverLog(LL_WARNING, "RDMA: FATAL error, recv corrupted cmd");
        return C_ERR;
    }

    _cmd.addr = ntohu64(cmd->addr);
    _cmd.length = ntohl(cmd->length);
    _cmd.key = ntohl(cmd->key);
    _cmd.opcode = cmd->opcode;

    switch (_cmd.opcode) {
    case RegisterLocalAddr:
        ctx->tx_addr = (char *)_cmd.addr;
        ctx->tx_length = _cmd.length;
        ctx->tx_key = _cmd.key;
        ctx->tx_offset = 0;
        rdmaAdjustSendbuf(ctx, ctx->tx_length);

        break;

    default:
        serverLog(LL_WARNING, "RDMA: FATAL error, unknown cmd");
        return C_ERR;
    }

    return rdmaPostRecv(ctx, cm_id, cmd);
}

static int connRdmaHandleSend(RedisRdmaCmd *cmd) {
    /* mark this cmd has already sent */
    cmd->magic = 0;

    return C_OK;
}

static int connRdmaHandleRecvImm(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd, uint32_t byte_len) {
    assert(byte_len + ctx->rx_offset <= ctx->recv_length);

    ctx->rx_offset += byte_len;

    return rdmaPostRecv(ctx, cm_id, cmd);
}

static int connRdmaHandleWrite(RdmaContext *ctx, uint32_t byte_len) {
    UNUSED(ctx);
    UNUSED(byte_len);

    return C_OK;
}


static int connRdmaHandleCq(rdma_connection *rdma_conn) {
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    struct ibv_cq *ev_cq = NULL;
    void *ev_ctx = NULL;
    struct ibv_wc wc = {0};
    RedisRdmaCmd *cmd;
    int ret;

    if (ibv_get_cq_event(ctx->comp_channel, &ev_cq, &ev_ctx) < 0) {
        if (errno != EAGAIN) {
            serverLog(LL_WARNING, "RDMA: get CQ event error");
            return C_ERR;
        }
    } else if (ibv_req_notify_cq(ev_cq, 0)) {
        serverLog(LL_WARNING, "RDMA: notify CQ error");
        return C_ERR;
    }

pollcq:
    ret = ibv_poll_cq(ctx->cq, 1, &wc);
    if (ret < 0) {
        serverLog(LL_WARNING, "RDMA: poll recv CQ error");
        return C_ERR;
    } else if (ret == 0) {
        return C_OK;
    }

    ibv_ack_cq_events(ctx->cq, 1);

    if (wc.status != IBV_WC_SUCCESS) {
        serverLog(LL_WARNING, "RDMA: CQ handle error status 0x%x", wc.status);
        return C_ERR;
    }

    switch (wc.opcode) {
    case IBV_WC_RECV:
        cmd = (RedisRdmaCmd *)wc.wr_id;
        if (connRdmaHandleRecv(ctx, cm_id, cmd, wc.byte_len) == C_ERR) {
            return C_ERR;
        }
        break;

    case IBV_WC_RECV_RDMA_WITH_IMM:
        cmd = (RedisRdmaCmd *)wc.wr_id;
        if (connRdmaHandleRecvImm(ctx, cm_id, cmd, wc.byte_len) == C_ERR) {
            rdma_conn->c.state = CONN_STATE_ERROR;
            return C_ERR;
        }

        break;
    case IBV_WC_RDMA_WRITE:
        if (connRdmaHandleWrite(ctx, wc.byte_len) == C_ERR) {
            return C_ERR;
        }

        break;

    case IBV_WC_SEND:
        cmd = (RedisRdmaCmd *)wc.wr_id;
        if (connRdmaHandleSend(cmd) == C_ERR) {
            return C_ERR;
        }

        break;

    default:
        serverLog(LL_WARNING, "RDMA: unexpected opcode 0x[%x]", wc.opcode);
        return C_ERR;
    }

    goto pollcq;
}

static int connRdmaAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    int ret = C_OK;

    if (conn->state != CONN_STATE_ACCEPTING)
        return C_ERR;

    conn->state = CONN_STATE_CONNECTED;

    connIncrRefs(conn);
    if (!callHandler(conn, accept_handler))
        ret = C_ERR;
    connDecrRefs(conn);

    return ret;
}

static void connRdmaEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    rdma_connection *rdma_conn = (rdma_connection *)clientData;
    connection *conn = &rdma_conn->c;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    int ret = 0;

    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);

    ret = connRdmaHandleCq(rdma_conn);
    if (ret == C_ERR) {
        conn->state = CONN_STATE_ERROR;
        return;
    }

    /* uplayer should read all */
    while (ctx->recv_offset < ctx->rx_offset) {
        if (conn->read_handler && (callHandler(conn, conn->read_handler) == C_ERR)) {
            return;
        }
    }

    /* recv buf is full, register a new RX buffer */
    if (ctx->recv_offset == ctx->recv_length) {
        connRdmaRegisterRx(ctx, cm_id);
    }

    /* RDMA comp channel has no POLLOUT event, try to send remaining buffer */
    if (conn->write_handler) {
        callHandler(conn, conn->write_handler);
    }
}

int connRdmaCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    connection *conn = (connection *)clientData;

    UNUSED(eventLoop);
    UNUSED(id);
    if (conn->state != CONN_STATE_CONNECTED) {
        return REDIS_SYNCIO_RES;
    }

    connRdmaEventHandler(NULL, -1, conn, 0);

    return REDIS_SYNCIO_RES;
}

static int connRdmaSetRwHandler(connection *conn) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;

    /* save conn into RdmaContext */
    ctx->conn = conn;

    /* IB channel only has POLLIN event */
    if (conn->read_handler || conn->write_handler) {
        if (aeCreateFileEvent(server.el, conn->fd, AE_READABLE, conn->type->ae_handler, conn) == AE_ERR) {
            return C_ERR;
        }

        if (ctx->timeEvent == -1) {
            ctx->timeEvent = aeCreateTimeEvent(server.el, REDIS_SYNCIO_RES, connRdmaCron, conn, NULL);
            if (ctx->timeEvent == AE_ERR) {
                return C_ERR;
            }
        }
    } else {
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE);
        if (ctx->timeEvent > 0) {
            aeDeleteTimeEvent(server.el, ctx->timeEvent);
            ctx->timeEvent = -1;
        }
    }

    return C_OK;
}

static int connRdmaSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {

    conn->write_handler = func;
    if (barrier) {
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    } else {
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    }

    return connRdmaSetRwHandler(conn);
}

static int connRdmaSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    conn->read_handler = func;

    return connRdmaSetRwHandler(conn);
}

static const char *connRdmaGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static inline void rdmaConnectFailed(rdma_connection *rdma_conn) {
    connection *conn = &rdma_conn->c;

    conn->state = CONN_STATE_ERROR;
    conn->last_errno = ENETUNREACH;
}

static int rdmaCreateResource(RdmaContext *ctx, struct rdma_cm_id *cm_id)
{
    int ret = C_OK;
    struct ibv_qp_init_attr init_attr;
    struct ibv_comp_channel *comp_channel = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_pd *pd = NULL;

    pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd) {
        serverLog(LL_WARNING, "RDMA: ibv alloc pd failed");
        return C_ERR;
    }

    ctx->pd = pd;

    comp_channel = ibv_create_comp_channel(cm_id->verbs);
    if (!comp_channel) {
        serverLog(LL_WARNING, "RDMA: ibv create comp channel failed");
        return C_ERR;
    }

    ctx->comp_channel = comp_channel;

    cq = ibv_create_cq(cm_id->verbs, REDIS_MAX_SGE * 2, NULL, comp_channel, 0);
    if (!cq) {
        serverLog(LL_WARNING, "RDMA: ibv create cq failed");
        return C_ERR;
    }

    ctx->cq = cq;
    ibv_req_notify_cq(cq, 0);

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = REDIS_MAX_SGE;
    init_attr.cap.max_recv_wr = REDIS_MAX_SGE;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.send_cq = cq;
    init_attr.recv_cq = cq;
    ret = rdma_create_qp(cm_id, pd, &init_attr);
    if (ret) {
        serverLog(LL_WARNING, "RDMA: create qp failed");
        return C_ERR;
    }

    if (rdmaSetupIoBuf(ctx, cm_id)) {
        return C_ERR;
    }

    return C_OK;
}

static void rdmaReleaseResource(RdmaContext *ctx) {
    rdmaDestroyIoBuf(ctx);

    if (ctx->cq) {
        ibv_destroy_cq(ctx->cq);
    }

    if (ctx->comp_channel) {
        ibv_destroy_comp_channel(ctx->comp_channel);
    }

    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
    }
}

static int rdmaConnect(RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    struct rdma_conn_param conn_param = {0};

    if (rdmaCreateResource(ctx, cm_id) == C_ERR) {
        return C_ERR;
    }

    /* rdma connect with param */
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 7;
    conn_param.rnr_retry_count = 7;
    if (rdma_connect(cm_id, &conn_param)) {
        return C_ERR;
    }

    anetNonBlock(NULL, ctx->comp_channel->fd);
    anetCloexec(ctx->comp_channel->fd);

    return C_OK;
}

static void rdmaCMeventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    rdma_connection *rdma_conn = (rdma_connection *)clientData;
    connection *conn = &rdma_conn->c;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    struct rdma_event_channel *cm_channel = ctx->cm_channel;
    struct rdma_cm_event *ev;
    enum rdma_cm_event_type ev_type;
    int ret = C_OK;

    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);

    ret = rdma_get_cm_event(cm_channel, &ev);
    if (ret) {
        if (errno != EAGAIN) {
            serverLog(LL_WARNING, "RDMA: client channel rdma_get_cm_event failed, %s", strerror(errno));
        }
        return;
    }

    ev_type = ev->event;
    switch (ev_type) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            /* resolve route at most 100ms */
            if (rdma_resolve_route(ev->id, 100)) {
                rdmaConnectFailed(rdma_conn);
            }
            break;

        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            if (rdmaConnect(ctx, ev->id) == C_ERR) {
                rdmaConnectFailed(rdma_conn);
            }
            break;

        case RDMA_CM_EVENT_ESTABLISHED:
            rdmaHandleEstablished(ev);
            conn->state = CONN_STATE_CONNECTED;
            conn->fd = ctx->comp_channel->fd;
            if (conn->conn_handler) {
                callHandler(conn, conn->conn_handler);
            }
            break;

        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_REJECTED:
            rdmaConnectFailed(rdma_conn);
            break;

        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
        case RDMA_CM_EVENT_CONNECT_REQUEST:
        case RDMA_CM_EVENT_ADDR_CHANGE:
        case RDMA_CM_EVENT_DISCONNECTED:
            rdmaHandleDisconnect(ev);
            break;

        case RDMA_CM_EVENT_MULTICAST_JOIN:
        case RDMA_CM_EVENT_MULTICAST_ERROR:
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_CONNECT_RESPONSE:
        default:
            serverLog(LL_NOTICE, "RDMA: client channel ignore event: %s", rdma_event_str(ev_type));
    }

    if (rdma_ack_cm_event(ev)) {
        serverLog(LL_NOTICE, "RDMA: ack cm event failed\n");
    }

    /* connection error or closed by remote peer */
    if (conn->state == CONN_STATE_ERROR) {
        callHandler(conn, conn->conn_handler);
    }
}

/* free resource during connection close */
static int rdmaResolveAddr(rdma_connection *rdma_conn, const char *addr, int port, const char *src_addr) {
    struct addrinfo hints, *servinfo = NULL, *p = NULL;
    struct rdma_event_channel *cm_channel = NULL;
    struct rdma_cm_id *cm_id = NULL;
    RdmaContext *ctx = NULL;
    struct sockaddr_storage saddr;
    char _port[6];  /* strlen("65535") */
    int availableAddrs = 0;
    int ret = C_ERR;

    UNUSED(src_addr);
    ctx = zcalloc(sizeof(RdmaContext));
    if (!ctx) {
        serverLog(LL_WARNING, "RDMA: Out of memory");
        goto out;
    }

    ctx->timeEvent = -1;
    cm_channel = rdma_create_event_channel();
    if (!cm_channel) {
        serverLog(LL_WARNING, "RDMA: create event channel failed");
        goto out;
    }
    ctx->cm_channel = cm_channel;

    if (rdma_create_id(cm_channel, &cm_id, (void *)ctx, RDMA_PS_TCP)) {
        serverLog(LL_WARNING, "RDMA: create id failed");
        goto out;
    }
    rdma_conn->cm_id = cm_id;

    if (anetNonBlock(NULL, cm_channel->fd) != C_OK) {
        serverLog(LL_WARNING, "RDMA: set cm channel fd non-block failed");
        goto out;
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(addr, _port, &hints, &servinfo)) {
         hints.ai_family = AF_INET6;
         if (getaddrinfo(addr, _port, &hints, &servinfo)) {
             serverLog(LL_WARNING, "RDMA: bad server addr info");
             goto out;
        }
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if (p->ai_family == PF_INET) {
                memcpy(&saddr, p->ai_addr, sizeof(struct sockaddr_in));
                ((struct sockaddr_in *)&saddr)->sin_port = htons(port);
        } else if (p->ai_family == PF_INET6) {
                memcpy(&saddr, p->ai_addr, sizeof(struct sockaddr_in6));
                ((struct sockaddr_in6 *)&saddr)->sin6_port = htons(port);
        } else {
            serverLog(LL_WARNING, "RDMA: Unsupported family");
            goto out;
        }

        /* resolve addr at most 100ms */
        if (rdma_resolve_addr(cm_id, NULL, (struct sockaddr *)&saddr, 100)) {
            continue;
        }
        availableAddrs++;
    }

    if (!availableAddrs) {
        serverLog(LL_WARNING, "RDMA: server addr not available");
        goto out;
    }

    ret = C_OK;

out:
    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    return ret;
}

static int connRdmaWait(connection *conn, long start, long timeout) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    long long remaining = timeout, wait, elapsed = 0;

    remaining = timeout - elapsed;
    wait = (remaining < REDIS_SYNCIO_RES) ? remaining : REDIS_SYNCIO_RES;
    aeWait(conn->fd, AE_READABLE, wait);
    elapsed = mstime() - start;
    if (elapsed >= timeout) {
        errno = ETIMEDOUT;
        return C_ERR;
    }

    if (connRdmaHandleCq(rdma_conn) == C_ERR) {
        conn->state = CONN_STATE_ERROR;
        return C_ERR;
    }

    return C_OK;
}

static int connRdmaConnect(connection *conn, const char *addr, int port, const char *src_addr, ConnectionCallbackFunc connect_handler) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id;
    RdmaContext *ctx;

    if (rdmaResolveAddr(rdma_conn, addr, port, src_addr) == C_ERR) {
        return C_ERR;
    }

    cm_id = rdma_conn->cm_id;
    ctx = cm_id->context;
    if (aeCreateFileEvent(server.el, ctx->cm_channel->fd, AE_READABLE, rdmaCMeventHandler, conn) == AE_ERR) {
        return C_ERR;
    }

    conn->conn_handler = connect_handler;

    return C_OK;
}

static int connRdmaBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id;
    RdmaContext *ctx;
    long long start = mstime();

    if (rdmaResolveAddr(rdma_conn, addr, port, NULL) == C_ERR) {
        return C_ERR;
    }

    cm_id = rdma_conn->cm_id;
    ctx = cm_id->context;
    if (aeCreateFileEvent(server.el, ctx->cm_channel->fd, AE_READABLE, rdmaCMeventHandler, conn) == AE_ERR) {
        return C_ERR;
    }

    do {
        if (connRdmaWait(conn, start, timeout) == C_ERR) {
            return C_ERR ;
        }
    } while (conn->state != CONN_STATE_CONNECTED);

    return C_OK;
}

static void connRdmaClose(connection *conn) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx;

    if (conn->fd != -1) {
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE);
        conn->fd = -1;
    }

    if (!cm_id) {
        return;
    }

    ctx = cm_id->context;
    if (ctx->timeEvent > 0) {
        aeDeleteTimeEvent(server.el, ctx->timeEvent);
    }

    rdma_disconnect(cm_id);

    /* poll all CQ before close */
    connRdmaHandleCq(rdma_conn);
    rdmaReleaseResource(ctx);
    if (cm_id->qp) {
        ibv_destroy_qp(cm_id->qp);
    }

    rdma_destroy_id(cm_id);
    if (ctx->cm_channel) {
        aeDeleteFileEvent(server.el, ctx->cm_channel->fd, AE_READABLE);
        rdma_destroy_event_channel(ctx->cm_channel);
    }

    rdma_conn->cm_id = NULL;
    zfree(ctx);
    zfree(conn);
}

static size_t connRdmaSend(connection *conn, const void *data, size_t data_len) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    struct ibv_send_wr send_wr, *bad_wr;
    struct ibv_sge sge;
    uint32_t off = ctx->tx_offset;
    char *addr = ctx->send_buf + off;
    char *remote_addr = ctx->tx_addr + ctx->tx_offset;
    int ret;

    memcpy(addr, data, data_len);

    sge.addr = (uint64_t)addr;
    sge.lkey = ctx->send_mr->lkey;
    sge.length = data_len;

    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = (++ctx->send_ops % (REDIS_MAX_SGE / 2)) ? 0 : IBV_SEND_SIGNALED;
    send_wr.imm_data = htonl(0);
    send_wr.wr.rdma.remote_addr = (uint64_t)remote_addr;
    send_wr.wr.rdma.rkey = ctx->tx_key;
    send_wr.wr_id = 0;
    send_wr.next = NULL;
    ret = ibv_post_send(cm_id->qp, &send_wr, &bad_wr);
    if (ret) {
        serverLog(LL_WARNING, "RDMA: post send failed: %d", ret);
        conn->state = CONN_STATE_ERROR;
        return C_ERR;
    }

    ctx->tx_offset += data_len;

    return data_len;
}

static int connRdmaWrite(connection *conn, const void *data, size_t data_len) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    uint32_t towrite;

    if (conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED) {
        return C_ERR;
    }

    assert(ctx->tx_offset <= ctx->tx_length);
    towrite = MIN(ctx->tx_length - ctx->tx_offset, data_len);
    if (!towrite) {
        return 0;
    }

    return connRdmaSend(conn, data, towrite);
}

static inline uint32_t rdmaRead(RdmaContext *ctx, void *buf, size_t buf_len) {
    uint32_t toread;

    toread = MIN(ctx->rx_offset - ctx->recv_offset, buf_len);

    assert(ctx->recv_offset + toread <= ctx->recv_length);
    memcpy(buf, ctx->recv_buf + ctx->recv_offset, toread);

    ctx->recv_offset += toread;

    return toread;
}

static int connRdmaRead(connection *conn, void *buf, size_t buf_len) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;

    if (conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED) {
        return C_ERR;
    }

    assert(ctx->recv_offset < ctx->rx_offset);

    return rdmaRead(ctx, buf, buf_len);
}

static ssize_t connRdmaSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    ssize_t nwritten = 0;
    long long start = mstime();
    uint32_t towrite;

    if (conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED) {
        return C_ERR;
    }

    assert(ctx->tx_offset <= ctx->tx_length);
    if (ctx->tx_offset < ctx->tx_length) {
        /* TX buffer is available */
        goto copy;
    }

wait:
    if (connRdmaWait(conn, start, timeout) == C_ERR) {
        return C_ERR;
    }

    if (unlikely(!ctx->send_mr)) {
        goto wait;
    }

copy:
    towrite = MIN(ctx->tx_length - ctx->tx_offset, size - nwritten);
    if (connRdmaSend(conn, ptr, towrite) == (size_t)C_ERR) {
        return C_ERR;
    } else {
        ptr += towrite;
        nwritten += towrite;
    }

    if (nwritten < size) {
        goto wait;
    }

    return size;
}

static ssize_t connRdmaSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    ssize_t nread = 0;
    long long start = mstime();
    uint32_t toread;

    if (conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED) {
        return C_ERR;
    }

    assert(ctx->recv_offset <= ctx->rx_offset);
    if (ctx->recv_offset < ctx->rx_offset) {
        goto copy;
    }

wait:
    if (connRdmaWait(conn, start, timeout) == C_ERR) {
        return C_ERR;
    }

copy:
    toread = rdmaRead(ctx, ptr, size - nread);
    ptr += toread;
    nread += toread;
    if (nread < size) {
        goto wait;
    }

    return size;
}

static ssize_t connRdmaSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    ssize_t nread = 0;
    long long start = mstime();
    uint32_t toread;
    char *c;
    char nl = 0;

    if (conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED) {
        return C_ERR;
    }

    assert(ctx->recv_offset <= ctx->rx_offset);
    if (ctx->recv_offset < ctx->rx_offset) {
        goto copy;
    }

wait:
    if (connRdmaWait(conn, start, timeout) == C_ERR) {
        return C_ERR;
    }

copy:
    for (toread = 0; toread <= ctx->rx_offset - ctx->recv_offset; toread++) {
        c = ctx->recv_buf + ctx->recv_offset + toread;
        if (*c == '\n') {
            *c = '\0';
            if (toread && *(c - 1) == '\r') {
                *(c - 1) = '\0';
            }
            nl = 1;
            break;
        }
    }

    toread = rdmaRead(ctx, ptr, MIN(toread + nl, size - nread));
    ptr += toread;
    nread += toread;
    if (nl) {
        return nread;
    }

    if (nread < size) {
        goto wait;
    }

    return size;
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

static int rdmaServer(char *err, int port, char *bindaddr, int af, int index)
{
    int s = ANET_OK, rv, afonly = 1;
    char _port[6];  /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage sock_addr;
    struct rdma_cm_id *listen_cmid;

    if (ibv_fork_init()) {
        serverLog(LL_WARNING, "RDMA: FATAL error, recv corrupted cmd");
        return ANET_ERR;
    }

    snprintf(_port,6,"%d",port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* No effect if bindaddr != NULL */
    if (bindaddr && !strcmp("*", bindaddr))
        bindaddr = NULL;

    if (af == AF_INET6 && bindaddr && !strcmp("::*", bindaddr))
        bindaddr = NULL;

    if ((rv = getaddrinfo(bindaddr, _port, &hints, &servinfo)) != 0) {
        serverNetError(err, "RDMA: %s", gai_strerror(rv));
        return ANET_ERR;
    } else if (!servinfo) {
        serverNetError(err, "RDMA: get addr info failed");
        s = ANET_ERR;
        goto end;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        memset(&sock_addr, 0, sizeof(sock_addr));
        if (p->ai_family == AF_INET6) {
            memcpy(&sock_addr, p->ai_addr, sizeof(struct sockaddr_in6));
            ((struct sockaddr_in6 *) &sock_addr)->sin6_family = AF_INET6;
            ((struct sockaddr_in6 *) &sock_addr)->sin6_port = htons(port);
        } else {
            memcpy(&sock_addr, p->ai_addr, sizeof(struct sockaddr_in));
            ((struct sockaddr_in *) &sock_addr)->sin_family = AF_INET;
            ((struct sockaddr_in *) &sock_addr)->sin_port = htons(port);
        }

        if (rdma_create_id(listen_channel, &listen_cmid, NULL, RDMA_PS_TCP)) {
            serverNetError(err, "RDMA: create listen cm id error");
            return ANET_ERR;
        }

        rdma_set_option(listen_cmid, RDMA_OPTION_ID, RDMA_OPTION_ID_AFONLY,
                        &afonly, sizeof(afonly));

        if (rdma_bind_addr(listen_cmid, (struct sockaddr *)&sock_addr)) {
            serverNetError(err, "RDMA: bind addr error");
            goto error;
        }

        if (rdma_listen(listen_cmid, 0)) {
            serverNetError(err, "RDMA: listen addr error");
            goto error;
        }

        listen_cmids[index] = listen_cmid;
        goto end;
    }

error:
    if (listen_cmid)
        rdma_destroy_id(listen_cmid);
    s = ANET_ERR;

end:
    freeaddrinfo(servinfo);
    return s;
}

int listenToRdma(int port, socketFds *sfd) {
    int j, index = 0, ret;
    char **bindaddr = server.bindaddr;
    int bindaddr_count = server.bindaddr_count;
    char *default_bindaddr[2] = {"*", "-::*"};

    assert(server.proto_max_bulk_len <= 512ll * 1024 * 1024);

    /* Force binding of 0.0.0.0 if no bind address is specified. */
    if (server.bindaddr_count == 0) {
        bindaddr_count = 2;
        bindaddr = default_bindaddr;
    }

    listen_channel = rdma_create_event_channel();
    if (!listen_channel) {
        serverLog(LL_WARNING, "RDMA: Could not create event channel");
        return C_ERR;
    }

    for (j = 0; j < bindaddr_count; j++) {
        char* addr = bindaddr[j];
        int optional = *addr == '-';

        if (optional)
            addr++;
        if (strchr(addr,':')) {
            /* Bind IPv6 address. */
            ret = rdmaServer(server.neterr, port, addr, AF_INET6, index);
        } else {
            /* Bind IPv4 address. */
            ret = rdmaServer(server.neterr, port, addr, AF_INET, index);
        }

        if (ret == ANET_ERR) {
            int net_errno = errno;
            serverLog(LL_WARNING, "RDMA: Could not create server for %s:%d: %s",
                      addr, port, server.neterr);

            if (net_errno == EADDRNOTAVAIL && optional)
                continue;

            if (net_errno == ENOPROTOOPT || net_errno == EPROTONOSUPPORT ||
                    net_errno == ESOCKTNOSUPPORT || net_errno == EPFNOSUPPORT ||
                    net_errno == EAFNOSUPPORT)
                continue;

            return C_ERR;
        }

        index++;
    }

    sfd->fd[sfd->count] = listen_channel->fd;
    anetNonBlock(NULL, sfd->fd[sfd->count]);
    anetCloexec(sfd->fd[sfd->count]);
    sfd->count++;

    return C_OK;
}

static int rdmaHandleConnect(char *err, struct rdma_cm_event *ev, char *ip, size_t ip_len, int *port)
{
    int ret = C_OK;
    struct rdma_cm_id *cm_id = ev->id;
    struct sockaddr_storage caddr;
    RdmaContext *ctx = NULL;
    struct rdma_conn_param conn_param = {
            .responder_resources = 1,
            .initiator_depth = 1,
            .retry_count = 5,
    };

    memcpy(&caddr, &cm_id->route.addr.dst_addr, sizeof(caddr));
    if (caddr.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&caddr;
        if (ip)
            inet_ntop(AF_INET, (void*)&(s->sin_addr), ip, ip_len);
        if (port)
            *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&caddr;
        if (ip)
            inet_ntop(AF_INET6, (void*)&(s->sin6_addr), ip, ip_len);
        if (port)
            *port = ntohs(s->sin6_port);
    }

    ctx = zcalloc(sizeof(RdmaContext));
    ctx->timeEvent = -1;
    ctx->ip = zstrdup(ip);
    ctx->port = *port;
    cm_id->context = ctx;
    if (rdmaCreateResource(ctx, cm_id) == C_ERR) {
        goto reject;
    }

    ret = rdma_accept(cm_id, &conn_param);
    if (ret) {
        serverNetError(err, "RDMA: accept failed");
        goto free_rdma;
    }

    return C_OK;

free_rdma:
    rdmaReleaseResource(ctx);
reject:
    /* reject connect request if hitting error */
    rdma_reject(cm_id, NULL, 0);

    return C_ERR;
}

/*
 * rdmaAccept, actually it works as cm-event handler for listen cm_id.
 * accept a connection logic works in two steps:
 * 1, handle RDMA_CM_EVENT_CONNECT_REQUEST and return CM fd on success
 * 2, handle RDMA_CM_EVENT_ESTABLISHED and return C_OK on success
 */
int rdmaAccept(char *err, int s, char *ip, size_t ip_len, int *port, void **priv) {
    struct rdma_cm_event *ev;
    enum rdma_cm_event_type ev_type;
    int ret = C_OK;
    UNUSED(s);

    ret = rdma_get_cm_event(listen_channel, &ev);
    if (ret) {
        if (errno != EAGAIN) {
            serverLog(LL_WARNING, "RDMA: listen channel rdma_get_cm_event failed, %s", strerror(errno));
            return ANET_ERR;
        }
        return ANET_OK;
    }

    ev_type = ev->event;
    switch (ev_type) {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
            ret = rdmaHandleConnect(err, ev, ip, ip_len, port);
            if (ret == C_OK) {
                RdmaContext *ctx = (RdmaContext *)ev->id->context;
                *priv = ev->id;
                ret = ctx->comp_channel->fd;
            }
            break;

        case RDMA_CM_EVENT_ESTABLISHED:
            ret = rdmaHandleEstablished(ev);
            break;

        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_ADDR_CHANGE:
        case RDMA_CM_EVENT_DISCONNECTED:
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            rdmaHandleDisconnect(ev);
            ret = C_OK;
            break;

        case RDMA_CM_EVENT_MULTICAST_JOIN:
        case RDMA_CM_EVENT_MULTICAST_ERROR:
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_ADDR_RESOLVED:
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
        case RDMA_CM_EVENT_CONNECT_RESPONSE:
        default:
            serverLog(LL_NOTICE, "RDMA: listen channel ignore event: %s", rdma_event_str(ev_type));
            break;
    }

    if (rdma_ack_cm_event(ev)) {
        serverLog(LL_WARNING, "ack cm event failed\n");
        return ANET_ERR;
    }

    return ret;
}

connection *connCreateRdma() {
    rdma_connection *rdma_conn = zcalloc(sizeof(rdma_connection));
    rdma_conn->c.type = &CT_RDMA;
    rdma_conn->c.fd = -1;

    return (connection *)rdma_conn;
}

connection *connCreateAcceptedRdma(int fd, void *priv) {
    rdma_connection *rdma_conn = (rdma_connection *)connCreateRdma();
    rdma_conn->c.fd = fd;
    rdma_conn->c.state = CONN_STATE_ACCEPTING;
    rdma_conn->cm_id = priv;

    return (connection *)rdma_conn;
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

int rdmaAccept(char *err, int s, char *ip, size_t ip_len, int *port, void **priv) {
    UNUSED(err);
    UNUSED(s);
    UNUSED(ip);
    UNUSED(ip_len);
    UNUSED(port);
    UNUSED(priv);

    serverNetError(server.neterr, "RDMA: disabled, need rebuild with BUILD_RDMA=yes");
    errno = EOPNOTSUPP;

    return C_ERR;
}

connection *connCreateRdma() {
    serverNetError(server.neterr, "RDMA: disabled, need rebuild with BUILD_RDMA=yes");
    errno = EOPNOTSUPP;

    return NULL;
}

connection *connCreateAcceptedRdma(int fd, void *priv) {
    UNUSED(fd);
    UNUSED(priv);
    serverNetError(server.neterr, "RDMA: disabled, need rebuild with BUILD_RDMA=yes");
    errno = EOPNOTSUPP;

    return NULL;
}

#endif
