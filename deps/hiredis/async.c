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
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include "async.h"
#include "net.h"
#include "dict.c"
#include "sds.h"
#ifdef _WIN32
  #include "../../src/win32fixes.h"
#endif

#define _EL_ADD_READ(ctx) do { \
        if ((ctx)->ev.addRead) (ctx)->ev.addRead((ctx)->ev.data); \
    } while(0)
#define _EL_DEL_READ(ctx) do { \
        if ((ctx)->ev.delRead) (ctx)->ev.delRead((ctx)->ev.data); \
    } while(0)
#define _EL_ADD_WRITE(ctx) do { \
        if ((ctx)->ev.addWrite) (ctx)->ev.addWrite((ctx)->ev.data); \
    } while(0)
#define _EL_DEL_WRITE(ctx) do { \
        if ((ctx)->ev.delWrite) (ctx)->ev.delWrite((ctx)->ev.data); \
    } while(0)
#define _EL_CLEANUP(ctx) do { \
        if ((ctx)->ev.cleanup) (ctx)->ev.cleanup((ctx)->ev.data); \
    } while(0);

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

/* Forward declaration of function in hiredis.c */
void __redisAppendCommand(redisContext *c, char *cmd, size_t len);

/* Functions managing dictionary of callbacks for pub/sub. */
static unsigned int callbackHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key,(int)sdslen((char*)key));
}

static void *callbackValDup(void *privdata, const void *src) {
    redisCallback *dup = malloc(sizeof(*dup));
    ((void) privdata);
    memcpy(dup,src,sizeof(*dup));
    return dup;
}

static int callbackKeyCompare(void *privdata, const void *key1, const void *key2) {
    size_t l1, l2;
    ((void) privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1,key2,l1) == 0;
}

static void callbackKeyDestructor(void *privdata, void *key) {
    ((void) privdata);
    sdsfree((sds)key);
}

static void callbackValDestructor(void *privdata, void *val) {
    ((void) privdata);
    free(val);
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
    redisAsyncContext *ac = realloc(c,sizeof(redisAsyncContext));
    c = &(ac->c);

    /* The regular connect functions will always set the flag REDIS_CONNECTED.
     * For the async API, we want to wait until the first write event is
     * received up before setting this flag, so reset it here. */
    c->flags &= ~REDIS_CONNECTED;

    ac->err = 0;
    ac->errstr = NULL;
    ac->data = NULL;

    ac->ev.data = NULL;
    ac->ev.addRead = NULL;
    ac->ev.delRead = NULL;
    ac->ev.addWrite = NULL;
    ac->ev.delWrite = NULL;
    ac->ev.cleanup = NULL;

    ac->onConnect = NULL;
    ac->onDisconnect = NULL;

    ac->replies.head = NULL;
    ac->replies.tail = NULL;
    ac->sub.invalid.head = NULL;
    ac->sub.invalid.tail = NULL;
    ac->sub.channels = dictCreate(&callbackDict,NULL);
    ac->sub.patterns = dictCreate(&callbackDict,NULL);
    return ac;
}

/* We want the error field to be accessible directly instead of requiring
 * an indirection to the redisContext struct. */
static void __redisAsyncCopyError(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    ac->err = c->err;
    ac->errstr = c->errstr;
}

#ifdef _WIN32
redisAsyncContext *redisAsyncConnect(const char *ip, int port) {
    struct sockaddr_in sa;
    redisContext *c = redisPreConnectNonBlock(ip, port, &sa);
    redisAsyncContext *ac = redisAsyncInitialize(c);
    c = &(ac->c);
    if (aeWinSocketConnect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        c->err = errno;
        strerror_r(errno,c->errstr,sizeof(c->errstr));
    }
    __redisAsyncCopyError(ac);
    return ac;
}
#else
redisAsyncContext *redisAsyncConnect(const char *ip, int port) {
    redisContext *c = redisConnectNonBlock(ip,port);
    redisAsyncContext *ac = redisAsyncInitialize(c);
    __redisAsyncCopyError(ac);
    return ac;
}
#endif

