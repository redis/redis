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
 * on by the req-res-logfile config."
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
 * The request is everything up until the __argv_end__ marker.
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


/* Zero out the clientReqResInfo struct inside the client,
 * and free the buffer if needed */
void reqresReset(client *c, int free_buf) {
    if (free_buf && c->reqres.buf)
        zfree(c->reqres.buf);
    memset(&c->reqres, 0, sizeof(c->reqres));
}

/* Save the offset of the reply buffer (or the reply list).
 * Should be called when adding a reply (but it will only save the offset
 * on the very first time it's called, because of c->reqres.offset.saved)
 * The idea is:
 * 1. When a client is executing a command, we save the reply offset.
 * 2. During the execution, the reply offset may grow, as addReply* functions are called.
 * 3. When client is done with the command (commandProcessed), reqresAppendResponse
 *    is called.
 * 4. reqresAppendResponse will append the diff between the current offset and the one from step (1)
 * 5. When client is reset before the next command, we clear c->reqres.offset.saved and start again
 *
 * We cannot reply on c->sentlen to keep track because it depends on the network
 * (reqresAppendResponse will always write the whole buffer, unlike writeToClient)
 *
 * Ideally, we would just have this code inside reqresAppendRequest, which is called
 * from processCommand, but we cannot save the reply offset inside processCommand
 * because of the following pipe-lining scenario:
 * set rd [redis_deferring_client]
 * set buf ""
 * append buf "SET key vale\r\n"
 * append buf "BLPOP mylist 0\r\n"
 * $rd write $buf
 * $rd flush
 *
 * Let's assume we save the reply offset in processCommand
 * When BLPOP is processed the offset is 5 (+OK\r\n from the SET)
 * Then beforeSleep is called, the +OK is written to network, and bufpos is 0
 * When the client is finally unblocked, the cached offset is 5, but bufpos is already
 * 0, so we would miss the first 5 bytes of the reply.
 **/
void reqresSaveClientReplyOffset(client *c) {
    if (!reqresShouldLog(c))
        return;

    if (c->reqres.offset.saved)
        return;

    c->reqres.offset.saved = 1;

    c->reqres.offset.bufpos = c->bufpos;
    if (listLength(c->reply) && listNodeValue(listLast(c->reply))) {
        c->reqres.offset.last_node.index = listLength(c->reply) - 1;
        c->reqres.offset.last_node.used = ((clientReplyBlock *)listNodeValue(listLast(c->reply)))->used;
    } else {
        c->reqres.offset.last_node.index = 0;
        c->reqres.offset.last_node.used = 0;
    }
}

size_t reqresAppendRequest(client *c) {
    robj **argv = c->argv;
    int argc = c->argc;

    serverAssert(argc);

    if (!reqresShouldLog(c))
        return 0;

    /* Ignore commands that have streaming non-standard response */
    sds cmd = argv[0]->ptr;
    if (!strcasecmp(cmd,"debug") || /* because of DEBUG SEGFAULT */
        !strcasecmp(cmd,"sync") ||
        !strcasecmp(cmd,"psync") ||
        !strcasecmp(cmd,"monitor") ||
        !strcasecmp(cmd,"subscribe") ||
        !strcasecmp(cmd,"unsubscribe") ||
        !strcasecmp(cmd,"ssubscribe") ||
        !strcasecmp(cmd,"sunsubscribe") ||
        !strcasecmp(cmd,"psubscribe") ||
        !strcasecmp(cmd,"punsubscribe"))
    {
        return 0;
    }

    c->reqres.argv_logged = 1;

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

    if (!c->reqres.argv_logged) /* Example: UNSUBSCRIBE */
        return 0;

    if (!c->reqres.offset.saved) /* Example: module client blocked on keys + CLIENT KILL */
        return 0;

    /* First append the static reply buffer */
    if (c->bufpos > c->reqres.offset.bufpos) {
        size_t written = reqresAppendBuffer(c, c->buf + c->reqres.offset.bufpos, c->bufpos - c->reqres.offset.bufpos);
        ret += written;
    }

    int curr_index = 0;
    size_t curr_used = 0;
    if (listLength(c->reply)) {
        curr_index = listLength(c->reply) - 1;
        curr_used = ((clientReplyBlock *)listNodeValue(listLast(c->reply)))->used;
    }

    /* Now, append reply bytes from the reply list */
    if (curr_index > c->reqres.offset.last_node.index ||
        curr_used > c->reqres.offset.last_node.used)
    {
        int i = 0;
        listIter iter;
        listNode *curr;
        clientReplyBlock *o;
        listRewind(c->reply, &iter);
        while ((curr = listNext(&iter)) != NULL) {
            size_t written;

            /* Skip nodes we had already processed */
            if (i < c->reqres.offset.last_node.index) {
                i++;
                continue;
            }
            o = listNodeValue(curr);
            if (o->used == 0) {
                i++;
                continue;
            }
            if (i == c->reqres.offset.last_node.index) {
                /* Write the potentially incomplete node, which had data from
                 * before the current command started */
                written = reqresAppendBuffer(c,
                                             o->buf + c->reqres.offset.last_node.used,
                                             o->used - c->reqres.offset.last_node.used);
            } else {
                /* New node */
                written = reqresAppendBuffer(c, o->buf, o->used);
            }
            ret += written;
            i++;
        }
    }
    serverAssert(ret);

    /* Flush both request and response to file */
    FILE *fp = fopen(server.req_res_logfile, "a");
    serverAssert(fp);
    fwrite(c->reqres.buf, c->reqres.used, 1, fp);
    fclose(fp);

    return ret;
}

#else /* #ifdef LOG_REQ_RES */

/* Just mimic the API without doing anything */

void reqresReset(client *c, int free_buf) {
    UNUSED(c);
    UNUSED(free_buf);
}

inline void reqresSaveClientReplyOffset(client *c) {
    UNUSED(c);
}

inline size_t reqresAppendRequest(client *c) {
    UNUSED(c);
    return 0;
}

inline size_t reqresAppendResponse(client *c) {
    UNUSED(c);
    return 0;
}

#endif /* #ifdef LOG_REQ_RES */
