/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
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

#include "fmacros.h"
#include "alloc.h"
#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include "async.h"
#include "net.h"
#include "dict.c"
#include "sds.h"
#include "win32.h"

#include "async_private.h"

#ifdef NDEBUG
#undef assert
#define assert(e) (void)(e)
#endif

/* Forward declarations of hiredis.c functions */
int __redisAppendCommand(redisContext *c, const char *cmd, size_t len);
void __redisSetError(redisContext *c, int type, const char *str);

/* Functions managing dictionary of callbacks for pub/sub. */
static unsigned int callbackHash(const void *key) {
    return dictGenHashFunction((const unsigned char *)key,
                               hi_sdslen((const hisds)key));
}

static void *callbackValDup(void *privdata, const void *src) {
    ((void) privdata);
    redisCallback *dup;

    dup = hi_malloc(sizeof(*dup));
    if (dup == NULL)
        return NULL;

    memcpy(dup,src,sizeof(*dup));
    return dup;
}

static int callbackKeyCompare(void *privdata, const void *key1, const void *key2) {
    int l1, l2;
    ((void) privdata);

    l1 = hi_sdslen((const hisds)key1);
    l2 = hi_sdslen((const hisds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1,key2,l1) == 0;
}

static void callbackKeyDestructor(void *privdata, void *key) {
    ((void) privdata);
    hi_sdsfree((hisds)key);
}

static void callbackValDestructor(void *privdata, void *val) {
    ((void) privdata);
    hi_free(val);
}

static dictType callbackDict = {
    callbackHash,
    NULL,
    callbackValDup,
    callbackKeyCompare,
    callbackKeyDestructor,
    callbackValDestructor
};

static redisAsyncContext *redisAsyncInitialize(redisContext *c) {
    redisAsyncContext *ac;
    dict *channels = NULL, *patterns = NULL;

    channels = dictCreate(&callbackDict,NULL);
    if (channels == NULL)
        goto oom;

    patterns = dictCreate(&callbackDict,NULL);
    if (patterns == NULL)
        goto oom;

    ac = hi_realloc(c,sizeof(redisAsyncContext));
    if (ac == NULL)
        goto oom;

    c = &(ac->c);

    /* The regular connect functions will always set the flag REDIS_CONNECTED.
     * For the async API, we want to wait until the first write event is
     * received up before setting this flag, so reset it here. */
    c->flags &= ~REDIS_CONNECTED;

    ac->err = 0;
    ac->errstr = NULL;
    ac->data = NULL;
    ac->dataCleanup = NULL;

    ac->ev.data = NULL;
    ac->ev.addRead = NULL;
    ac->ev.delRead = NULL;
    ac->ev.addWrite = NULL;
    ac->ev.delWrite = NULL;
    ac->ev.cleanup = NULL;
    ac->ev.scheduleTimer = NULL;

    ac->onConnect = NULL;
    ac->onConnectNC = NULL;
    ac->onDisconnect = NULL;

    ac->replies.head = NULL;
    ac->replies.tail = NULL;
    ac->sub.replies.head = NULL;
    ac->sub.replies.tail = NULL;
    ac->sub.channels = channels;
    ac->sub.patterns = patterns;
    ac->sub.pending_unsubs = 0;

    return ac;
oom:
    if (channels) dictRelease(channels);
    if (patterns) dictRelease(patterns);
    return NULL;
}

/* We want the error field to be accessible directly instead of requiring
 * an indirection to the redisContext struct. */
static void __redisAsyncCopyError(redisAsyncContext *ac) {
    if (!ac)
        return;

    redisContext *c = &(ac->c);
    ac->err = c->err;
    ac->errstr = c->errstr;
}

redisAsyncContext *redisAsyncConnectWithOptions(const redisOptions *options) {
    redisOptions myOptions = *options;
    redisContext *c;
    redisAsyncContext *ac;

    /* Clear any erroneously set sync callback and flag that we don't want to
     * use freeReplyObject by default. */
    myOptions.push_cb = NULL;
    myOptions.options |= REDIS_OPT_NO_PUSH_AUTOFREE;

    myOptions.options |= REDIS_OPT_NONBLOCK;
    c = redisConnectWithOptions(&myOptions);
    if (c == NULL) {
        return NULL;
    }

    ac = redisAsyncInitialize(c);
    if (ac == NULL) {
        redisFree(c);
        return NULL;
    }

    /* Set any configured async push handler */
    redisAsyncSetPushCallback(ac, myOptions.async_push_cb);

    __redisAsyncCopyError(ac);
    return ac;
}

redisAsyncContext *redisAsyncConnect(const char *ip, int port) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    return redisAsyncConnectWithOptions(&options);
}