redisAsyncContext *redisAsyncConnectUnix(const char *path) {
    redisContext *c = redisConnectUnixNonBlock(path);
    redisAsyncContext *ac = redisAsyncInitialize(c);
    __redisAsyncCopyError(ac);
    return ac;
}

int redisAsyncSetConnectCallback(redisAsyncContext *ac, redisConnectCallback *fn) {
    if (ac->onConnect == NULL) {
        ac->onConnect = fn;

        /* The common way to detect an established connection is to wait for
         * the first write event to be fired. This assumes the related event
         * library functions are already set. */
        _EL_ADD_WRITE(ac);
        return REDIS_OK;
    }
    return REDIS_ERR;
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
    cb = malloc(sizeof(*cb));
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
        free(cb);
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

/* Helper function to free the context. */
static void __redisAsyncFree(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisCallback cb;
    dictIterator *it;
    dictEntry *de;

    /* Execute pending callbacks with NULL reply. */
    while (__redisShiftCallback(&ac->replies,&cb) == REDIS_OK)
        __redisRunCallback(ac,&cb,NULL);

    /* Execute callbacks for invalid commands */
    while (__redisShiftCallback(&ac->sub.invalid,&cb) == REDIS_OK)
        __redisRunCallback(ac,&cb,NULL);

    /* Run subscription callbacks callbacks with NULL reply */
    it = dictGetIterator(ac->sub.channels);
    while ((de = dictNext(it)) != NULL)
        __redisRunCallback(ac,dictGetEntryVal(de),NULL);
    dictReleaseIterator(it);
    dictRelease(ac->sub.channels);

    it = dictGetIterator(ac->sub.patterns);
    while ((de = dictNext(it)) != NULL)
        __redisRunCallback(ac,dictGetEntryVal(de),NULL);
    dictReleaseIterator(it);
    dictRelease(ac->sub.patterns);

    /* Signal event lib to clean up */
    _EL_CLEANUP(ac);

    /* Execute disconnect callback. When redisAsyncFree() initiated destroying
     * this context, the status will always be REDIS_OK. */
    if (ac->onDisconnect && (c->flags & REDIS_CONNECTED)) {
        if (c->flags & REDIS_FREEING) {
            ac->onDisconnect(ac,REDIS_OK);
        } else {
            ac->onDisconnect(ac,(ac->err == 0) ? REDIS_OK : REDIS_ERR);
        }
    }

    /* Cleanup self */
    redisFree(c);
}

/* Free the async context. When this function is called from a callback,
 * control needs to be returned to redisProcessCallbacks() before actual
 * free'ing. To do so, a flag is set on the context which is picked up by
 * redisProcessCallbacks(). Otherwise, the context is immediately free'd. */
void redisAsyncFree(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    c->flags |= REDIS_FREEING;
    if (!(c->flags & REDIS_IN_CALLBACK))
        __redisAsyncFree(ac);
}

/* Helper function to make the disconnect happen and clean up. */
static void __redisAsyncDisconnect(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);

    /* Make sure error is accessible if there is any */
    __redisAsyncCopyError(ac);

    if (ac->err == 0) {
        /* For clean disconnects, there should be no pending callbacks. */
        assert(__redisShiftCallback(&ac->replies,NULL) == REDIS_ERR);
    } else {
        /* Disconnection is caused by an error, make sure that pending
         * callbacks cannot call new commands. */
        c->flags |= REDIS_DISCONNECTING;
    }

    /* For non-clean disconnects, __redisAsyncFree() will execute pending
     * callbacks with a NULL-reply. */
    __redisAsyncFree(ac);
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
    if (!(c->flags & REDIS_IN_CALLBACK) && ac->replies.head == NULL)
        __redisAsyncDisconnect(ac);
}

