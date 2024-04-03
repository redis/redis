/* ==========================================================================
 * rdma.c - support RDMA protocol for transport layer.
 * --------------------------------------------------------------------------
 * Copyright (C) 2021-2023  zhenwei pi
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

#define REDISMODULE_CORE_MODULE
#include "server.h"

#ifdef USE_RDMA
#ifdef __linux__    /* currently RDMA is only supported on Linux */
#include "connection.h"
#include "connhelpers.h"
#include "cluster.h"

#include <assert.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define CONN_TYPE_RDMA "rdma"

typedef struct RedisRdmaFeature {
    /* defined as following Opcodes */
    uint16_t opcode;
    /* select features */
    uint16_t select;
    uint8_t rsvd[20];
    /* feature bits */
    uint64_t features;
} RedisRdmaFeature;

typedef struct RedisRdmaKeepalive {
    /* defined as following Opcodes */
    uint16_t opcode;
    uint8_t rsvd[30];
} RedisRdmaKeepalive;

typedef struct RedisRdmaMemory {
    /* defined as following Opcodes */
    uint16_t opcode;
    uint8_t rsvd[14];
    /* address of a transfer buffer which is used to receive remote streaming data,
     * aka 'RX buffer address'. The remote side should use this as 'TX buffer address' */
    uint64_t addr;
    /* length of the 'RX buffer' */
    uint32_t length;
    /* the RDMA remote key of 'RX buffer' */
    uint32_t key;
} RedisRdmaMemory;

typedef union RedisRdmaCmd {
    RedisRdmaFeature feature;
    RedisRdmaKeepalive keepalive;
    RedisRdmaMemory memory;
} RedisRdmaCmd;

typedef enum RedisRdmaOpcode {
    GetServerFeature = 0,
    SetClientFeature = 1,
    Keepalive = 2,
    RegisterXferMemory = 3,
} RedisRdmaOpcode;

#define REDIS_BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2*!!(cond)]))
#define MIN(a, b) (a) < (b) ? a : b
#define REDIS_MAX_WQE 1024
#define REDIS_RDMA_DEFAULT_RX_SIZE  (1024*1024)
#define REDIS_RDMA_MIN_RX_SIZE  (64*1024)
#define REDIS_RDMA_MAX_RX_SIZE  (16*1024*1024)
#define REDIS_SYNCIO_RES 10
#define REDIS_RDMA_INVALID_OPCODE 0xffff

typedef struct rdma_connection {
    connection c;
    struct rdma_cm_id *cm_id;
    int last_errno;
    listNode *pending_list_node;
} rdma_connection;

typedef struct RdmaXfer {
    struct ibv_mr *mr; /* memory region of the transfer buffer */
    char *addr;        /* address of transfer buffer in local memory */
    uint32_t length;   /* bytes of transfer buffer */
    uint32_t offset;   /* the offset of consumed transfer buffer */
    uint32_t pos;      /* the position in use of the transfer buffer */
} RdmaXfer;

typedef struct RdmaContext {
    connection *conn;
    char *ip;
    int port;
    struct ibv_pd *pd;
    struct rdma_event_channel *cm_channel;
    struct ibv_comp_channel *comp_channel;
    struct ibv_cq *cq;

    /* TX */
    RdmaXfer tx;
    char *tx_addr;      /* remote transfer buffer address */
    uint32_t tx_key;    /* remote transfer buffer key */
    uint32_t tx_length; /* remote transfer buffer length */
    uint32_t tx_offset; /* remote transfer buffer offset */
    uint32_t tx_ops;    /* operations on remote transfer */

    /* RX */
    RdmaXfer rx;

    /* CMD 0 ~ REDIS_MAX_WQE for recv buffer
     * REDIS_MAX_WQE ~ 2 * REDIS_MAX_WQE -1 for send buffer */
    RedisRdmaCmd *cmd_buf;
    struct ibv_mr *cmd_mr;
} RdmaContext;

typedef struct rdma_listener {
    struct rdma_cm_id *cm_id;
    struct rdma_event_channel *cm_channel;
} rdma_listener;

/* RDMA connection is always writable, it has no POLLOUT event to drive the write handler, record available write handler into pending list */
static list *pending_list;

static ConnectionType CT_RDMA;

static int redis_rdma_rx_size = REDIS_RDMA_DEFAULT_RX_SIZE;
static int redis_rdma_comp_vector = -1; /* -1 means a random one */

