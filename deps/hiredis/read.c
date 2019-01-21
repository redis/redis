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
#include <string.h>
#include <stdlib.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "read.h"
#include "sds.h"

static void __redisReaderSetError(redisReader *r, int type, const char *str) {
    size_t len;

    if (r->reply != NULL && r->fn && r->fn->freeObject) {
        r->fn->freeObject(r->reply);
        r->reply = NULL;
    }

    /* Clear input buffer on errors. */
    sdsfree(r->buf);
    r->buf = NULL;
    r->pos = r->len = 0;

    /* Reset task stack. */
    r->ridx = -1;

    /* Set error. */
    r->err = type;
    len = strlen(str);
    len = len < (sizeof(r->errstr)-1) ? len : (sizeof(r->errstr)-1);
    memcpy(r->errstr,str,len);
    r->errstr[len] = '\0';
}

static size_t chrtos(char *buf, size_t size, char byte) {
    size_t len = 0;

    switch(byte) {
    case '\\':
    case '"':
        len = snprintf(buf,size,"\"\\%c\"",byte);
        break;
    case '\n': len = snprintf(buf,size,"\"\\n\""); break;
    case '\r': len = snprintf(buf,size,"\"\\r\""); break;
    case '\t': len = snprintf(buf,size,"\"\\t\""); break;
    case '\a': len = snprintf(buf,size,"\"\\a\""); break;
    case '\b': len = snprintf(buf,size,"\"\\b\""); break;
    default:
        if (isprint(byte))
            len = snprintf(buf,size,"\"%c\"",byte);
        else
            len = snprintf(buf,size,"\"\\x%02x\"",(unsigned char)byte);
        break;
    }

    return len;
}

static void __redisReaderSetErrorProtocolByte(redisReader *r, char byte) {
    char cbuf[8], sbuf[128];

    chrtos(cbuf,sizeof(cbuf),byte);
    snprintf(sbuf,sizeof(sbuf),
        "Protocol error, got %s as reply type byte", cbuf);
    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,sbuf);
}

static void __redisReaderSetErrorOOM(redisReader *r) {
    __redisReaderSetError(r,REDIS_ERR_OOM,"Out of memory");
}

static char *readBytes(redisReader *r, unsigned int bytes) {
    char *p;
    if (r->len-r->pos >= bytes) {
        p = r->buf+r->pos;
        r->pos += bytes;
        return p;
    }
    return NULL;
}

/* Find pointer to \r\n. */
static char *seekNewline(char *s, size_t len) {
    int pos = 0;
    int _len = len-1;

    /* Position should be < len-1 because the character at "pos" should be
     * followed by a \n. Note that strchr cannot be used because it doesn't
     * allow to search a limited length and the buffer that is being searched
     * might not have a trailing NULL character. */
    while (pos < _len) {
        while(pos < _len && s[pos] != '\r') pos++;
        if (pos==_len) {
            /* Not found. */
            return NULL;
        } else {
            if (s[pos+1] == '\n') {
                /* Found. */
                return s+pos;
            } else {
                /* Continue searching. */
                pos++;
            }
        }
    }
    return NULL;
}

/* Convert a string into a long long. Returns REDIS_OK if the string could be
 * parsed into a (non-overflowing) long long, REDIS_ERR otherwise. The value
 * will be set to the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a long long: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. */
static int string2ll(const char *s, size_t slen, long long *value) {
    const char *p = s;
    size_t plen = 0;
    int negative = 0;
    unsigned long long v;

    if (plen == slen)
        return REDIS_ERR;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return REDIS_OK;
    }

    if (p[0] == '-') {
        negative = 1;
        p++; plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return REDIS_ERR;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0]-'0';
        p++; plen++;
    } else if (p[0] == '0' && slen == 1) {
        *value = 0;
        return REDIS_OK;
    } else {
        return REDIS_ERR;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (ULLONG_MAX / 10)) /* Overflow. */
            return REDIS_ERR;
        v *= 10;

        if (v > (ULLONG_MAX - (p[0]-'0'))) /* Overflow. */
            return REDIS_ERR;
        v += p[0]-'0';

        p++; plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return REDIS_ERR;

    if (negative) {
        if (v > ((unsigned long long)(-(LLONG_MIN+1))+1)) /* Overflow. */
            return REDIS_ERR;
        if (value != NULL) *value = -v;
    } else {
        if (v > LLONG_MAX) /* Overflow. */
            return REDIS_ERR;
        if (value != NULL) *value = v;
    }
    return REDIS_OK;
}

