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

#include "fmacros.h"

#include "async.h"
#include "async_private.h"
#include "hiredis.h"
#include "rdma.h"
#include <errno.h>

#define UNUSED(x) (void)(x)

void __redisSetError(redisContext *c, int type, const char *str);

#ifdef USE_RDMA
#ifdef __linux__    /* currently RDMA is only supported on Linux */
#define __USE_MISC
#include <arpa/inet.h>
#include <assert.h>
#include <endian.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>


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

typedef struct RdmaContext {
    struct rdma_cm_id *cm_id;
    struct rdma_event_channel *cm_channel;
    struct ibv_comp_channel *comp_channel;
    struct ibv_cq *cq;
    struct ibv_pd *pd;

    /* TX */
    char *tx_addr;
    uint32_t tx_length;
    uint32_t tx_offset;
    uint32_t tx_key;
    char *send_buf;
    uint32_t send_length;
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

int redisContextTimeoutMsec(redisContext *c, long *result);
int redisContextUpdateConnectTimeout(redisContext *c, const struct timeval *timeout);
int redisSetFdBlocking(redisContext *c, redisFD fd, int blocking);

static inline long redisNowMs(void) {
    struct timeval tv;

    if (gettimeofday(&tv, NULL) < 0)
            return -1;

    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}


static int rdmaPostRecv(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd) {
    struct ibv_sge sge;
    size_t length = sizeof(RedisRdmaCmd);
    struct ibv_recv_wr recv_wr, *bad_wr;


    sge.addr = (uint64_t)cmd;
    sge.length = length;
    sge.lkey = ctx->cmd_mr->lkey;

    recv_wr.wr_id = (uint64_t)cmd;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    recv_wr.next = NULL;

    if (ibv_post_recv(cm_id->qp, &recv_wr, &bad_wr)) {
        return REDIS_ERR;
    }

    return REDIS_OK;
}

static void rdmaDestroyIoBuf(RdmaContext *ctx)
{
    if (ctx->recv_mr) {
        ibv_dereg_mr(ctx->recv_mr);
        ctx->recv_mr = NULL;
    }

    hi_free(ctx->recv_buf);
    ctx->recv_buf = NULL;

    if (ctx->send_mr) {
        ibv_dereg_mr(ctx->send_mr);
        ctx->send_mr = NULL;
    }

    hi_free(ctx->send_buf);
    ctx->send_buf = NULL;

    if (ctx->cmd_mr) {
        ibv_dereg_mr(ctx->cmd_mr);
        ctx->cmd_mr = NULL;
    }

    hi_free(ctx->cmd_buf);
    ctx->cmd_buf = NULL;
}

static int rdmaSetupIoBuf(redisContext *c, RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    int access = IBV_ACCESS_LOCAL_WRITE;
    size_t length = sizeof(RedisRdmaCmd) * REDIS_MAX_SGE * 2;
    RedisRdmaCmd *cmd;
    int i;

    /* setup CMD buf & MR */
    ctx->cmd_buf = hi_calloc(length, 1);
    ctx->cmd_mr = ibv_reg_mr(ctx->pd, ctx->cmd_buf, length, access);
    if (!ctx->cmd_mr) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: reg recv mr failed");
	goto destroy_iobuf;
    }

    for (i = 0; i < REDIS_MAX_SGE; i++) {
        cmd = ctx->cmd_buf + i;

        if (rdmaPostRecv(ctx, cm_id, cmd) == REDIS_ERR) {
            __redisSetError(c, REDIS_ERR_OTHER, "RDMA: post recv failed");
            goto destroy_iobuf;
        }
    }

    /* setup recv buf & MR */
    access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    length = REDIS_RDMA_DEFAULT_RX_LEN;
    ctx->recv_buf = hi_calloc(length, 1);
    ctx->recv_length = length;
    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, length, access);
    if (!ctx->recv_mr) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: reg send mr failed");
	goto destroy_iobuf;
    }

    return REDIS_OK;

destroy_iobuf:
    rdmaDestroyIoBuf(ctx);
    return REDIS_ERR;
}

