/*
 * Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* TODO:
 * - After loading a stream, populate the last ID.
 */

#include "server.h"
#include "endianconv.h"
#include "stream.h"

#define STREAM_BYTES_PER_LISTPACK 4096

/* -----------------------------------------------------------------------
 * Low level stream encoding: a radix tree of listpacks.
 * ----------------------------------------------------------------------- */

/* Create a new stream data structure. */
stream *streamNew(void) {
    stream *s = zmalloc(sizeof(*s));
    s->rax = raxNew();
    s->length = 0;
    s->last_id.ms = 0;
    s->last_id.seq = 0;
    return s;
}

/* Free a stream, including the listpacks stored inside the radix tree. */
void freeStream(stream *s) {
    raxFreeWithCallback(s->rax,(void(*)(void*))lpFree);
}

/* Generate the next stream item ID given the previous one. If the current
 * milliseconds Unix time is greater than the previous one, just use this
 * as time part and start with sequence part of zero. Otherwise we use the
 * previous time (and never go backward) and increment the sequence. */
void streamNextID(streamID *last_id, streamID *new_id) {
    uint64_t ms = mstime();
    if (ms > last_id->ms) {
        new_id->ms = ms;
        new_id->seq = 0;
    } else {
        new_id->ms = last_id->ms;
        new_id->seq = last_id->seq+1;
    }
}

/* This is just a wrapper for lpAppend() to directly use a 64 bit integer
 * instead of a string. */
unsigned char *lpAppendInteger(unsigned char *lp, int64_t value) {
    char buf[LONG_STR_SIZE];
    int slen = ll2string(buf,sizeof(buf),value);
    return lpAppend(lp,(unsigned char*)buf,slen);
}

/* This is a wrapper function for lpGet() to directly get an integer value
 * from the listpack (that may store numbers as a string), converting
 * the string if needed. */
int64_t lpGetInteger(unsigned char *ele) {
    int64_t v;
    unsigned char *e = lpGet(ele,&v,NULL);
    if (e == NULL) return v;
    /* The following code path should never be used for how listpacks work:
     * they should always be able to store an int64_t value in integer
     * encoded form. However the implementation may change. */
    int retval = string2ll((char*)e,v,&v);
    serverAssert(retval != 0);
    return v;
}

/* Convert the specified stream entry ID as a 128 bit big endian number, so
 * that the IDs can be sorted lexicographically. */
void streamEncodeID(void *buf, streamID *id) {
    uint64_t e[2];
    e[0] = htonu64(id->ms);
    e[1] = htonu64(id->seq);
    memcpy(buf,e,sizeof(e));
}

/* This is the reverse of streamEncodeID(): the decoded ID will be stored
 * in the 'id' structure passed by reference. The buffer 'buf' must point
 * to a 128 bit big-endian encoded ID. */
void streamDecodeID(void *buf, streamID *id) {
    uint64_t e[2];
    memcpy(e,buf,sizeof(e));
    id->ms = ntohu64(e[0]);
    id->seq = ntohu64(e[1]);
}

/* Adds a new item into the stream 's' having the specified number of
 * field-value pairs as specified in 'numfields' and stored into 'argv'.
 * Returns the new entry ID populating the 'added_id' structure. */
void streamAppendItem(stream *s, robj **argv, int numfields, streamID *added_id) {
    raxIterator ri;
    raxStart(&ri,s->rax);
    raxSeek(&ri,"$",NULL,0);

    size_t lp_bytes = 0;        /* Total bytes in the tail listpack. */
    unsigned char *lp = NULL;   /* Tail listpack pointer. */

    /* Get a reference to the tail node listpack. */
    if (raxNext(&ri)) {
        lp = ri.data;
        lp_bytes = lpBytes(lp);
    }
    raxStop(&ri);

    /* Generate the new entry ID. */
    streamID id;
    streamNextID(&s->last_id,&id);

    /* We have to add the key into the radix tree in lexicographic order,
     * to do so we consider the ID as a single 128 bit number written in
     * big endian, so that the most significant bytes are the first ones. */
    uint64_t rax_key[2];    /* Key in the radix tree containing the listpack.*/
    uint64_t entry_id[2];   /* Entry ID of the new item as 128 bit string. */
    streamEncodeID(entry_id,&id);

    /* Create a new listpack and radix tree node if needed. */
    if (lp == NULL || lp_bytes > STREAM_BYTES_PER_LISTPACK) {
        lp = lpNew();
        rax_key[0] = entry_id[0];
        rax_key[1] = entry_id[1];
        raxInsert(s->rax,(unsigned char*)&rax_key,sizeof(rax_key),lp,NULL);
    } else {
        serverAssert(ri.key_len == sizeof(rax_key));
        memcpy(rax_key,ri.key,sizeof(rax_key));
    }

    /* Populate the listpack with the new entry. We use the following
     * encoding:
     *
     * +--------+----------+-------+-------+-/-+-------+-------+
     * |entry-id|num-fields|field-1|value-1|...|field-N|value-N|
     * +--------+----------+-------+-------+-/-+-------+-------+
     */
    lp = lpAppend(lp,(unsigned char*)entry_id,sizeof(entry_id));
    lp = lpAppendInteger(lp,numfields);
    for (int i = 0; i < numfields; i++) {
        sds field = argv[i*2]->ptr, value = argv[i*2+1]->ptr;
        lp = lpAppend(lp,(unsigned char*)field,sdslen(field));
        lp = lpAppend(lp,(unsigned char*)value,sdslen(value));
    }

    /* Insert back into the tree in order to update the listpack pointer. */
    raxInsert(s->rax,(unsigned char*)&rax_key,sizeof(rax_key),lp,NULL);
    s->length++;
    s->last_id = id;
    if (added_id) *added_id = id;
}