redisAsyncContext *redisAsyncConnectBind(const char *ip, int port,
                                         const char *source_addr) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    options.endpoint.tcp.source_addr = source_addr;
    return redisAsyncConnectWithOptions(&options);
}

redisAsyncContext *redisAsyncConnectBindWithReuse(const char *ip, int port,
                                                  const char *source_addr) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    options.options |= REDIS_OPT_REUSEADDR;
    options.endpoint.tcp.source_addr = source_addr;
    return redisAsyncConnectWithOptions(&options);
}

redisAsyncContext *redisAsyncConnectUnix(const char *path) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_UNIX(&options, path);
    return redisAsyncConnectWithOptions(&options);
}

static int
redisAsyncSetConnectCallbackImpl(redisAsyncContext *ac, redisConnectCallback *fn,
                                 redisConnectCallbackNC *fn_nc)
{
    /* If either are already set, this is an error */
    if (ac->onConnect || ac->onConnectNC)
        return REDIS_ERR;

    if (fn) {
        ac->onConnect = fn;
    } else if (fn_nc) {
        ac->onConnectNC = fn_nc;
    }

    /* The common way to detect an established connection is to wait for
     * the first write event to be fired. This assumes the related event
     * library functions are already set. */
    _EL_ADD_WRITE(ac);

    return REDIS_OK;
}

int redisAsyncSetConnectCallback(redisAsyncContext *ac, redisConnectCallback *fn) {
    return redisAsyncSetConnectCallbackImpl(ac, fn, NULL);
}

int redisAsyncSetConnectCallbackNC(redisAsyncContext *ac, redisConnectCallbackNC *fn) {
    return redisAsyncSetConnectCallbackImpl(ac, NULL, fn);
}

int redisAsyncSetDisconnectCallback(redisAsyncContext *ac, redisDisconnectCallback *fn) {
    if (ac->onDisconnect == NULL) {
        ac->onDisconnect = fn;
        return REDIS_OK;
    }
    return REDIS_ERR;
}

/* Helper functions to push/shift callbacks */
static int __redisPushCallback(redisCallbackList *list, redisCallback *source) {
    redisCallback *cb;

    /* Copy callback from stack to heap */
    cb = hi_malloc(sizeof(*cb));
    if (cb == NULL)
        return REDIS_ERR_OOM;

    if (source != NULL) {
        memcpy(cb,source,sizeof(*cb));
        cb->next = NULL;
    }

    /* Store callback in list */
    if (list->head == NULL)
        list->head = cb;
    if (list->tail != NULL)
        list->tail->next = cb;
    list->tail = cb;
    return REDIS_OK;
}

static int __redisShiftCallback(redisCallbackList *list, redisCallback *target) {
    redisCallback *cb = list->head;
    if (cb != NULL) {
        list->head = cb->next;
        if (cb == list->tail)
            list->tail = NULL;

        /* Copy callback from heap to stack */
        if (target != NULL)
            memcpy(target,cb,sizeof(*cb));
        hi_free(cb);
        return REDIS_OK;
    }
    return REDIS_ERR;
}

static void __redisRunCallback(redisAsyncContext *ac, redisCallback *cb, redisReply *reply) {
    redisContext *c = &(ac->c);
    if (cb->fn != NULL) {
        c->flags |= REDIS_IN_CALLBACK;
        cb->fn(ac,reply,cb->privdata);
        c->flags &= ~REDIS_IN_CALLBACK;
    }
}

static void __redisRunPushCallback(redisAsyncContext *ac, redisReply *reply) {
    if (ac->push_cb != NULL) {
        ac->c.flags |= REDIS_IN_CALLBACK;
        ac->push_cb(ac, reply);
        ac->c.flags &= ~REDIS_IN_CALLBACK;
    }
}