static char *readLine(redisReader *r, int *_len) {
    char *p, *s;
    int len;

    p = r->buf+r->pos;
    s = seekNewline(p,(r->len-r->pos));
    if (s != NULL) {
        len = s-(r->buf+r->pos);
        r->pos += len+2; /* skip \r\n */
        if (_len) *_len = len;
        return p;
    }
    return NULL;
}

static void moveToNextTask(redisReader *r) {
    redisReadTask *cur, *prv;
    while (r->ridx >= 0) {
        /* Return a.s.a.p. when the stack is now empty. */
        if (r->ridx == 0) {
            r->ridx--;
            return;
        }

        cur = &(r->rstack[r->ridx]);
        prv = &(r->rstack[r->ridx-1]);
        assert(prv->type == REDIS_REPLY_ARRAY ||
               prv->type == REDIS_REPLY_MAP ||
               prv->type == REDIS_REPLY_SET);
        if (cur->idx == prv->elements-1) {
            r->ridx--;
        } else {
            /* Reset the type because the next item can be anything */
            assert(cur->idx < prv->elements);
            cur->type = -1;
            cur->elements = -1;
            cur->idx++;
            return;
        }
    }
}

static int processLineItem(redisReader *r) {
    redisReadTask *cur = &(r->rstack[r->ridx]);
    void *obj;
    char *p;
    int len;

    if ((p = readLine(r,&len)) != NULL) {
        if (cur->type == REDIS_REPLY_INTEGER) {
            if (r->fn && r->fn->createInteger) {
                long long v;
                if (string2ll(p, len, &v) == REDIS_ERR) {
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Bad integer value");
                    return REDIS_ERR;
                }
                obj = r->fn->createInteger(cur,v);
            } else {
                obj = (void*)REDIS_REPLY_INTEGER;
            }
        } else if (cur->type == REDIS_REPLY_DOUBLE) {
            if (r->fn && r->fn->createDouble) {
                char buf[326], *eptr;
                double d;

                if ((size_t)len >= sizeof(buf)) {
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Double value is too large");
                    return REDIS_ERR;
                }

                memcpy(buf,p,len);
                buf[len] = '\0';

                if (strcasecmp(buf,",inf") == 0) {
                    d = 1.0/0.0; /* Positive infinite. */
                } else if (strcasecmp(buf,",-inf") == 0) {
                    d = -1.0/0.0; /* Nevative infinite. */
                } else {
                    d = strtod((char*)buf,&eptr);
                    if (buf[0] == '\0' || eptr[0] != '\0' || isnan(d)) {
                        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                                "Bad double value");
                        return REDIS_ERR;
                    }
                }
                obj = r->fn->createDouble(cur,d,buf,len);
            } else {
                obj = (void*)REDIS_REPLY_DOUBLE;
            }
        } else if (cur->type == REDIS_REPLY_NIL) {
            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void*)REDIS_REPLY_NIL;
        } else if (cur->type == REDIS_REPLY_BOOL) {
            int bval = p[0] == 't' || p[0] == 'T';
            if (r->fn && r->fn->createBool)
                obj = r->fn->createBool(cur,bval);
            else
                obj = (void*)REDIS_REPLY_BOOL;
        } else {
            /* Type will be error or status. */
            if (r->fn && r->fn->createString)
                obj = r->fn->createString(cur,p,len);
            else
                obj = (void*)(size_t)(cur->type);
        }

        if (obj == NULL) {
            __redisReaderSetErrorOOM(r);
            return REDIS_ERR;
        }

        /* Set reply if this is the root object. */
        if (r->ridx == 0) r->reply = obj;
        moveToNextTask(r);
        return REDIS_OK;
    }

    return REDIS_ERR;
}