static int __redisGetSubscribeCallback(redisAsyncContext *ac, redisReply *reply, redisCallback *dstcb) {
    redisContext *c = &(ac->c);
    dict *callbacks;
    dictEntry *de;
    int pvariant;
    char *stype;
    sds sname;

    /* Custom reply functions are not supported for pub/sub. This will fail
     * very hard when they are used... */
    if (reply->type == REDIS_REPLY_ARRAY) {
        assert(reply->elements >= 2);
        assert(reply->element[0]->type == REDIS_REPLY_STRING);
        stype = reply->element[0]->str;
        pvariant = (tolower(stype[0]) == 'p') ? 1 : 0;

        if (pvariant)
            callbacks = ac->sub.patterns;
        else
            callbacks = ac->sub.channels;

        /* Locate the right callback */
        assert(reply->element[1]->type == REDIS_REPLY_STRING);
        sname = sdsnewlen(reply->element[1]->str,reply->element[1]->len);
        de = dictFind(callbacks,sname);
        if (de != NULL) {
            memcpy(dstcb,dictGetEntryVal(de),sizeof(*dstcb));

            /* If this is an unsubscribe message, remove it. */
            if (strcasecmp(stype+pvariant,"unsubscribe") == 0) {
                dictDelete(callbacks,sname);

                /* If this was the last unsubscribe message, revert to
                 * non-subscribe mode. */
                assert(reply->element[2]->type == REDIS_REPLY_INTEGER);
                if (reply->element[2]->integer == 0)
                    c->flags &= ~REDIS_SUBSCRIBED;
            }
        }
        sdsfree(sname);
    } else {
        /* Shift callback for invalid commands. */
        __redisShiftCallback(&ac->sub.invalid,dstcb);
    }
    return REDIS_OK;
}

void redisProcessCallbacks(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisCallback cb;
    void *reply = NULL;
    int status;

    while((status = redisGetReply(c,&reply)) == REDIS_OK) {
        if (reply == NULL) {
            /* When the connection is being disconnected and there are
             * no more replies, this is the cue to really disconnect. */
            if (c->flags & REDIS_DISCONNECTING && sdslen(c->obuf) == 0) {
                __redisAsyncDisconnect(ac);
                return;
            }
            
            /* If monitor mode, repush callback */
            if(c->flags & REDIS_MONITORING) {
                __redisPushCallback(&ac->replies,&cb);
            }

            /* When the connection is not being disconnected, simply stop
             * trying to get replies and wait for the next loop tick. */
            break;
        }

        /* Even if the context is subscribed, pending regular callbacks will
         * get a reply before pub/sub messages arrive. */
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
                __redisAsyncDisconnect(ac);
                return;
            }
            /* No more regular callbacks and no errors, the context *must* be subscribed or monitoring. */
            assert((c->flags & REDIS_SUBSCRIBED || c->flags & REDIS_MONITORING));
            if(c->flags & REDIS_SUBSCRIBED)
                __redisGetSubscribeCallback(ac,reply,&cb);
        }

        if (cb.fn != NULL) {
            __redisRunCallback(ac,&cb,reply);
            c->reader->fn->freeObject(reply);

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
    }

    /* Disconnect when there was an error reading the reply */
    if (status != REDIS_OK)
        __redisAsyncDisconnect(ac);
}

/* Internal helper function to detect socket status the first time a read or
 * write event fires. When connecting was not succesful, the connect callback
 * is called with a REDIS_ERR status and the context is free'd. */
static int __redisAsyncHandleConnect(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);

    if (redisCheckSocketError(c,(int)c->fd) == REDIS_ERR) {
        /* Try again later when connect(2) is still in progress. */
        if (errno == EINPROGRESS)
            return REDIS_OK;

        if (ac->onConnect) ac->onConnect(ac,REDIS_ERR);
        __redisAsyncDisconnect(ac);
        return REDIS_ERR;
    }

    /* Mark context as connected. */
    c->flags |= REDIS_CONNECTED;
    if (ac->onConnect) ac->onConnect(ac,REDIS_OK);
    return REDIS_OK;
}

/* This function should be called when the socket is readable.
 * It processes all replies that can be read and executes their callbacks.
 */
void redisAsyncHandleRead(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);

    if (!(c->flags & REDIS_CONNECTED)) {
        /* Abort connect was not successful. */
        if (__redisAsyncHandleConnect(ac) != REDIS_OK)
            return;
        /* Try again later when the context is still not connected. */
        if (!(c->flags & REDIS_CONNECTED))
            return;
    }

    if (redisBufferRead(c) == REDIS_ERR) {
        __redisAsyncDisconnect(ac);
    } else {
        /* Always re-schedule reads */
        _EL_ADD_READ(ac);
        redisProcessCallbacks(ac);
    }
}