static int rdmaAdjustSendbuf(redisContext *c, RdmaContext *ctx, unsigned int length) {
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (length == ctx->send_length) {
        return REDIS_OK;
    }

    /* try to free old MR & buffer */
    if (ctx->send_length) {
        ibv_dereg_mr(ctx->send_mr);
        hi_free(ctx->send_buf);
        ctx->send_length = 0;
    }

    /* create a new buffer & MR */
    ctx->send_buf = hi_calloc(length, 1);
    ctx->send_length = length;
    ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, length, access);
    if (!ctx->send_mr) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: reg send buf mr failed");
        hi_free(ctx->send_buf);
        ctx->send_buf = NULL;
        ctx->send_length = 0;
        return REDIS_ERR;
    }

    return REDIS_OK;
}


static int rdmaSendCommand(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd) {
    struct ibv_send_wr send_wr, *bad_wr;
    struct ibv_sge sge;
    RedisRdmaCmd *_cmd;
    int i;
    int ret;

    /* find an unused cmd buffer */
    for (i = REDIS_MAX_SGE; i < 2 * REDIS_MAX_SGE; i++) {
        _cmd = ctx->cmd_buf + i;
        if (!_cmd->magic) {
            break;
        }
    }

    assert(i < 2 * REDIS_MAX_SGE);

    _cmd->addr = htobe64(cmd->addr);
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
        return REDIS_ERR;
    }

    return REDIS_OK;
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

static int connRdmaHandleRecv(redisContext *c, RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd, uint32_t byte_len) {
    RedisRdmaCmd _cmd;

    if (byte_len != sizeof(RedisRdmaCmd)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: FATAL error, recv corrupted cmd");
        return REDIS_ERR;
    }

    _cmd.addr = be64toh(cmd->addr);
    _cmd.length = ntohl(cmd->length);
    _cmd.key = ntohl(cmd->key);
    _cmd.opcode = cmd->opcode;

    switch (_cmd.opcode) {
    case RegisterLocalAddr:
        ctx->tx_addr = (char *)_cmd.addr;
        ctx->tx_length = _cmd.length;
        ctx->tx_key = _cmd.key;
        ctx->tx_offset = 0;
        rdmaAdjustSendbuf(c, ctx, ctx->tx_length);
        break;

    default:
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: FATAL error, unknown cmd");
        return REDIS_ERR;
    }

    return rdmaPostRecv(ctx, cm_id, cmd);
}

static int connRdmaHandleRecvImm(RdmaContext *ctx, struct rdma_cm_id *cm_id, RedisRdmaCmd *cmd, uint32_t byte_len) {
    assert(byte_len + ctx->rx_offset <= ctx->recv_length);
    ctx->rx_offset += byte_len;

    return rdmaPostRecv(ctx, cm_id, cmd);
}

static int connRdmaHandleSend(RedisRdmaCmd *cmd) {
    /* mark this cmd has already sent */
    cmd->magic = 0;

    return REDIS_OK;
}

static int connRdmaHandleWrite(RdmaContext *ctx, uint32_t byte_len) {
    UNUSED(ctx);
    UNUSED(byte_len);

    return REDIS_OK;
}