static int processBulkItem(redisReader *r) {
    redisReadTask *cur = &(r->rstack[r->ridx]);
    void *obj = NULL;
    char *p, *s;
    long long len;
    unsigned long bytelen;
    int success = 0;

    p = r->buf+r->pos;
    s = seekNewline(p,r->len-r->pos);
    if (s != NULL) {
        p = r->buf+r->pos;
        bytelen = s-(r->buf+r->pos)+2; /* include \r\n */

        if (string2ll(p, bytelen - 2, &len) == REDIS_ERR) {
            __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                    "Bad bulk string length");
            return REDIS_ERR;
        }

        if (len < -1 || (LLONG_MAX > SIZE_MAX && len > (long long)SIZE_MAX)) {
            __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                    "Bulk string length out of range");
            return REDIS_ERR;
        }

        if (len == -1) {
            /* The nil object can always be created. */
            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void*)REDIS_REPLY_NIL;
            success = 1;
        } else {
            /* Only continue when the buffer contains the entire bulk item. */
            bytelen += len+2; /* include \r\n */
            if (r->pos+bytelen <= r->len) {
                if (r->fn && r->fn->createString)
                    obj = r->fn->createString(cur,s+2,len);
                else
                    obj = (void*)REDIS_REPLY_STRING;
                success = 1;
            }
        }

        /* Proceed when obj was created. */
        if (success) {
            if (obj == NULL) {
                __redisReaderSetErrorOOM(r);
                return REDIS_ERR;
            }

            r->pos += bytelen;

            /* Set reply if this is the root object. */
            if (r->ridx == 0) r->reply = obj;
            moveToNextTask(r);
            return REDIS_OK;
        }
    }

    return REDIS_ERR;
}

/* Process the array, map and set types. */
static int processAggregateItem(redisReader *r) {
    redisReadTask *cur = &(r->rstack[r->ridx]);
    void *obj;
    char *p;
    long long elements;
    int root = 0, len;

    /* Set error for nested multi bulks with depth > 7 */
    if (r->ridx == 8) {
        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
            "No support for nested multi bulk replies with depth > 7");
        return REDIS_ERR;
    }

    if ((p = readLine(r,&len)) != NULL) {
        if (string2ll(p, len, &elements) == REDIS_ERR) {
            __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                    "Bad multi-bulk length");
            return REDIS_ERR;
        }

        root = (r->ridx == 0);

        if (elements < -1 || elements > INT_MAX) {
            __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                    "Multi-bulk length out of range");
            return REDIS_ERR;
        }

        if (elements == -1) {
            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void*)REDIS_REPLY_NIL;

            if (obj == NULL) {
                __redisReaderSetErrorOOM(r);
                return REDIS_ERR;
            }

            moveToNextTask(r);
        } else {
            if (cur->type == REDIS_REPLY_MAP) elements *= 2;

            if (r->fn && r->fn->createArray)
                obj = r->fn->createArray(cur,elements);
            else
                obj = (void*)(long)cur->type;

            if (obj == NULL) {
                __redisReaderSetErrorOOM(r);
                return REDIS_ERR;
            }

            /* Modify task stack when there are more than 0 elements. */
            if (elements > 0) {
                cur->elements = elements;
                cur->obj = obj;
                r->ridx++;
                r->rstack[r->ridx].type = -1;
                r->rstack[r->ridx].elements = -1;
                r->rstack[r->ridx].idx = 0;
                r->rstack[r->ridx].obj = NULL;
                r->rstack[r->ridx].parent = cur;
                r->rstack[r->ridx].privdata = r->privdata;
            } else {
                moveToNextTask(r);
            }
        }

        /* Set reply if this is the root object. */
        if (root) r->reply = obj;
        return REDIS_OK;
    }

    return REDIS_ERR;
}

