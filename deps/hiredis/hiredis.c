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
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>

#include "hiredis.h"
#include "net.h"
#include "sds.h"

static redisReply *createReplyObject(int type);
static void *createStringObject(const redisReadTask *task, char *str, size_t len);
static void *createArrayObject(const redisReadTask *task, int elements);
static void *createIntegerObject(const redisReadTask *task, long long value);
static void *createNilObject(const redisReadTask *task);

/* Default set of functions to build the reply. Keep in mind that such a
 * function returning NULL is interpreted as OOM. */
static redisReplyObjectFunctions defaultFunctions = {
    createStringObject,
    createArrayObject,
    createIntegerObject,
    createNilObject,
    freeReplyObject
};

/* Create a reply object */
static redisReply *createReplyObject(int type) {
    redisReply *r = calloc(1,sizeof(*r));

    if (r == NULL)
        return NULL;

    r->type = type;
    return r;
}

/* Free a reply object */
void freeReplyObject(void *reply) {
    redisReply *r = reply;
    size_t j;

    switch(r->type) {
    case REDIS_REPLY_INTEGER:
        break; /* Nothing to free */
    case REDIS_REPLY_ARRAY:
        if (r->element != NULL) {
            for (j = 0; j < r->elements; j++)
                if (r->element[j] != NULL)
                    freeReplyObject(r->element[j]);
            free(r->element);
        }
        break;
    case REDIS_REPLY_ERROR:
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_STRING:
        if (r->str != NULL)
            free(r->str);
        break;
    }
    free(r);
}