static void __redisRunConnectCallback(redisAsyncContext *ac, int status)
{
    if (ac->onConnect == NULL && ac->onConnectNC == NULL)
        return;

    if (!(ac->c.flags & REDIS_IN_CALLBACK)) {
        ac->c.flags |= REDIS_IN_CALLBACK;
        if (ac->onConnect) {
            ac->onConnect(ac, status);
        } else {
            ac->onConnectNC(ac, status);
        }
        ac->c.flags &= ~REDIS_IN_CALLBACK;
    } else {
        /* already in callback */
        if (ac->onConnect) {
            ac->onConnect(ac, status);
        } else {
            ac->onConnectNC(ac, status);
        }
    }
}

static void __redisRunDisconnectCallback(redisAsyncContext *ac, int status)
{
    if (ac->onDisconnect) {
        if (!(ac->c.flags & REDIS_IN_CALLBACK)) {
            ac->c.flags |= REDIS_IN_CALLBACK;
            ac->onDisconnect(ac, status);
            ac->c.flags &= ~REDIS_IN_CALLBACK;
        } else {
            /* already in callback */
            ac->onDisconnect(ac, status);
        }
    }
}

/* Helper function to free the context. */
static void __redisAsyncFree(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisCallback cb;
    dictIterator it;
    dictEntry *de;

    /* Execute pending callbacks with NULL reply. */
    while (__redisShiftCallback(&ac->replies,&cb) == REDIS_OK)
        __redisRunCallback(ac,&cb,NULL);
    while (__redisShiftCallback(&ac->sub.replies,&cb) == REDIS_OK)
        __redisRunCallback(ac,&cb,NULL);

    /* Run subscription callbacks with NULL reply */
    if (ac->sub.channels) {
        dictInitIterator(&it,ac->sub.channels);
        while ((de = dictNext(&it)) != NULL)
            __redisRunCallback(ac,dictGetEntryVal(de),NULL);

        dictRelease(ac->sub.channels);
    }

    if (ac->sub.patterns) {
        dictInitIterator(&it,ac->sub.patterns);
        while ((de = dictNext(&it)) != NULL)
            __redisRunCallback(ac,dictGetEntryVal(de),NULL);

        dictRelease(ac->sub.patterns);
    }

    /* Signal event lib to clean up */
    _EL_CLEANUP(ac);

    /* Execute disconnect callback. When redisAsyncFree() initiated destroying
     * this context, the status will always be REDIS_OK. */
    if (c->flags & REDIS_CONNECTED) {
        int status = ac->err == 0 ? REDIS_OK : REDIS_ERR;
        if (c->flags & REDIS_FREEING)
            status = REDIS_OK;
        __redisRunDisconnectCallback(ac, status);
    }

    if (ac->dataCleanup) {
        ac->dataCleanup(ac->data);
    }

    /* Cleanup self */
    redisFree(c);
}

/* Free the async context. When this function is called from a callback,
 * control needs to be returned to redisProcessCallbacks() before actual
 * free'ing. To do so, a flag is set on the context which is picked up by
 * redisProcessCallbacks(). Otherwise, the context is immediately free'd. */
void redisAsyncFree(redisAsyncContext *ac) {
    if (ac == NULL)
        return;

    redisContext *c = &(ac->c);

    c->flags |= REDIS_FREEING;
    if (!(c->flags & REDIS_IN_CALLBACK))
        __redisAsyncFree(ac);
}

/* Helper function to make the disconnect happen and clean up. */
void __redisAsyncDisconnect(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);

    /* Make sure error is accessible if there is any */
    __redisAsyncCopyError(ac);

    if (ac->err == 0) {
        /* For clean disconnects, there should be no pending callbacks. */
        int ret = __redisShiftCallback(&ac->replies,NULL);
        assert(ret == REDIS_ERR);
    } else {
        /* Disconnection is caused by an error, make sure that pending
         * callbacks cannot call new commands. */
        c->flags |= REDIS_DISCONNECTING;
    }

    /* cleanup event library on disconnect.
     * this is safe to call multiple times */
    _EL_CLEANUP(ac);

    /* For non-clean disconnects, __redisAsyncFree() will execute pending
     * callbacks with a NULL-reply. */
    if (!(c->flags & REDIS_NO_AUTO_FREE)) {
      __redisAsyncFree(ac);
    }
}