static int processItem(redisReader *r) {
    redisReadTask *cur = &(r->rstack[r->ridx]);
    char *p;

    /* check if we need to read type */
    if (cur->type < 0) {
        if ((p = readBytes(r,1)) != NULL) {
            switch (p[0]) {
            case '-':
                cur->type = REDIS_REPLY_ERROR;
                break;
            case '+':
                cur->type = REDIS_REPLY_STATUS;
                break;
            case ':':
                cur->type = REDIS_REPLY_INTEGER;
                break;
            case ',':
                cur->type = REDIS_REPLY_DOUBLE;
                break;
            case '_':
                cur->type = REDIS_REPLY_NIL;
                break;
            case '$':
                cur->type = REDIS_REPLY_STRING;
                break;
            case '*':
                cur->type = REDIS_REPLY_ARRAY;
                break;
            case '%':
                cur->type = REDIS_REPLY_MAP;
                break;
            case '~':
                cur->type = REDIS_REPLY_SET;
                break;
            case '#':
                cur->type = REDIS_REPLY_BOOL;
                break;
            default:
                __redisReaderSetErrorProtocolByte(r,*p);
                return REDIS_ERR;
            }
        } else {
            /* could not consume 1 byte */
            return REDIS_ERR;
        }
    }

    /* process typed item */
    switch(cur->type) {
    case REDIS_REPLY_ERROR:
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_INTEGER:
    case REDIS_REPLY_DOUBLE:
    case REDIS_REPLY_NIL:
    case REDIS_REPLY_BOOL:
        return processLineItem(r);
    case REDIS_REPLY_STRING:
        return processBulkItem(r);
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_MAP:
    case REDIS_REPLY_SET:
        return processAggregateItem(r);
    default:
        assert(NULL);
        return REDIS_ERR; /* Avoid warning. */
    }
}

redisReader *redisReaderCreateWithFunctions(redisReplyObjectFunctions *fn) {
    redisReader *r;

    r = calloc(1,sizeof(redisReader));
    if (r == NULL)
        return NULL;

    r->fn = fn;
    r->buf = sdsempty();
    r->maxbuf = REDIS_READER_MAX_BUF;
    if (r->buf == NULL) {
        free(r);
        return NULL;
    }

    r->ridx = -1;
    return r;
}

void redisReaderFree(redisReader *r) {
    if (r == NULL)
        return;
    if (r->reply != NULL && r->fn && r->fn->freeObject)
        r->fn->freeObject(r->reply);
    sdsfree(r->buf);
    free(r);
}

int redisReaderFeed(redisReader *r, const char *buf, size_t len) {
    sds newbuf;

    /* Return early when this reader is in an erroneous state. */
    if (r->err)
        return REDIS_ERR;

    /* Copy the provided buffer. */
    if (buf != NULL && len >= 1) {
        /* Destroy internal buffer when it is empty and is quite large. */
        if (r->len == 0 && r->maxbuf != 0 && sdsavail(r->buf) > r->maxbuf) {
            sdsfree(r->buf);
            r->buf = sdsempty();
            r->pos = 0;

            /* r->buf should not be NULL since we just free'd a larger one. */
            assert(r->buf != NULL);
        }

        newbuf = sdscatlen(r->buf,buf,len);
        if (newbuf == NULL) {
            __redisReaderSetErrorOOM(r);
            return REDIS_ERR;
        }

        r->buf = newbuf;
        r->len = sdslen(r->buf);
    }

    return REDIS_OK;
}

int redisReaderGetReply(redisReader *r, void **reply) {
    /* Default target pointer to NULL. */
    if (reply != NULL)
        *reply = NULL;

    /* Return early when this reader is in an erroneous state. */
    if (r->err)
        return REDIS_ERR;

    /* When the buffer is empty, there will never be a reply. */
    if (r->len == 0)
        return REDIS_OK;

    /* Set first item to process when the stack is empty. */
    if (r->ridx == -1) {
        r->rstack[0].type = -1;
        r->rstack[0].elements = -1;
        r->rstack[0].idx = -1;
        r->rstack[0].obj = NULL;
        r->rstack[0].parent = NULL;
        r->rstack[0].privdata = r->privdata;
        r->ridx = 0;
    }

    /* Process items in reply. */
    while (r->ridx >= 0)
        if (processItem(r) != REDIS_OK)
            break;

    /* Return ASAP when an error occurred. */
    if (r->err)
        return REDIS_ERR;

    /* Discard part of the buffer when we've consumed at least 1k, to avoid
     * doing unnecessary calls to memmove() in sds.c. */
    if (r->pos >= 1024) {
        sdsrange(r->buf,r->pos,-1);
        r->pos = 0;
        r->len = sdslen(r->buf);
    }

    /* Emit a reply when there is one. */
    if (r->ridx == -1) {
        if (reply != NULL)
            *reply = r->reply;
        r->reply = NULL;
    }
    return REDIS_OK;
}