static void *createStringObject(const redisReadTask *task, char *str, size_t len) {
    redisReply *r, *parent;
    char *buf;

    r = createReplyObject(task->type);
    if (r == NULL)
        return NULL;

    buf = malloc(len+1);
    if (buf == NULL) {
        freeReplyObject(r);
        return NULL;
    }

    assert(task->type == REDIS_REPLY_ERROR  ||
           task->type == REDIS_REPLY_STATUS ||
           task->type == REDIS_REPLY_STRING);

    /* Copy string value */
    memcpy(buf,str,len);
    buf[len] = '\0';
    r->str = buf;
    r->len = len;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createArrayObject(const redisReadTask *task, int elements) {
    redisReply *r, *parent;

    r = createReplyObject(REDIS_REPLY_ARRAY);
    if (r == NULL)
        return NULL;

    if (elements > 0) {
        r->element = calloc(elements,sizeof(redisReply*));
        if (r->element == NULL) {
            freeReplyObject(r);
            return NULL;
        }
    }

    r->elements = elements;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createIntegerObject(const redisReadTask *task, long long value) {
    redisReply *r, *parent;

    r = createReplyObject(REDIS_REPLY_INTEGER);
    if (r == NULL)
        return NULL;

    r->integer = value;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createNilObject(const redisReadTask *task) {
    redisReply *r, *parent;

    r = createReplyObject(REDIS_REPLY_NIL);
    if (r == NULL)
        return NULL;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY);
        parent->element[task->idx] = r;
    }
    return r;
}

static void __redisReaderSetError(redisReader *r, int type, const char *str) {
    size_t len;

    if (r->reply != NULL && r->fn && r->fn->freeObject) {
        r->fn->freeObject(r->reply);
        r->reply = NULL;
    }

    /* Clear input buffer on errors. */
    if (r->buf != NULL) {
        sdsfree(r->buf);
        r->buf = NULL;
        r->pos = r->len = 0;
    }

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
        if (s[pos] != '\r') {
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

/* Read a long long value starting at *s, under the assumption that it will be
 * terminated by \r\n. Ambiguously returns -1 for unexpected input. */
static long long readLongLong(char *s) {
    long long v = 0;
    int dec, mult = 1;
    char c;

    if (*s == '-') {
        mult = -1;
        s++;
    } else if (*s == '+') {
        mult = 1;
        s++;
    }

    while ((c = *(s++)) != '\r') {
        dec = c - '0';
        if (dec >= 0 && dec < 10) {
            v *= 10;
            v += dec;
        } else {
            /* Should not happen... */
            return -1;
        }
    }

    return mult*v;
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
        assert(prv->type == REDIS_REPLY_ARRAY);
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
            if (r->fn && r->fn->createInteger)
                obj = r->fn->createInteger(cur,readLongLong(p));
            else
                obj = (void*)REDIS_REPLY_INTEGER;
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
    long len;
    unsigned long bytelen;
    int success = 0;

    p = r->buf+r->pos;
    s = seekNewline(p,r->len-r->pos);
    if (s != NULL) {
        p = r->buf+r->pos;
        bytelen = s-(r->buf+r->pos)+2; /* include \r\n */
        len = readLongLong(p);

        if (len < 0) {
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

static int processMultiBulkItem(redisReader *r) {
    redisReadTask *cur = &(r->rstack[r->ridx]);
    void *obj;
    char *p;
    long elements;
    int root = 0;

    /* Set error for nested multi bulks with depth > 2 */
    if (r->ridx == 3) {
        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
            "No support for nested multi bulk replies with depth > 2");
        return REDIS_ERR;
    }

    if ((p = readLine(r,NULL)) != NULL) {
        elements = readLongLong(p);
        root = (r->ridx == 0);

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
            if (r->fn && r->fn->createArray)
                obj = r->fn->createArray(cur,elements);
            else
                obj = (void*)REDIS_REPLY_ARRAY;

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
            case '$':
                cur->type = REDIS_REPLY_STRING;
                break;
            case '*':
                cur->type = REDIS_REPLY_ARRAY;
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
        return processLineItem(r);
    case REDIS_REPLY_STRING:
        return processBulkItem(r);
    case REDIS_REPLY_ARRAY:
        return processMultiBulkItem(r);
    default:
        assert(NULL);
        return REDIS_ERR; /* Avoid warning. */
    }
}

redisReader *redisReaderCreate(void) {
    redisReader *r;

    r = calloc(sizeof(redisReader),1);
    if (r == NULL)
        return NULL;

    r->err = 0;
    r->errstr[0] = '\0';
    r->fn = &defaultFunctions;
    r->buf = sdsempty();
    if (r->buf == NULL) {
        free(r);
        return NULL;
    }

    r->ridx = -1;
    return r;
}

void redisReaderFree(redisReader *r) {
    if (r->reply != NULL && r->fn && r->fn->freeObject)
        r->fn->freeObject(r->reply);
    if (r->buf != NULL)
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
        if (r->len == 0 && sdsavail(r->buf) > 16*1024) {
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
        r->buf = sdsrange(r->buf,r->pos,-1);
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

/* Calculate the number of bytes needed to represent an integer as string. */
static int intlen(int i) {
    int len = 0;
    if (i < 0) {
        len++;
        i = -i;
    }
    do {
        len++;
        i /= 10;
    } while(i);
    return len;
}

/* Helper that calculates the bulk length given a certain string length. */
static size_t bulklen(size_t len) {
    return 1+intlen(len)+2+len+2;
}

int redisvFormatCommand(char **target, const char *format, va_list ap) {
    const char *c = format;
    char *cmd = NULL; /* final command */
    int pos; /* position in final command */
    sds curarg, newarg; /* current argument */
    int touched = 0; /* was the current argument touched? */
    char **curargv = NULL, **newargv = NULL;
    int argc = 0;
    int totlen = 0;
    int j;

    /* Abort if there is not target to set */
    if (target == NULL)
        return -1;

    /* Build the command string accordingly to protocol */
    curarg = sdsempty();
    if (curarg == NULL)
        return -1;

    while(*c != '\0') {
        if (*c != '%' || c[1] == '\0') {
            if (*c == ' ') {
                if (touched) {
                    newargv = realloc(curargv,sizeof(char*)*(argc+1));
                    if (newargv == NULL) goto err;
                    curargv = newargv;
                    curargv[argc++] = curarg;
                    totlen += bulklen(sdslen(curarg));

                    /* curarg is put in argv so it can be overwritten. */
                    curarg = sdsempty();
                    if (curarg == NULL) goto err;
                    touched = 0;
                }
            } else {
                newarg = sdscatlen(curarg,c,1);
                if (newarg == NULL) goto err;
                curarg = newarg;
                touched = 1;
            }
        } else {
            char *arg;
            size_t size;

            /* Set newarg so it can be checked even if it is not touched. */
            newarg = curarg;

            switch(c[1]) {
            case 's':
                arg = va_arg(ap,char*);
                size = strlen(arg);
                if (size > 0)
                    newarg = sdscatlen(curarg,arg,size);
                break;
            case 'b':
                arg = va_arg(ap,char*);
                size = va_arg(ap,size_t);
                if (size > 0)
                    newarg = sdscatlen(curarg,arg,size);
                break;
            case '%':
                newarg = sdscat(curarg,"%");
                break;
            default:
                /* Try to detect printf format */
                {
                    static const char intfmts[] = "diouxX";
                    char _format[16];
                    const char *_p = c+1;
                    size_t _l = 0;
                    va_list _cpy;

                    /* Flags */
                    if (*_p != '\0' && *_p == '#') _p++;
                    if (*_p != '\0' && *_p == '0') _p++;
                    if (*_p != '\0' && *_p == '-') _p++;
                    if (*_p != '\0' && *_p == ' ') _p++;
                    if (*_p != '\0' && *_p == '+') _p++;

                    /* Field width */
                    while (*_p != '\0' && isdigit(*_p)) _p++;

                    /* Precision */
                    if (*_p == '.') {
                        _p++;
                        while (*_p != '\0' && isdigit(*_p)) _p++;
                    }

                    /* Copy va_list before consuming with va_arg */
                    va_copy(_cpy,ap);

                    /* Integer conversion (without modifiers) */
                    if (strchr(intfmts,*_p) != NULL) {
                        va_arg(ap,int);
                        goto fmt_valid;
                    }

                    /* Double conversion (without modifiers) */
                    if (strchr("eEfFgGaA",*_p) != NULL) {
                        va_arg(ap,double);
                        goto fmt_valid;
                    }

                    /* Size: char */
                    if (_p[0] == 'h' && _p[1] == 'h') {
                        _p += 2;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,int); /* char gets promoted to int */
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: short */
                    if (_p[0] == 'h') {
                        _p += 1;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,int); /* short gets promoted to int */
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: long long */
                    if (_p[0] == 'l' && _p[1] == 'l') {
                        _p += 2;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,long long);
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: long */
                    if (_p[0] == 'l') {
                        _p += 1;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,long);
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                fmt_invalid:
                    va_end(_cpy);
                    goto err;

                fmt_valid:
                    _l = (_p+1)-c;
                    if (_l < sizeof(_format)-2) {
                        memcpy(_format,c,_l);
                        _format[_l] = '\0';
                        newarg = sdscatvprintf(curarg,_format,_cpy);

                        /* Update current position (note: outer blocks
                         * increment c twice so compensate here) */
                        c = _p-1;
                    }

                    va_end(_cpy);
                    break;
                }
            }

            if (newarg == NULL) goto err;
            curarg = newarg;

            touched = 1;
            c++;
        }
        c++;
    }

    /* Add the last argument if needed */
    if (touched) {
        newargv = realloc(curargv,sizeof(char*)*(argc+1));
        if (newargv == NULL) goto err;
        curargv = newargv;
        curargv[argc++] = curarg;
        totlen += bulklen(sdslen(curarg));
    } else {
        sdsfree(curarg);
    }

    /* Clear curarg because it was put in curargv or was free'd. */
    curarg = NULL;

    /* Add bytes needed to hold multi bulk count */
    totlen += 1+intlen(argc)+2;

    /* Build the command at protocol level */
    cmd = malloc(totlen+1);
    if (cmd == NULL) goto err;

    pos = sprintf(cmd,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        pos += sprintf(cmd+pos,"$%zu\r\n",sdslen(curargv[j]));
        memcpy(cmd+pos,curargv[j],sdslen(curargv[j]));
        pos += sdslen(curargv[j]);
        sdsfree(curargv[j]);
        cmd[pos++] = '\r';
        cmd[pos++] = '\n';
    }
    assert(pos == totlen);
    cmd[pos] = '\0';

    free(curargv);
    *target = cmd;
    return totlen;

err:
    while(argc--)
        sdsfree(curargv[argc]);
    free(curargv);

    if (curarg != NULL)
        sdsfree(curarg);

    /* No need to check cmd since it is the last statement that can fail,
     * but do it anyway to be as defensive as possible. */
    if (cmd != NULL)
        free(cmd);

    return -1;
}

/* Format a command according to the Redis protocol. This function
 * takes a format similar to printf:
 *
 * %s represents a C null terminated string you want to interpolate
 * %b represents a binary safe string
 *
 * When using %b you need to provide both the pointer to the string
 * and the length in bytes. Examples:
 *
 * len = redisFormatCommand(target, "GET %s", mykey);
 * len = redisFormatCommand(target, "SET %s %b", mykey, myval, myvallen);
 */
int redisFormatCommand(char **target, const char *format, ...) {
    va_list ap;
    int len;
    va_start(ap,format);
    len = redisvFormatCommand(target,format,ap);
    va_end(ap);
    return len;
}

/* Format a command according to the Redis protocol. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
int redisFormatCommandArgv(char **target, int argc, const char **argv, const size_t *argvlen) {
    char *cmd = NULL; /* final command */
    int pos; /* position in final command */
    size_t len;
    int totlen, j;

    /* Calculate number of bytes needed for the command */
    totlen = 1+intlen(argc)+2;
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        totlen += bulklen(len);
    }

    /* Build the command at protocol level */
    cmd = malloc(totlen+1);
    if (cmd == NULL)
        return -1;

    pos = sprintf(cmd,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        pos += sprintf(cmd+pos,"$%zu\r\n",len);
        memcpy(cmd+pos,argv[j],len);
        pos += len;
        cmd[pos++] = '\r';
        cmd[pos++] = '\n';
    }
    assert(pos == totlen);
    cmd[pos] = '\0';

    *target = cmd;
    return totlen;
}

void __redisSetError(redisContext *c, int type, const char *str) {
    size_t len;

    c->err = type;
    if (str != NULL) {
        len = strlen(str);
        len = len < (sizeof(c->errstr)-1) ? len : (sizeof(c->errstr)-1);
        memcpy(c->errstr,str,len);
        c->errstr[len] = '\0';
    } else {
        /* Only REDIS_ERR_IO may lack a description! */
        assert(type == REDIS_ERR_IO);
        strerror_r(errno,c->errstr,sizeof(c->errstr));
    }
}

static redisContext *redisContextInit(void) {
    redisContext *c;

    c = calloc(1,sizeof(redisContext));
    if (c == NULL)
        return NULL;

    c->err = 0;
    c->errstr[0] = '\0';
    c->obuf = sdsempty();
    c->reader = redisReaderCreate();
    return c;
}

void redisFree(redisContext *c) {
    if (c->fd > 0)
        close(c->fd);
    if (c->obuf != NULL)
        sdsfree(c->obuf);
    if (c->reader != NULL)
        redisReaderFree(c->reader);
    free(c);
}

/* Connect to a Redis instance. On error the field error in the returned
 * context will be set to the return value of the error function.
 * When no set of reply functions is given, the default set will be used. */
redisContext *redisConnect(const char *ip, int port) {
    redisContext *c = redisContextInit();
    c->flags |= REDIS_BLOCK;
    redisContextConnectTcp(c,ip,port,NULL);
    return c;
}

redisContext *redisConnectWithTimeout(const char *ip, int port, struct timeval tv) {
    redisContext *c = redisContextInit();
    c->flags |= REDIS_BLOCK;
    redisContextConnectTcp(c,ip,port,&tv);
    return c;
}

redisContext *redisConnectNonBlock(const char *ip, int port) {
    redisContext *c = redisContextInit();
    c->flags &= ~REDIS_BLOCK;
    redisContextConnectTcp(c,ip,port,NULL);
    return c;
}

redisContext *redisConnectUnix(const char *path) {
    redisContext *c = redisContextInit();
    c->flags |= REDIS_BLOCK;
    redisContextConnectUnix(c,path,NULL);
    return c;
}

redisContext *redisConnectUnixWithTimeout(const char *path, struct timeval tv) {
    redisContext *c = redisContextInit();
    c->flags |= REDIS_BLOCK;
    redisContextConnectUnix(c,path,&tv);
    return c;
}

redisContext *redisConnectUnixNonBlock(const char *path) {
    redisContext *c = redisContextInit();
    c->flags &= ~REDIS_BLOCK;
    redisContextConnectUnix(c,path,NULL);
    return c;
}

/* Set read/write timeout on a blocking socket. */
int redisSetTimeout(redisContext *c, struct timeval tv) {
    if (c->flags & REDIS_BLOCK)
        return redisContextSetTimeout(c,tv);
    return REDIS_ERR;
}

/* Use this function to handle a read event on the descriptor. It will try
 * and read some bytes from the socket and feed them to the reply parser.
 *
 * After this function is called, you may use redisContextReadReply to
 * see if there is a reply available. */
int redisBufferRead(redisContext *c) {
    char buf[2048];
    int nread;

    /* Return early when the context has seen an error. */
    if (c->err)
        return REDIS_ERR;

    nread = read(c->fd,buf,sizeof(buf));
    if (nread == -1) {
        if (errno == EAGAIN && !(c->flags & REDIS_BLOCK)) {
            /* Try again later */
        } else {
            __redisSetError(c,REDIS_ERR_IO,NULL);
            return REDIS_ERR;
        }
    } else if (nread == 0) {
        __redisSetError(c,REDIS_ERR_EOF,"Server closed the connection");
        return REDIS_ERR;
    } else {
        if (redisReaderFeed(c->reader,buf,nread) != REDIS_OK) {
            __redisSetError(c,c->reader->err,c->reader->errstr);
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

/* Write the output buffer to the socket.
 *
 * Returns REDIS_OK when the buffer is empty, or (a part of) the buffer was
 * succesfully written to the socket. When the buffer is empty after the
 * write operation, "done" is set to 1 (if given).
 *
 * Returns REDIS_ERR if an error occured trying to write and sets
 * c->errstr to hold the appropriate error string.
 */
int redisBufferWrite(redisContext *c, int *done) {
    int nwritten;

    /* Return early when the context has seen an error. */
    if (c->err)
        return REDIS_ERR;

    if (sdslen(c->obuf) > 0) {
        nwritten = write(c->fd,c->obuf,sdslen(c->obuf));
        if (nwritten == -1) {
            if (errno == EAGAIN && !(c->flags & REDIS_BLOCK)) {
                /* Try again later */
            } else {
                __redisSetError(c,REDIS_ERR_IO,NULL);
                return REDIS_ERR;
            }
        } else if (nwritten > 0) {
            if (nwritten == (signed)sdslen(c->obuf)) {
                sdsfree(c->obuf);
                c->obuf = sdsempty();
            } else {
                c->obuf = sdsrange(c->obuf,nwritten,-1);
            }
        }
    }
    if (done != NULL) *done = (sdslen(c->obuf) == 0);
    return REDIS_OK;
}

/* Internal helper function to try and get a reply from the reader,
 * or set an error in the context otherwise. */
int redisGetReplyFromReader(redisContext *c, void **reply) {
    if (redisReaderGetReply(c->reader,reply) == REDIS_ERR) {
        __redisSetError(c,c->reader->err,c->reader->errstr);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisGetReply(redisContext *c, void **reply) {
    int wdone = 0;
    void *aux = NULL;

    /* Try to read pending replies */
    if (redisGetReplyFromReader(c,&aux) == REDIS_ERR)
        return REDIS_ERR;

    /* For the blocking context, flush output buffer and read reply */
    if (aux == NULL && c->flags & REDIS_BLOCK) {
        /* Write until done */
        do {
            if (redisBufferWrite(c,&wdone) == REDIS_ERR)
                return REDIS_ERR;
        } while (!wdone);

        /* Read until there is a reply */
        do {
            if (redisBufferRead(c) == REDIS_ERR)
                return REDIS_ERR;
            if (redisGetReplyFromReader(c,&aux) == REDIS_ERR)
                return REDIS_ERR;
        } while (aux == NULL);
    }

    /* Set reply object */
    if (reply != NULL) *reply = aux;
    return REDIS_OK;
}


/* Helper function for the redisAppendCommand* family of functions.
 *
 * Write a formatted command to the output buffer. When this family
 * is used, you need to call redisGetReply yourself to retrieve
 * the reply (or replies in pub/sub).
 */
int __redisAppendCommand(redisContext *c, char *cmd, size_t len) {
    sds newbuf;

    newbuf = sdscatlen(c->obuf,cmd,len);
    if (newbuf == NULL) {
        __redisSetError(c,REDIS_ERR_OOM,"Out of memory");
        return REDIS_ERR;
    }

    c->obuf = newbuf;
    return REDIS_OK;
}

int redisvAppendCommand(redisContext *c, const char *format, va_list ap) {
    char *cmd;
    int len;

    len = redisvFormatCommand(&cmd,format,ap);
    if (len == -1) {
        __redisSetError(c,REDIS_ERR_OOM,"Out of memory");
        return REDIS_ERR;
    }

    if (__redisAppendCommand(c,cmd,len) != REDIS_OK) {
        free(cmd);
        return REDIS_ERR;
    }

    free(cmd);
    return REDIS_OK;
}

int redisAppendCommand(redisContext *c, const char *format, ...) {
    va_list ap;
    int ret;

    va_start(ap,format);
    ret = redisvAppendCommand(c,format,ap);
    va_end(ap);
    return ret;
}

int redisAppendCommandArgv(redisContext *c, int argc, const char **argv, const size_t *argvlen) {
    char *cmd;
    int len;

    len = redisFormatCommandArgv(&cmd,argc,argv,argvlen);
    if (len == -1) {
        __redisSetError(c,REDIS_ERR_OOM,"Out of memory");
        return REDIS_ERR;
    }

    if (__redisAppendCommand(c,cmd,len) != REDIS_OK) {
        free(cmd);
        return REDIS_ERR;
    }

    free(cmd);
    return REDIS_OK;
}

/* Helper function for the redisCommand* family of functions.
 *
 * Write a formatted command to the output buffer. If the given context is
 * blocking, immediately read the reply into the "reply" pointer. When the
 * context is non-blocking, the "reply" pointer will not be used and the
 * command is simply appended to the write buffer.
 *
 * Returns the reply when a reply was succesfully retrieved. Returns NULL
 * otherwise. When NULL is returned in a blocking context, the error field
 * in the context will be set.
 */
static void *__redisBlockForReply(redisContext *c) {
    void *reply;

    if (c->flags & REDIS_BLOCK) {
        if (redisGetReply(c,&reply) != REDIS_OK)
            return NULL;
        return reply;
    }
    return NULL;
}

void *redisvCommand(redisContext *c, const char *format, va_list ap) {
    if (redisvAppendCommand(c,format,ap) != REDIS_OK)
        return NULL;
    return __redisBlockForReply(c);
}

void *redisCommand(redisContext *c, const char *format, ...) {
    va_list ap;
    void *reply = NULL;
    va_start(ap,format);
    reply = redisvCommand(c,format,ap);
    va_end(ap);
    return reply;
}

void *redisCommandArgv(redisContext *c, int argc, const char **argv, const size_t *argvlen) {
    if (redisAppendCommandArgv(c,argc,argv,argvlen) != REDIS_OK)
        return NULL;
    return __redisBlockForReply(c);
}