static int connRdmaHandleCq(redisContext *c) {
    RdmaContext *ctx = (RdmaContext *)c->privctx;
    struct rdma_cm_id *cm_id = ctx->cm_id;
    struct ibv_cq *ev_cq = NULL;
    void *ev_ctx = NULL;
    struct ibv_wc wc = {0};
    RedisRdmaCmd *cmd;
    int ret;

    if (ibv_get_cq_event(ctx->comp_channel, &ev_cq, &ev_ctx) < 0) {
        if (errno != EAGAIN) {
            __redisSetError(c, REDIS_ERR_OTHER, "RDMA: get cq event failed");
            return REDIS_ERR;
        }
    } else if (ibv_req_notify_cq(ev_cq, 0)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: notify cq failed");
        return REDIS_ERR;
    }

pollcq:
    ret = ibv_poll_cq(ctx->cq, 1, &wc);
    if (ret < 0) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: poll cq failed");
        return REDIS_ERR;
    } else if (ret == 0) {
        return REDIS_OK;
    }

    ibv_ack_cq_events(ctx->cq, 1);

    if (wc.status != IBV_WC_SUCCESS) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: send/recv failed");
        return REDIS_ERR;
    }

    switch (wc.opcode) {
    case IBV_WC_RECV:
        cmd = (RedisRdmaCmd *)wc.wr_id;
        if (connRdmaHandleRecv(c, ctx, cm_id, cmd, wc.byte_len) == REDIS_ERR) {
            return REDIS_ERR;
        }

        break;

    case IBV_WC_RECV_RDMA_WITH_IMM:
        cmd = (RedisRdmaCmd *)wc.wr_id;
        if (connRdmaHandleRecvImm(ctx, cm_id, cmd, wc.byte_len) == REDIS_ERR) {
            return REDIS_ERR;
        }

        break;
    case IBV_WC_RDMA_WRITE:
        if (connRdmaHandleWrite(ctx, wc.byte_len) == REDIS_ERR) {
            return REDIS_ERR;
        }

        break;
    case IBV_WC_SEND:
        cmd = (RedisRdmaCmd *)wc.wr_id;
        if (connRdmaHandleSend(cmd) == REDIS_ERR) {
            return REDIS_ERR;
        }

        break;
    default:
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: unexpected opcode");
        return REDIS_ERR;
    }

    goto pollcq;

    return REDIS_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

static int redisCommandTimeoutMsec(redisContext *c, long *result)
{
    const struct timeval *timeout = c->command_timeout;
    long msec = INT_MAX;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            *result = msec;
            return REDIS_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    *result = msec;
    return REDIS_OK;
}

static ssize_t redisRdmaRead(redisContext *c, char *buf, size_t bufcap) {
    RdmaContext *ctx = (RdmaContext *)c->privctx;
    struct rdma_cm_id *cm_id = ctx->cm_id;
    struct pollfd pfd;
    long timed = -1;
    long start = redisNowMs();
    uint32_t toread, remained;

    if (redisCommandTimeoutMsec(c, &timed)) {
        return REDIS_ERR;
    }

copy:
    if (ctx->recv_offset < ctx->rx_offset) {
        remained = ctx->rx_offset - ctx->recv_offset;
        toread = MIN(remained, bufcap);

        memcpy(buf, ctx->recv_buf + ctx->recv_offset, toread);
        ctx->recv_offset += toread;

        if (ctx->recv_offset == ctx->recv_length) {
            connRdmaRegisterRx(ctx, cm_id);
        }

        return toread;
    }

pollcq:
    /* try to poll a CQ firstly */
    if (connRdmaHandleCq(c) == REDIS_ERR) {
        return REDIS_ERR;
    }

    if (ctx->recv_offset < ctx->rx_offset) {
        goto copy;
    }

    pfd.fd = ctx->comp_channel->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 1000) < 0) {
        return REDIS_ERR;
    }

    if ((redisNowMs() - start) < timed) {
        goto pollcq;
    }

    __redisSetError(c, REDIS_ERR_TIMEOUT, "RDMA: read timeout");
    return REDIS_ERR;
}


static size_t connRdmaSend(RdmaContext *ctx, struct rdma_cm_id *cm_id, const void *data, size_t data_len) {
    struct ibv_send_wr send_wr, *bad_wr;
    struct ibv_sge sge;
    uint32_t off = ctx->tx_offset;
    char *addr = ctx->send_buf + off;
    char *remote_addr = ctx->tx_addr + off;
    int ret;

    assert(data_len <= ctx->tx_length);
    memcpy(addr, data, data_len);

    sge.addr = (uint64_t)addr;
    sge.lkey = ctx->send_mr->lkey;
    sge.length = data_len;

    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = (++ctx->send_ops % REDIS_MAX_SGE) ? 0 : IBV_SEND_SIGNALED;
    send_wr.imm_data = htonl(0);
    send_wr.wr.rdma.remote_addr = (uint64_t)remote_addr;
    send_wr.wr.rdma.rkey = ctx->tx_key;
    send_wr.next = NULL;
    ret = ibv_post_send(cm_id->qp, &send_wr, &bad_wr);
    if (ret) {
        return REDIS_ERR;
    }

    ctx->tx_offset += data_len;

    return data_len;
}