void redisAsyncHandleWrite(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    int done = 0;

    if (!(c->flags & REDIS_CONNECTED)) {
        /* Abort connect was not successful. */
        if (__redisAsyncHandleConnect(ac) != REDIS_OK)
            return;
        /* Try again later when the context is still not connected. */
        if (!(c->flags & REDIS_CONNECTED))
            return;
    }

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

#ifdef _WIN32
/* The redisAsyncHandleWrite is split into a Prep and Complete routines
   To allow using a write routine suitable for async behavior.
   For Windows this will use IOCP on write. */
int redisAsyncHandleWritePrep(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);

    if (!(c->flags & REDIS_CONNECTED)) {
        /* Abort connect was not successful. */
        if (__redisAsyncHandleConnect(ac) != REDIS_OK)
            return REDIS_ERR;
        /* Try again later when the context is still not connected. */
        if (!(c->flags & REDIS_CONNECTED))
            return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisAsyncHandleWriteComplete(redisAsyncContext *ac, int written) {
    redisContext *c = &(ac->c);
    int done = 0;
    int rc;

    rc = redisBufferWriteDone(c, written, &done);
    if (rc == REDIS_ERR) {
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
    return REDIS_OK;
}
#endif

/* Sets a pointer to the first argument and its length starting at p. Returns
 * the number of bytes to skip to get to the following argument. */
static char *nextArgument(char *start, char **str, size_t *len) {
    char *p = start;
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
static int __redisAsyncCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, char *cmd, size_t len) {
    redisContext *c = &(ac->c);
    redisCallback cb;
    int pvariant, hasnext;
    char *cstr, *astr;
    size_t clen, alen;
    char *p;
    sds sname;

    /* Don't accept new commands when the connection is about to be closed. */
    if (c->flags & (REDIS_DISCONNECTING | REDIS_FREEING)) return REDIS_ERR;

    /* Setup callback */
    cb.fn = fn;
    cb.privdata = privdata;

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
            sname = sdsnewlen(astr,alen);
            if (pvariant)
                dictReplace(ac->sub.patterns,sname,&cb);
            else
                dictReplace(ac->sub.channels,sname,&cb);
        }
    } else if (strncasecmp(cstr,"unsubscribe\r\n",13) == 0) {
        /* It is only useful to call (P)UNSUBSCRIBE when the context is
         * subscribed to one or more channels or patterns. */
        if (!(c->flags & REDIS_SUBSCRIBED)) return REDIS_ERR;

        /* (P)UNSUBSCRIBE does not have its own response: every channel or
         * pattern that is unsubscribed will receive a message. This means we
         * should not append a callback function for this command. */
     } else if(strncasecmp(cstr,"monitor\r\n",9) == 0) {
         /* Set monitor flag and push callback */
         c->flags |= REDIS_MONITORING;
         __redisPushCallback(&ac->replies,&cb);
    } else {
        if (c->flags & REDIS_SUBSCRIBED)
            /* This will likely result in an error reply, but it needs to be
             * received and passed to the callback. */
            __redisPushCallback(&ac->sub.invalid,&cb);
        else
            __redisPushCallback(&ac->replies,&cb);
    }

    __redisAppendCommand(c,cmd,len);

    /* Always schedule a write when the write buffer is non-empty */
    _EL_ADD_WRITE(ac);

    return REDIS_OK;
}

int redisvAsyncCommand(redisAsyncContext *ac, redisCallbackFn *fn, void *privdata, const char *format, va_list ap) {
    char *cmd;
    int len;
    int status;
    len = redisvFormatCommand(&cmd,format,ap);
    status = __redisAsyncCommand(ac,fn,privdata,cmd,len);
    free(cmd);
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
    char *cmd;
    int len;
    int status;
    len = redisFormatCommandArgv(&cmd,argc,argv,argvlen);
    status = __redisAsyncCommand(ac,fn,privdata,cmd,len);
    free(cmd);
    return status;
}