/* Send the specified range to the client 'c'. The range the client will
 * receive is between start and end inclusive, if 'count' is non zero, no more
 * than 'count' elemnets are sent. The 'end' pointer can be NULL to mean that
 * we want all the elements from 'start' till the end of the stream. */
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count) {
    void *arraylen_ptr = addDeferredMultiBulkLength(c);
    size_t arraylen = 0;

    /* Seek the radix tree node that contains our start item. */
    uint64_t key[2];
    uint64_t end_key[2];
    streamEncodeID(key,start);
    if (end) streamEncodeID(end_key,end);
    raxIterator ri;
    raxStart(&ri,s->rax);

    /* Seek the correct node in the radix tree. */
    if (start->ms || start->seq) {
        raxSeek(&ri,"<=",(unsigned char*)key,sizeof(key));
        if (raxEOF(&ri)) raxSeek(&ri,">",(unsigned char*)key,sizeof(key));
    } else {
        raxSeek(&ri,"^",NULL,0);
    }

    /* For every radix tree node, iterate the corresponding listpack,
     * returning elmeents when they are within range. */
    while (raxNext(&ri)) {
        serverAssert(ri.key_len == sizeof(key));
        unsigned char *lp = ri.data;
        unsigned char *lp_ele = lpFirst(lp);
        while(lp_ele) {
            int64_t e_len;
            unsigned char buf[LP_INTBUF_SIZE];
            unsigned char *e = lpGet(lp_ele,&e_len,buf);
            serverAssert(e_len == sizeof(streamID));

            /* Seek next field: number of elements. */
            lp_ele = lpNext(lp,lp_ele);
            if (memcmp(e,key,sizeof(key)) >= 0) { /* If current >= start */
                if (end && memcmp(e,end_key,sizeof(key)) > 0) {
                    break; /* We are already out of range. */
                }
                streamID thisid;
                streamDecodeID(e,&thisid);
                sds replyid = sdscatfmt(sdsempty(),"+%U.%U\r\n",
                              thisid.ms,thisid.seq);

                /* Emit this stream entry in the client output. */
                addReplyMultiBulkLen(c,2);
                addReplySds(c,replyid);
                int64_t numfields = lpGetInteger(lp_ele);
                lp_ele = lpNext(lp,lp_ele);
                addReplyMultiBulkLen(c,numfields*2);
                for (int64_t i = 0; i < numfields; i++) {
                    /* Emit two items (key-value) per iteration. */
                    for (int k = 0; k < 2; k++) {
                        e = lpGet(lp_ele,&e_len,buf);
                        addReplyBulkCBuffer(c,e,e_len);
                        lp_ele = lpNext(lp,lp_ele);
                    }
                }

                arraylen++;
                if (count && count == arraylen) break;
            } else {
                /* If we do not emit, we have to discard. */
                int64_t numfields = lpGetInteger(lp_ele);
                lp_ele = lpNext(lp,lp_ele);
                for (int64_t i = 0; i < numfields*2; i++)
                    lp_ele = lpNext(lp,lp_ele);
            }
        }
        if (count && count == arraylen) break;
    }
    raxStop(&ri);
    setDeferredMultiBulkLength(c,arraylen_ptr,arraylen);
    return arraylen;
}

/* -----------------------------------------------------------------------
 * Stream commands implementation
 * ----------------------------------------------------------------------- */

