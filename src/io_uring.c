#include "server.h"

#ifdef USE_IO_URING
#include <liburing.h>
/* io_uring instance queue depth */
static const unsigned int IOUringDepth = 256;
static unsigned int uringQueueLen = 0;

void initIOUring(void) {
    struct io_uring_params params;
    struct io_uring *ring = zmalloc(sizeof(struct io_uring));
    memset(&params, 0, sizeof(params));
    /* On success, io_uring_queue_init_params(3) returns 0 and ring will
     * point to the shared memory containing the io_uring queues.
     * On failure -errno is returned. */
    int ret = io_uring_queue_init_params(IOUringDepth, ring, &params);
    if (ret != 0) {
        serverLog(LL_WARNING, "System doesn't support io_uring, disable io_uring.");
        zfree(ring);
        server.io_uring = NULL;
        server.io_uring_enabled = 0;
    } else {
        serverLog(LL_NOTICE, "System support io_uring, enable io_uring.");
        server.io_uring = ring;
        server.io_uring_enabled = 1;
    }
}

void ioUringPrepWrite(client *c) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(server.io_uring);
    io_uring_prep_send(sqe, c->conn->fd, c->buf + c->sentlen,
        c->bufpos - c->sentlen, MSG_DONTWAIT);
    io_uring_sqe_set_data(sqe, c);
    uringQueueLen++;
}

void ioUringSubmitAndWait(void) {
    /* wait for all submitted queue entries complete. */
    while (uringQueueLen) {
        io_uring_submit(server.io_uring);
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(server.io_uring, &cqe) == 0) {
            client *c = io_uring_cqe_get_data(cqe);
            c->nwritten = cqe->res;
            if ((c->bufpos - c->sentlen) > c->nwritten && c->nwritten > 0) {
                c->sentlen += c->nwritten;
                ioUringPrepWrite(c);
            }
            io_uring_cqe_seen(server.io_uring, cqe);
            uringQueueLen--;
        }
    }
}

void freeIOUring(void) {
    if(server.io_uring_enabled) {
        io_uring_queue_exit(server.io_uring);
        zfree(server.io_uring);
        server.io_uring = NULL;
        server.io_uring_enabled = 0;
    }
}
#else
void initIOUring(void) {
    serverLog(LL_WARNING, "System doesn't support io_uring, disable io_uring.");
    server.io_uring = NULL;
    server.io_uring_enabled = 0;
}

void ioUringPrepWrite(client *c) {
    UNUSED(c);
}

void ioUringSubmitAndWait(void) {}

void freeIOUring(void) {}
#endif