/* Tries to do a clean disconnect from Redis, meaning it stops new commands
 * from being issued, but tries to flush the output buffer and execute
 * callbacks for all remaining replies. When this function is called from a
 * callback, there might be more replies and we can safely defer disconnecting
 * to redisProcessCallbacks(). Otherwise, we can only disconnect immediately
 * when there are no pending callbacks. */
void redisAsyncDisconnect(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    c->flags |= REDIS_DISCONNECTING;

    /** unset the auto-free flag here, because disconnect undoes this */
    c->flags &= ~REDIS_NO_AUTO_FREE;
    if (!(c->flags & REDIS_IN_CALLBACK) && ac->replies.head == NULL)
        __redisAsyncDisconnect(ac);
}

static int __redisGetSubscribeCallback(redisAsyncContext *ac, redisReply *reply, redisCallback *dstcb) {
    redisContext *c = &(ac->c);
    dict *callbacks;
    redisCallback *cb = NULL;
    dictEntry *de;
    int pvariant;
    char *stype;
    hisds sname = NULL;

    /* Match reply with the expected format of a pushed message.
     * The type and number of elements (3 to 4) are specified at:
     * https://redis.io/topics/pubsub#format-of-pushed-messages */
    if ((reply->type == REDIS_REPLY_ARRAY && !(c->flags & REDIS_SUPPORTS_PUSH) && reply->elements >= 3) ||
        reply->type == REDIS_REPLY_PUSH) {
        assert(reply->element[0]->type == REDIS_REPLY_STRING);
        stype = reply->element[0]->str;
        pvariant = (tolower(stype[0]) == 'p') ? 1 : 0;

        if (pvariant)
            callbacks = ac->sub.patterns;
        else
            callbacks = ac->sub.channels;

        /* Locate the right callback */
        if (reply->element[1]->type == REDIS_REPLY_STRING) {
            sname = hi_sdsnewlen(reply->element[1]->str,reply->element[1]->len);
            if (sname == NULL) goto oom;

            if ((de = dictFind(callbacks,sname)) != NULL) {
                cb = dictGetEntryVal(de);
                memcpy(dstcb,cb,sizeof(*dstcb));
            }
        }

        /* If this is an subscribe reply decrease pending counter. */
        if (strcasecmp(stype+pvariant,"subscribe") == 0) {
            assert(cb != NULL);
            cb->pending_subs -= 1;

        } else if (strcasecmp(stype+pvariant,"unsubscribe") == 0) {
            if (cb == NULL)
                ac->sub.pending_unsubs -= 1;
            else if (cb->pending_subs == 0)
                dictDelete(callbacks,sname);

            /* If this was the last unsubscribe message, revert to
             * non-subscribe mode. */
            assert(reply->element[2]->type == REDIS_REPLY_INTEGER);

            /* Unset subscribed flag only when no pipelined pending subscribe
             * or pending unsubscribe replies. */
            if (reply->element[2]->integer == 0
                && dictSize(ac->sub.channels) == 0
                && dictSize(ac->sub.patterns) == 0
                && ac->sub.pending_unsubs == 0) {
                c->flags &= ~REDIS_SUBSCRIBED;

                /* Move ongoing regular command callbacks. */
                redisCallback cb;
                while (__redisShiftCallback(&ac->sub.replies,&cb) == REDIS_OK) {
                    __redisPushCallback(&ac->replies,&cb);
                }
            }
        }
        hi_sdsfree(sname);
    } else {
        /* Shift callback for pending command in subscribed context. */
        __redisShiftCallback(&ac->sub.replies,dstcb);
    }
    return REDIS_OK;
oom:
    __redisSetError(&(ac->c), REDIS_ERR_OOM, "Out of memory");
    __redisAsyncCopyError(ac);
    return REDIS_ERR;
}