static ssize_t redisRdmaWrite(redisContext *c) {
    RdmaContext *ctx = (RdmaContext *)c->privctx;
    struct rdma_cm_id *cm_id = ctx->cm_id;
    size_t data_len = hi_sdslen(c->obuf);
    struct pollfd pfd;
    long timed = -1;
    long start = redisNowMs();
    uint32_t towrite, wrote = 0;
    size_t ret;

    if (redisCommandTimeoutMsec(c, &timed)) {
        return REDIS_ERR;
    }

    /* try to pollcq to */
    goto pollcq;

waitcq:
    pfd.fd = ctx->comp_channel->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 1) < 0) {
        return REDIS_ERR;
    }

pollcq:
    if (connRdmaHandleCq(c) == REDIS_ERR) {
        return REDIS_ERR;
    }

    assert(ctx->tx_offset <= ctx->tx_length);
    if (ctx->tx_offset == ctx->tx_length) {
        /* wait a new TX buffer */
	goto waitcq;
    }

    towrite = MIN(ctx->tx_length - ctx->tx_offset, data_len - wrote);
    ret = connRdmaSend(ctx, cm_id, c->obuf + wrote, towrite);
    if (ret == (size_t)REDIS_ERR) {
        return REDIS_ERR;
    }

    wrote += ret;
    if (wrote == data_len) {
        return data_len;
    }

    if ((redisNowMs() - start) < timed) {
        goto waitcq;
    }

    __redisSetError(c, REDIS_ERR_TIMEOUT, "RDMA: write timeout");

    return REDIS_ERR;
}

/* RDMA has no POLLOUT event supported, so it could't work well with hiredis async mechanism */
void redisRdmaAsyncRead(redisAsyncContext *ac) {
    UNUSED(ac);
    assert("hiredis async mechanism can't work with RDMA" == NULL);
}

void redisRdmaAsyncWrite(redisAsyncContext *ac) {
    UNUSED(ac);
    assert("hiredis async mechanism can't work with RDMA" == NULL);
}

static void redisRdmaClose(redisContext *c) {
    RdmaContext *ctx = (RdmaContext *)c->privctx;
    struct rdma_cm_id *cm_id = ctx->cm_id;

    connRdmaHandleCq(c);
    rdma_disconnect(cm_id);
    ibv_destroy_cq(ctx->cq);
    rdmaDestroyIoBuf(ctx);
    ibv_destroy_qp(cm_id->qp);
    ibv_destroy_comp_channel(ctx->comp_channel);
    ibv_dealloc_pd(ctx->pd);
    rdma_destroy_id(cm_id);

    rdma_destroy_event_channel(ctx->cm_channel);
}

static void redisRdmaFree(void *privctx) {
    if (!privctx)
        return;

    hi_free(privctx);
}

redisContextFuncs redisContextRdmaFuncs = {
    .close = redisRdmaClose,
    .free_privctx = redisRdmaFree,
    .async_read = redisRdmaAsyncRead,
    .async_write = redisRdmaAsyncWrite,
    .read = redisRdmaRead,
    .write = redisRdmaWrite,
};