/* Look the stream at 'key' and return the corresponding stream object.
 * The function creates a key setting it to an empty stream if needed. */
robj *streamTypeLookupWriteOrCreate(client *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);
    if (o == NULL) {
        o = createStreamObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != OBJ_STREAM) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

/* Helper function to convert a string to an unsigned long long value.
 * The function attempts to use the faster string2ll() function inside
 * Redis: if it fails, strtoull() is used instead. The function returns
 * 1 if the conversion happened successfully or 0 if the number is
 * invalid or out of range. */
int string2ull(const char *s, unsigned long long *value) {
    long long ll;
    if (string2ll(s,strlen(s),&ll)) {
        if (ll < 0) return 0; /* Negative values are out of range. */
        *value = ll;
        return 1;
    }
    errno = 0;
    *value = strtoull(s,NULL,10);
    if (errno == EINVAL || errno == ERANGE) return 0; /* strtoull() failed. */
    return 1; /* Conversion done! */
}

/* Parse a stream ID in the format given by clients to Redis, that is
 * <ms>.<seq>, and converts it into a streamID structure. If
 * the specified ID is invalid C_ERR is returned and an error is reported
 * to the client, otherwise C_OK is returned. The ID may be in incomplete
 * form, just stating the milliseconds time part of the stream. In such a case
 * the missing part is set according to the value of 'missing_seq' parameter.
 * The IDs "-" and "+" specify respectively the minimum and maximum IDs
 * that can be represented. */
int streamParseIDOrReply(client *c, robj *o, streamID *id, uint64_t missing_seq) {
    char buf[128];
    if (sdslen(o->ptr) > sizeof(buf)-1) goto invalid;
    memcpy(buf,o->ptr,sdslen(o->ptr)+1);

    /* Handle the "-" and "+" special cases. */
    if (buf[0] == '-' && buf[1] == '\0') {
        id->ms = 0;
        id->seq = 0;
        return C_OK;
    } else if (buf[0] == '+' && buf[1] == '\0') {
        id->ms = UINT64_MAX;
        id->seq = UINT64_MAX;
        return C_OK;
    }

    /* Parse <ms>.<seq> form. */
    char *dot = strchr(buf,'.');
    if (dot) *dot = '\0';
    uint64_t ms, seq;
    if (string2ull(buf,&ms) == 0) goto invalid;
    if (dot && string2ull(dot+1,&seq) == 0) goto invalid;
    if (!dot) seq = missing_seq;
    id->ms = ms;
    id->seq = seq;
    return C_OK;

invalid:
    addReplyError(c,"Invalid stream ID specified as stream command argument");
    return C_ERR;
}

/* XADD key [field value] [field value] ... */
void xaddCommand(client *c) {
    if ((c->argc % 2) == 1) {
        addReplyError(c,"wrong number of arguments for XADD");
        return;
    }

    /* Lookup the stream at key. */
    robj *o;
    stream *s;
    if ((o = streamTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    s = o->ptr;

    /* Append using the low level function and return the ID. */
    streamID id;
    streamAppendItem(s,c->argv+2,(c->argc-2)/2,&id);
    sds reply = sdscatfmt(sdsempty(),"+%U.%U\r\n",id.ms,id.seq);
    addReplySds(c,reply);

    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"xadd",c->argv[1],c->db->id);
    server.dirty++;
}

/* XRANGE key start end [COUNT <n>] */
void xrangeCommand(client *c) {
    robj *o;
    stream *s;
    streamID startid, endid;
    long long count = 0;

    if (streamParseIDOrReply(c,c->argv[2],&startid,0) == C_ERR) return;
    if (streamParseIDOrReply(c,c->argv[3],&endid,UINT64_MAX) == C_ERR) return;

    /* Parse the COUNT option if any. */
    if (c->argc > 4) {
        if (strcasecmp(c->argv[4]->ptr,"COUNT") == 0) {
            if (getLongLongFromObjectOrReply(c,c->argv[5],&count,NULL) != C_OK)
                return;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Return the specified range to the user. */
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,OBJ_STREAM)) return;
    s = o->ptr;
    streamReplyWithRange(c,s,&startid,&endid,count);
}

/* XLEN */
void xlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL
        || checkType(c,o,OBJ_STREAM)) return;
    stream *s = o->ptr;
    addReplyLongLong(c,s->length);
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       [RETRY <milliseconds> <ttl>] STREAMS key_1 ID_1 key_2 ID_2 ...
 *       key_N ID_N */
void xreadCommand(client *c) {
}