#define redisIsSpontaneousPushReply(r) \
    (redisIsPushReply(r) && !redisIsSubscribeReply(r))

static int redisIsSubscribeReply(redisReply *reply) {
    char *str;
    size_t len, off;

    /* We will always have at least one string with the subscribe/message type */
    if (reply->elements < 1 || reply->element[0]->type != REDIS_REPLY_STRING ||
        reply->element[0]->len < sizeof("message") - 1)
    {
        return 0;
    }

    /* Get the string/len moving past 'p' if needed */
    off = tolower(reply->element[0]->str[0]) == 'p';
    str = reply->element[0]->str + off;
    len = reply->element[0]->len - off;

    return !strncasecmp(str, "subscribe", len) ||
           !strncasecmp(str, "message", len) ||
           !strncasecmp(str, "unsubscribe", len);
}

void redisProcessCallbacks(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    void *reply = NULL;
    int status;

    while((status = redisGetReply(c,&reply)) == REDIS_OK) {
        if (reply == NULL) {
            /* When the connection is being disconnected and there are
             * no more replies, this is the cue to really disconnect. */
            if (c->flags & REDIS_DISCONNECTING && hi_sdslen(c->obuf) == 0
                && ac->replies.head == NULL) {
                __redisAsyncDisconnect(ac);
                return;
            }
            /* When the connection is not being disconnected, simply stop
             * trying to get replies and wait for the next loop tick. */
            break;
        }

        /* Keep track of push message support for subscribe handling */
        if (redisIsPushReply(reply)) c->flags |= REDIS_SUPPORTS_PUSH;

        /* Send any non-subscribe related PUSH messages to our PUSH handler
         * while allowing subscribe related PUSH messages to pass through.
         * This allows existing code to be backward compatible and work in
         * either RESP2 or RESP3 mode. */
        if (redisIsSpontaneousPushReply(reply)) {
            __redisRunPushCallback(ac, reply);
            c->reader->fn->freeObject(reply);
            continue;
        }

        /* Even if the context is subscribed, pending regular
         * callbacks will get a reply before pub/sub messages arrive. */
        redisCallback cb = {NULL, NULL, 0, 0, NULL};
        if (__redisShiftCallback(&ac->replies,&cb) != REDIS_OK) {
            /*
             * A spontaneous reply in a not-subscribed context can be the error
             * reply that is sent when a new connection exceeds the maximum
             * number of allowed connections on the server side.
             *
             * This is seen as an error instead of a regular reply because the
             * server closes the connection after sending it.
             *
             * To prevent the error from being overwritten by an EOF error the
             * connection is closed here. See issue #43.
             *
             * Another possibility is that the server is loading its dataset.
             * In this case we also want to close the connection, and have the
             * user wait until the server is ready to take our request.
             */
            if (((redisReply*)reply)->type == REDIS_REPLY_ERROR) {
                c->err = REDIS_ERR_OTHER;
                snprintf(c->errstr,sizeof(c->errstr),"%s",((redisReply*)reply)->str);
                c->reader->fn->freeObject(reply);
                __redisAsyncDisconnect(ac);
                return;
            }
            /* No more regular callbacks and no errors, the context *must* be subscribed. */
            assert(c->flags & REDIS_SUBSCRIBED);
            if (c->flags & REDIS_SUBSCRIBED)
                __redisGetSubscribeCallback(ac,reply,&cb);
        }

        if (cb.fn != NULL) {
            __redisRunCallback(ac,&cb,reply);
            if (!(c->flags & REDIS_NO_AUTO_FREE_REPLIES)){
                c->reader->fn->freeObject(reply);
            }

            /* Proceed with free'ing when redisAsyncFree() was called. */
            if (c->flags & REDIS_FREEING) {
                __redisAsyncFree(ac);
                return;
            }
        } else {
            /* No callback for this reply. This can either be a NULL callback,
             * or there were no callbacks to begin with. Either way, don't
             * abort with an error, but simply ignore it because the client
             * doesn't know what the server will spit out over the wire. */
            c->reader->fn->freeObject(reply);
        }

        /* If in monitor mode, repush the callback */
        if (c->flags & REDIS_MONITORING) {
            __redisPushCallback(&ac->replies,&cb);
        }
    }

    /* Disconnect when there was an error reading the reply */
    if (status != REDIS_OK)
        __redisAsyncDisconnect(ac);
}