static int redisRdmaConnect(redisContext *c, struct rdma_cm_id *cm_id) {
    RdmaContext *ctx = (RdmaContext *)c->privctx;
    struct ibv_comp_channel *comp_channel = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_qp_init_attr init_attr = {0};
    struct rdma_conn_param conn_param = {0};

    pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: alloc pd failed");
        goto error;
    }

    comp_channel = ibv_create_comp_channel(cm_id->verbs);
    if (!comp_channel) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: alloc pd failed");
        goto error;
    }

    if (redisSetFdBlocking(c, comp_channel->fd, 0) != REDIS_OK) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: set recv comp channel fd non-block failed");
        goto error;
    }

    cq = ibv_create_cq(cm_id->verbs, REDIS_MAX_SGE * 2, ctx, comp_channel, 0);
    if (!cq) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: create send cq failed");
        goto error;
    }

    if (ibv_req_notify_cq(cq, 0)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: notify send cq failed");
        goto error;
    }

    /* create qp with attr */
    init_attr.cap.max_send_wr = REDIS_MAX_SGE;
    init_attr.cap.max_recv_wr = REDIS_MAX_SGE; 
    init_attr.cap.max_send_sge = 1; 
    init_attr.cap.max_recv_sge = 1; 
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.send_cq = cq;
    init_attr.recv_cq = cq;
    if (rdma_create_qp(cm_id, pd, &init_attr)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: create qp failed");
        goto error;
    }

    ctx->cm_id = cm_id;
    ctx->comp_channel = comp_channel;
    ctx->cq = cq;
    ctx->pd = pd;

    if (rdmaSetupIoBuf(c, ctx, cm_id) != REDIS_OK)
        goto free_qp;

    /* rdma connect with param */
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 7;
    conn_param.rnr_retry_count = 7;
    if (rdma_connect(cm_id, &conn_param)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: connect failed");
        goto destroy_iobuf;
    }

    return REDIS_OK;

destroy_iobuf:
    rdmaDestroyIoBuf(ctx);
free_qp:
    ibv_destroy_qp(cm_id->qp);
error:
    if (cq)
        ibv_destroy_cq(cq);
    if (pd)
        ibv_dealloc_pd(pd);
    if (comp_channel)
        ibv_destroy_comp_channel(comp_channel);

    return REDIS_ERR;
}

static int redisRdmaEstablished(redisContext *c, struct rdma_cm_id *cm_id) {
    RdmaContext *ctx = (RdmaContext *)c->privctx;

    /* it's time to tell redis we have already connected */
    c->flags |= REDIS_CONNECTED;
    c->funcs = &redisContextRdmaFuncs;
    c->fd = ctx->comp_channel->fd;

    return connRdmaRegisterRx(ctx, cm_id);
}

static int redisRdmaCM(redisContext *c, int timeout) {
    RdmaContext *ctx = (RdmaContext *)c->privctx;
    struct rdma_cm_event *event;
    char errorstr[128];
    int ret = REDIS_ERR;

    while (rdma_get_cm_event(ctx->cm_channel, &event) == 0) {
        switch (event->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            if (timeout < 0 || timeout > 100)
                timeout = 100; /* at most 100ms to resolve route */
            ret = rdma_resolve_route(event->id, timeout);
            if (ret) {
                __redisSetError(c, REDIS_ERR_OTHER, "RDMA: route resolve failed");
            }
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            ret = redisRdmaConnect(c, event->id);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            ret = redisRdmaEstablished(c, event->id);
            break;
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            ret = REDIS_ERR;
            __redisSetError(c, REDIS_ERR_TIMEOUT, "RDMA: connect timeout");
            break;
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_DISCONNECTED:
        case RDMA_CM_EVENT_ADDR_CHANGE:
        default:
            snprintf(errorstr, sizeof(errorstr), "RDMA: connect failed - %s", rdma_event_str(event->event));
            __redisSetError(c, REDIS_ERR_OTHER, errorstr);
            ret = REDIS_ERR;
            break;
        }

        rdma_ack_cm_event(event);
    }

    return ret;
}

static int redisRdmaWaitConn(redisContext *c, long timeout) {
    int timed;
    struct pollfd pfd;
    long now = redisNowMs();
    long start = now;
    RdmaContext *ctx = (RdmaContext *)c->privctx;

    while (now - start < timeout) {
        timed = (int)(timeout - (now - start));

        pfd.fd = ctx->cm_channel->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, timed) < 0) {
            return REDIS_ERR;
        }

        if (redisRdmaCM(c, timed) == REDIS_ERR) {
            return REDIS_ERR;
        }

        if (c->flags & REDIS_CONNECTED) {
            return REDIS_OK;
        }

        now = redisNowMs();
    }

    return REDIS_ERR;
}