static void serverRdmaError(char *err, const char *fmt, ...) {
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

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

/* To make Redis forkable, buffer which is registered as RDMA
 * memory region should be aligned to page size. And the length
 * also need be aligned to page size.
 * Random segment-fault case like this:
 * 0x7f2764ac5000      -      0x7f2764ac7000
 * |ptr0 128| ... |ptr1 4096| ... |ptr2 512|
 *
 * After ibv_reg_mr(pd, ptr1, 4096, access), the full range of 8K
 * becomes DONTFORK. And the child process will hit a segment fault
 * during access ptr0/ptr2.
 * Note that the memory can be freed by libc free only.
 * TODO: move it to zmalloc.c if necessary
 */
static void *page_aligned_zalloc(size_t size) {
    void *tmp;
    size_t aligned_size, page_size = sysconf(_SC_PAGESIZE);

    aligned_size = (size + page_size - 1) & (~(page_size - 1));
    if (posix_memalign(&tmp, page_size, aligned_size)) {
        serverPanic("posix_memalign failed");
    }

    memset(tmp, 0x00, aligned_size);

    return tmp;
}

static void rdmaDestroyIoBuf(RdmaContext *ctx) {
    if (ctx->rx.mr) {
        ibv_dereg_mr(ctx->rx.mr);
        ctx->rx.mr = NULL;
    }

    zlibc_free(ctx->rx.addr);
    ctx->rx.addr = NULL;

    if (ctx->tx.mr) {
        ibv_dereg_mr(ctx->tx.mr);
        ctx->tx.mr = NULL;
    }

    zlibc_free(ctx->tx.addr);
    ctx->tx.addr = NULL;

    if (ctx->cmd_mr) {
        ibv_dereg_mr(ctx->cmd_mr);
        ctx->cmd_mr = NULL;
    }

    zlibc_free(ctx->cmd_buf);
    ctx->cmd_buf = NULL;
}

static int rdmaSetupIoBuf(RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    int access = IBV_ACCESS_LOCAL_WRITE;
    size_t length = sizeof(RedisRdmaCmd) * REDIS_MAX_WQE * 2;
    RedisRdmaCmd *cmd;
    int i;

    /* setup CMD buf & MR */
    ctx->cmd_buf = page_aligned_zalloc(length);
    ctx->cmd_mr = ibv_reg_mr(ctx->pd, ctx->cmd_buf, length, access);
    if (!ctx->cmd_mr) {
        serverLog(LL_WARNING, "RDMA: reg mr for CMD failed");
        goto destroy_iobuf;
    }

    for (i = 0; i < REDIS_MAX_WQE; i++) {
        cmd = ctx->cmd_buf + i;

        if (rdmaPostRecv(ctx, cm_id, cmd) == C_ERR) {
            serverLog(LL_WARNING, "RDMA: post recv failed");
            goto destroy_iobuf;
        }
    }

    for (i = REDIS_MAX_WQE; i < REDIS_MAX_WQE * 2; i++) {
        cmd = ctx->cmd_buf + i;
        cmd->keepalive.opcode = REDIS_RDMA_INVALID_OPCODE;
    }

    /* setup recv buf & MR */
    access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    length = redis_rdma_rx_size;
    ctx->rx.addr = page_aligned_zalloc(length);
    ctx->rx.length = length;
    ctx->rx.mr = ibv_reg_mr(ctx->pd, ctx->rx.addr, length, access);
    if (!ctx->rx.mr) {
        serverLog(LL_WARNING, "RDMA: reg mr for recv buffer failed");
        goto destroy_iobuf;
    }

    return C_OK;

destroy_iobuf:
    rdmaDestroyIoBuf(ctx);
    return C_ERR;
}

static int rdmaCreateResource(RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    int ret = C_OK;
    struct ibv_device_attr device_attr;
    struct ibv_qp_init_attr init_attr;
    struct ibv_comp_channel *comp_channel = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_pd *pd = NULL;

    if (ibv_query_device(cm_id->verbs, &device_attr)) {
        serverLog(LL_WARNING, "RDMA: ibv ibv query device failed");
        return C_ERR;
    }

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

    cq = ibv_create_cq(cm_id->verbs, REDIS_MAX_WQE * 2, NULL, comp_channel, redis_rdma_comp_vector % cm_id->verbs->num_comp_vectors);
    if (!cq) {
        serverLog(LL_WARNING, "RDMA: ibv create cq failed");
        return C_ERR;
    }

    ctx->cq = cq;
    ibv_req_notify_cq(cq, 0);

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = REDIS_MAX_WQE;
    init_attr.cap.max_recv_wr = REDIS_MAX_WQE;
    init_attr.cap.max_send_sge = device_attr.max_sge;
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

static int rdmaAdjustSendbuf(RdmaContext *ctx, unsigned int length) {
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (length == ctx->tx_length) {
        return C_OK;
    }

    /* try to free old MR & buffer */
    if (ctx->tx_length) {
        ibv_dereg_mr(ctx->tx.mr);
        zlibc_free(ctx->tx.addr);
        ctx->tx_length = 0;
    }

    /* create a new buffer & MR */
    ctx->tx.addr = page_aligned_zalloc(length);
    ctx->tx_length = length;
    ctx->tx.mr = ibv_reg_mr(ctx->pd, ctx->tx.addr, length, access);
    if (!ctx->tx.mr) {
        serverRdmaError(server.neterr, "RDMA: reg send mr failed");
        serverLog(LL_WARNING, "RDMA: FATAL error, recv corrupted cmd");
        zlibc_free(ctx->tx.addr);
        ctx->tx.addr = NULL;
        ctx->tx_length = 0;
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
    for (i = REDIS_MAX_WQE; i < 2 * REDIS_MAX_WQE; i++) {
        _cmd = ctx->cmd_buf + i;
        if (_cmd->keepalive.opcode == REDIS_RDMA_INVALID_OPCODE) {
            break;
        }
    }

    assert(i < 2 * REDIS_MAX_WQE);

    memcpy(_cmd, cmd, sizeof(RedisRdmaCmd));
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

    cmd.memory.opcode = htons(RegisterXferMemory);
    cmd.memory.addr = htonu64((uint64_t)ctx->rx.addr);
    cmd.memory.length = htonl(ctx->rx.length);
    cmd.memory.key = htonl(ctx->rx.mr->rkey);

    ctx->rx.offset = 0;
    ctx->rx.pos = 0;

    return rdmaSendCommand(ctx, cm_id, &cmd);
}

static int connRdmaGetFeature(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd) {
    RedisRdmaCmd _cmd;

    _cmd.feature.opcode = htons(GetServerFeature);
    _cmd.feature.select = cmd->feature.select;
    _cmd.feature.features = htonu64(0); /* currently no feature support */

    return rdmaSendCommand(ctx, cm_id, &_cmd);
}

static int connRdmaSetFeature(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd) {
    UNUSED(ctx);
    UNUSED(cm_id);

    /* currently no feature support */
    if (ntohu64(cmd->feature.features))
        return C_ERR;

    return C_OK;
}

static int rdmaHandleEstablished(struct rdma_cm_event *ev) {
    struct rdma_cm_id *cm_id = ev->id;
    RdmaContext *ctx = cm_id->context;

    connRdmaRegisterRx(ctx, cm_id);

    return C_OK;
}

static int rdmaHandleDisconnect(struct rdma_cm_event *ev) {
    struct rdma_cm_id *cm_id = ev->id;
    RdmaContext *ctx = cm_id->context;
    connection *conn = ctx->conn;
    rdma_connection *rdma_conn = (rdma_connection *)conn;

    conn->state = CONN_STATE_CLOSED;

    /* we can't close connection now, let's mark this connection as closed state */
    listAddNodeTail(pending_list, conn);
    rdma_conn->pending_list_node = listLast(pending_list);

    return C_OK;
}

static int connRdmaHandleRecv(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd, uint32_t byte_len) {
    if (unlikely(byte_len != sizeof(RedisRdmaCmd))) {
        serverLog(LL_WARNING, "RDMA: FATAL error, recv corrupted cmd");
        return C_ERR;
    }

    switch (ntohs(cmd->keepalive.opcode)) {
    case GetServerFeature:
        connRdmaGetFeature(ctx, cm_id, cmd);
        break;

    case SetClientFeature:
        connRdmaSetFeature(ctx, cm_id, cmd);
        break;

    case Keepalive:
        break;

    case RegisterXferMemory:
        ctx->tx_addr = (char *)ntohu64(cmd->memory.addr);
        ctx->tx.length = ntohl(cmd->memory.length);
        ctx->tx_key = ntohl(cmd->memory.key);
        ctx->tx.offset = 0;
        rdmaAdjustSendbuf(ctx, ctx->tx.length);
        break;

    default:
        serverLog(LL_WARNING, "RDMA: FATAL error, unknown cmd");
        return C_ERR;
    }

    return rdmaPostRecv(ctx, cm_id, cmd);
}

static int connRdmaHandleSend(RedisRdmaCmd *cmd) {
    /* clear cmd and mark this cmd has already sent */
    memset(cmd, 0x00, sizeof(*cmd));
    cmd->keepalive.opcode = REDIS_RDMA_INVALID_OPCODE;

    return C_OK;
}

static int connRdmaHandleRecvImm(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd, uint32_t byte_len) {
    assert(byte_len + ctx->rx.offset <= ctx->rx.length);

    ctx->rx.offset += byte_len;

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
        if (rdma_conn->c.state == CONN_STATE_CONNECTED) {
            serverLog(LL_WARNING, "RDMA: CQ handle error status: %s[0x%x], opcode : 0x%x", ibv_wc_status_str(wc.status), wc.status, wc.opcode);
        }
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
        if (connRdmaHandleRecvImm(ctx, cm_id, cmd, ntohl(wc.imm_data)) == C_ERR) {
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
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;
    struct ibv_device_attr device_attr;
    int ret = C_OK;

    if (conn->state != CONN_STATE_ACCEPTING)
        return C_ERR;

    conn->state = CONN_STATE_CONNECTED;

    connIncrRefs(conn);
    if (!callHandler(conn, accept_handler))
        ret = C_ERR;
    connDecrRefs(conn);

    if (ibv_query_device(cm_id->verbs, &device_attr)) {
        serverLog(LL_WARNING, "RDMA: ibv ibv query device failed");
        return C_ERR;
    }

    conn->iovcnt = min(device_attr.max_sge, IOV_MAX);
    ctx->conn = conn; /* save conn into RdmaContext */

    return ret;
}

static connection *connCreateRdma(void) {
    rdma_connection *rdma_conn = zcalloc(sizeof(rdma_connection));
    rdma_conn->c.type = &CT_RDMA;
    rdma_conn->c.fd = -1;
    rdma_conn->c.iovcnt = 1; /* at least 1, overwrite this on connect */

    return (connection *)rdma_conn;
}

static connection *connCreateAcceptedRdma(int fd, void *priv) {
    rdma_connection *rdma_conn = (rdma_connection *)connCreateRdma();
    rdma_conn->c.fd = fd;
    rdma_conn->c.state = CONN_STATE_ACCEPTING;
    rdma_conn->cm_id = priv;
    /* The comp channel fd should be always non block */
    connNonBlock(&rdma_conn->c);

    return (connection *)rdma_conn;
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
    while (ctx->rx.pos < ctx->rx.offset) {
        if (conn->read_handler && (callHandler(conn, conn->read_handler) == C_ERR)) {
            return;
        }
    }

    /* recv buf is full, register a new RX buffer */
    if (ctx->rx.pos == ctx->rx.length) {
        connRdmaRegisterRx(ctx, cm_id);
    }

    /* RDMA comp channel has no POLLOUT event, try to send remaining buffer */
    if ((ctx->tx.offset < ctx->tx.length) && conn->write_handler) {
        callHandler(conn, conn->write_handler);
    }
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
    ctx->ip = zstrdup(ip);
    ctx->port = *port;
    cm_id->context = ctx;
    if (rdmaCreateResource(ctx, cm_id) == C_ERR) {
        goto reject;
    }

    ret = rdma_accept(cm_id, &conn_param);
    if (ret) {
        serverRdmaError(err, "RDMA: accept failed");
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

static rdma_listener *rdmaFdToListener(connListener *listener, int fd) {
    for (int i = 0; i < listener->count; i++) {
        if (listener->fd[i] != fd)
            continue;

        return (rdma_listener *)listener->priv + i;
    }

    return NULL;
}

/*
 * rdmaAccept, actually it works as cm-event handler for listen cm_id.
 * accept a connection logic works in two steps:
 * 1, handle RDMA_CM_EVENT_CONNECT_REQUEST and return CM fd on success
 * 2, handle RDMA_CM_EVENT_ESTABLISHED and return C_OK on success
 */
static int rdmaAccept(connListener *listener, char *err, int fd, char *ip, size_t ip_len, int *port, void **priv) {
    struct rdma_cm_event *ev;
    enum rdma_cm_event_type ev_type;
    int ret = C_OK;
    rdma_listener *rdma_listener;

    rdma_listener = rdmaFdToListener(listener, fd);
    if (!rdma_listener) {
        serverPanic("RDMA: unexpected listen file descriptor");
    }

    ret = rdma_get_cm_event(rdma_listener->cm_channel, &ev);
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

static void connRdmaAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport = 0, cfd, max = server.max_new_conns_per_cycle;
    char cip[NET_IP_STR_LEN];
    void *connpriv = NULL;
    connListener *listener = (connListener *)privdata;
    UNUSED(el);
    UNUSED(mask);

    while(max--) {
        cfd = rdmaAccept(listener, server.neterr, fd, cip, sizeof(cip), &cport, &connpriv);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "RDMA Accepting client connection: %s", server.neterr);
            return;
        } else if (cfd == ANET_OK)
            continue;

        serverLog(LL_VERBOSE,"RDMA Accepted %s:%d", cip, cport);
        acceptCommonHandler(connCreateAcceptedRdma(cfd, connpriv), 0, cip);
    }
}

static int connRdmaSetRwHandler(connection *conn) {
    /* IB channel only has POLLIN event */
    if (conn->read_handler || conn->write_handler) {
        if (aeCreateFileEvent(server.el, conn->fd, AE_READABLE, conn->type->ae_handler, conn) == AE_ERR) {
            return C_ERR;
        }
    } else {
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE);
    }

    return C_OK;
}

static int connRdmaSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;

    if (conn->state != CONN_STATE_CONNECTED) {
        return C_OK;
    }

    conn->write_handler = func;
    if (barrier) {
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    } else {
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    }

    /* does this connection has pending write data? */
    if (func) {
        listAddNodeTail(pending_list, conn);
        rdma_conn->pending_list_node = listLast(pending_list);
    } else if (rdma_conn->pending_list_node) {
        listDelNode(pending_list, rdma_conn->pending_list_node);
        rdma_conn->pending_list_node = NULL;
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

/* TODO: rdmaAccept also deals with RDMA event, server side has different logic with client side, maybe we can merge this CM logic in future */
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

static void connRdmaShutdown(connection *conn) {
    UNUSED(conn);
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
    uint32_t off = ctx->tx.offset;
    char *addr = ctx->tx.addr + off;
    char *remote_addr = ctx->tx_addr + ctx->tx.offset;
    int ret;

    memcpy(addr, data, data_len);

    sge.addr = (uint64_t)addr;
    sge.lkey = ctx->tx.mr->lkey;
    sge.length = data_len;

    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = (++ctx->tx_ops % (REDIS_MAX_WQE / 2)) ? 0 : IBV_SEND_SIGNALED;
    send_wr.imm_data = htonl(data_len);
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

    ctx->tx.offset += data_len;

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

    assert(ctx->tx.offset <= ctx->tx.length);
    towrite = MIN(ctx->tx.length - ctx->tx.offset, data_len);
    if (!towrite) {
        return 0;
    }

    return connRdmaSend(conn, data, towrite);
}

static int connRdmaWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    int ret, nwritten = 0;

    for (int i = 0; i < iovcnt; i++) {
        ret = connRdmaWrite(conn, iov[i].iov_base, iov[i].iov_len);
        if (ret == C_ERR)
            return C_ERR;
        nwritten += ret;
    }

    return nwritten;
}

static inline uint32_t rdmaRead(RdmaContext *ctx, void *buf, size_t buf_len) {
    uint32_t toread;

    toread = MIN(ctx->rx.offset - ctx->rx.pos, buf_len);

    assert(ctx->rx.pos + toread <= ctx->rx.length);
    memcpy(buf, ctx->rx.addr + ctx->rx.pos, toread);

    ctx->rx.pos += toread;

    return toread;
}

static int connRdmaRead(connection *conn, void *buf, size_t buf_len) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    RdmaContext *ctx = cm_id->context;

    if (conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED) {
        return C_ERR;
    }

    /* No more data to read */
    if (ctx->rx.pos == ctx->rx.offset) {
        return -1;
    }

    assert(ctx->rx.pos < ctx->rx.offset);

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

    assert(ctx->tx.offset <= ctx->tx.length);
    if (ctx->tx.offset < ctx->tx.length) {
        /* TX buffer is available */
        goto copy;
    }

wait:
    if (connRdmaWait(conn, start, timeout) == C_ERR) {
        return C_ERR;
    }

    if (unlikely(!ctx->tx.mr)) {
        goto wait;
    }

copy:
    towrite = MIN(ctx->tx.length - ctx->tx.offset, size - nwritten);
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

    assert(ctx->rx.pos <= ctx->rx.offset);
    if (ctx->rx.pos < ctx->rx.offset) {
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

    assert(ctx->rx.pos <= ctx->rx.offset);
    if (ctx->rx.pos < ctx->rx.offset) {
        goto copy;
    }

wait:
    if (connRdmaWait(conn, start, timeout) == C_ERR) {
        return C_ERR;
    }

copy:
    for (toread = 0; toread <= ctx->rx.offset - ctx->rx.pos; toread++) {
        c = ctx->rx.addr + ctx->rx.pos + toread;
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

static const char *connRdmaGetType(connection *conn) {
    UNUSED(conn);

    return CONN_TYPE_RDMA;
}

static int rdmaServer(char *err, int port, char *bindaddr, int af, rdma_listener *rdma_listener) {
    int ret = ANET_OK, rv, afonly = 1;
    char _port[6];  /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage sock_addr;
    struct rdma_cm_id *listen_cmid = NULL;
    struct rdma_event_channel *listen_channel = NULL;

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
        serverRdmaError(err, "RDMA: %s", gai_strerror(rv));
        return ANET_ERR;
    } else if (!servinfo) {
        serverRdmaError(err, "RDMA: get addr info failed");
        ret = ANET_ERR;
        goto end;
    }

    listen_channel = rdma_create_event_channel();
    if (!listen_channel) {
        serverLog(LL_WARNING, "RDMA: create event channel failed");
        goto error;
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
            serverRdmaError(err, "RDMA: create listen cm id error");
            return ANET_ERR;
        }

        rdma_set_option(listen_cmid, RDMA_OPTION_ID, RDMA_OPTION_ID_AFONLY,
                        &afonly, sizeof(afonly));

        if (rdma_bind_addr(listen_cmid, (struct sockaddr *)&sock_addr)) {
            serverRdmaError(err, "RDMA: bind addr error");
            goto error;
        }

        if (rdma_listen(listen_cmid, 0)) {
            serverRdmaError(err, "RDMA: listen addr error");
            goto error;
        }

        rdma_listener->cm_id = listen_cmid;
        rdma_listener->cm_channel = listen_channel;
        goto end;
    }

error:
    if (listen_cmid)
        rdma_destroy_id(listen_cmid);
    if (listen_channel)
        rdma_destroy_event_channel(listen_channel);
    ret = ANET_ERR;

end:
    freeaddrinfo(servinfo);
    return ret;
}

int connRdmaListen(connListener *listener) {
    int j, ret;
    char **bindaddr = listener->bindaddr;
    int bindaddr_count = listener->bindaddr_count;
    int port = listener->port;
    char *default_bindaddr[2] = {"*", "-::*"};
    rdma_listener *rdma_listener;

    assert(server.proto_max_bulk_len <= 512ll * 1024 * 1024);

    /* Force binding of 0.0.0.0 if no bind address is specified. */
    if (listener->bindaddr_count == 0) {
        bindaddr_count = 2;
        bindaddr = default_bindaddr;
    }

    listener->priv = rdma_listener = zcalloc_num(bindaddr_count, sizeof(*rdma_listener));
    for (j = 0; j < bindaddr_count; j++) {
        char* addr = bindaddr[j];
        int optional = *addr == '-';

        if (optional)
            addr++;
        if (strchr(addr,':')) {
            /* Bind IPv6 address. */
            ret = rdmaServer(server.neterr, port, addr, AF_INET6, rdma_listener);
        } else {
            /* Bind IPv4 address. */
            ret = rdmaServer(server.neterr, port, addr, AF_INET, rdma_listener);
        }

        if (ret == ANET_ERR) {
            serverLog(LL_WARNING, "RDMA: Could not create server for %s:%d: %s",
                      addr, port, server.neterr);

            return C_ERR;
        }

        int fd = rdma_listener->cm_channel->fd;
        anetNonBlock(NULL, fd);
        anetCloexec(fd);
        listener->fd[listener->count++] = fd;
        rdma_listener++;
    }

    return C_OK;
}

static int connRdmaAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    rdma_connection *rdma_conn = (rdma_connection *)conn;
    struct rdma_cm_id *cm_id = rdma_conn->cm_id;
    struct sockaddr_storage *ss = NULL;
    struct sockaddr_in *sa4;
    struct sockaddr_in6 *sa6;

    if (remote)
        ss = (struct sockaddr_storage *)rdma_get_peer_addr(cm_id);
    else
        ss = (struct sockaddr_storage *)rdma_get_local_addr(cm_id);

    if (!ss) {
        goto error;
    }

    if (ss->ss_family == AF_INET) {
        sa4 = (struct sockaddr_in *)ss;
        if (ip) {
            if (inet_ntop(AF_INET, (void *)&(sa4->sin_addr), ip, ip_len) == NULL) {
                goto error;
            }
        }

        if (port) {
            *port = ntohs(sa4->sin_port);
         }
    } else if (ss->ss_family == AF_INET6) {
        sa6 = (struct sockaddr_in6 *)ss;
        if (ip) {
            if (inet_ntop(AF_INET6, (void *)&(sa6->sin6_addr), ip, ip_len) == NULL) {
                goto error;
            }
        }

        if (port) {
            *port = ntohs(sa6->sin6_port);
        }
    } else {
        /* TODO IB protocol */
        goto error;
    }

    return 0;

error:
    if (ip) {
        if (ip_len >= 2) {
            ip[0] = '?';
            ip[1] = '\0';
        } else if (ip_len == 1) {
            ip[0] = '\0';
        }
    }

    if (port)
        *port = 0;

    return -1;
}

static void rdmaInit(void) {
    pending_list = listCreate();

    REDIS_BUILD_BUG_ON(sizeof(RedisRdmaFeature) != 32);
    REDIS_BUILD_BUG_ON(sizeof(RedisRdmaKeepalive) != 32);
    REDIS_BUILD_BUG_ON(sizeof(RedisRdmaMemory) != 32);
    REDIS_BUILD_BUG_ON(sizeof(RedisRdmaCmd) != 32);

    if (ibv_fork_init()) {
        serverLog(LL_WARNING, "RDMA: FATAL error, ibv_fork_init failed");
    }
}

static int rdmaHasPendingData(void) {
    if (!pending_list)
        return 0;

    return listLength(pending_list) > 0;
}

static int rdmaProcessPendingData(void) {
    listIter li;
    listNode *ln;
    rdma_connection *rdma_conn;
    connection *conn;
    listNode *node;
    int processed;

    processed = listLength(pending_list);
    listRewind(pending_list, &li);
    while((ln = listNext(&li))) {
        rdma_conn = listNodeValue(ln);
        conn = &rdma_conn->c;
        node = rdma_conn->pending_list_node;

        /* a connection can be disconnected by remote peer, CM event mark state as CONN_STATE_CLOSED, kick connection read/write handler to close connection */
        if (conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED) {
            listDelNode(pending_list, node);
            /* do NOT call callHandler(conn, conn->read_handler) here, conn is freed in handler! */
            if (conn->read_handler) {
                conn->read_handler(conn);
            } else if (conn->write_handler) {
                conn->write_handler(conn);
            }

            continue;
        }

        connRdmaEventHandler(NULL, -1, rdma_conn, 0);
    }

    return processed;
}

static ConnectionType CT_RDMA = {
    /* connection type */
    .get_type = connRdmaGetType,

    /* connection type initialize & finalize & configure */
    .init = rdmaInit,
    .cleanup = NULL,

    /* ae & accept & listen & error & address handler */
    .ae_handler = connRdmaEventHandler,
    .accept_handler = connRdmaAcceptHandler,
    //.cluster_accept_handler = NULL,
    .listen = connRdmaListen,
    .addr = connRdmaAddr,

    /* create/close connection */
    .conn_create = connCreateRdma,
    .conn_create_accepted = connCreateAcceptedRdma,
    .shutdown = connRdmaShutdown,
    .close = connRdmaClose,

    /* connect & accept */
    .connect = connRdmaConnect,
    .blocking_connect = connRdmaBlockingConnect,
    .accept = connRdmaAccept,

    /* IO */
    .write = connRdmaWrite,
    .writev = connRdmaWritev,
    .read = connRdmaRead,
    .set_write_handler = connRdmaSetWriteHandler,
    .set_read_handler = connRdmaSetReadHandler,
    .get_last_error = connRdmaGetLastError,
    .sync_write = connRdmaSyncWrite,
    .sync_read = connRdmaSyncRead,
    .sync_readline = connRdmaSyncReadLine,

    /* pending data */
    .has_pending_data = rdmaHasPendingData,
    .process_pending_data = rdmaProcessPendingData,
};

static struct connListener *rdmaListener(void) {
    static struct connListener *listener = NULL;

    if (listener)
        return listener;

    listener = listenerByType(CONN_TYPE_RDMA);
    serverAssert(listener != NULL);

    return listener;
}

ConnectionType *connectionTypeRdma(void) {
    static ConnectionType *ct_rdma = NULL;

    if (ct_rdma != NULL)
        return ct_rdma;

    ct_rdma = connectionByType(CONN_TYPE_RDMA);
    serverAssert(ct_rdma != NULL);

    return ct_rdma;
}

/* rdma listener has different create/close logic from TCP, we can't re-use 'int changeListener(connListener *listener)' directly */
static int rdmaChangeListener(void) {
    struct connListener *listener = rdmaListener();

    /* Close old servers */
    for (int i = 0; i < listener->count; i++) {
        if (listener->fd[i] == -1)
            continue;

        aeDeleteFileEvent(server.el, listener->fd[i], AE_READABLE);
        listener->fd[i] = -1;
        struct rdma_listener *rdma_listener = (struct rdma_listener *)listener->priv + i;
        rdma_destroy_id(rdma_listener->cm_id);
        rdma_destroy_event_channel(rdma_listener->cm_channel);
    }

    listener->count = 0;
    zfree(listener->priv);

    closeListener(listener);

    /* Just close the server if port disabled */
    if (listener->port == 0) {
        if (server.set_proc_title) redisSetProcTitle(NULL);
        return REDISMODULE_OK;
    }

    /* Re-create listener */
    if (connListen(listener) != C_OK) {
        return REDISMODULE_ERR;
    }

    /* Create event handlers */
    if (createSocketAcceptHandler(listener, listener->ct->accept_handler) != C_OK) {
        serverPanic("Unrecoverable error creating %s accept handler.", listener->ct->get_type(NULL));
    }

    if (server.set_proc_title) redisSetProcTitle(NULL);

    return REDISMODULE_OK;
}

#else    /* __linux__ */

"BUILD ERROR: RDMA is only supported on Linux"

#endif   /* __linux__ */

#ifdef BUILD_RDMA_MODULE

#include "release.h"

static long long rdmaGetPort(const char *name, void *privdata) {
    UNUSED(name);
    UNUSED(privdata);
    struct connListener *listener = rdmaListener();

    return listener->port;
}

static int rdmaSetPort(const char *name, long long val, void *privdata, RedisModuleString **err) {
    UNUSED(name);
    UNUSED(privdata);
    UNUSED(err);
    struct connListener *listener = rdmaListener();
    listener->port = val;

    return REDISMODULE_OK;
}

static RedisModuleString *rdma_bind;

static void rdmaBuildBind(void *ctx) {
    struct connListener *listener = rdmaListener();

    if (rdma_bind)
        RedisModule_FreeString(NULL, rdma_bind);

    sds rdma_bind_str = sdsjoin(listener->bindaddr, listener->bindaddr_count, " ");
    rdma_bind = RedisModule_CreateString(ctx, rdma_bind_str, sdslen(rdma_bind_str));
}

static RedisModuleString *rdmaGetBind(const char *name, void *privdata) {
    UNUSED(name);
    UNUSED(privdata);

    return rdma_bind;
}

static int rdmaSetBind(const char *name, RedisModuleString *val, void *privdata, RedisModuleString **err) {
    UNUSED(name);
    UNUSED(err);
    struct connListener *listener = rdmaListener();
    const char *bind = RedisModule_StringPtrLen(val, NULL);
    int nexts;
    sds *exts = sdssplitlen(bind, strlen(bind), " ", 1, &nexts);

    if (nexts > CONFIG_BINDADDR_MAX) {
        serverLog(LL_WARNING, "RDMA: Unsupported bind ( > %d)", CONFIG_BINDADDR_MAX);
        return REDISMODULE_ERR;
    }

    /* Free old bind addresses */
    for (int j = 0; j < listener->bindaddr_count; j++) {
        zfree(listener->bindaddr[j]);
    }

    for (int j = 0; j < nexts; j++)
        listener->bindaddr[j] = zstrdup(exts[j]);
    listener->bindaddr_count = nexts;

    sdsfreesplitres(exts, nexts);
    rdmaBuildBind(privdata);

    return REDISMODULE_OK;
}

static int rdmaApplyListener(RedisModuleCtx *ctx, void *privdata, RedisModuleString **err) {
    UNUSED(ctx);
    UNUSED(privdata);
    UNUSED(err);

    return rdmaChangeListener();
}

static void rdmaListenerAddConfig(void *ctx) {
    serverAssert(RedisModule_RegisterNumericConfig(ctx, "port", 0, REDISMODULE_CONFIG_DEFAULT, 0, 65535, rdmaGetPort, rdmaSetPort, rdmaApplyListener, NULL) == REDISMODULE_OK);
    serverAssert(RedisModule_RegisterStringConfig(ctx, "bind", "", REDISMODULE_CONFIG_DEFAULT, rdmaGetBind, rdmaSetBind, rdmaApplyListener, ctx) == REDISMODULE_OK);
    serverAssert(RedisModule_LoadConfigs(ctx) == REDISMODULE_OK);
}

int RedisModule_OnLoad(void *ctx, RedisModuleString **argv, int argc)
{
    /* Connection modules MUST be part of the same build as redis. */
    if (strcmp(REDIS_BUILD_ID_RAW, redisBuildIdRaw())) {
        serverLog(LL_NOTICE, "Connection type %s was not built together with the redis-server used.", CONN_TYPE_RDMA);
        return REDISMODULE_ERR;
    }

    if (RedisModule_Init(ctx, CONN_TYPE_RDMA, 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Connection modules is available only bootup. */
    if ((RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_SERVER_STARTUP) == 0) {
        serverLog(LL_NOTICE, "Connection type %s can be loaded only during bootup", CONN_TYPE_RDMA);
        return REDISMODULE_ERR;
    }

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD);

    if (connTypeRegister(&CT_RDMA) != C_OK)
        return REDISMODULE_ERR;

    rdmaListenerAddConfig(ctx);

    struct connListener *listener = rdmaListener();
    listener->ct = connectionTypeRdma();
    listener->bindaddr = zcalloc_num(CONFIG_BINDADDR_MAX, sizeof(listener->bindaddr[0]));

    for (int i = 0; i < argc; i++) {
        robj *str = (robj *)argv[i];
        int nexts;
        sds *exts = sdssplitlen(str->ptr, strlen(str->ptr), "=", 1, &nexts);
        if (nexts != 2) {
            serverLog(LL_WARNING, "RDMA: Unsupported argument \"%s\"", (char *)str->ptr);
            return REDISMODULE_ERR;
        }

        if (!strcasecmp(exts[0], "bind")) {
            listener->bindaddr[listener->bindaddr_count++] = zstrdup(exts[1]);
        } else if(!strcasecmp(exts[0], "port")) {
            listener->port = atoi(exts[1]);
        } else if(!strcasecmp(exts[0], "rx-size")) {
            redis_rdma_rx_size = atoi(exts[1]);
        } else if(!strcasecmp(exts[0], "comp-vector")) {
            redis_rdma_comp_vector = atoi(exts[1]);
        } else {
            serverLog(LL_WARNING, "RDMA: Unsupported argument \"%s\"", (char *)str->ptr);
            return REDISMODULE_ERR;
        }

        sdsfreesplitres(exts, nexts);
    }

    rdmaBuildBind(ctx);
    if (redis_rdma_comp_vector == -1)
        redis_rdma_comp_vector = abs((int)random());

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(void *arg) {
    UNUSED(arg);
    serverLog(LL_NOTICE, "Connection type %s can not be unloaded", CONN_TYPE_RDMA);
    return REDISMODULE_ERR;
}

#endif /* BUILD_RDMA_MODULE */

#endif    /* USE_RDMA */