static void __redisAsyncHandleConnectFailure(redisAsyncContext *ac) {
    __redisRunConnectCallback(ac, REDIS_ERR);
    __redisAsyncDisconnect(ac);
}

/* Internal helper function to detect socket status the first time a read or
 * write event fires. When connecting was not successful, the connect callback
 * is called with a REDIS_ERR status and the context is free'd. */
static int __redisAsyncHandleConnect(redisAsyncContext *ac) {
    int completed = 0;
    redisContext *c = &(ac->c);

    if (redisCheckConnectDone(c, &completed) == REDIS_ERR) {
        /* Error! */
        if (redisCheckSocketError(c) == REDIS_ERR)
            __redisAsyncCopyError(ac);
        __redisAsyncHandleConnectFailure(ac);
        return REDIS_ERR;
    } else if (completed == 1) {
        /* connected! */
        if (c->connection_type == REDIS_CONN_TCP &&
            redisSetTcpNoDelay(c) == REDIS_ERR) {
            __redisAsyncHandleConnectFailure(ac);
            return REDIS_ERR;
        }

        /* flag us as fully connect, but allow the callback
         * to disconnect.  For that reason, permit the function
         * to delete the context here after callback return.
         */
        c->flags |= REDIS_CONNECTED;
        __redisRunConnectCallback(ac, REDIS_OK);
        if ((ac->c.flags & REDIS_DISCONNECTING)) {
            redisAsyncDisconnect(ac);
            return REDIS_ERR;
        } else if ((ac->c.flags & REDIS_FREEING)) {
            redisAsyncFree(ac);
            return REDIS_ERR;
        }
        return REDIS_OK;
    } else {
        return REDIS_OK;
    }
}

void redisAsyncRead(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);

    if (redisBufferRead(c) == REDIS_ERR) {
        __redisAsyncDisconnect(ac);
    } else {
        /* Always re-schedule reads */
        _EL_ADD_READ(ac);
        redisProcessCallbacks(ac);
    }
}

/* This function should be called when the socket is readable.
 * It processes all replies that can be read and executes their callbacks.
 */
void redisAsyncHandleRead(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    /* must not be called from a callback */
    assert(!(c->flags & REDIS_IN_CALLBACK));

    if (!(c->flags & REDIS_CONNECTED)) {
        /* Abort connect was not successful. */
        if (__redisAsyncHandleConnect(ac) != REDIS_OK)
            return;
        /* Try again later when the context is still not connected. */
        if (!(c->flags & REDIS_CONNECTED))
            return;
    }

    c->funcs->async_read(ac);
}

void redisAsyncWrite(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    int done = 0;

    if (redisBufferWrite(c,&done) == REDIS_ERR) {
        __redisAsyncDisconnect(ac);
    } else {
        /* Continue writing when not done, stop writing otherwise */
        if (!done)
            _EL_ADD_WRITE(ac);
        else
            _EL_DEL_WRITE(ac);

        /* Always schedule reads after writes */
        _EL_ADD_READ(ac);
    }
}

void redisAsyncHandleWrite(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    /* must not be called from a callback */
    assert(!(c->flags & REDIS_IN_CALLBACK));

    if (!(c->flags & REDIS_CONNECTED)) {
        /* Abort connect was not successful. */
        if (__redisAsyncHandleConnect(ac) != REDIS_OK)
            return;
        /* Try again later when the context is still not connected. */
        if (!(c->flags & REDIS_CONNECTED))
            return;
    }

    c->funcs->async_write(ac);
}

