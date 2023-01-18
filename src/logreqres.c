/*
 * Copyright (c) 2021, Redis Ltd.
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

/* This file implements the interface of logging clients' requests and
 * responses into a file.
 * This feature needs the LOG_REQ_RES macro to be compiled and is turned
 * on/off by "DEBUG set-req-res-logfile <path>"
 *
 * Some examples:
 *
 * PING:
 *
 * 4
 * ping
 * 12
 * __argv_end__
 * +PONG
 *
 * LRANGE:
 *
 * 6
 * lrange
 * 4
 * list
 * 1
 * 0
 * 2
 * -1
 * 12
 * __argv_end__
 * *1
 * $3
 * ele
 *
 * The request is everything up until the __argv_end__ sentinel.
 * The format is:
 * <number of characters>
 * <the argument>
 *
 * After __argv_end__ the response appears, and the format is
 * RESP (2 or 3, depending on what the client has configured)
 */

#include "server.h"
#include <ctype.h>

#ifdef LOG_REQ_RES

/* ----- Helpers ----- */

static int reqresShouldLog(client *c) {
    if (!server.req_res_logfile)
        return 0;

    /* Ignore client with streaming non-standard response */
    if (c->flags & (CLIENT_PUBSUB|CLIENT_MONITOR|CLIENT_SLAVE))
        return 0;

    /* We only work on masters (didn't implement reqresAppendResponse to work on shared slave buffers) */
    if (getClientType(c) == CLIENT_TYPE_MASTER)
        return 0;

    return 1;
}

static size_t reqresAppendBuffer(client *c, void *buf, size_t len) {
    if (!c->reqres.buf) {
        c->reqres.capacity = max(len, 1024);
        c->reqres.buf = zmalloc(c->reqres.capacity);
    } else if (c->reqres.capacity - c->reqres.used < len) {
        c->reqres.capacity += len;
        c->reqres.buf = zrealloc(c->reqres.buf, c->reqres.capacity);
    }

    memcpy(c->reqres.buf + c->reqres.used, buf, len);
    c->reqres.used += len;
    return len;
}

/* Functions for requests */

static size_t reqresAppendArg(client *c, char *arg, size_t arg_len) {
    char argv_len_buf[LONG_STR_SIZE];
    size_t argv_len_buf_len = ll2string(argv_len_buf,sizeof(argv_len_buf),(long)arg_len);
    size_t ret = reqresAppendBuffer(c, argv_len_buf, argv_len_buf_len);
    ret += reqresAppendBuffer(c, "\r\n", 2);
    ret += reqresAppendBuffer(c, arg, arg_len);
    ret += reqresAppendBuffer(c, "\r\n", 2);
    return ret;
}

/* ----- API ----- */

size_t reqresAppendRequest(client *c) {
    robj **argv = c->argv;
    int argc = c->argc;

    if (!reqresShouldLog(c))
        return 0;

    serverLog(LL_WARNING, "GUYBE in request");
    
    /* At this point we also remember the current reply offset, so that
     * we won't write the same bytes (in case we couldn't write all pending
     * data to socket in the previous writeToClient) */
    memset(&c->reqres.offset, 0, sizeof(c->reqres.offset));
    if (c->bufpos > 0) {
        /* Here, `sentlen` represents the offset in the static buffer. */
        c->reqres.offset.reply_buf = c->sentlen;
    } else if (listLength(c->reply)) {
        /* Here, `sentlen` represents the offset in the last reply node. */
        c->reqres.offset.last_node.index = listLength(c->reply) - 1;
        c->reqres.offset.last_node.bytes = c->sentlen;
    }

    if (argc == 0)
        return 0;

    /* Ignore commands that have streaming non-standard response */
    sds cmd = argv[0]->ptr;
    if (!strcasecmp(cmd,"sync") ||
        !strcasecmp(cmd,"psync") ||
        !strcasecmp(cmd,"monitor") ||
        !strcasecmp(cmd,"subscribe") ||
        !strcasecmp(cmd,"ssubscribe") ||
        !strcasecmp(cmd,"psubscribe"))
    {
        return 0;
    }

    size_t ret = 0;
    for (int i = 0; i < argc; i++) {
        if (sdsEncodedObject(argv[i])) {
            ret += reqresAppendArg(c, argv[i]->ptr, sdslen(argv[i]->ptr));
        } else if (argv[i]->encoding == OBJ_ENCODING_INT) {
            char buf[LONG_STR_SIZE];
            size_t len = ll2string(buf,sizeof(buf),(long)argv[i]->ptr);
            ret += reqresAppendArg(c, buf, len);
        } else {
            serverPanic("Wrong encoding in reqresAppendRequest()");
        }
    }
    return ret + reqresAppendArg(c, "__argv_end__", 12);
}

size_t reqresAppendResponse(client *c) {
    size_t ret = 0;

    if (!reqresShouldLog(c))
        return 0;

    serverLog(LL_WARNING, "GUYBE in response");

    /* First append the static reply buffer */
    if (c->bufpos > 0) {
        ret += reqresAppendBuffer(c, c->buf + c->reqres.sentlen, c->bufpos - c->reqres.sentlen);
        c->reqres.sentlen += ret;
    }

    /* Now, append reply bytes from the reply list */
    int i = 0;
    listIter iter;
    listNode *curr;
    clientReplyBlock *o;
    listRewind(c->reply, &iter);
    while ((curr = listNext(&iter)) != NULL) {
        /* Skip nodes we had already processed */
        if (i++ < c->reqres.offset.last_node.index)
            continue;
        o = listNodeValue(curr);
        if (o->used == 0)
            continue;

        if (i == c->reqres.offset.last_node.index) {
            /* Write the potentially incomplete node from last writeToClient */
            ret += reqresAppendBuffer(c,
                                      o->buf + c->reqres.offset.last_node.bytes,
                                      o->used - c->reqres.offset.last_node.bytes);
        } else {
            /* New node */
            ret += reqresAppendBuffer(c, o->buf, o->used);
        }
    }

    if (!ret)
        return 0;

    /* Flush both request and response to file */
    FILE *fp = fopen(server.req_res_logfile, "a");
    serverAssert(fp);

    fwrite(c->reqres.buf, c->reqres.used, 1, fp);
    fflush(fp);
    fclose(fp);

    zfree(c->reqres.buf);
    c->reqres.buf = NULL;
    c->reqres.used = c->reqres.capacity = 0;

    return ret;
}

#else /* #ifdef LOG_REQ_RES */

/* Just mimic the API without doing anything */

inline size_t reqresAppendRequest(client *c) {
    UNUSED(c);
    return 0;
}

inline size_t reqresAppendResponse(client *c) {
    UNUSED(c);
    return 0;
}

#endif /* #ifdef LOG_REQ_RES */