int redisContextConnectRdma(redisContext *c, const char *addr, int port,
                            const struct timeval *timeout) {
    int ret;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *p;
    long timeout_msec = -1;
    struct rdma_event_channel *cm_channel = NULL;
    struct rdma_cm_id *cm_id = NULL;
    RdmaContext *ctx = NULL;
    struct sockaddr_storage saddr;
    long start = redisNowMs(), timed;

    servinfo = NULL;
    c->connection_type = REDIS_CONN_RDMA;
    c->tcp.port = port;

    if (c->tcp.host != addr) {
        hi_free(c->tcp.host);

        c->tcp.host = hi_strdup(addr);
        if (c->tcp.host == NULL) {
            __redisSetError(c, REDIS_ERR_OOM, "RDMA: Out of memory");
            return REDIS_ERR;
        }
    }

    if (timeout) {
        if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR) {
            __redisSetError(c, REDIS_ERR_OOM, "RDMA: Out of memory");
            return REDIS_ERR;
        }
    } else {
        hi_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (redisContextTimeoutMsec(c, &timeout_msec) != REDIS_OK) {
        __redisSetError(c, REDIS_ERR_IO, "RDMA: Invalid timeout specified");
        return REDIS_ERR;
    } else if (timeout_msec == -1) {
        timeout_msec = INT_MAX;
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if ((ret = getaddrinfo(c->tcp.host, _port, &hints, &servinfo)) != 0) {
         hints.ai_family = AF_INET6;
         if ((ret = getaddrinfo(addr, _port, &hints, &servinfo)) != 0) {
            __redisSetError(c, REDIS_ERR_OTHER, gai_strerror(ret));
            return REDIS_ERR;
        }
    }

    ctx = hi_calloc(sizeof(RdmaContext), 1);
    if (!ctx) {
        __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
        goto error;
    }

    c->privctx = ctx;

   cm_channel = rdma_create_event_channel();
    if (!cm_channel) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: create event channel failed");
        goto error;
    }

    ctx->cm_channel = cm_channel;

    if (rdma_create_id(cm_channel, &cm_id, (void *)ctx, RDMA_PS_TCP)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: create id failed");
        return REDIS_ERR;
    }
    ctx->cm_id = cm_id;

    if ((redisSetFdBlocking(c, cm_channel->fd, 0) != REDIS_OK)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: set cm channel fd non-block failed");
        goto free_rdma;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if (p->ai_family == PF_INET) {
                memcpy(&saddr, p->ai_addr, sizeof(struct sockaddr_in));
                ((struct sockaddr_in *)&saddr)->sin_port = htons(port);
        } else if (p->ai_family == PF_INET6) {
                memcpy(&saddr, p->ai_addr, sizeof(struct sockaddr_in6));
                ((struct sockaddr_in6 *)&saddr)->sin6_port = htons(port);
        } else {
            __redisSetError(c, REDIS_ERR_PROTOCOL, "RDMA: unsupported family");
            goto free_rdma;
        }

        /* resolve addr as most 100ms */
        if (rdma_resolve_addr(cm_id, NULL, (struct sockaddr *)&saddr, 100)) {
            continue;
        }

        timed = timeout_msec - (redisNowMs() - start);
        if ((redisRdmaWaitConn(c, timed) == REDIS_OK) && (c->flags & REDIS_CONNECTED)) {
            ret = REDIS_OK;
            goto end;
        }
    }

    if ((!c->err) && (p == NULL)) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: resolve failed");
    }

free_rdma:
    if (cm_id) {
        rdma_destroy_id(cm_id);
    }
    if (cm_channel) {
        rdma_destroy_event_channel(cm_channel);
    }

error:
    ret = REDIS_ERR;
    if (ctx) {
        hi_free(ctx);
    }

end:
    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    return ret;
}

#else    /* __linux__ */

"BUILD ERROR: RDMA is only supported on linux"

#endif   /* __linux__ */
#else    /* USE_RDMA */

int redisContextConnectRdma(redisContext *c, const char *addr, int port,
                            const struct timeval *timeout) {
    UNUSED(c);
    UNUSED(addr);
    UNUSED(port);
    UNUSED(timeout);
    __redisSetError(c, REDIS_ERR_PROTOCOL, "RDMA: disabled, please rebuild with BUILD_RDMA");
    return -EPROTONOSUPPORT;
}

#endif