void redisAsyncHandleTimeout(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisCallback cb;
    /* must not be called from a callback */
    assert(!(c->flags & REDIS_IN_CALLBACK));

    if ((c->flags & REDIS_CONNECTED)) {
        if (ac->replies.head == NULL && ac->sub.replies.head == NULL) {
            /* Nothing to do - just an idle timeout */
            return;
        }

        if (!ac->c.command_timeout ||
            (!ac->c.command_timeout->tv_sec && !ac->c.command_timeout->tv_usec)) {
            /* A belated connect timeout arriving, ignore */
            return;
        }
    }

    if (!c->err) {
        __redisSetError(c, REDIS_ERR_TIMEOUT, "Timeout");
        __redisAsyncCopyError(ac);
    }

    if (!(c->flags & REDIS_CONNECTED)) {
        __redisRunConnectCallback(ac, REDIS_ERR);
    }

    while (__redisShiftCallback(&ac->replies, &cb) == REDIS_OK) {
        __redisRunCallback(ac, &cb, NULL);
    }

    /**
     * TODO: Don't automatically sever the connection,
     * rather, allow to ignore <x> responses before the queue is clear
     */
    __redisAsyncDisconnect(ac);
}

/* Sets a pointer to the first argument and its length starting at p. Returns
 * the number of bytes to skip to get to the following argument. */
static const char *nextArgument(const char *start, const char **str, size_t *len) {
    const char *p = start;
    if (p[0] != '$') {
        p = strchr(p,'$');
        if (p == NULL) return NULL;
    }

    *len = (int)strtol(p+1,NULL,10);
    p = strchr(p,'\r');
    assert(p);
    *str = p+2;
    return p+2+(*len)+2;
}

/* Helper function for the redisAsyncCommand* family of functions. Writes a
 * formatted command to the output buffer and registers the provided callback
 * function with the context. */
static int __redisAsyncCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, const char *cmd, size_t len) {
    redisContext *c = &(ac->c);
    redisCallback cb;
    struct dict *cbdict;
    dictIterator it;
    dictEntry *de;
    redisCallback *existcb;
    int pvariant, hasnext;
    const char *cstr, *astr;
    size_t clen, alen;
    const char *p;
    hisds sname;
    int ret;

    /* Don't accept new commands when the connection is about to be closed. */
    if (c->flags & (REDIS_DISCONNECTING | REDIS_FREEING)) return REDIS_ERR;

    /* Setup callback */
    cb.fn = fn;
    cb.privdata = privdata;
    cb.pending_subs = 1;
    cb.unsubscribe_sent = 0;

    /* Find out which command will be appended. */
    p = nextArgument(cmd,&cstr,&clen);
    assert(p != NULL);
    hasnext = (p[0] == '$');
    pvariant = (tolower(cstr[0]) == 'p') ? 1 : 0;
    cstr += pvariant;
    clen -= pvariant;

    if (hasnext && strncasecmp(cstr,"subscribe\r\n",11) == 0) {
        c->flags |= REDIS_SUBSCRIBED;

        /* Add every channel/pattern to the list of subscription callbacks. */
        while ((p = nextArgument(p,&astr,&alen)) != NULL) {
            sname = hi_sdsnewlen(astr,alen);
            if (sname == NULL)
                goto oom;

            if (pvariant)
                cbdict = ac->sub.patterns;
            else
                cbdict = ac->sub.channels;

            de = dictFind(cbdict,sname);

            if (de != NULL) {
                existcb = dictGetEntryVal(de);
                cb.pending_subs = existcb->pending_subs + 1;
            }

            ret = dictReplace(cbdict,sname,&cb);

            if (ret == 0) hi_sdsfree(sname);
        }
    } else if (strncasecmp(cstr,"unsubscribe\r\n",13) == 0) {
        /* It is only useful to call (P)UNSUBSCRIBE when the context is
         * subscribed to one or more channels or patterns. */
        if (!(c->flags & REDIS_SUBSCRIBED)) return REDIS_ERR;

        if (pvariant)
            cbdict = ac->sub.patterns;
        else
            cbdict = ac->sub.channels;

        if (hasnext) {
            /* Send an unsubscribe with specific channels/patterns.
             * Bookkeeping the number of expected replies */
            while ((p = nextArgument(p,&astr,&alen)) != NULL) {
                sname = hi_sdsnewlen(astr,alen);
                if (sname == NULL)
                    goto oom;

                de = dictFind(cbdict,sname);
                if (de != NULL) {
                    existcb = dictGetEntryVal(de);
                    if (existcb->unsubscribe_sent == 0)
                        existcb->unsubscribe_sent = 1;
                    else
                        /* Already sent, reply to be ignored */
                        ac->sub.pending_unsubs += 1;
                } else {
                    /* Not subscribed to, reply to be ignored */
                    ac->sub.pending_unsubs += 1;
                }
                hi_sdsfree(sname);
            }
        } else {
            /* Send an unsubscribe without specific channels/patterns.
             * Bookkeeping the number of expected replies */
            int no_subs = 1;
            dictInitIterator(&it,cbdict);
            while ((de = dictNext(&it)) != NULL) {
                existcb = dictGetEntryVal(de);
                if (existcb->unsubscribe_sent == 0) {
                    existcb->unsubscribe_sent = 1;
                    no_subs = 0;
                }
            }
            /* Unsubscribing to all channels/patterns, where none is
             * subscribed to, results in a single reply to be ignored. */
            if (no_subs == 1)
                ac->sub.pending_unsubs += 1;
        }

        /* (P)UNSUBSCRIBE does not have its own response: every channel or
         * pattern that is unsubscribed will receive a message. This means we
         * should not append a callback function for this command. */
    } else if (strncasecmp(cstr,"monitor\r\n",9) == 0) {
        /* Set monitor flag and push callback */
        c->flags |= REDIS_MONITORING;
        if (__redisPushCallback(&ac->replies,&cb) != REDIS_OK)
            goto oom;
    } else {
        if (c->flags & REDIS_SUBSCRIBED) {
            if (__redisPushCallback(&ac->sub.replies,&cb) != REDIS_OK)
                goto oom;
        } else {
            if (__redisPushCallback(&ac->replies,&cb) != REDIS_OK)
                goto oom;
        }
    }

    __redisAppendCommand(c,cmd,len);

    /* Always schedule a write when the write buffer is non-empty */
    _EL_ADD_WRITE(ac);

    return REDIS_OK;
oom:
    __redisSetError(&(ac->c), REDIS_ERR_OOM, "Out of memory");
    __redisAsyncCopyError(ac);
    return REDIS_ERR;
}

int redisvAsyncCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, const char *format, va_list ap) {
    char *cmd;
    int len;
    int status;
    len = redisvFormatCommand(&cmd,format,ap);

    /* We don't want to pass -1 or -2 to future functions as a length. */
    if (len < 0)
        return REDIS_ERR;

    status = __redisAsyncCommand(ac,fn,privdata,cmd,len);
    hi_free(cmd);
    return status;
}

int redisAsyncCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, const char *format, ...) {
    va_list ap;
    int status;
    va_start(ap,format);
    status = redisvAsyncCommand(ac,fn,privdata,format,ap);
    va_end(ap);
    return status;
}

int redisAsyncCommandArgv(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, int argc, const char **argv, const size_t *argvlen) {
    hisds cmd;
    long long len;
    int status;
    len = redisFormatSdsCommandArgv(&cmd,argc,argv,argvlen);
    if (len < 0)
        return REDIS_ERR;
    status = __redisAsyncCommand(ac,fn,privdata,cmd,len);
    hi_sdsfree(cmd);
    return status;
}

int redisAsyncFormattedCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, const char *cmd, size_t len) {
    int status = __redisAsyncCommand(ac,fn,privdata,cmd,len);
    return status;
}

redisAsyncPushFn *redisAsyncSetPushCallback(redisAsyncContext *ac, redisAsyncPushFn *fn) {
    redisAsyncPushFn *old = ac->push_cb;
    ac->push_cb = fn;
    return old;
}

int redisAsyncSetTimeout(redisAsyncContext *ac, struct timeval tv) {
    if (!ac->c.command_timeout) {
        ac->c.command_timeout = hi_calloc(1, sizeof(tv));
        if (ac->c.command_timeout == NULL) {
            __redisSetError(&ac->c, REDIS_ERR_OOM, "Out of memory");
            __redisAsyncCopyError(ac);
            return REDIS_ERR;
        }
    }

    if (tv.tv_sec != ac->c.command_timeout->tv_sec ||
        tv.tv_usec != ac->c.command_timeout->tv_usec)
    {
        *ac->c.command_timeout = tv;
    }

    return REDIS_OK;
}
