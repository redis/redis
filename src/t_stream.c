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

#include "server.h"
#include "endianconv.h"
#include "stream.h"

/* Every stream item inside the listpack, has a flags field that is used to
 * mark the entry as deleted, or having the same field as the "master"
 * entry at the start of the listpack> */
#define STREAM_ITEM_FLAG_NONE 0             /* No special flags. */
#define STREAM_ITEM_FLAG_DELETED (1<<0)     /* Entry is deleted. Skip it. */
#define STREAM_ITEM_FLAG_SAMEFIELDS (1<<1)  /* Same fields as master entry. */

/* For stream commands that require multiple IDs
 * when the number of IDs is less than 'STREAMID_STATIC_VECTOR_LEN',
 * avoid malloc allocation.*/
#define STREAMID_STATIC_VECTOR_LEN 8

/* Max pre-allocation for listpack. This is done to avoid abuse of a user
 * setting stream_node_max_bytes to a huge number. */
#define STREAM_LISTPACK_MAX_PRE_ALLOCATE 4096

/* Don't let listpacks grow too big, even if the user config allows it.
 * doing so can lead to an overflow (trying to store more than 32bit length
 * into the listpack header), or actually an assertion since lpInsert
 * will return NULL. */
#define STREAM_LISTPACK_MAX_SIZE (1<<30)

void streamFreeCG(streamCG *cg);
void streamFreeNACK(streamNACK *na);
size_t streamReplyWithRangeFromConsumerPEL(client *c, stream *s, streamID *start, streamID *end, size_t count, streamConsumer *consumer);
int streamParseStrictIDOrReply(client *c, robj *o, streamID *id, uint64_t missing_seq);
int streamParseIDOrReply(client *c, robj *o, streamID *id, uint64_t missing_seq);

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
    s->cgroups = NULL; /* Created on demand to save memory when not used. */
    return s;
}

/* Free a stream, including the listpacks stored inside the radix tree. */
void freeStream(stream *s) {
    raxFreeWithCallback(s->rax,(void(*)(void*))lpFree);
    if (s->cgroups)
        raxFreeWithCallback(s->cgroups,(void(*)(void*))streamFreeCG);
    zfree(s);
}

/* Return the length of a stream. */
unsigned long streamLength(const robj *subject) {
    stream *s = subject->ptr;
    return s->length;
}

/* Set 'id' to be its successor stream ID.
 * If 'id' is the maximal possible id, it is wrapped around to 0-0 and a
 * C_ERR is returned. */
int streamIncrID(streamID *id) {
    int ret = C_OK;
    if (id->seq == UINT64_MAX) {
        if (id->ms == UINT64_MAX) {
            /* Special case where 'id' is the last possible streamID... */
            id->ms = id->seq = 0;
            ret = C_ERR;
        } else {
            id->ms++;
            id->seq = 0;
        }
    } else {
        id->seq++;
    }
    return ret;
}

/* Set 'id' to be its predecessor stream ID.
 * If 'id' is the minimal possible id, it remains 0-0 and a C_ERR is
 * returned. */
int streamDecrID(streamID *id) {
    int ret = C_OK;
    if (id->seq == 0) {
        if (id->ms == 0) {
            /* Special case where 'id' is the first possible streamID... */
            id->ms = id->seq = UINT64_MAX;
            ret = C_ERR;
        } else {
            id->ms--;
            id->seq = UINT64_MAX;
        }
    } else {
        id->seq--;
    }
    return ret;
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
        *new_id = *last_id;
        streamIncrID(new_id);
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a Stream object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *streamDup(robj *o) {
    robj *sobj;

    serverAssert(o->type == OBJ_STREAM);

    switch (o->encoding) {
        case OBJ_ENCODING_STREAM:
            sobj = createStreamObject();
            break;
        default:
            serverPanic("Wrong encoding.");
            break;
    }

    stream *s;
    stream *new_s;
    s = o->ptr;
    new_s = sobj->ptr;

    raxIterator ri;
    uint64_t rax_key[2];
    raxStart(&ri, s->rax);
    raxSeek(&ri, "^", NULL, 0);
    size_t lp_bytes = 0;      /* Total bytes in the listpack. */
    unsigned char *lp = NULL; /* listpack pointer. */
    /* Get a reference to the listpack node. */
    while (raxNext(&ri)) {
        lp = ri.data;
        lp_bytes = lpBytes(lp);
        unsigned char *new_lp = zmalloc(lp_bytes);
        memcpy(new_lp, lp, lp_bytes);
        memcpy(rax_key, ri.key, sizeof(rax_key));
        raxInsert(new_s->rax, (unsigned char *)&rax_key, sizeof(rax_key),
                  new_lp, NULL);
    }
    new_s->length = s->length;
    new_s->last_id = s->last_id;
    raxStop(&ri);

    if (s->cgroups == NULL) return sobj;

    /* Consumer Groups */
    raxIterator ri_cgroups;
    raxStart(&ri_cgroups, s->cgroups);
    raxSeek(&ri_cgroups, "^", NULL, 0);
    while (raxNext(&ri_cgroups)) {
        streamCG *cg = ri_cgroups.data;
        streamCG *new_cg = streamCreateCG(new_s, (char *)ri_cgroups.key,
                                          ri_cgroups.key_len, &cg->last_id);

        serverAssert(new_cg != NULL);

        /* Consumer Group PEL */
        raxIterator ri_cg_pel;
        raxStart(&ri_cg_pel,cg->pel);
        raxSeek(&ri_cg_pel,"^",NULL,0);
        while(raxNext(&ri_cg_pel)){
            streamNACK *nack = ri_cg_pel.data;
            streamNACK *new_nack = streamCreateNACK(NULL);
            new_nack->delivery_time = nack->delivery_time;
            new_nack->delivery_count = nack->delivery_count;
            raxInsert(new_cg->pel, ri_cg_pel.key, sizeof(streamID), new_nack, NULL);
        }
        raxStop(&ri_cg_pel);

        /* Consumers */
        raxIterator ri_consumers;
        raxStart(&ri_consumers, cg->consumers);
        raxSeek(&ri_consumers, "^", NULL, 0);
        while (raxNext(&ri_consumers)) {
            streamConsumer *consumer = ri_consumers.data;
            streamConsumer *new_consumer;
            new_consumer = zmalloc(sizeof(*new_consumer));
            new_consumer->name = sdsdup(consumer->name);
            new_consumer->pel = raxNew();
            raxInsert(new_cg->consumers,(unsigned char *)new_consumer->name,
                        sdslen(new_consumer->name), new_consumer, NULL);
            new_consumer->seen_time = consumer->seen_time;

            /* Consumer PEL */
            raxIterator ri_cpel;
            raxStart(&ri_cpel, consumer->pel);
            raxSeek(&ri_cpel, "^", NULL, 0);
            while (raxNext(&ri_cpel)) {
                streamNACK *new_nack = raxFind(new_cg->pel,ri_cpel.key,sizeof(streamID));

                serverAssert(new_nack != raxNotFound);

                new_nack->consumer = new_consumer;
                raxInsert(new_consumer->pel,ri_cpel.key,sizeof(streamID),new_nack,NULL);
            }
            raxStop(&ri_cpel);
        }
        raxStop(&ri_consumers);
    }
    raxStop(&ri_cgroups);
    return sobj;
}

/* This is just a wrapper for lpAppend() to directly use a 64 bit integer
 * instead of a string. */
unsigned char *lpAppendInteger(unsigned char *lp, int64_t value) {
    char buf[LONG_STR_SIZE];
    int slen = ll2string(buf,sizeof(buf),value);
    return lpAppend(lp,(unsigned char*)buf,slen);
}

/* This is just a wrapper for lpReplace() to directly use a 64 bit integer
 * instead of a string to replace the current element. The function returns
 * the new listpack as return value, and also updates the current cursor
 * by updating '*pos'. */
unsigned char *lpReplaceInteger(unsigned char *lp, unsigned char **pos, int64_t value) {
    char buf[LONG_STR_SIZE];
    int slen = ll2string(buf,sizeof(buf),value);
    return lpInsert(lp, (unsigned char*)buf, slen, *pos, LP_REPLACE, pos);
}

/* This is a wrapper function for lpGet() to directly get an integer value
 * from the listpack (that may store numbers as a string), converting
 * the string if needed.
 * The 'valid" argument is an optional output parameter to get an indication
 * if the record was valid, when this parameter is NULL, the function will
 * fail with an assertion. */
static inline int64_t lpGetIntegerIfValid(unsigned char *ele, int *valid) {
    int64_t v;
    unsigned char *e = lpGet(ele,&v,NULL);
    if (e == NULL) {
        if (valid)
            *valid = 1;
        return v;
    }
    /* The following code path should never be used for how listpacks work:
     * they should always be able to store an int64_t value in integer
     * encoded form. However the implementation may change. */
    long long ll;
    int ret = string2ll((char*)e,v,&ll);
    if (valid)
        *valid = ret;
    else
        serverAssert(ret != 0);
    v = ll;
    return v;
}

#define lpGetInteger(ele) lpGetIntegerIfValid(ele, NULL)

/* Get an edge streamID of a given listpack.
 * 'master_id' is an input param, used to build the 'edge_id' output param */
int lpGetEdgeStreamID(unsigned char *lp, int first, streamID *master_id, streamID *edge_id)
{
   if (lp == NULL)
       return 0;

   unsigned char *lp_ele;

   /* We need to seek either the first or the last entry depending
    * on the direction of the iteration. */
   if (first) {
       /* Get the master fields count. */
       lp_ele = lpFirst(lp);        /* Seek items count */
       lp_ele = lpNext(lp, lp_ele); /* Seek deleted count. */
       lp_ele = lpNext(lp, lp_ele); /* Seek num fields. */
       int64_t master_fields_count = lpGetInteger(lp_ele);
       lp_ele = lpNext(lp, lp_ele); /* Seek first field. */

       /* If we are iterating in normal order, skip the master fields
        * to seek the first actual entry. */
       for (int64_t i = 0; i < master_fields_count; i++)
           lp_ele = lpNext(lp, lp_ele);

       /* If we are going forward, skip the previous entry's
        * lp-count field (or in case of the master entry, the zero
        * term field) */
       lp_ele = lpNext(lp, lp_ele);
       if (lp_ele == NULL)
           return 0;
   } else {
       /* If we are iterating in reverse direction, just seek the
        * last part of the last entry in the listpack (that is, the
        * fields count). */
       lp_ele = lpLast(lp);

       /* If we are going backward, read the number of elements this
        * entry is composed of, and jump backward N times to seek
        * its start. */
       int64_t lp_count = lpGetInteger(lp_ele);
       if (lp_count == 0) /* We reached the master entry. */
           return 0;

       while (lp_count--)
           lp_ele = lpPrev(lp, lp_ele);
   }

   lp_ele = lpNext(lp, lp_ele); /* Seek ID (lp_ele currently points to 'flags'). */

   /* Get the ID: it is encoded as difference between the master
    * ID and this entry ID. */
   streamID id = *master_id;
   id.ms += lpGetInteger(lp_ele);
   lp_ele = lpNext(lp, lp_ele);
   id.seq += lpGetInteger(lp_ele);
   *edge_id = id;
   return 1;
}

/* Debugging function to log the full content of a listpack. Useful
 * for development and debugging. */
void streamLogListpackContent(unsigned char *lp) {
    unsigned char *p = lpFirst(lp);
    while(p) {
        unsigned char buf[LP_INTBUF_SIZE];
        int64_t v;
        unsigned char *ele = lpGet(p,&v,buf);
        serverLog(LL_WARNING,"- [%d] '%.*s'", (int)v, (int)v, ele);
        p = lpNext(lp,p);
    }
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

/* Compare two stream IDs. Return -1 if a < b, 0 if a == b, 1 if a > b. */
int streamCompareID(streamID *a, streamID *b) {
    if (a->ms > b->ms) return 1;
    else if (a->ms < b->ms) return -1;
    /* The ms part is the same. Check the sequence part. */
    else if (a->seq > b->seq) return 1;
    else if (a->seq < b->seq) return -1;
    /* Everything is the same: IDs are equal. */
    return 0;
}

void streamGetEdgeID(stream *s, int first, streamID *edge_id)
{
    raxIterator ri;
    raxStart(&ri, s->rax);
    int empty;
    if (first) {
        raxSeek(&ri, "^", NULL, 0);
        empty = !raxNext(&ri);
    } else {
        raxSeek(&ri, "$", NULL, 0);
        empty = !raxPrev(&ri);
    }

    if (empty) {
        /* Stream is empty, mark edge ID as lowest/highest possible. */
        edge_id->ms = first ? UINT64_MAX : 0;
        edge_id->seq = first ? UINT64_MAX : 0;
        raxStop(&ri);
        return;
    }

    unsigned char *lp = ri.data;

    /* Read the master ID from the radix tree key. */
    streamID master_id;
    streamDecodeID(ri.key, &master_id);

    /* Construct edge ID. */
    lpGetEdgeStreamID(lp, first, &master_id, edge_id);

    raxStop(&ri);
}

/* Adds a new item into the stream 's' having the specified number of
 * field-value pairs as specified in 'numfields' and stored into 'argv'.
 * Returns the new entry ID populating the 'added_id' structure.
 *
 * If 'use_id' is not NULL, the ID is not auto-generated by the function,
 * but instead the passed ID is used to add the new entry. In this case
 * adding the entry may fail as specified later in this comment.
 *
 * The function returns C_OK if the item was added, this is always true
 * if the ID was generated by the function. However the function may return
 * C_ERR in several cases:
 * 1. If an ID was given via 'use_id', but adding it failed since the
 *    current top ID is greater or equal. errno will be set to EDOM.
 * 2. If a size of a single element or the sum of the elements is too big to
 *    be stored into the stream. errno will be set to ERANGE. */
int streamAppendItem(stream *s, robj **argv, int64_t numfields, streamID *added_id, streamID *use_id) {

    /* Generate the new entry ID. */
    streamID id;
    if (use_id)
        id = *use_id;
    else
        streamNextID(&s->last_id,&id);

    /* Check that the new ID is greater than the last entry ID
     * or return an error. Automatically generated IDs might
     * overflow (and wrap-around) when incrementing the sequence
       part. */
    if (streamCompareID(&id,&s->last_id) <= 0) {
        errno = EDOM;
        return C_ERR;
    }

    /* Avoid overflow when trying to add an element to the stream (listpack
     * can only host up to 32bit length sttrings, and also a total listpack size
     * can't be bigger than 32bit length. */
    size_t totelelen = 0;
    for (int64_t i = 0; i < numfields*2; i++) {
        sds ele = argv[i]->ptr;
        totelelen += sdslen(ele);
    }
    if (totelelen > STREAM_LISTPACK_MAX_SIZE) {
        errno = ERANGE;
        return C_ERR;
    }

    /* Add the new entry. */
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

    /* We have to add the key into the radix tree in lexicographic order,
     * to do so we consider the ID as a single 128 bit number written in
     * big endian, so that the most significant bytes are the first ones. */
    uint64_t rax_key[2];    /* Key in the radix tree containing the listpack.*/
    streamID master_id;     /* ID of the master entry in the listpack. */

    /* Create a new listpack and radix tree node if needed. Note that when
     * a new listpack is created, we populate it with a "master entry". This
     * is just a set of fields that is taken as references in order to compress
     * the stream entries that we'll add inside the listpack.
     *
     * Note that while we use the first added entry fields to create
     * the master entry, the first added entry is NOT represented in the master
     * entry, which is a stand alone object. But of course, the first entry
     * will compress well because it's used as reference.
     *
     * The master entry is composed like in the following example:
     *
     * +-------+---------+------------+---------+--/--+---------+---------+-+
     * | count | deleted | num-fields | field_1 | field_2 | ... | field_N |0|
     * +-------+---------+------------+---------+--/--+---------+---------+-+
     *
     * count and deleted just represent respectively the total number of
     * entries inside the listpack that are valid, and marked as deleted
     * (deleted flag in the entry flags set). So the total number of items
     * actually inside the listpack (both deleted and not) is count+deleted.
     *
     * The real entries will be encoded with an ID that is just the
     * millisecond and sequence difference compared to the key stored at
     * the radix tree node containing the listpack (delta encoding), and
     * if the fields of the entry are the same as the master entry fields, the
     * entry flags will specify this fact and the entry fields and number
     * of fields will be omitted (see later in the code of this function).
     *
     * The "0" entry at the end is the same as the 'lp-count' entry in the
     * regular stream entries (see below), and marks the fact that there are
     * no more entries, when we scan the stream from right to left. */

    /* First of all, check if we can append to the current macro node or
     * if we need to switch to the next one. 'lp' will be set to NULL if
     * the current node is full. */
    if (lp != NULL) {
        size_t node_max_bytes = server.stream_node_max_bytes;
        if (node_max_bytes == 0 || node_max_bytes > STREAM_LISTPACK_MAX_SIZE)
            node_max_bytes = STREAM_LISTPACK_MAX_SIZE;
        if (lp_bytes + totelelen >= node_max_bytes) {
            lp = NULL;
        } else if (server.stream_node_max_entries) {
            unsigned char *lp_ele = lpFirst(lp);
            /* Count both live entries and deleted ones. */
            int64_t count = lpGetInteger(lp_ele) + lpGetInteger(lpNext(lp,lp_ele));
            if (count >= server.stream_node_max_entries) {
                /* Shrink extra pre-allocated memory */
                lp = lpShrinkToFit(lp);
                if (ri.data != lp)
                    raxInsert(s->rax,ri.key,ri.key_len,lp,NULL);
                lp = NULL;
            }
        }
    }

    int flags = STREAM_ITEM_FLAG_NONE;
    if (lp == NULL) {
        master_id = id;
        streamEncodeID(rax_key,&id);
        /* Create the listpack having the master entry ID and fields.
         * Pre-allocate some bytes when creating listpack to avoid realloc on
         * every XADD. Since listpack.c uses malloc_size, it'll grow in steps,
         * and won't realloc on every XADD.
         * When listpack reaches max number of entries, we'll shrink the
         * allocation to fit the data. */
        size_t prealloc = STREAM_LISTPACK_MAX_PRE_ALLOCATE;
        if (server.stream_node_max_bytes > 0 && server.stream_node_max_bytes < prealloc) {
            prealloc = server.stream_node_max_bytes;
        }
        lp = lpNew(prealloc);
        lp = lpAppendInteger(lp,1); /* One item, the one we are adding. */
        lp = lpAppendInteger(lp,0); /* Zero deleted so far. */
        lp = lpAppendInteger(lp,numfields);
        for (int64_t i = 0; i < numfields; i++) {
            sds field = argv[i*2]->ptr;
            lp = lpAppend(lp,(unsigned char*)field,sdslen(field));
        }
        lp = lpAppendInteger(lp,0); /* Master entry zero terminator. */
        raxInsert(s->rax,(unsigned char*)&rax_key,sizeof(rax_key),lp,NULL);
        /* The first entry we insert, has obviously the same fields of the
         * master entry. */
        flags |= STREAM_ITEM_FLAG_SAMEFIELDS;
    } else {
        serverAssert(ri.key_len == sizeof(rax_key));
        memcpy(rax_key,ri.key,sizeof(rax_key));

        /* Read the master ID from the radix tree key. */
        streamDecodeID(rax_key,&master_id);
        unsigned char *lp_ele = lpFirst(lp);

        /* Update count and skip the deleted fields. */
        int64_t count = lpGetInteger(lp_ele);
        lp = lpReplaceInteger(lp,&lp_ele,count+1);
        lp_ele = lpNext(lp,lp_ele); /* seek deleted. */
        lp_ele = lpNext(lp,lp_ele); /* seek master entry num fields. */

        /* Check if the entry we are adding, have the same fields
         * as the master entry. */
        int64_t master_fields_count = lpGetInteger(lp_ele);
        lp_ele = lpNext(lp,lp_ele);
        if (numfields == master_fields_count) {
            int64_t i;
            for (i = 0; i < master_fields_count; i++) {
                sds field = argv[i*2]->ptr;
                int64_t e_len;
                unsigned char buf[LP_INTBUF_SIZE];
                unsigned char *e = lpGet(lp_ele,&e_len,buf);
                /* Stop if there is a mismatch. */
                if (sdslen(field) != (size_t)e_len ||
                    memcmp(e,field,e_len) != 0) break;
                lp_ele = lpNext(lp,lp_ele);
            }
            /* All fields are the same! We can compress the field names
             * setting a single bit in the flags. */
            if (i == master_fields_count) flags |= STREAM_ITEM_FLAG_SAMEFIELDS;
        }
    }

    /* Populate the listpack with the new entry. We use the following
     * encoding:
     *
     * +-----+--------+----------+-------+-------+-/-+-------+-------+--------+
     * |flags|entry-id|num-fields|field-1|value-1|...|field-N|value-N|lp-count|
     * +-----+--------+----------+-------+-------+-/-+-------+-------+--------+
     *
     * However if the SAMEFIELD flag is set, we have just to populate
     * the entry with the values, so it becomes:
     *
     * +-----+--------+-------+-/-+-------+--------+
     * |flags|entry-id|value-1|...|value-N|lp-count|
     * +-----+--------+-------+-/-+-------+--------+
     *
     * The entry-id field is actually two separated fields: the ms
     * and seq difference compared to the master entry.
     *
     * The lp-count field is a number that states the number of listpack pieces
     * that compose the entry, so that it's possible to travel the entry
     * in reverse order: we can just start from the end of the listpack, read
     * the entry, and jump back N times to seek the "flags" field to read
     * the stream full entry. */
    lp = lpAppendInteger(lp,flags);
    lp = lpAppendInteger(lp,id.ms - master_id.ms);
    lp = lpAppendInteger(lp,id.seq - master_id.seq);
    if (!(flags & STREAM_ITEM_FLAG_SAMEFIELDS))
        lp = lpAppendInteger(lp,numfields);
    for (int64_t i = 0; i < numfields; i++) {
        sds field = argv[i*2]->ptr, value = argv[i*2+1]->ptr;
        if (!(flags & STREAM_ITEM_FLAG_SAMEFIELDS))
            lp = lpAppend(lp,(unsigned char*)field,sdslen(field));
        lp = lpAppend(lp,(unsigned char*)value,sdslen(value));
    }
    /* Compute and store the lp-count field. */
    int64_t lp_count = numfields;
    lp_count += 3; /* Add the 3 fixed fields flags + ms-diff + seq-diff. */
    if (!(flags & STREAM_ITEM_FLAG_SAMEFIELDS)) {
        /* If the item is not compressed, it also has the fields other than
         * the values, and an additional num-fileds field. */
        lp_count += numfields+1;
    }
    lp = lpAppendInteger(lp,lp_count);

    /* Insert back into the tree in order to update the listpack pointer. */
    if (ri.data != lp)
        raxInsert(s->rax,(unsigned char*)&rax_key,sizeof(rax_key),lp,NULL);
    s->length++;
    s->last_id = id;
    if (added_id) *added_id = id;
    return C_OK;
}

typedef struct {
    /* XADD options */
    streamID id; /* User-provided ID, for XADD only. */
    int id_given; /* Was an ID different than "*" specified? for XADD only. */
    int no_mkstream; /* if set to 1 do not create new stream */

    /* XADD + XTRIM common options */
    int trim_strategy; /* TRIM_STRATEGY_* */
    int trim_strategy_arg_idx; /* Index of the count in MAXLEN/MINID, for rewriting. */
    int approx_trim; /* If 1 only delete whole radix tree nodes, so
                      * the trim argument is not applied verbatim. */
    long long limit; /* Maximum amount of entries to trim. If 0, no limitation
                      * on the amount of trimming work is enforced. */
    /* TRIM_STRATEGY_MAXLEN options */
    long long maxlen; /* After trimming, leave stream at this length . */
    /* TRIM_STRATEGY_MINID options */
    streamID minid; /* Trim by ID (No stream entries with ID < 'minid' will remain) */
} streamAddTrimArgs;

#define TRIM_STRATEGY_NONE 0
#define TRIM_STRATEGY_MAXLEN 1
#define TRIM_STRATEGY_MINID 2

/* Trim the stream 's' according to args->trim_strategy, and return the
 * number of elements removed from the stream. The 'approx' option, if non-zero,
 * specifies that the trimming must be performed in a approximated way in
 * order to maximize performances. This means that the stream may contain
 * entries with IDs < 'id' in case of MINID (or more elements than 'maxlen'
 * in case of MAXLEN), and elements are only removed if we can remove
 * a *whole* node of the radix tree. The elements are removed from the head
 * of the stream (older elements).
 *
 * The function may return zero if:
 *
 * 1) The minimal entry ID of the stream is already < 'id' (MINID); or
 * 2) The stream is already shorter or equal to the specified max length (MAXLEN); or
 * 3) The 'approx' option is true and the head node did not have enough elements
 *    to be deleted.
 *
 * args->limit is the maximum number of entries to delete. The purpose is to
 * prevent this function from taking to long.
 * If 'limit' is 0 then we do not limit the number of deleted entries.
 * Much like the 'approx', if 'limit' is smaller than the number of entries
 * that should be trimmed, there is a chance we will still have entries with
 * IDs < 'id' (or number of elements >= maxlen in case of MAXLEN).
 */
int64_t streamTrim(stream *s, streamAddTrimArgs *args) {
    size_t maxlen = args->maxlen;
    streamID *id = &args->minid;
    int approx = args->approx_trim;
    int64_t limit = args->limit;
    int trim_strategy = args->trim_strategy;

    if (trim_strategy == TRIM_STRATEGY_NONE)
        return 0;

    raxIterator ri;
    raxStart(&ri,s->rax);
    raxSeek(&ri,"^",NULL,0);

    int64_t deleted = 0;
    while (raxNext(&ri)) {
        if (trim_strategy == TRIM_STRATEGY_MAXLEN && s->length <= maxlen)
            break;

        unsigned char *lp = ri.data, *p = lpFirst(lp);
        int64_t entries = lpGetInteger(p);

        /* Check if we exceeded the amount of work we could do */
        if (limit && (deleted + entries) > limit)
            break;

        /* Check if we can remove the whole node. */
        int remove_node;
        streamID master_id = {0}; /* For MINID */
        if (trim_strategy == TRIM_STRATEGY_MAXLEN) {
            remove_node = s->length - entries >= maxlen;
        } else {
            /* Read the master ID from the radix tree key. */
            streamDecodeID(ri.key, &master_id);

            /* Read last ID. */
            streamID last_id;
            lpGetEdgeStreamID(lp, 0, &master_id, &last_id);

            /* We can remove the entire node id its last ID < 'id' */
            remove_node = streamCompareID(&last_id, id) < 0;
        }

        if (remove_node) {
            lpFree(lp);
            raxRemove(s->rax,ri.key,ri.key_len,NULL);
            raxSeek(&ri,">=",ri.key,ri.key_len);
            s->length -= entries;
            deleted += entries;
            continue;
        }

        /* If we cannot remove a whole element, and approx is true,
         * stop here. */
        if (approx) break;

        /* Now we have to trim entries from within 'lp' */
        int64_t deleted_from_lp = 0;

        p = lpNext(lp, p); /* Skip deleted field. */
        p = lpNext(lp, p); /* Skip num-of-fields in the master entry. */

        /* Skip all the master fields. */
        int64_t master_fields_count = lpGetInteger(p);
        p = lpNext(lp,p); /* Skip the first field. */
        for (int64_t j = 0; j < master_fields_count; j++)
            p = lpNext(lp,p); /* Skip all master fields. */
        p = lpNext(lp,p); /* Skip the zero master entry terminator. */

        /* 'p' is now pointing to the first entry inside the listpack.
         * We have to run entry after entry, marking entries as deleted
         * if they are already not deleted. */
        while (p) {
            /* We keep a copy of p (which point to flags part) in order to
             * update it after (and if) we actually remove the entry */
            unsigned char *pcopy = p;

            int flags = lpGetInteger(p);
            p = lpNext(lp, p); /* Skip flags. */
            int to_skip;

            int ms_delta = lpGetInteger(p);
            p = lpNext(lp, p); /* Skip ID ms delta */
            int seq_delta = lpGetInteger(p);
            p = lpNext(lp, p); /* Skip ID seq delta */

            streamID currid = {0}; /* For MINID */
            if (trim_strategy == TRIM_STRATEGY_MINID) {
                currid.ms = master_id.ms + ms_delta;
                currid.seq = master_id.seq + seq_delta;
            }

            int stop;
            if (trim_strategy == TRIM_STRATEGY_MAXLEN) {
                stop = s->length <= maxlen;
            } else {
                /* Following IDs will definitely be greater because the rax
                 * tree is sorted, no point of continuing. */
                stop = streamCompareID(&currid, id) >= 0;
            }
            if (stop)
                break;

            if (flags & STREAM_ITEM_FLAG_SAMEFIELDS) {
                to_skip = master_fields_count;
            } else {
                to_skip = lpGetInteger(p); /* Get num-fields. */
                p = lpNext(lp,p); /* Skip num-fields. */
                to_skip *= 2; /* Fields and values. */
            }

            while(to_skip--) p = lpNext(lp,p); /* Skip the whole entry. */
            p = lpNext(lp,p); /* Skip the final lp-count field. */

            /* Mark the entry as deleted. */
            if (!(flags & STREAM_ITEM_FLAG_DELETED)) {
                intptr_t delta = p - lp;
                flags |= STREAM_ITEM_FLAG_DELETED;
                lp = lpReplaceInteger(lp, &pcopy, flags);
                deleted_from_lp++;
                s->length--;
                p = lp + delta;
            }
        }
        deleted += deleted_from_lp;

        /* Now we the entries/deleted counters. */
        p = lpFirst(lp);
        lp = lpReplaceInteger(lp,&p,entries-deleted_from_lp);
        p = lpNext(lp,p); /* Skip deleted field. */
        int64_t marked_deleted = lpGetInteger(p);
        lp = lpReplaceInteger(lp,&p,marked_deleted+deleted_from_lp);
        p = lpNext(lp,p); /* Skip num-of-fields in the master entry. */

        /* Here we should perform garbage collection in case at this point
         * there are too many entries deleted inside the listpack. */
        entries -= deleted_from_lp;
        marked_deleted += deleted_from_lp;
        if (entries + marked_deleted > 10 && marked_deleted > entries/2) {
            /* TODO: perform a garbage collection. */
        }

        /* Update the listpack with the new pointer. */
        raxInsert(s->rax,ri.key,ri.key_len,lp,NULL);

        break; /* If we are here, there was enough to delete in the current
                  node, so no need to go to the next node. */
    }

    raxStop(&ri);
    return deleted;
}

/* Trims a stream by length. Returns the number of deleted items. */
int64_t streamTrimByLength(stream *s, long long maxlen, int approx) {
    streamAddTrimArgs args = {
        .trim_strategy = TRIM_STRATEGY_MAXLEN,
        .approx_trim = approx,
        .limit = approx ? 100 * server.stream_node_max_entries : 0,
        .maxlen = maxlen
    };
    return streamTrim(s, &args);
}

/* Trims a stream by minimum ID. Returns the number of deleted items. */
int64_t streamTrimByID(stream *s, streamID minid, int approx) {
    streamAddTrimArgs args = {
        .trim_strategy = TRIM_STRATEGY_MINID,
        .approx_trim = approx,
        .limit = approx ? 100 * server.stream_node_max_entries : 0,
        .minid = minid
    };
    return streamTrim(s, &args);
}

/* Parse the arguments of XADD/XTRIM.
 *
 * See streamAddTrimArgs for more details about the arguments handled.
 *
 * This function returns the position of the ID argument (relevant only to XADD).
 * On error -1 is returned and a reply is sent. */
static int streamParseAddOrTrimArgsOrReply(client *c, streamAddTrimArgs *args, int xadd) {
    /* Initialize arguments to defaults */
    memset(args, 0, sizeof(*args));

    /* Parse options. */
    int i = 2; /* This is the first argument position where we could
                  find an option, or the ID. */
    int limit_given = 0;
    for (; i < c->argc; i++) {
        int moreargs = (c->argc-1) - i; /* Number of additional arguments. */
        char *opt = c->argv[i]->ptr;
        if (xadd && opt[0] == '*' && opt[1] == '\0') {
            /* This is just a fast path for the common case of auto-ID
             * creation. */
            break;
        } else if (!strcasecmp(opt,"maxlen") && moreargs) {
            if (args->trim_strategy != TRIM_STRATEGY_NONE) {
                addReplyError(c,"syntax error, MAXLEN and MINID options at the same time are not compatible");
                return -1;
            }
            args->approx_trim = 0;
            char *next = c->argv[i+1]->ptr;
            /* Check for the form MAXLEN ~ <count>. */
            if (moreargs >= 2 && next[0] == '~' && next[1] == '\0') {
                args->approx_trim = 1;
                i++;
            } else if (moreargs >= 2 && next[0] == '=' && next[1] == '\0') {
                i++;
            }
            if (getLongLongFromObjectOrReply(c,c->argv[i+1],&args->maxlen,NULL)
                != C_OK) return -1;

            if (args->maxlen < 0) {
                addReplyError(c,"The MAXLEN argument must be >= 0.");
                return -1;
            }
            i++;
            args->trim_strategy = TRIM_STRATEGY_MAXLEN;
            args->trim_strategy_arg_idx = i;
        } else if (!strcasecmp(opt,"minid") && moreargs) {
            if (args->trim_strategy != TRIM_STRATEGY_NONE) {
                addReplyError(c,"syntax error, MAXLEN and MINID options at the same time are not compatible");
                return -1;
            }
            args->approx_trim = 0;
            char *next = c->argv[i+1]->ptr;
            /* Check for the form MINID ~ <id>|<age>. */
            if (moreargs >= 2 && next[0] == '~' && next[1] == '\0') {
                args->approx_trim = 1;
                i++;
            } else if (moreargs >= 2 && next[0] == '=' && next[1] == '\0') {
                i++;
            }

            if (streamParseStrictIDOrReply(c,c->argv[i+1],&args->minid,0) != C_OK)
                return -1;
            
            i++;
            args->trim_strategy = TRIM_STRATEGY_MINID;
            args->trim_strategy_arg_idx = i;
        } else if (!strcasecmp(opt,"limit") && moreargs) {
            /* Note about LIMIT: If it was not provided by the caller we set
             * it to 100*server.stream_node_max_entries, and that's to prevent the
             * trimming from taking too long, on the expense of not deleting entries
             * that should be trimmed.
             * If user wanted exact trimming (i.e. no '~') we never limit the number
             * of trimmed entries */
            if (getLongLongFromObjectOrReply(c,c->argv[i+1],&args->limit,NULL) != C_OK)
                return -1;

            if (args->limit < 0) {
                addReplyError(c,"The LIMIT argument must be >= 0.");
                return -1;
            }
            limit_given = 1;
            i++;
        } else if (xadd && !strcasecmp(opt,"nomkstream")) {
            args->no_mkstream = 1;
        } else if (xadd) {
            /* If we are here is a syntax error or a valid ID. */
            if (streamParseStrictIDOrReply(c,c->argv[i],&args->id,0) != C_OK)
                return -1;
            args->id_given = 1;
            break;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return -1;
        }
    }

    if (args->limit && args->trim_strategy == TRIM_STRATEGY_NONE) {
        addReplyError(c,"syntax error, LIMIT cannot be used without specifying a trimming strategy");
        return -1;
    }

    if (!xadd && args->trim_strategy == TRIM_STRATEGY_NONE) {
        addReplyError(c,"syntax error, XTRIM must be called with a trimming strategy");
        return -1;
    }

    if (c == server.master || c->id == CLIENT_ID_AOF) {
        /* If command cam from master or from AOF we must not enforce maxnodes
         * (The maxlen/minid argument was re-written to make sure there's no
         * inconsistency). */
        args->limit = 0;
    } else {
        /* We need to set the limit (only if we got '~') */
        if (limit_given) {
            if (!args->approx_trim) {
                /* LIMIT was provided without ~ */
                addReplyError(c,"syntax error, LIMIT cannot be used without the special ~ option");
                return -1;
            }
        } else {
            /* User didn't provide LIMIT, we must set it. */

            if (args->approx_trim) {
                /* In order to prevent from trimming to do too much work and cause
                 * latency spikes we limit the amount of work it can do */
                args->limit = 100 * server.stream_node_max_entries; /* Maximum 100 rax nodes. */
            } else {
                /* No LIMIT for exact trimming */
                args->limit = 0;
            }
        }
    }

    return i;
}

/* Initialize the stream iterator, so that we can call iterating functions
 * to get the next items. This requires a corresponding streamIteratorStop()
 * at the end. The 'rev' parameter controls the direction. If it's zero the
 * iteration is from the start to the end element (inclusive), otherwise
 * if rev is non-zero, the iteration is reversed.
 *
 * Once the iterator is initialized, we iterate like this:
 *
 *  streamIterator myiterator;
 *  streamIteratorStart(&myiterator,...);
 *  int64_t numfields;
 *  while(streamIteratorGetID(&myiterator,&ID,&numfields)) {
 *      while(numfields--) {
 *          unsigned char *key, *value;
 *          size_t key_len, value_len;
 *          streamIteratorGetField(&myiterator,&key,&value,&key_len,&value_len);
 *
 *          ... do what you want with key and value ...
 *      }
 *  }
 *  streamIteratorStop(&myiterator); */
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end, int rev) {
    /* Initialize the iterator and translates the iteration start/stop
     * elements into a 128 big big-endian number. */
    if (start) {
        streamEncodeID(si->start_key,start);
    } else {
        si->start_key[0] = 0;
        si->start_key[1] = 0;
    }

    if (end) {
        streamEncodeID(si->end_key,end);
    } else {
        si->end_key[0] = UINT64_MAX;
        si->end_key[1] = UINT64_MAX;
    }

    /* Seek the correct node in the radix tree. */
    raxStart(&si->ri,s->rax);
    if (!rev) {
        if (start && (start->ms || start->seq)) {
            raxSeek(&si->ri,"<=",(unsigned char*)si->start_key,
                    sizeof(si->start_key));
            if (raxEOF(&si->ri)) raxSeek(&si->ri,"^",NULL,0);
        } else {
            raxSeek(&si->ri,"^",NULL,0);
        }
    } else {
        if (end && (end->ms || end->seq)) {
            raxSeek(&si->ri,"<=",(unsigned char*)si->end_key,
                    sizeof(si->end_key));
            if (raxEOF(&si->ri)) raxSeek(&si->ri,"$",NULL,0);
        } else {
            raxSeek(&si->ri,"$",NULL,0);
        }
    }
    si->stream = s;
    si->lp = NULL; /* There is no current listpack right now. */
    si->lp_ele = NULL; /* Current listpack cursor. */
    si->rev = rev;  /* Direction, if non-zero reversed, from end to start. */
}

/* Return 1 and store the current item ID at 'id' if there are still
 * elements within the iteration range, otherwise return 0 in order to
 * signal the iteration terminated. */
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields) {
    while(1) { /* Will stop when element > stop_key or end of radix tree. */
        /* If the current listpack is set to NULL, this is the start of the
         * iteration or the previous listpack was completely iterated.
         * Go to the next node. */
        if (si->lp == NULL || si->lp_ele == NULL) {
            if (!si->rev && !raxNext(&si->ri)) return 0;
            else if (si->rev && !raxPrev(&si->ri)) return 0;
            serverAssert(si->ri.key_len == sizeof(streamID));
            /* Get the master ID. */
            streamDecodeID(si->ri.key,&si->master_id);
            /* Get the master fields count. */
            si->lp = si->ri.data;
            si->lp_ele = lpFirst(si->lp);           /* Seek items count */
            si->lp_ele = lpNext(si->lp,si->lp_ele); /* Seek deleted count. */
            si->lp_ele = lpNext(si->lp,si->lp_ele); /* Seek num fields. */
            si->master_fields_count = lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele); /* Seek first field. */
            si->master_fields_start = si->lp_ele;
            /* We are now pointing to the first field of the master entry.
             * We need to seek either the first or the last entry depending
             * on the direction of the iteration. */
            if (!si->rev) {
                /* If we are iterating in normal order, skip the master fields
                 * to seek the first actual entry. */
                for (uint64_t i = 0; i < si->master_fields_count; i++)
                    si->lp_ele = lpNext(si->lp,si->lp_ele);
            } else {
                /* If we are iterating in reverse direction, just seek the
                 * last part of the last entry in the listpack (that is, the
                 * fields count). */
                si->lp_ele = lpLast(si->lp);
            }
        } else if (si->rev) {
            /* If we are iterating in the reverse order, and this is not
             * the first entry emitted for this listpack, then we already
             * emitted the current entry, and have to go back to the previous
             * one. */
            int lp_count = lpGetInteger(si->lp_ele);
            while(lp_count--) si->lp_ele = lpPrev(si->lp,si->lp_ele);
            /* Seek lp-count of prev entry. */
            si->lp_ele = lpPrev(si->lp,si->lp_ele);
        }

        /* For every radix tree node, iterate the corresponding listpack,
         * returning elements when they are within range. */
        while(1) {
            if (!si->rev) {
                /* If we are going forward, skip the previous entry
                 * lp-count field (or in case of the master entry, the zero
                 * term field) */
                si->lp_ele = lpNext(si->lp,si->lp_ele);
                if (si->lp_ele == NULL) break;
            } else {
                /* If we are going backward, read the number of elements this
                 * entry is composed of, and jump backward N times to seek
                 * its start. */
                int64_t lp_count = lpGetInteger(si->lp_ele);
                if (lp_count == 0) { /* We reached the master entry. */
                    si->lp = NULL;
                    si->lp_ele = NULL;
                    break;
                }
                while(lp_count--) si->lp_ele = lpPrev(si->lp,si->lp_ele);
            }

            /* Get the flags entry. */
            si->lp_flags = si->lp_ele;
            int flags = lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele); /* Seek ID. */

            /* Get the ID: it is encoded as difference between the master
             * ID and this entry ID. */
            *id = si->master_id;
            id->ms += lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele);
            id->seq += lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele);
            unsigned char buf[sizeof(streamID)];
            streamEncodeID(buf,id);

            /* The number of entries is here or not depending on the
             * flags. */
            if (flags & STREAM_ITEM_FLAG_SAMEFIELDS) {
                *numfields = si->master_fields_count;
            } else {
                *numfields = lpGetInteger(si->lp_ele);
                si->lp_ele = lpNext(si->lp,si->lp_ele);
            }
            serverAssert(*numfields>=0);

            /* If current >= start, and the entry is not marked as
             * deleted, emit it. */
            if (!si->rev) {
                if (memcmp(buf,si->start_key,sizeof(streamID)) >= 0 &&
                    !(flags & STREAM_ITEM_FLAG_DELETED))
                {
                    if (memcmp(buf,si->end_key,sizeof(streamID)) > 0)
                        return 0; /* We are already out of range. */
                    si->entry_flags = flags;
                    if (flags & STREAM_ITEM_FLAG_SAMEFIELDS)
                        si->master_fields_ptr = si->master_fields_start;
                    return 1; /* Valid item returned. */
                }
            } else {
                if (memcmp(buf,si->end_key,sizeof(streamID)) <= 0 &&
                    !(flags & STREAM_ITEM_FLAG_DELETED))
                {
                    if (memcmp(buf,si->start_key,sizeof(streamID)) < 0)
                        return 0; /* We are already out of range. */
                    si->entry_flags = flags;
                    if (flags & STREAM_ITEM_FLAG_SAMEFIELDS)
                        si->master_fields_ptr = si->master_fields_start;
                    return 1; /* Valid item returned. */
                }
            }

            /* If we do not emit, we have to discard if we are going
             * forward, or seek the previous entry if we are going
             * backward. */
            if (!si->rev) {
                int64_t to_discard = (flags & STREAM_ITEM_FLAG_SAMEFIELDS) ?
                                      *numfields : *numfields*2;
                for (int64_t i = 0; i < to_discard; i++)
                    si->lp_ele = lpNext(si->lp,si->lp_ele);
            } else {
                int64_t prev_times = 4; /* flag + id ms + id seq + one more to
                                           go back to the previous entry "count"
                                           field. */
                /* If the entry was not flagged SAMEFIELD we also read the
                 * number of fields, so go back one more. */
                if (!(flags & STREAM_ITEM_FLAG_SAMEFIELDS)) prev_times++;
                while(prev_times--) si->lp_ele = lpPrev(si->lp,si->lp_ele);
            }
        }

        /* End of listpack reached. Try the next/prev radix tree node. */
    }
}

/* Get the field and value of the current item we are iterating. This should
 * be called immediately after streamIteratorGetID(), and for each field
 * according to the number of fields returned by streamIteratorGetID().
 * The function populates the field and value pointers and the corresponding
 * lengths by reference, that are valid until the next iterator call, assuming
 * no one touches the stream meanwhile. */
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen) {
    if (si->entry_flags & STREAM_ITEM_FLAG_SAMEFIELDS) {
        *fieldptr = lpGet(si->master_fields_ptr,fieldlen,si->field_buf);
        si->master_fields_ptr = lpNext(si->lp,si->master_fields_ptr);
    } else {
        *fieldptr = lpGet(si->lp_ele,fieldlen,si->field_buf);
        si->lp_ele = lpNext(si->lp,si->lp_ele);
    }
    *valueptr = lpGet(si->lp_ele,valuelen,si->value_buf);
    si->lp_ele = lpNext(si->lp,si->lp_ele);
}

/* Remove the current entry from the stream: can be called after the
 * GetID() API or after any GetField() call, however we need to iterate
 * a valid entry while calling this function. Moreover the function
 * requires the entry ID we are currently iterating, that was previously
 * returned by GetID().
 *
 * Note that after calling this function, next calls to GetField() can't
 * be performed: the entry is now deleted. Instead the iterator will
 * automatically re-seek to the next entry, so the caller should continue
 * with GetID(). */
void streamIteratorRemoveEntry(streamIterator *si, streamID *current) {
    unsigned char *lp = si->lp;
    int64_t aux;

    /* We do not really delete the entry here. Instead we mark it as
     * deleted flagging it, and also incrementing the count of the
     * deleted entries in the listpack header.
     *
     * We start flagging: */
    int flags = lpGetInteger(si->lp_flags);
    flags |= STREAM_ITEM_FLAG_DELETED;
    lp = lpReplaceInteger(lp,&si->lp_flags,flags);

    /* Change the valid/deleted entries count in the master entry. */
    unsigned char *p = lpFirst(lp);
    aux = lpGetInteger(p);

    if (aux == 1) {
        /* If this is the last element in the listpack, we can remove the whole
         * node. */
        lpFree(lp);
        raxRemove(si->stream->rax,si->ri.key,si->ri.key_len,NULL);
    } else {
        /* In the base case we alter the counters of valid/deleted entries. */
        lp = lpReplaceInteger(lp,&p,aux-1);
        p = lpNext(lp,p); /* Seek deleted field. */
        aux = lpGetInteger(p);
        lp = lpReplaceInteger(lp,&p,aux+1);

        /* Update the listpack with the new pointer. */
        if (si->lp != lp)
            raxInsert(si->stream->rax,si->ri.key,si->ri.key_len,lp,NULL);
    }

    /* Update the number of entries counter. */
    si->stream->length--;

    /* Re-seek the iterator to fix the now messed up state. */
    streamID start, end;
    if (si->rev) {
        streamDecodeID(si->start_key,&start);
        end = *current;
    } else {
        start = *current;
        streamDecodeID(si->end_key,&end);
    }
    streamIteratorStop(si);
    streamIteratorStart(si,si->stream,&start,&end,si->rev);

    /* TODO: perform a garbage collection here if the ration between
     * deleted and valid goes over a certain limit. */
}

/* Stop the stream iterator. The only cleanup we need is to free the rax
 * iterator, since the stream iterator itself is supposed to be stack
 * allocated. */
void streamIteratorStop(streamIterator *si) {
    raxStop(&si->ri);
}

/* Delete the specified item ID from the stream, returning 1 if the item
 * was deleted 0 otherwise (if it does not exist). */
int streamDeleteItem(stream *s, streamID *id) {
    int deleted = 0;
    streamIterator si;
    streamIteratorStart(&si,s,id,id,0);
    streamID myid;
    int64_t numfields;
    if (streamIteratorGetID(&si,&myid,&numfields)) {
        streamIteratorRemoveEntry(&si,&myid);
        deleted = 1;
    }
    streamIteratorStop(&si);
    return deleted;
}

/* Get the last valid (non-tombstone) streamID of 's'. */
void streamLastValidID(stream *s, streamID *maxid)
{
    streamIterator si;
    streamIteratorStart(&si,s,NULL,NULL,1);
    int64_t numfields;
    if (!streamIteratorGetID(&si,maxid,&numfields) && s->length)
        serverPanic("Corrupt stream, length is %llu, but no max id", (unsigned long long)s->length);
    streamIteratorStop(&si);
}

/* Emit a reply in the client output buffer by formatting a Stream ID
 * in the standard <ms>-<seq> format, using the simple string protocol
 * of REPL. */
void addReplyStreamID(client *c, streamID *id) {
    sds replyid = sdscatfmt(sdsempty(),"%U-%U",id->ms,id->seq);
    addReplyBulkSds(c,replyid);
}

void setDeferredReplyStreamID(client *c, void *dr, streamID *id) {
    sds replyid = sdscatfmt(sdsempty(),"%U-%U",id->ms,id->seq);
    setDeferredReplyBulkSds(c, dr, replyid);
}

/* Similar to the above function, but just creates an object, usually useful
 * for replication purposes to create arguments. */
robj *createObjectFromStreamID(streamID *id) {
    return createObject(OBJ_STRING, sdscatfmt(sdsempty(),"%U-%U",
                        id->ms,id->seq));
}

/* As a result of an explicit XCLAIM or XREADGROUP command, new entries
 * are created in the pending list of the stream and consumers. We need
 * to propagate this changes in the form of XCLAIM commands. */
void streamPropagateXCLAIM(client *c, robj *key, streamCG *group, robj *groupname, robj *id, streamNACK *nack) {
    /* We need to generate an XCLAIM that will work in a idempotent fashion:
     *
     * XCLAIM <key> <group> <consumer> 0 <id> TIME <milliseconds-unix-time>
     *        RETRYCOUNT <count> FORCE JUSTID LASTID <id>.
     *
     * Note that JUSTID is useful in order to avoid that XCLAIM will do
     * useless work in the slave side, trying to fetch the stream item. */
    robj *argv[14];
    argv[0] = shared.xclaim;
    argv[1] = key;
    argv[2] = groupname;
    argv[3] = createStringObject(nack->consumer->name,sdslen(nack->consumer->name));
    argv[4] = shared.integers[0];
    argv[5] = id;
    argv[6] = shared.time;
    argv[7] = createStringObjectFromLongLong(nack->delivery_time);
    argv[8] = shared.retrycount;
    argv[9] = createStringObjectFromLongLong(nack->delivery_count);
    argv[10] = shared.force;
    argv[11] = shared.justid;
    argv[12] = shared.lastid;
    argv[13] = createObjectFromStreamID(&group->last_id);

    /* We use progagate() because this code path is not always called from
     * the command execution context. Moreover this will just alter the
     * consumer group state, and we don't need MULTI/EXEC wrapping because
     * there is no message state cross-message atomicity required. */
    propagate(server.xclaimCommand,c->db->id,argv,14,PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(argv[3]);
    decrRefCount(argv[7]);
    decrRefCount(argv[9]);
    decrRefCount(argv[13]);
}

/* We need this when we want to propoagate the new last-id of a consumer group
 * that was consumed by XREADGROUP with the NOACK option: in that case we can't
 * propagate the last ID just using the XCLAIM LASTID option, so we emit
 *
 *  XGROUP SETID <key> <groupname> <id>
 */
void streamPropagateGroupID(client *c, robj *key, streamCG *group, robj *groupname) {
    robj *argv[5];
    argv[0] = shared.xgroup;
    argv[1] = shared.setid;
    argv[2] = key;
    argv[3] = groupname;
    argv[4] = createObjectFromStreamID(&group->last_id);

    /* We use progagate() because this code path is not always called from
     * the command execution context. Moreover this will just alter the
     * consumer group state, and we don't need MULTI/EXEC wrapping because
     * there is no message state cross-message atomicity required. */
    propagate(server.xgroupCommand,c->db->id,argv,5,PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(argv[4]);
}

/* We need this when we want to propagate creation of consumer that was created
 * by XREADGROUP with the NOACK option. In that case, the only way to create
 * the consumer at the replica is by using XGROUP CREATECONSUMER (see issue #7140)
 *
 *  XGROUP CREATECONSUMER <key> <groupname> <consumername>
 */
void streamPropagateConsumerCreation(client *c, robj *key, robj *groupname, sds consumername) {
    robj *argv[5];
    argv[0] = shared.xgroup;
    argv[1] = shared.createconsumer;
    argv[2] = key;
    argv[3] = groupname;
    argv[4] = createObject(OBJ_STRING,sdsdup(consumername));

    /* We use progagate() because this code path is not always called from
     * the command execution context. Moreover this will just alter the
     * consumer group state, and we don't need MULTI/EXEC wrapping because
     * there is no message state cross-message atomicity required. */
    propagate(server.xgroupCommand,c->db->id,argv,5,PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(argv[4]);
}

/* Send the stream items in the specified range to the client 'c'. The range
 * the client will receive is between start and end inclusive, if 'count' is
 * non zero, no more than 'count' elements are sent.
 *
 * The 'end' pointer can be NULL to mean that we want all the elements from
 * 'start' till the end of the stream. If 'rev' is non zero, elements are
 * produced in reversed order from end to start.
 *
 * The function returns the number of entries emitted.
 *
 * If group and consumer are not NULL, the function performs additional work:
 * 1. It updates the last delivered ID in the group in case we are
 *    sending IDs greater than the current last ID.
 * 2. If the requested IDs are already assigned to some other consumer, the
 *    function will not return it to the client.
 * 3. An entry in the pending list will be created for every entry delivered
 *    for the first time to this consumer.
 *
 * The behavior may be modified passing non-zero flags:
 *
 * STREAM_RWR_NOACK: Do not create PEL entries, that is, the point "3" above
 *                   is not performed.
 * STREAM_RWR_RAWENTRIES: Do not emit array boundaries, but just the entries,
 *                        and return the number of entries emitted as usually.
 *                        This is used when the function is just used in order
 *                        to emit data and there is some higher level logic.
 *
 * The final argument 'spi' (stream propagation info pointer) is a structure
 * filled with information needed to propagate the command execution to AOF
 * and slaves, in the case a consumer group was passed: we need to generate
 * XCLAIM commands to create the pending list into AOF/slaves in that case.
 *
 * If 'spi' is set to NULL no propagation will happen even if the group was
 * given, but currently such a feature is never used by the code base that
 * will always pass 'spi' and propagate when a group is passed.
 *
 * Note that this function is recursive in certain cases. When it's called
 * with a non NULL group and consumer argument, it may call
 * streamReplyWithRangeFromConsumerPEL() in order to get entries from the
 * consumer pending entries list. However such a function will then call
 * streamReplyWithRange() in order to emit single entries (found in the
 * PEL by ID) to the client. This is the use case for the STREAM_RWR_RAWENTRIES
 * flag.
 */
#define STREAM_RWR_NOACK (1<<0)         /* Do not create entries in the PEL. */
#define STREAM_RWR_RAWENTRIES (1<<1)    /* Do not emit protocol for array
                                           boundaries, just the entries. */
#define STREAM_RWR_HISTORY (1<<2)       /* Only serve consumer local PEL. */
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count, int rev, streamCG *group, streamConsumer *consumer, int flags, streamPropInfo *spi) {
    void *arraylen_ptr = NULL;
    size_t arraylen = 0;
    streamIterator si;
    int64_t numfields;
    streamID id;
    int propagate_last_id = 0;
    int noack = flags & STREAM_RWR_NOACK;

    /* If the client is asking for some history, we serve it using a
     * different function, so that we return entries *solely* from its
     * own PEL. This ensures each consumer will always and only see
     * the history of messages delivered to it and not yet confirmed
     * as delivered. */
    if (group && (flags & STREAM_RWR_HISTORY)) {
        return streamReplyWithRangeFromConsumerPEL(c,s,start,end,count,
                                                   consumer);
    }

    if (!(flags & STREAM_RWR_RAWENTRIES))
        arraylen_ptr = addReplyDeferredLen(c);
    streamIteratorStart(&si,s,start,end,rev);
    while(streamIteratorGetID(&si,&id,&numfields)) {
        /* Update the group last_id if needed. */
        if (group && streamCompareID(&id,&group->last_id) > 0) {
            group->last_id = id;
            /* Group last ID should be propagated only if NOACK was
             * specified, otherwise the last id will be included
             * in the propagation of XCLAIM itself. */
            if (noack) propagate_last_id = 1;
        }

        /* Emit a two elements array for each item. The first is
         * the ID, the second is an array of field-value pairs. */
        addReplyArrayLen(c,2);
        addReplyStreamID(c,&id);

        addReplyArrayLen(c,numfields*2);

        /* Emit the field-value pairs. */
        while(numfields--) {
            unsigned char *key, *value;
            int64_t key_len, value_len;
            streamIteratorGetField(&si,&key,&value,&key_len,&value_len);
            addReplyBulkCBuffer(c,key,key_len);
            addReplyBulkCBuffer(c,value,value_len);
        }

        /* If a group is passed, we need to create an entry in the
         * PEL (pending entries list) of this group *and* this consumer.
         *
         * Note that we cannot be sure about the fact the message is not
         * already owned by another consumer, because the admin is able
         * to change the consumer group last delivered ID using the
         * XGROUP SETID command. So if we find that there is already
         * a NACK for the entry, we need to associate it to the new
         * consumer. */
        if (group && !noack) {
            unsigned char buf[sizeof(streamID)];
            streamEncodeID(buf,&id);

            /* Try to add a new NACK. Most of the time this will work and
             * will not require extra lookups. We'll fix the problem later
             * if we find that there is already a entry for this ID. */
            streamNACK *nack = streamCreateNACK(consumer);
            int group_inserted =
                raxTryInsert(group->pel,buf,sizeof(buf),nack,NULL);
            int consumer_inserted =
                raxTryInsert(consumer->pel,buf,sizeof(buf),nack,NULL);

            /* Now we can check if the entry was already busy, and
             * in that case reassign the entry to the new consumer,
             * or update it if the consumer is the same as before. */
            if (group_inserted == 0) {
                streamFreeNACK(nack);
                nack = raxFind(group->pel,buf,sizeof(buf));
                serverAssert(nack != raxNotFound);
                raxRemove(nack->consumer->pel,buf,sizeof(buf),NULL);
                /* Update the consumer and NACK metadata. */
                nack->consumer = consumer;
                nack->delivery_time = mstime();
                nack->delivery_count = 1;
                /* Add the entry in the new consumer local PEL. */
                raxInsert(consumer->pel,buf,sizeof(buf),nack,NULL);
            } else if (group_inserted == 1 && consumer_inserted == 0) {
                serverPanic("NACK half-created. Should not be possible.");
            }

            /* Propagate as XCLAIM. */
            if (spi) {
                robj *idarg = createObjectFromStreamID(&id);
                streamPropagateXCLAIM(c,spi->keyname,group,spi->groupname,idarg,nack);
                decrRefCount(idarg);
            }
        }

        arraylen++;
        if (count && count == arraylen) break;
    }

    if (spi && propagate_last_id)
        streamPropagateGroupID(c,spi->keyname,group,spi->groupname);

    streamIteratorStop(&si);
    if (arraylen_ptr) setDeferredArrayLen(c,arraylen_ptr,arraylen);
    return arraylen;
}

/* This is an helper function for streamReplyWithRange() when called with
 * group and consumer arguments, but with a range that is referring to already
 * delivered messages. In this case we just emit messages that are already
 * in the history of the consumer, fetching the IDs from its PEL.
 *
 * Note that this function does not have a 'rev' argument because it's not
 * possible to iterate in reverse using a group. Basically this function
 * is only called as a result of the XREADGROUP command.
 *
 * This function is more expensive because it needs to inspect the PEL and then
 * seek into the radix tree of the messages in order to emit the full message
 * to the client. However clients only reach this code path when they are
 * fetching the history of already retrieved messages, which is rare. */
size_t streamReplyWithRangeFromConsumerPEL(client *c, stream *s, streamID *start, streamID *end, size_t count, streamConsumer *consumer) {
    raxIterator ri;
    unsigned char startkey[sizeof(streamID)];
    unsigned char endkey[sizeof(streamID)];
    streamEncodeID(startkey,start);
    if (end) streamEncodeID(endkey,end);

    size_t arraylen = 0;
    void *arraylen_ptr = addReplyDeferredLen(c);
    raxStart(&ri,consumer->pel);
    raxSeek(&ri,">=",startkey,sizeof(startkey));
    while(raxNext(&ri) && (!count || arraylen < count)) {
        if (end && memcmp(ri.key,end,ri.key_len) > 0) break;
        streamID thisid;
        streamDecodeID(ri.key,&thisid);
        if (streamReplyWithRange(c,s,&thisid,&thisid,1,0,NULL,NULL,
                                 STREAM_RWR_RAWENTRIES,NULL) == 0)
        {
            /* Note that we may have a not acknowledged entry in the PEL
             * about a message that's no longer here because was removed
             * by the user by other means. In that case we signal it emitting
             * the ID but then a NULL entry for the fields. */
            addReplyArrayLen(c,2);
            addReplyStreamID(c,&thisid);
            addReplyNullArray(c);
        } else {
            streamNACK *nack = ri.data;
            nack->delivery_time = mstime();
            nack->delivery_count++;
        }
        arraylen++;
    }
    raxStop(&ri);
    setDeferredArrayLen(c,arraylen_ptr,arraylen);
    return arraylen;
}

/* -----------------------------------------------------------------------
 * Stream commands implementation
 * ----------------------------------------------------------------------- */

/* Look the stream at 'key' and return the corresponding stream object.
 * The function creates a key setting it to an empty stream if needed. */
robj *streamTypeLookupWriteOrCreate(client *c, robj *key, int no_create) {
    robj *o = lookupKeyWrite(c->db,key);
    if (checkType(c,o,OBJ_STREAM)) return NULL;
    if (o == NULL) {
        if (no_create) {
            addReplyNull(c);
            return NULL;
        }
        o = createStreamObject();
        dbAdd(c->db,key,o);
    }
    return o;
}

/* Parse a stream ID in the format given by clients to Redis, that is
 * <ms>-<seq>, and converts it into a streamID structure. If
 * the specified ID is invalid C_ERR is returned and an error is reported
 * to the client, otherwise C_OK is returned. The ID may be in incomplete
 * form, just stating the milliseconds time part of the stream. In such a case
 * the missing part is set according to the value of 'missing_seq' parameter.
 *
 * The IDs "-" and "+" specify respectively the minimum and maximum IDs
 * that can be represented. If 'strict' is set to 1, "-" and "+" will be
 * treated as an invalid ID.
 *
 * If 'c' is set to NULL, no reply is sent to the client. */
int streamGenericParseIDOrReply(client *c, const robj *o, streamID *id, uint64_t missing_seq, int strict) {
    char buf[128];
    if (sdslen(o->ptr) > sizeof(buf)-1) goto invalid;
    memcpy(buf,o->ptr,sdslen(o->ptr)+1);

    if (strict && (buf[0] == '-' || buf[0] == '+') && buf[1] == '\0')
        goto invalid;

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

    /* Parse <ms>-<seq> form. */
    char *dot = strchr(buf,'-');
    if (dot) *dot = '\0';
    unsigned long long ms, seq;
    if (string2ull(buf,&ms) == 0) goto invalid;
    if (dot && string2ull(dot+1,&seq) == 0) goto invalid;
    if (!dot) seq = missing_seq;
    id->ms = ms;
    id->seq = seq;
    return C_OK;

invalid:
    if (c) addReplyError(c,"Invalid stream ID specified as stream "
                           "command argument");
    return C_ERR;
}

/* Wrapper for streamGenericParseIDOrReply() used by module API. */
int streamParseID(const robj *o, streamID *id) {
    return streamGenericParseIDOrReply(NULL, o, id, 0, 0);
}

/* Wrapper for streamGenericParseIDOrReply() with 'strict' argument set to
 * 0, to be used when - and + are acceptable IDs. */
int streamParseIDOrReply(client *c, robj *o, streamID *id, uint64_t missing_seq) {
    return streamGenericParseIDOrReply(c,o,id,missing_seq,0);
}

/* Wrapper for streamGenericParseIDOrReply() with 'strict' argument set to
 * 1, to be used when we want to return an error if the special IDs + or -
 * are provided. */
int streamParseStrictIDOrReply(client *c, robj *o, streamID *id, uint64_t missing_seq) {
    return streamGenericParseIDOrReply(c,o,id,missing_seq,1);
}

/* Helper for parsing a stream ID that is a range query interval. When the
 * exclude argument is NULL, streamParseIDOrReply() is called and the interval
 * is treated as close (inclusive). Otherwise, the exclude argument is set if 
 * the interval is open (the "(" prefix) and streamParseStrictIDOrReply() is
 * called in that case.
 */
int streamParseIntervalIDOrReply(client *c, robj *o, streamID *id, int *exclude, uint64_t missing_seq) {
    char *p = o->ptr;
    size_t len = sdslen(p);
    int invalid = 0;
    
    if (exclude != NULL) *exclude = (len > 1 && p[0] == '(');
    if (exclude != NULL && *exclude) {
        robj *t = createStringObject(p+1,len-1);
        invalid = (streamParseStrictIDOrReply(c,t,id,missing_seq) == C_ERR);
        decrRefCount(t);
    } else 
        invalid = (streamParseIDOrReply(c,o,id,missing_seq) == C_ERR);
    if (invalid)
        return C_ERR;
    return C_OK;
}

void streamRewriteApproxSpecifier(client *c, int idx) {
    rewriteClientCommandArgument(c,idx,shared.special_equals);
}

/* We propagate MAXLEN/MINID ~ <count> as MAXLEN/MINID = <resulting-len-of-stream>
 * otherwise trimming is no longer deterministic on replicas / AOF. */
void streamRewriteTrimArgument(client *c, stream *s, int trim_strategy, int idx) {
    robj *arg;
    if (trim_strategy == TRIM_STRATEGY_MAXLEN) {
        arg = createStringObjectFromLongLong(s->length);
    } else {
        streamID first_id;
        streamGetEdgeID(s, 1, &first_id);
        arg = createObjectFromStreamID(&first_id);
    }

    rewriteClientCommandArgument(c,idx,arg);
    decrRefCount(arg);
}

/* XADD key [(MAXLEN [~|=] <count> | MINID [~|=] <id>) [LIMIT <entries>]] [NOMKSTREAM] <ID or *> [field value] [field value] ... */
void xaddCommand(client *c) {
    /* Parse options. */
    streamAddTrimArgs parsed_args;
    int idpos = streamParseAddOrTrimArgsOrReply(c, &parsed_args, 1);
    if (idpos < 0)
        return; /* streamParseAddOrTrimArgsOrReply already replied. */
    int field_pos = idpos+1; /* The ID is always one argument before the first field */

    /* Check arity. */
    if ((c->argc - field_pos) < 2 || ((c->argc-field_pos) % 2) == 1) {
        addReplyError(c,"wrong number of arguments for XADD");
        return;
    }

    /* Return ASAP if minimal ID (0-0) was given so we avoid possibly creating
     * a new stream and have streamAppendItem fail, leaving an empty key in the
     * database. */
    if (parsed_args.id_given &&
        parsed_args.id.ms == 0 && parsed_args.id.seq == 0)
    {
        addReplyError(c,"The ID specified in XADD must be greater than 0-0");
        return;
    }

    /* Lookup the stream at key. */
    robj *o;
    stream *s;
    if ((o = streamTypeLookupWriteOrCreate(c,c->argv[1],parsed_args.no_mkstream)) == NULL) return;
    s = o->ptr;

    /* Return ASAP if the stream has reached the last possible ID */
    if (s->last_id.ms == UINT64_MAX && s->last_id.seq == UINT64_MAX) {
        addReplyError(c,"The stream has exhausted the last possible ID, "
                        "unable to add more items");
        return;
    }

    /* Append using the low level function and return the ID. */
    streamID id;
    if (streamAppendItem(s,c->argv+field_pos,(c->argc-field_pos)/2,
        &id, parsed_args.id_given ? &parsed_args.id : NULL) == C_ERR)
    {
        if (errno == EDOM)
            addReplyError(c,"The ID specified in XADD is equal or smaller than "
                            "the target stream top item");
        else
            addReplyError(c,"Elements are too large to be stored");
        return;
    }
    addReplyStreamID(c,&id);

    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STREAM,"xadd",c->argv[1],c->db->id);
    server.dirty++;

    /* Trim if needed. */
    if (parsed_args.trim_strategy != TRIM_STRATEGY_NONE) {
        if (streamTrim(s, &parsed_args)) {
            notifyKeyspaceEvent(NOTIFY_STREAM,"xtrim",c->argv[1],c->db->id);
        }
        if (parsed_args.approx_trim) {
            /* In case our trimming was limited (by LIMIT or by ~) we must
             * re-write the relevant trim argument to make sure there will be
             * no inconsistencies in AOF loading or in the replica.
             * It's enough to check only args->approx because there is no
             * way LIMIT is given without the ~ option. */
            streamRewriteApproxSpecifier(c,parsed_args.trim_strategy_arg_idx-1);
            streamRewriteTrimArgument(c,s,parsed_args.trim_strategy,parsed_args.trim_strategy_arg_idx);
        }
    }

    /* Let's rewrite the ID argument with the one actually generated for
     * AOF/replication propagation. */
    robj *idarg = createObjectFromStreamID(&id);
    rewriteClientCommandArgument(c,idpos,idarg);
    decrRefCount(idarg);

    /* We need to signal to blocked clients that there is new data on this
     * stream. */
    signalKeyAsReady(c->db, c->argv[1], OBJ_STREAM);
}

/* XRANGE/XREVRANGE actual implementation.
 * The 'start' and 'end' IDs are parsed as follows:
 *   Incomplete 'start' has its sequence set to 0, and 'end' to UINT64_MAX.
 *   "-" and "+"" mean the minimal and maximal ID values, respectively.
 *   The "(" prefix means an open (exclusive) range, so XRANGE stream (1-0 (2-0
 *   will match anything from 1-1 and 1-UINT64_MAX.
 */
void xrangeGenericCommand(client *c, int rev) {
    robj *o;
    stream *s;
    streamID startid, endid;
    long long count = -1;
    robj *startarg = rev ? c->argv[3] : c->argv[2];
    robj *endarg = rev ? c->argv[2] : c->argv[3];
    int startex = 0, endex = 0;
    
    /* Parse start and end IDs. */
    if (streamParseIntervalIDOrReply(c,startarg,&startid,&startex,0) != C_OK)
        return;
    if (startex && streamIncrID(&startid) != C_OK) {
        addReplyError(c,"invalid start ID for the interval");
        return;
    }
    if (streamParseIntervalIDOrReply(c,endarg,&endid,&endex,UINT64_MAX) != C_OK)
        return;
    if (endex && streamDecrID(&endid) != C_OK) {
        addReplyError(c,"invalid end ID for the interval");
        return;
    }

    /* Parse the COUNT option if any. */
    if (c->argc > 4) {
        for (int j = 4; j < c->argc; j++) {
            int additional = c->argc-j-1;
            if (strcasecmp(c->argv[j]->ptr,"COUNT") == 0 && additional >= 1) {
                if (getLongLongFromObjectOrReply(c,c->argv[j+1],&count,NULL)
                    != C_OK) return;
                if (count < 0) count = 0;
                j++; /* Consume additional arg. */
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Return the specified range to the user. */
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray)) == NULL ||
         checkType(c,o,OBJ_STREAM)) return;

    s = o->ptr;

    if (count == 0) {
        addReplyNullArray(c);
    } else {
        if (count == -1) count = 0;
        streamReplyWithRange(c,s,&startid,&endid,count,rev,NULL,NULL,0,NULL);
    }
}

/* XRANGE key start end [COUNT <n>] */
void xrangeCommand(client *c) {
    xrangeGenericCommand(c,0);
}

/* XREVRANGE key end start [COUNT <n>] */
void xrevrangeCommand(client *c) {
    xrangeGenericCommand(c,1);
}

/* XLEN */
void xlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL
        || checkType(c,o,OBJ_STREAM)) return;
    stream *s = o->ptr;
    addReplyLongLong(c,s->length);
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] STREAMS key_1 key_2 ... key_N
 *       ID_1 ID_2 ... ID_N
 *
 * This function also implements the XREAD-GROUP command, which is like XREAD
 * but accepting the [GROUP group-name consumer-name] additional option.
 * This is useful because while XREAD is a read command and can be called
 * on slaves, XREAD-GROUP is not. */
#define XREAD_BLOCKED_DEFAULT_COUNT 1000
void xreadCommand(client *c) {
    long long timeout = -1; /* -1 means, no BLOCK argument given. */
    long long count = 0;
    int streams_count = 0;
    int streams_arg = 0;
    int noack = 0;          /* True if NOACK option was specified. */
    streamID static_ids[STREAMID_STATIC_VECTOR_LEN];
    streamID *ids = static_ids;
    streamCG **groups = NULL;
    int xreadgroup = sdslen(c->argv[0]->ptr) == 10; /* XREAD or XREADGROUP? */
    robj *groupname = NULL;
    robj *consumername = NULL;

    /* Parse arguments. */
    for (int i = 1; i < c->argc; i++) {
        int moreargs = c->argc-i-1;
        char *o = c->argv[i]->ptr;
        if (!strcasecmp(o,"BLOCK") && moreargs) {
            if (c->flags & CLIENT_LUA) {
                /*
                 * Although the CLIENT_DENY_BLOCKING flag should protect from blocking the client
                 * on Lua/MULTI/RM_Call we want special treatment for Lua to keep backword compatibility.
                 * There is no sense to use BLOCK option within Lua. */
                addReplyErrorFormat(c, "%s command is not allowed with BLOCK option from scripts", (char *)c->argv[0]->ptr);
                return;
            }
            i++;
            if (getTimeoutFromObjectOrReply(c,c->argv[i],&timeout,
                UNIT_MILLISECONDS) != C_OK) return;
        } else if (!strcasecmp(o,"COUNT") && moreargs) {
            i++;
            if (getLongLongFromObjectOrReply(c,c->argv[i],&count,NULL) != C_OK)
                return;
            if (count < 0) count = 0;
        } else if (!strcasecmp(o,"STREAMS") && moreargs) {
            streams_arg = i+1;
            streams_count = (c->argc-streams_arg);
            if ((streams_count % 2) != 0) {
                addReplyError(c,"Unbalanced XREAD list of streams: "
                                "for each stream key an ID or '$' must be "
                                "specified.");
                return;
            }
            streams_count /= 2; /* We have two arguments for each stream. */
            break;
        } else if (!strcasecmp(o,"GROUP") && moreargs >= 2) {
            if (!xreadgroup) {
                addReplyError(c,"The GROUP option is only supported by "
                                "XREADGROUP. You called XREAD instead.");
                return;
            }
            groupname = c->argv[i+1];
            consumername = c->argv[i+2];
            i += 2;
        } else if (!strcasecmp(o,"NOACK")) {
            if (!xreadgroup) {
                addReplyError(c,"The NOACK option is only supported by "
                                "XREADGROUP. You called XREAD instead.");
                return;
            }
            noack = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* STREAMS option is mandatory. */
    if (streams_arg == 0) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* If the user specified XREADGROUP then it must also
     * provide the GROUP option. */
    if (xreadgroup && groupname == NULL) {
        addReplyError(c,"Missing GROUP option for XREADGROUP");
        return;
    }

    /* Parse the IDs and resolve the group name. */
    if (streams_count > STREAMID_STATIC_VECTOR_LEN)
        ids = zmalloc(sizeof(streamID)*streams_count);
    if (groupname) groups = zmalloc(sizeof(streamCG*)*streams_count);

    for (int i = streams_arg + streams_count; i < c->argc; i++) {
        /* Specifying "$" as last-known-id means that the client wants to be
         * served with just the messages that will arrive into the stream
         * starting from now. */
        int id_idx = i - streams_arg - streams_count;
        robj *key = c->argv[i-streams_count];
        robj *o = lookupKeyRead(c->db,key);
        if (checkType(c,o,OBJ_STREAM)) goto cleanup;
        streamCG *group = NULL;

        /* If a group was specified, than we need to be sure that the
         * key and group actually exist. */
        if (groupname) {
            if (o == NULL ||
                (group = streamLookupCG(o->ptr,groupname->ptr)) == NULL)
            {
                addReplyErrorFormat(c, "-NOGROUP No such key '%s' or consumer "
                                       "group '%s' in XREADGROUP with GROUP "
                                       "option",
                                    (char*)key->ptr,(char*)groupname->ptr);
                goto cleanup;
            }
            groups[id_idx] = group;
        }

        if (strcmp(c->argv[i]->ptr,"$") == 0) {
            if (xreadgroup) {
                addReplyError(c,"The $ ID is meaningless in the context of "
                                "XREADGROUP: you want to read the history of "
                                "this consumer by specifying a proper ID, or "
                                "use the > ID to get new messages. The $ ID would "
                                "just return an empty result set.");
                goto cleanup;
            }
            if (o) {
                stream *s = o->ptr;
                ids[id_idx] = s->last_id;
            } else {
                ids[id_idx].ms = 0;
                ids[id_idx].seq = 0;
            }
            continue;
        } else if (strcmp(c->argv[i]->ptr,">") == 0) {
            if (!xreadgroup) {
                addReplyError(c,"The > ID can be specified only when calling "
                                "XREADGROUP using the GROUP <group> "
                                "<consumer> option.");
                goto cleanup;
            }
            /* We use just the maximum ID to signal this is a ">" ID, anyway
             * the code handling the blocking clients will have to update the
             * ID later in order to match the changing consumer group last ID. */
            ids[id_idx].ms = UINT64_MAX;
            ids[id_idx].seq = UINT64_MAX;
            continue;
        }
        if (streamParseStrictIDOrReply(c,c->argv[i],ids+id_idx,0) != C_OK)
            goto cleanup;
    }

    /* Try to serve the client synchronously. */
    size_t arraylen = 0;
    void *arraylen_ptr = NULL;
    for (int i = 0; i < streams_count; i++) {
        robj *o = lookupKeyRead(c->db,c->argv[streams_arg+i]);
        if (o == NULL) continue;
        stream *s = o->ptr;
        streamID *gt = ids+i; /* ID must be greater than this. */
        int serve_synchronously = 0;
        int serve_history = 0; /* True for XREADGROUP with ID != ">". */

        /* Check if there are the conditions to serve the client
         * synchronously. */
        if (groups) {
            /* If the consumer is blocked on a group, we always serve it
             * synchronously (serving its local history) if the ID specified
             * was not the special ">" ID. */
            if (gt->ms != UINT64_MAX ||
                gt->seq != UINT64_MAX)
            {
                serve_synchronously = 1;
                serve_history = 1;
            } else if (s->length) {
                /* We also want to serve a consumer in a consumer group
                 * synchronously in case the group top item delivered is smaller
                 * than what the stream has inside. */
                streamID maxid, *last = &groups[i]->last_id;
                streamLastValidID(s, &maxid);
                if (streamCompareID(&maxid, last) > 0) {
                    serve_synchronously = 1;
                    *gt = *last;
                }
            }
        } else if (s->length) {
            /* For consumers without a group, we serve synchronously if we can
             * actually provide at least one item from the stream. */
            streamID maxid;
            streamLastValidID(s, &maxid);
            if (streamCompareID(&maxid, gt) > 0) {
                serve_synchronously = 1;
            }
        }

        if (serve_synchronously) {
            arraylen++;
            if (arraylen == 1) arraylen_ptr = addReplyDeferredLen(c);
            /* streamReplyWithRange() handles the 'start' ID as inclusive,
             * so start from the next ID, since we want only messages with
             * IDs greater than start. */
            streamID start = *gt;
            streamIncrID(&start);

            /* Emit the two elements sub-array consisting of the name
             * of the stream and the data we extracted from it. */
            if (c->resp == 2) addReplyArrayLen(c,2);
            addReplyBulk(c,c->argv[streams_arg+i]);
            int created = 0;
            streamConsumer *consumer = NULL;
            if (groups) consumer = streamLookupConsumer(groups[i],
                                                        consumername->ptr,
                                                        SLC_NONE,
                                                        &created);
            streamPropInfo spi = {c->argv[i+streams_arg],groupname};
            if (created && noack)
                streamPropagateConsumerCreation(c,spi.keyname,
                                                spi.groupname,
                                                consumer->name);
            int flags = 0;
            if (noack) flags |= STREAM_RWR_NOACK;
            if (serve_history) flags |= STREAM_RWR_HISTORY;
            streamReplyWithRange(c,s,&start,NULL,count,0,
                                 groups ? groups[i] : NULL,
                                 consumer, flags, &spi);
            if (groups) server.dirty++;
        }
    }

     /* We replied synchronously! Set the top array len and return to caller. */
    if (arraylen) {
        if (c->resp == 2)
            setDeferredArrayLen(c,arraylen_ptr,arraylen);
        else
            setDeferredMapLen(c,arraylen_ptr,arraylen);
        goto cleanup;
    }

    /* Block if needed. */
    if (timeout != -1) {
        /* If we are not allowed to block the client, the only thing
         * we can do is treating it as a timeout (even with timeout 0). */
        if (c->flags & CLIENT_DENY_BLOCKING) {
            addReplyNullArray(c);
            goto cleanup;
        }
        blockForKeys(c, BLOCKED_STREAM, c->argv+streams_arg, streams_count,
                     timeout, NULL, NULL, ids);
        /* If no COUNT is given and we block, set a relatively small count:
         * in case the ID provided is too low, we do not want the server to
         * block just to serve this client a huge stream of messages. */
        c->bpop.xread_count = count ? count : XREAD_BLOCKED_DEFAULT_COUNT;

        /* If this is a XREADGROUP + GROUP we need to remember for which
         * group and consumer name we are blocking, so later when one of the
         * keys receive more data, we can call streamReplyWithRange() passing
         * the right arguments. */
        if (groupname) {
            incrRefCount(groupname);
            incrRefCount(consumername);
            c->bpop.xread_group = groupname;
            c->bpop.xread_consumer = consumername;
            c->bpop.xread_group_noack = noack;
        } else {
            c->bpop.xread_group = NULL;
            c->bpop.xread_consumer = NULL;
        }
        goto cleanup;
    }

    /* No BLOCK option, nor any stream we can serve. Reply as with a
     * timeout happened. */
    addReplyNullArray(c);
    /* Continue to cleanup... */

cleanup: /* Cleanup. */

    /* The command is propagated (in the READGROUP form) as a side effect
     * of calling lower level APIs. So stop any implicit propagation. */
    preventCommandPropagation(c);
    if (ids != static_ids) zfree(ids);
    zfree(groups);
}

/* -----------------------------------------------------------------------
 * Low level implementation of consumer groups
 * ----------------------------------------------------------------------- */

/* Create a NACK entry setting the delivery count to 1 and the delivery
 * time to the current time. The NACK consumer will be set to the one
 * specified as argument of the function. */
streamNACK *streamCreateNACK(streamConsumer *consumer) {
    streamNACK *nack = zmalloc(sizeof(*nack));
    nack->delivery_time = mstime();
    nack->delivery_count = 1;
    nack->consumer = consumer;
    return nack;
}

/* Free a NACK entry. */
void streamFreeNACK(streamNACK *na) {
    zfree(na);
}

/* Free a consumer and associated data structures. Note that this function
 * will not reassign the pending messages associated with this consumer
 * nor will delete them from the stream, so when this function is called
 * to delete a consumer, and not when the whole stream is destroyed, the caller
 * should do some work before. */
void streamFreeConsumer(streamConsumer *sc) {
    raxFree(sc->pel); /* No value free callback: the PEL entries are shared
                         between the consumer and the main stream PEL. */
    sdsfree(sc->name);
    zfree(sc);
}

/* Create a new consumer group in the context of the stream 's', having the
 * specified name and last server ID. If a consumer group with the same name
 * already existed NULL is returned, otherwise the pointer to the consumer
 * group is returned. */
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id) {
    if (s->cgroups == NULL) s->cgroups = raxNew();
    if (raxFind(s->cgroups,(unsigned char*)name,namelen) != raxNotFound)
        return NULL;

    streamCG *cg = zmalloc(sizeof(*cg));
    cg->pel = raxNew();
    cg->consumers = raxNew();
    cg->last_id = *id;
    raxInsert(s->cgroups,(unsigned char*)name,namelen,cg,NULL);
    return cg;
}

/* Free a consumer group and all its associated data. */
void streamFreeCG(streamCG *cg) {
    raxFreeWithCallback(cg->pel,(void(*)(void*))streamFreeNACK);
    raxFreeWithCallback(cg->consumers,(void(*)(void*))streamFreeConsumer);
    zfree(cg);
}

/* Lookup the consumer group in the specified stream and returns its
 * pointer, otherwise if there is no such group, NULL is returned. */
streamCG *streamLookupCG(stream *s, sds groupname) {
    if (s->cgroups == NULL) return NULL;
    streamCG *cg = raxFind(s->cgroups,(unsigned char*)groupname,
                           sdslen(groupname));
    return (cg == raxNotFound) ? NULL : cg;
}

/* Lookup the consumer with the specified name in the group 'cg': if the
 * consumer does not exist it is created unless SLC_NOCREAT flag was specified.
 * Its last seen time is updated unless SLC_NOREFRESH flag was specified. */
streamConsumer *streamLookupConsumer(streamCG *cg, sds name, int flags, int *created) {
    if (created) *created = 0;
    int create = !(flags & SLC_NOCREAT);
    int refresh = !(flags & SLC_NOREFRESH);
    streamConsumer *consumer = raxFind(cg->consumers,(unsigned char*)name,
                               sdslen(name));
    if (consumer == raxNotFound) {
        if (!create) return NULL;
        consumer = zmalloc(sizeof(*consumer));
        consumer->name = sdsdup(name);
        consumer->pel = raxNew();
        raxInsert(cg->consumers,(unsigned char*)name,sdslen(name),
                  consumer,NULL);
        consumer->seen_time = mstime();
        if (created) *created = 1;
    } else if (refresh)
        consumer->seen_time = mstime();
    return consumer;
}

/* Delete the consumer specified in the consumer group 'cg'. The consumer
 * may have pending messages: they are removed from the PEL, and the number
 * of pending messages "lost" is returned. */
uint64_t streamDelConsumer(streamCG *cg, sds name) {
    streamConsumer *consumer =
        streamLookupConsumer(cg,name,SLC_NOCREAT|SLC_NOREFRESH,NULL);
    if (consumer == NULL) return 0;

    uint64_t retval = raxSize(consumer->pel);

    /* Iterate all the consumer pending messages, deleting every corresponding
     * entry from the global entry. */
    raxIterator ri;
    raxStart(&ri,consumer->pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamNACK *nack = ri.data;
        raxRemove(cg->pel,ri.key,ri.key_len,NULL);
        streamFreeNACK(nack);
    }
    raxStop(&ri);

    /* Deallocate the consumer. */
    raxRemove(cg->consumers,(unsigned char*)name,sdslen(name),NULL);
    streamFreeConsumer(consumer);
    return retval;
}

/* -----------------------------------------------------------------------
 * Consumer groups commands
 * ----------------------------------------------------------------------- */

/* XGROUP CREATE <key> <groupname> <id or $> [MKSTREAM]
 * XGROUP SETID <key> <groupname> <id or $>
 * XGROUP DESTROY <key> <groupname>
 * XGROUP CREATECONSUMER <key> <groupname> <consumer>
 * XGROUP DELCONSUMER <key> <groupname> <consumername> */
void xgroupCommand(client *c) {
    stream *s = NULL;
    sds grpname = NULL;
    streamCG *cg = NULL;
    char *opt = c->argv[1]->ptr; /* Subcommand name. */
    int mkstream = 0;
    robj *o;

    /* CREATE has an MKSTREAM option that creates the stream if it
     * does not exist. */
    if (c->argc == 6 && !strcasecmp(opt,"CREATE")) {
        if (strcasecmp(c->argv[5]->ptr,"MKSTREAM")) {
            addReplySubcommandSyntaxError(c);
            return;
        }
        mkstream = 1;
        grpname = c->argv[3]->ptr;
    }

    /* Everything but the "HELP" option requires a key and group name. */
    if (c->argc >= 4) {
        o = lookupKeyWrite(c->db,c->argv[2]);
        if (o) {
            if (checkType(c,o,OBJ_STREAM)) return;
            s = o->ptr;
        }
        grpname = c->argv[3]->ptr;
    }

    /* Check for missing key/group. */
    if (c->argc >= 4 && !mkstream) {
        /* At this point key must exist, or there is an error. */
        if (s == NULL) {
            addReplyError(c,
                "The XGROUP subcommand requires the key to exist. "
                "Note that for CREATE you may want to use the MKSTREAM "
                "option to create an empty stream automatically.");
            return;
        }

        /* Certain subcommands require the group to exist. */
        if ((cg = streamLookupCG(s,grpname)) == NULL &&
            (!strcasecmp(opt,"SETID") ||
             !strcasecmp(opt,"CREATECONSUMER") ||
             !strcasecmp(opt,"DELCONSUMER")))
        {
            addReplyErrorFormat(c, "-NOGROUP No such consumer group '%s' "
                                   "for key name '%s'",
                                   (char*)grpname, (char*)c->argv[2]->ptr);
            return;
        }
    }

    /* Dispatch the different subcommands. */
    if (c->argc == 2 && !strcasecmp(opt,"HELP")) {
        const char *help[] = {
"CREATE <key> <groupname> <id|$> [option]",
"    Create a new consumer group. Options are:",
"    * MKSTREAM",
"      Create the empty stream if it does not exist.",
"CREATECONSUMER <key> <groupname> <consumer>",
"    Create a new consumer in the specified group.",
"DELCONSUMER <key> <groupname> <consumer>",
"    Remove the specified consumer.",
"DESTROY <key> <groupname>"
"    Remove the specified group.",
"SETID <key> <groupname> <id|$>",
"    Set the current group ID.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(opt,"CREATE") && (c->argc == 5 || c->argc == 6)) {
        streamID id;
        if (!strcmp(c->argv[4]->ptr,"$")) {
            if (s) {
                id = s->last_id;
            } else {
                id.ms = 0;
                id.seq = 0;
            }
        } else if (streamParseStrictIDOrReply(c,c->argv[4],&id,0) != C_OK) {
            return;
        }

        /* Handle the MKSTREAM option now that the command can no longer fail. */
        if (s == NULL) {
            serverAssert(mkstream);
            o = createStreamObject();
            dbAdd(c->db,c->argv[2],o);
            s = o->ptr;
            signalModifiedKey(c,c->db,c->argv[2]);
        }

        streamCG *cg = streamCreateCG(s,grpname,sdslen(grpname),&id);
        if (cg) {
            addReply(c,shared.ok);
            server.dirty++;
            notifyKeyspaceEvent(NOTIFY_STREAM,"xgroup-create",
                                c->argv[2],c->db->id);
        } else {
            addReplyError(c,"-BUSYGROUP Consumer Group name already exists");
        }
    } else if (!strcasecmp(opt,"SETID") && c->argc == 5) {
        streamID id;
        if (!strcmp(c->argv[4]->ptr,"$")) {
            id = s->last_id;
        } else if (streamParseIDOrReply(c,c->argv[4],&id,0) != C_OK) {
            return;
        }
        cg->last_id = id;
        addReply(c,shared.ok);
        server.dirty++;
        notifyKeyspaceEvent(NOTIFY_STREAM,"xgroup-setid",c->argv[2],c->db->id);
    } else if (!strcasecmp(opt,"DESTROY") && c->argc == 4) {
        if (cg) {
            raxRemove(s->cgroups,(unsigned char*)grpname,sdslen(grpname),NULL);
            streamFreeCG(cg);
            addReply(c,shared.cone);
            server.dirty++;
            notifyKeyspaceEvent(NOTIFY_STREAM,"xgroup-destroy",
                                c->argv[2],c->db->id);
            /* We want to unblock any XREADGROUP consumers with -NOGROUP. */
            signalKeyAsReady(c->db,c->argv[2],OBJ_STREAM);
        } else {
            addReply(c,shared.czero);
        }
    } else if (!strcasecmp(opt,"CREATECONSUMER") && c->argc == 5) {
        int created = 0;
        streamLookupConsumer(cg,c->argv[4]->ptr,SLC_NOREFRESH,&created);
        if (created) {
            server.dirty++;
            notifyKeyspaceEvent(NOTIFY_STREAM,"xgroup-createconsumer",
                                c->argv[2],c->db->id);
        }
        addReplyLongLong(c,created);
    } else if (!strcasecmp(opt,"DELCONSUMER") && c->argc == 5) {
        /* Delete the consumer and returns the number of pending messages
         * that were yet associated with such a consumer. */
        long long pending = streamDelConsumer(cg,c->argv[4]->ptr);
        addReplyLongLong(c,pending);
        server.dirty++;
        notifyKeyspaceEvent(NOTIFY_STREAM,"xgroup-delconsumer",
                            c->argv[2],c->db->id);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* XSETID <stream> <id>
 *
 * Set the internal "last ID" of a stream. */
void xsetidCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_STREAM)) return;

    stream *s = o->ptr;
    streamID id;
    if (streamParseStrictIDOrReply(c,c->argv[2],&id,0) != C_OK) return;

    /* If the stream has at least one item, we want to check that the user
     * is setting a last ID that is equal or greater than the current top
     * item, otherwise the fundamental ID monotonicity assumption is violated. */
    if (s->length > 0) {
        streamID maxid;
        streamLastValidID(s,&maxid);

        if (streamCompareID(&id,&maxid) < 0) {
            addReplyError(c,"The ID specified in XSETID is smaller than the "
                            "target stream top item");
            return;
        }
    }
    s->last_id = id;
    addReply(c,shared.ok);
    server.dirty++;
    notifyKeyspaceEvent(NOTIFY_STREAM,"xsetid",c->argv[1],c->db->id);
}

/* XACK <key> <group> <id> <id> ... <id>
 *
 * Acknowledge a message as processed. In practical terms we just check the
 * pendine entries list (PEL) of the group, and delete the PEL entry both from
 * the group and the consumer (pending messages are referenced in both places).
 *
 * Return value of the command is the number of messages successfully
 * acknowledged, that is, the IDs we were actually able to resolve in the PEL.
 */
void xackCommand(client *c) {
    streamCG *group = NULL;
    robj *o = lookupKeyRead(c->db,c->argv[1]);
    if (o) {
        if (checkType(c,o,OBJ_STREAM)) return; /* Type error. */
        group = streamLookupCG(o->ptr,c->argv[2]->ptr);
    }

    /* No key or group? Nothing to ack. */
    if (o == NULL || group == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* Start parsing the IDs, so that we abort ASAP if there is a syntax
     * error: the return value of this command cannot be an error in case
     * the client successfully acknowledged some messages, so it should be
     * executed in a "all or nothing" fashion. */
    streamID static_ids[STREAMID_STATIC_VECTOR_LEN];
    streamID *ids = static_ids;
    int id_count = c->argc-3;
    if (id_count > STREAMID_STATIC_VECTOR_LEN)
        ids = zmalloc(sizeof(streamID)*id_count);
    for (int j = 3; j < c->argc; j++) {
        if (streamParseStrictIDOrReply(c,c->argv[j],&ids[j-3],0) != C_OK) goto cleanup;
    }

    int acknowledged = 0;
    for (int j = 3; j < c->argc; j++) {
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf,&ids[j-3]);

        /* Lookup the ID in the group PEL: it will have a reference to the
         * NACK structure that will have a reference to the consumer, so that
         * we are able to remove the entry from both PELs. */
        streamNACK *nack = raxFind(group->pel,buf,sizeof(buf));
        if (nack != raxNotFound) {
            raxRemove(group->pel,buf,sizeof(buf),NULL);
            raxRemove(nack->consumer->pel,buf,sizeof(buf),NULL);
            streamFreeNACK(nack);
            acknowledged++;
            server.dirty++;
        }
    }
    addReplyLongLong(c,acknowledged);
cleanup:
    if (ids != static_ids) zfree(ids);
}

/* XPENDING <key> <group> [[IDLE <idle>] <start> <stop> <count> [<consumer>]]
 *
 * If start and stop are omitted, the command just outputs information about
 * the amount of pending messages for the key/group pair, together with
 * the minimum and maximum ID of pending messages.
 *
 * If start and stop are provided instead, the pending messages are returned
 * with information about the current owner, number of deliveries and last
 * delivery time and so forth. */
void xpendingCommand(client *c) {
    int justinfo = c->argc == 3; /* Without the range just outputs general
                                    informations about the PEL. */
    robj *key = c->argv[1];
    robj *groupname = c->argv[2];
    robj *consumername = NULL;
    streamID startid, endid;
    long long count = 0;
    long long minidle = 0;
    int startex = 0, endex = 0;

    /* Start and stop, and the consumer, can be omitted. Also the IDLE modifier. */
    if (c->argc != 3 && (c->argc < 6 || c->argc > 9)) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Parse start/end/count arguments ASAP if needed, in order to report
     * syntax errors before any other error. */
    if (c->argc >= 6) {
        int startidx = 3; /* Without IDLE */

        if (!strcasecmp(c->argv[3]->ptr, "IDLE")) {
            if (getLongLongFromObjectOrReply(c, c->argv[4], &minidle, NULL) == C_ERR)
                return;
            if (c->argc < 8) {
                /* If IDLE was provided we must have at least 'start end count' */
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            /* Search for rest of arguments after 'IDLE <idle>' */
            startidx += 2;
        }

        /* count argument. */
        if (getLongLongFromObjectOrReply(c,c->argv[startidx+2],&count,NULL) == C_ERR)
            return;
        if (count < 0) count = 0;

        /* start and end arguments. */
        if (streamParseIntervalIDOrReply(c,c->argv[startidx],&startid,&startex,0) != C_OK)
            return;
        if (startex && streamIncrID(&startid) != C_OK) {
            addReplyError(c,"invalid start ID for the interval");
            return;
        }
        if (streamParseIntervalIDOrReply(c,c->argv[startidx+1],&endid,&endex,UINT64_MAX) != C_OK)
            return;
        if (endex && streamDecrID(&endid) != C_OK) {
            addReplyError(c,"invalid end ID for the interval");
            return;
        }

        if (startidx+3 < c->argc) {
            /* 'consumer' was provided */
            consumername = c->argv[startidx+3];
        }
    }

    /* Lookup the key and the group inside the stream. */
    robj *o = lookupKeyRead(c->db,c->argv[1]);
    streamCG *group;

    if (checkType(c,o,OBJ_STREAM)) return;
    if (o == NULL ||
        (group = streamLookupCG(o->ptr,groupname->ptr)) == NULL)
    {
        addReplyErrorFormat(c, "-NOGROUP No such key '%s' or consumer "
                               "group '%s'",
                               (char*)key->ptr,(char*)groupname->ptr);
        return;
    }

    /* XPENDING <key> <group> variant. */
    if (justinfo) {
        addReplyArrayLen(c,4);
        /* Total number of messages in the PEL. */
        addReplyLongLong(c,raxSize(group->pel));
        /* First and last IDs. */
        if (raxSize(group->pel) == 0) {
            addReplyNull(c); /* Start. */
            addReplyNull(c); /* End. */
            addReplyNullArray(c); /* Clients. */
        } else {
            /* Start. */
            raxIterator ri;
            raxStart(&ri,group->pel);
            raxSeek(&ri,"^",NULL,0);
            raxNext(&ri);
            streamDecodeID(ri.key,&startid);
            addReplyStreamID(c,&startid);

            /* End. */
            raxSeek(&ri,"$",NULL,0);
            raxNext(&ri);
            streamDecodeID(ri.key,&endid);
            addReplyStreamID(c,&endid);
            raxStop(&ri);

            /* Consumers with pending messages. */
            raxStart(&ri,group->consumers);
            raxSeek(&ri,"^",NULL,0);
            void *arraylen_ptr = addReplyDeferredLen(c);
            size_t arraylen = 0;
            while(raxNext(&ri)) {
                streamConsumer *consumer = ri.data;
                if (raxSize(consumer->pel) == 0) continue;
                addReplyArrayLen(c,2);
                addReplyBulkCBuffer(c,ri.key,ri.key_len);
                addReplyBulkLongLong(c,raxSize(consumer->pel));
                arraylen++;
            }
            setDeferredArrayLen(c,arraylen_ptr,arraylen);
            raxStop(&ri);
        }
    } else { /* <start>, <stop> and <count> provided, return actual pending entries (not just info) */
        streamConsumer *consumer = NULL;
        if (consumername) {
            consumer = streamLookupConsumer(group,
                                            consumername->ptr,
                                            SLC_NOCREAT|SLC_NOREFRESH,
                                            NULL);

            /* If a consumer name was mentioned but it does not exist, we can
             * just return an empty array. */
            if (consumer == NULL) {
                addReplyArrayLen(c,0);
                return;
            }
        }

        rax *pel = consumer ? consumer->pel : group->pel;
        unsigned char startkey[sizeof(streamID)];
        unsigned char endkey[sizeof(streamID)];
        raxIterator ri;
        mstime_t now = mstime();

        streamEncodeID(startkey,&startid);
        streamEncodeID(endkey,&endid);
        raxStart(&ri,pel);
        raxSeek(&ri,">=",startkey,sizeof(startkey));
        void *arraylen_ptr = addReplyDeferredLen(c);
        size_t arraylen = 0;

        while(count && raxNext(&ri) && memcmp(ri.key,endkey,ri.key_len) <= 0) {
            streamNACK *nack = ri.data;

            if (minidle) {
                mstime_t this_idle = now - nack->delivery_time;
                if (this_idle < minidle) continue;
            }

            arraylen++;
            count--;
            addReplyArrayLen(c,4);

            /* Entry ID. */
            streamID id;
            streamDecodeID(ri.key,&id);
            addReplyStreamID(c,&id);

            /* Consumer name. */
            addReplyBulkCBuffer(c,nack->consumer->name,
                                sdslen(nack->consumer->name));

            /* Milliseconds elapsed since last delivery. */
            mstime_t elapsed = now - nack->delivery_time;
            if (elapsed < 0) elapsed = 0;
            addReplyLongLong(c,elapsed);

            /* Number of deliveries. */
            addReplyLongLong(c,nack->delivery_count);
        }
        raxStop(&ri);
        setDeferredArrayLen(c,arraylen_ptr,arraylen);
    }
}

/* XCLAIM <key> <group> <consumer> <min-idle-time> <ID-1> <ID-2>
 *        [IDLE <milliseconds>] [TIME <mstime>] [RETRYCOUNT <count>]
 *        [FORCE] [JUSTID]
 *
 * Gets ownership of one or multiple messages in the Pending Entries List
 * of a given stream consumer group.
 *
 * If the message ID (among the specified ones) exists, and its idle
 * time greater or equal to <min-idle-time>, then the message new owner
 * becomes the specified <consumer>. If the minimum idle time specified
 * is zero, messages are claimed regardless of their idle time.
 *
 * All the messages that cannot be found inside the pending entries list
 * are ignored, but in case the FORCE option is used. In that case we
 * create the NACK (representing a not yet acknowledged message) entry in
 * the consumer group PEL.
 *
 * This command creates the consumer as side effect if it does not yet
 * exists. Moreover the command reset the idle time of the message to 0,
 * even if by using the IDLE or TIME options, the user can control the
 * new idle time.
 *
 * The options at the end can be used in order to specify more attributes
 * to set in the representation of the pending message:
 *
 * 1. IDLE <ms>:
 *      Set the idle time (last time it was delivered) of the message.
 *      If IDLE is not specified, an IDLE of 0 is assumed, that is,
 *      the time count is reset because the message has now a new
 *      owner trying to process it.
 *
 * 2. TIME <ms-unix-time>:
 *      This is the same as IDLE but instead of a relative amount of
 *      milliseconds, it sets the idle time to a specific unix time
 *      (in milliseconds). This is useful in order to rewrite the AOF
 *      file generating XCLAIM commands.
 *
 * 3. RETRYCOUNT <count>:
 *      Set the retry counter to the specified value. This counter is
 *      incremented every time a message is delivered again. Normally
 *      XCLAIM does not alter this counter, which is just served to clients
 *      when the XPENDING command is called: this way clients can detect
 *      anomalies, like messages that are never processed for some reason
 *      after a big number of delivery attempts.
 *
 * 4. FORCE:
 *      Creates the pending message entry in the PEL even if certain
 *      specified IDs are not already in the PEL assigned to a different
 *      client. However the message must be exist in the stream, otherwise
 *      the IDs of non existing messages are ignored.
 *
 * 5. JUSTID:
 *      Return just an array of IDs of messages successfully claimed,
 *      without returning the actual message.
 *
 * 6. LASTID <id>:
 *      Update the consumer group last ID with the specified ID if the
 *      current last ID is smaller than the provided one.
 *      This is used for replication / AOF, so that when we read from a
 *      consumer group, the XCLAIM that gets propagated to give ownership
 *      to the consumer, is also used in order to update the group current
 *      ID.
 *
 * The command returns an array of messages that the user
 * successfully claimed, so that the caller is able to understand
 * what messages it is now in charge of. */
void xclaimCommand(client *c) {
    streamCG *group = NULL;
    robj *o = lookupKeyRead(c->db,c->argv[1]);
    long long minidle; /* Minimum idle time argument. */
    long long retrycount = -1;   /* -1 means RETRYCOUNT option not given. */
    mstime_t deliverytime = -1;  /* -1 means IDLE/TIME options not given. */
    int force = 0;
    int justid = 0;

    if (o) {
        if (checkType(c,o,OBJ_STREAM)) return; /* Type error. */
        group = streamLookupCG(o->ptr,c->argv[2]->ptr);
    }

    /* No key or group? Send an error given that the group creation
     * is mandatory. */
    if (o == NULL || group == NULL) {
        addReplyErrorFormat(c,"-NOGROUP No such key '%s' or "
                              "consumer group '%s'", (char*)c->argv[1]->ptr,
                              (char*)c->argv[2]->ptr);
        return;
    }

    if (getLongLongFromObjectOrReply(c,c->argv[4],&minidle,
        "Invalid min-idle-time argument for XCLAIM")
        != C_OK) return;
    if (minidle < 0) minidle = 0;

    /* Start parsing the IDs, so that we abort ASAP if there is a syntax
     * error: the return value of this command cannot be an error in case
     * the client successfully claimed some message, so it should be
     * executed in a "all or nothing" fashion. */
    int j;
    streamID static_ids[STREAMID_STATIC_VECTOR_LEN];
    streamID *ids = static_ids;
    int id_count = c->argc-5;
    if (id_count > STREAMID_STATIC_VECTOR_LEN)
        ids = zmalloc(sizeof(streamID)*id_count);
    for (j = 5; j < c->argc; j++) {
        if (streamParseStrictIDOrReply(NULL,c->argv[j],&ids[j-5],0) != C_OK) break;
    }
    int last_id_arg = j-1; /* Next time we iterate the IDs we now the range. */

    /* If we stopped because some IDs cannot be parsed, perhaps they
     * are trailing options. */
    mstime_t now = mstime();
    streamID last_id = {0,0};
    int propagate_last_id = 0;
    for (; j < c->argc; j++) {
        int moreargs = (c->argc-1) - j; /* Number of additional arguments. */
        char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt,"FORCE")) {
            force = 1;
        } else if (!strcasecmp(opt,"JUSTID")) {
            justid = 1;
        } else if (!strcasecmp(opt,"IDLE") && moreargs) {
            j++;
            if (getLongLongFromObjectOrReply(c,c->argv[j],&deliverytime,
                "Invalid IDLE option argument for XCLAIM")
                != C_OK) goto cleanup;
            deliverytime = now - deliverytime;
        } else if (!strcasecmp(opt,"TIME") && moreargs) {
            j++;
            if (getLongLongFromObjectOrReply(c,c->argv[j],&deliverytime,
                "Invalid TIME option argument for XCLAIM")
                != C_OK) goto cleanup;
        } else if (!strcasecmp(opt,"RETRYCOUNT") && moreargs) {
            j++;
            if (getLongLongFromObjectOrReply(c,c->argv[j],&retrycount,
                "Invalid RETRYCOUNT option argument for XCLAIM")
                != C_OK) goto cleanup;
        } else if (!strcasecmp(opt,"LASTID") && moreargs) {
            j++;
            if (streamParseStrictIDOrReply(c,c->argv[j],&last_id,0) != C_OK) goto cleanup;
        } else {
            addReplyErrorFormat(c,"Unrecognized XCLAIM option '%s'",opt);
            goto cleanup;
        }
    }

    if (streamCompareID(&last_id,&group->last_id) > 0) {
        group->last_id = last_id;
        propagate_last_id = 1;
    }

    if (deliverytime != -1) {
        /* If a delivery time was passed, either with IDLE or TIME, we
         * do some sanity check on it, and set the deliverytime to now
         * (which is a sane choice usually) if the value is bogus.
         * To raise an error here is not wise because clients may compute
         * the idle time doing some math starting from their local time,
         * and this is not a good excuse to fail in case, for instance,
         * the computer time is a bit in the future from our POV. */
        if (deliverytime < 0 || deliverytime > now) deliverytime = now;
    } else {
        /* If no IDLE/TIME option was passed, we want the last delivery
         * time to be now, so that the idle time of the message will be
         * zero. */
        deliverytime = now;
    }

    /* Do the actual claiming. */
    streamConsumer *consumer = NULL;
    void *arraylenptr = addReplyDeferredLen(c);
    size_t arraylen = 0;
    for (int j = 5; j <= last_id_arg; j++) {
        streamID id = ids[j-5];
        unsigned char buf[sizeof(streamID)];
        streamEncodeID(buf,&id);

        /* Lookup the ID in the group PEL. */
        streamNACK *nack = raxFind(group->pel,buf,sizeof(buf));

        /* If FORCE is passed, let's check if at least the entry
         * exists in the Stream. In such case, we'll crate a new
         * entry in the PEL from scratch, so that XCLAIM can also
         * be used to create entries in the PEL. Useful for AOF
         * and replication of consumer groups. */
        if (force && nack == raxNotFound) {
            streamIterator myiterator;
            streamIteratorStart(&myiterator,o->ptr,&id,&id,0);
            int64_t numfields;
            int found = 0;
            streamID item_id;
            if (streamIteratorGetID(&myiterator,&item_id,&numfields)) found = 1;
            streamIteratorStop(&myiterator);

            /* Item must exist for us to create a NACK for it. */
            if (!found) continue;

            /* Create the NACK. */
            nack = streamCreateNACK(NULL);
            raxInsert(group->pel,buf,sizeof(buf),nack,NULL);
        }

        if (nack != raxNotFound) {
            /* We need to check if the minimum idle time requested
             * by the caller is satisfied by this entry.
             *
             * Note that the nack could be created by FORCE, in this
             * case there was no pre-existing entry and minidle should
             * be ignored, but in that case nack->consumer is NULL. */
            if (nack->consumer && minidle) {
                mstime_t this_idle = now - nack->delivery_time;
                if (this_idle < minidle) continue;
            }
            if (consumer == NULL)
                consumer = streamLookupConsumer(group,c->argv[3]->ptr,SLC_NONE,NULL);
            if (nack->consumer != consumer) {
                /* Remove the entry from the old consumer.
                 * Note that nack->consumer is NULL if we created the
                 * NACK above because of the FORCE option. */
                if (nack->consumer)
                    raxRemove(nack->consumer->pel,buf,sizeof(buf),NULL);
            }
            nack->delivery_time = deliverytime;
            /* Set the delivery attempts counter if given, otherwise
             * autoincrement unless JUSTID option provided */
            if (retrycount >= 0) {
                nack->delivery_count = retrycount;
            } else if (!justid) {
                nack->delivery_count++;
            }
            if (nack->consumer != consumer) {
                /* Add the entry in the new consumer local PEL. */
                raxInsert(consumer->pel,buf,sizeof(buf),nack,NULL);
                nack->consumer = consumer;
            }
            /* Send the reply for this entry. */
            if (justid) {
                addReplyStreamID(c,&id);
            } else {
                size_t emitted = streamReplyWithRange(c,o->ptr,&id,&id,1,0,
                                    NULL,NULL,STREAM_RWR_RAWENTRIES,NULL);
                if (!emitted) addReplyNull(c);
            }
            arraylen++;

            /* Propagate this change. */
            streamPropagateXCLAIM(c,c->argv[1],group,c->argv[2],c->argv[j],nack);
            propagate_last_id = 0; /* Will be propagated by XCLAIM itself. */
            server.dirty++;
        }
    }
    if (propagate_last_id) {
        streamPropagateGroupID(c,c->argv[1],group,c->argv[2]);
        server.dirty++;
    }
    setDeferredArrayLen(c,arraylenptr,arraylen);
    preventCommandPropagation(c);
cleanup:
    if (ids != static_ids) zfree(ids);
}

/* XAUTOCLAIM <key> <group> <consumer> <min-idle-time> <start> [COUNT <count>] [JUSTID]
 *
 * Gets ownership of one or multiple messages in the Pending Entries List
 * of a given stream consumer group.
 *
 * For each PEL entry, if its idle time greater or equal to <min-idle-time>,
 * then the message new owner becomes the specified <consumer>.
 * If the minimum idle time specified is zero, messages are claimed
 * regardless of their idle time.
 *
 * This command creates the consumer as side effect if it does not yet
 * exists. Moreover the command reset the idle time of the message to 0.
 *
 * The command returns an array of messages that the user
 * successfully claimed, so that the caller is able to understand
 * what messages it is now in charge of. */
void xautoclaimCommand(client *c) {
    streamCG *group = NULL;
    robj *o = lookupKeyRead(c->db,c->argv[1]);
    long long minidle; /* Minimum idle time argument, in milliseconds. */
    long count = 100; /* Maximum entries to claim. */
    streamID startid;
    int startex;
    int justid = 0;

    /* Parse idle/start/end/count arguments ASAP if needed, in order to report
     * syntax errors before any other error. */
    if (getLongLongFromObjectOrReply(c,c->argv[4],&minidle,"Invalid min-idle-time argument for XAUTOCLAIM") != C_OK)
        return;
    if (minidle < 0) minidle = 0;

    if (streamParseIntervalIDOrReply(c,c->argv[5],&startid,&startex,0) != C_OK)
        return;
    if (startex && streamIncrID(&startid) != C_OK) {
        addReplyError(c,"invalid start ID for the interval");
        return;
    }

    int j = 6; /* options start at argv[6] */
    while(j < c->argc) {
        int moreargs = (c->argc-1) - j; /* Number of additional arguments. */
        char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt,"COUNT") && moreargs) {
            if (getRangeLongFromObjectOrReply(c,c->argv[j+1],1,LONG_MAX,&count,"COUNT must be > 0") != C_OK)
                return;
            j++;
        } else if (!strcasecmp(opt,"JUSTID")) {
            justid = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
        j++;
    }

    if (o) {
        if (checkType(c,o,OBJ_STREAM))
            return; /* Type error. */
        group = streamLookupCG(o->ptr,c->argv[2]->ptr);
    }

    /* No key or group? Send an error given that the group creation
     * is mandatory. */
    if (o == NULL || group == NULL) {
        addReplyErrorFormat(c,"-NOGROUP No such key '%s' or consumer group '%s'",
                            (char*)c->argv[1]->ptr,
                            (char*)c->argv[2]->ptr);
        return;
    }

    /* Do the actual claiming. */
    streamConsumer *consumer = NULL;
    long long attempts = count*10;

    addReplyArrayLen(c, 2);
    void *endidptr = addReplyDeferredLen(c);
    void *arraylenptr = addReplyDeferredLen(c);

    unsigned char startkey[sizeof(streamID)];
    streamEncodeID(startkey,&startid);
    raxIterator ri;
    raxStart(&ri,group->pel);
    raxSeek(&ri,">=",startkey,sizeof(startkey));
    size_t arraylen = 0;
    mstime_t now = mstime();
    while (attempts-- && count && raxNext(&ri)) {
        streamNACK *nack = ri.data;

        if (minidle) {
            mstime_t this_idle = now - nack->delivery_time;
            if (this_idle < minidle)
                continue;
        }

        streamID id;
        streamDecodeID(ri.key, &id);

        if (consumer == NULL)
            consumer = streamLookupConsumer(group,c->argv[3]->ptr,SLC_NONE,NULL);
        if (nack->consumer != consumer) {
            /* Remove the entry from the old consumer.
             * Note that nack->consumer is NULL if we created the
             * NACK above because of the FORCE option. */
            if (nack->consumer)
                raxRemove(nack->consumer->pel,ri.key,ri.key_len,NULL);
        }

        /* Update the consumer and idle time. */
        nack->delivery_time = now;
        /* Increment the delivery attempts counter unless JUSTID option provided */
        if (!justid)
            nack->delivery_count++;

        if (nack->consumer != consumer) {
            /* Add the entry in the new consumer local PEL. */
            raxInsert(consumer->pel,ri.key,ri.key_len,nack,NULL);
            nack->consumer = consumer;
        }

        /* Send the reply for this entry. */
        if (justid) {
            addReplyStreamID(c,&id);
        } else {
            size_t emitted =
                streamReplyWithRange(c,o->ptr,&id,&id,1,0,NULL,NULL,
                                     STREAM_RWR_RAWENTRIES,NULL);
            if (!emitted)
                addReplyNull(c);
        }
        arraylen++;
        count--;

        /* Propagate this change. */
        robj *idstr = createObjectFromStreamID(&id);
        streamPropagateXCLAIM(c,c->argv[1],group,c->argv[2],idstr,nack);
        decrRefCount(idstr);
        server.dirty++;
    }

    /* We need to return the next entry as a cursor for the next XAUTOCLAIM call */
    raxNext(&ri);

    streamID endid;
    if (raxEOF(&ri)) {
        endid.ms = endid.seq = 0;
    } else {
        streamDecodeID(ri.key, &endid);
    }
    raxStop(&ri);

    setDeferredArrayLen(c,arraylenptr,arraylen);
    setDeferredReplyStreamID(c,endidptr,&endid);

    preventCommandPropagation(c);
}

/* XDEL <key> [<ID1> <ID2> ... <IDN>]
 *
 * Removes the specified entries from the stream. Returns the number
 * of items actually deleted, that may be different from the number
 * of IDs passed in case certain IDs do not exist. */
void xdelCommand(client *c) {
    robj *o;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL
        || checkType(c,o,OBJ_STREAM)) return;
    stream *s = o->ptr;

    /* We need to sanity check the IDs passed to start. Even if not
     * a big issue, it is not great that the command is only partially
     * executed because at some point an invalid ID is parsed. */
    streamID static_ids[STREAMID_STATIC_VECTOR_LEN];
    streamID *ids = static_ids;
    int id_count = c->argc-2;
    if (id_count > STREAMID_STATIC_VECTOR_LEN)
        ids = zmalloc(sizeof(streamID)*id_count);
    for (int j = 2; j < c->argc; j++) {
        if (streamParseStrictIDOrReply(c,c->argv[j],&ids[j-2],0) != C_OK) goto cleanup;
    }

    /* Actually apply the command. */
    int deleted = 0;
    for (int j = 2; j < c->argc; j++) {
        deleted += streamDeleteItem(s,&ids[j-2]);
    }

    /* Propagate the write if needed. */
    if (deleted) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STREAM,"xdel",c->argv[1],c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
cleanup:
    if (ids != static_ids) zfree(ids);
}

/* General form: XTRIM <key> [... options ...]
 *
 * List of options:
 *
 * Trim strategies:
 *
 * MAXLEN [~|=] <count>     -- Trim so that the stream will be capped at
 *                             the specified length. Use ~ before the
 *                             count in order to demand approximated trimming
 *                             (like XADD MAXLEN option).
 * MINID [~|=] <id>         -- Trim so that the stream will not contain entries
 *                             with IDs smaller than 'id'. Use ~ before the
 *                             count in order to demand approximated trimming
 *                             (like XADD MINID option).
 *
 * Other options:
 *
 * LIMIT <entries>          -- The maximum number of entries to trim.
 *                             0 means unlimited. Unless specified, it is set
 *                             to a default of 100*server.stream_node_max_entries,
 *                             and that's in order to keep the trimming time sane.
 *                             Has meaning only if `~` was provided.
 */
void xtrimCommand(client *c) {
    robj *o;

    /* Argument parsing. */
    streamAddTrimArgs parsed_args;
    if (streamParseAddOrTrimArgsOrReply(c, &parsed_args, 0) < 0)
        return; /* streamParseAddOrTrimArgsOrReply already replied. */

    /* If the key does not exist, we are ok returning zero, that is, the
     * number of elements removed from the stream. */
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL
        || checkType(c,o,OBJ_STREAM)) return;
    stream *s = o->ptr;

    /* Perform the trimming. */
    int64_t deleted = streamTrim(s, &parsed_args);
    if (deleted) {
        notifyKeyspaceEvent(NOTIFY_STREAM,"xtrim",c->argv[1],c->db->id);
        if (parsed_args.approx_trim) {
            /* In case our trimming was limited (by LIMIT or by ~) we must
             * re-write the relevant trim argument to make sure there will be
             * no inconsistencies in AOF loading or in the replica.
             * It's enough to check only args->approx because there is no
             * way LIMIT is given without the ~ option. */
            streamRewriteApproxSpecifier(c,parsed_args.trim_strategy_arg_idx-1);
            streamRewriteTrimArgument(c,s,parsed_args.trim_strategy,parsed_args.trim_strategy_arg_idx);
        }

        /* Propagate the write. */
        signalModifiedKey(c, c->db,c->argv[1]);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

/* Helper function for xinfoCommand.
 * Handles the variants of XINFO STREAM */
void xinfoReplyWithStreamInfo(client *c, stream *s) {
    int full = 1;
    long long count = 10; /* Default COUNT is 10 so we don't block the server */
    robj **optv = c->argv + 3; /* Options start after XINFO STREAM <key> */
    int optc = c->argc - 3;

    /* Parse options. */
    if (optc == 0) {
        full = 0;
    } else {
        /* Valid options are [FULL] or [FULL COUNT <count>] */
        if (optc != 1 && optc != 3) {
            addReplySubcommandSyntaxError(c);
            return;
        }

        /* First option must be "FULL" */
        if (strcasecmp(optv[0]->ptr,"full")) {
            addReplySubcommandSyntaxError(c);
            return;
        }

        if (optc == 3) {
            /* First option must be "FULL" */
            if (strcasecmp(optv[1]->ptr,"count")) {
                addReplySubcommandSyntaxError(c);
                return;
            }
            if (getLongLongFromObjectOrReply(c,optv[2],&count,NULL) == C_ERR)
                return;
            if (count < 0) count = 10;
        }
    }

    addReplyMapLen(c,full ? 6 : 7);
    addReplyBulkCString(c,"length");
    addReplyLongLong(c,s->length);
    addReplyBulkCString(c,"radix-tree-keys");
    addReplyLongLong(c,raxSize(s->rax));
    addReplyBulkCString(c,"radix-tree-nodes");
    addReplyLongLong(c,s->rax->numnodes);
    addReplyBulkCString(c,"last-generated-id");
    addReplyStreamID(c,&s->last_id);

    if (!full) {
        /* XINFO STREAM <key> */

        addReplyBulkCString(c,"groups");
        addReplyLongLong(c,s->cgroups ? raxSize(s->cgroups) : 0);

        /* To emit the first/last entry we use streamReplyWithRange(). */
        int emitted;
        streamID start, end;
        start.ms = start.seq = 0;
        end.ms = end.seq = UINT64_MAX;
        addReplyBulkCString(c,"first-entry");
        emitted = streamReplyWithRange(c,s,&start,&end,1,0,NULL,NULL,
                                       STREAM_RWR_RAWENTRIES,NULL);
        if (!emitted) addReplyNull(c);
        addReplyBulkCString(c,"last-entry");
        emitted = streamReplyWithRange(c,s,&start,&end,1,1,NULL,NULL,
                                       STREAM_RWR_RAWENTRIES,NULL);
        if (!emitted) addReplyNull(c);
    } else {
        /* XINFO STREAM <key> FULL [COUNT <count>] */

        /* Stream entries */
        addReplyBulkCString(c,"entries");
        streamReplyWithRange(c,s,NULL,NULL,count,0,NULL,NULL,0,NULL);

        /* Consumer groups */
        addReplyBulkCString(c,"groups");
        if (s->cgroups == NULL) {
            addReplyArrayLen(c,0);
        } else {
            addReplyArrayLen(c,raxSize(s->cgroups));
            raxIterator ri_cgroups;
            raxStart(&ri_cgroups,s->cgroups);
            raxSeek(&ri_cgroups,"^",NULL,0);
            while(raxNext(&ri_cgroups)) {
                streamCG *cg = ri_cgroups.data;
                addReplyMapLen(c,5);

                /* Name */
                addReplyBulkCString(c,"name");
                addReplyBulkCBuffer(c,ri_cgroups.key,ri_cgroups.key_len);

                /* Last delivered ID */
                addReplyBulkCString(c,"last-delivered-id");
                addReplyStreamID(c,&cg->last_id);

                /* Group PEL count */
                addReplyBulkCString(c,"pel-count");
                addReplyLongLong(c,raxSize(cg->pel));

                /* Group PEL */
                addReplyBulkCString(c,"pending");
                long long arraylen_cg_pel = 0;
                void *arrayptr_cg_pel = addReplyDeferredLen(c);
                raxIterator ri_cg_pel;
                raxStart(&ri_cg_pel,cg->pel);
                raxSeek(&ri_cg_pel,"^",NULL,0);
                while(raxNext(&ri_cg_pel) && (!count || arraylen_cg_pel < count)) {
                    streamNACK *nack = ri_cg_pel.data;
                    addReplyArrayLen(c,4);

                    /* Entry ID. */
                    streamID id;
                    streamDecodeID(ri_cg_pel.key,&id);
                    addReplyStreamID(c,&id);

                    /* Consumer name. */
                    serverAssert(nack->consumer); /* assertion for valgrind (avoid NPD) */
                    addReplyBulkCBuffer(c,nack->consumer->name,
                                        sdslen(nack->consumer->name));

                    /* Last delivery. */
                    addReplyLongLong(c,nack->delivery_time);

                    /* Number of deliveries. */
                    addReplyLongLong(c,nack->delivery_count);

                    arraylen_cg_pel++;
                }
                setDeferredArrayLen(c,arrayptr_cg_pel,arraylen_cg_pel);
                raxStop(&ri_cg_pel);

                /* Consumers */
                addReplyBulkCString(c,"consumers");
                addReplyArrayLen(c,raxSize(cg->consumers));
                raxIterator ri_consumers;
                raxStart(&ri_consumers,cg->consumers);
                raxSeek(&ri_consumers,"^",NULL,0);
                while(raxNext(&ri_consumers)) {
                    streamConsumer *consumer = ri_consumers.data;
                    addReplyMapLen(c,4);

                    /* Consumer name */
                    addReplyBulkCString(c,"name");
                    addReplyBulkCBuffer(c,consumer->name,sdslen(consumer->name));

                    /* Seen-time */
                    addReplyBulkCString(c,"seen-time");
                    addReplyLongLong(c,consumer->seen_time);

                    /* Consumer PEL count */
                    addReplyBulkCString(c,"pel-count");
                    addReplyLongLong(c,raxSize(consumer->pel));

                    /* Consumer PEL */
                    addReplyBulkCString(c,"pending");
                    long long arraylen_cpel = 0;
                    void *arrayptr_cpel = addReplyDeferredLen(c);
                    raxIterator ri_cpel;
                    raxStart(&ri_cpel,consumer->pel);
                    raxSeek(&ri_cpel,"^",NULL,0);
                    while(raxNext(&ri_cpel) && (!count || arraylen_cpel < count)) {
                        streamNACK *nack = ri_cpel.data;
                        addReplyArrayLen(c,3);

                        /* Entry ID. */
                        streamID id;
                        streamDecodeID(ri_cpel.key,&id);
                        addReplyStreamID(c,&id);

                        /* Last delivery. */
                        addReplyLongLong(c,nack->delivery_time);

                        /* Number of deliveries. */
                        addReplyLongLong(c,nack->delivery_count);

                        arraylen_cpel++;
                    }
                    setDeferredArrayLen(c,arrayptr_cpel,arraylen_cpel);
                    raxStop(&ri_cpel);
                }
                raxStop(&ri_consumers);
            }
            raxStop(&ri_cgroups);
        }
    }
}

/* XINFO CONSUMERS <key> <group>
 * XINFO GROUPS <key>
 * XINFO STREAM <key> [FULL [COUNT <count>]]
 * XINFO HELP. */
void xinfoCommand(client *c) {
    stream *s = NULL;
    char *opt;
    robj *key;

    /* HELP is special. Handle it ASAP. */
    if (!strcasecmp(c->argv[1]->ptr,"HELP")) {
        const char *help[] = {
"CONSUMERS <key> <groupname>",
"    Show consumers of <groupname>.",
"GROUPS <key>",
"    Show the stream consumer groups.",
"STREAM <key> [FULL [COUNT <count>]",
"    Show information about the stream.",
NULL
        };
        addReplyHelp(c, help);
        return;
    } else if (c->argc < 3) {
        addReplySubcommandSyntaxError(c);
        return;
    }

    /* With the exception of HELP handled before any other sub commands, all
     * the ones are in the form of "<subcommand> <key>". */
    opt = c->argv[1]->ptr;
    key = c->argv[2];

    /* Lookup the key now, this is common for all the subcommands but HELP. */
    robj *o = lookupKeyReadOrReply(c,key,shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_STREAM)) return;
    s = o->ptr;

    /* Dispatch the different subcommands. */
    if (!strcasecmp(opt,"CONSUMERS") && c->argc == 4) {
        /* XINFO CONSUMERS <key> <group>. */
        streamCG *cg = streamLookupCG(s,c->argv[3]->ptr);
        if (cg == NULL) {
            addReplyErrorFormat(c, "-NOGROUP No such consumer group '%s' "
                                   "for key name '%s'",
                                   (char*)c->argv[3]->ptr, (char*)key->ptr);
            return;
        }

        addReplyArrayLen(c,raxSize(cg->consumers));
        raxIterator ri;
        raxStart(&ri,cg->consumers);
        raxSeek(&ri,"^",NULL,0);
        mstime_t now = mstime();
        while(raxNext(&ri)) {
            streamConsumer *consumer = ri.data;
            mstime_t idle = now - consumer->seen_time;
            if (idle < 0) idle = 0;

            addReplyMapLen(c,3);
            addReplyBulkCString(c,"name");
            addReplyBulkCBuffer(c,consumer->name,sdslen(consumer->name));
            addReplyBulkCString(c,"pending");
            addReplyLongLong(c,raxSize(consumer->pel));
            addReplyBulkCString(c,"idle");
            addReplyLongLong(c,idle);
        }
        raxStop(&ri);
    } else if (!strcasecmp(opt,"GROUPS") && c->argc == 3) {
        /* XINFO GROUPS <key>. */
        if (s->cgroups == NULL) {
            addReplyArrayLen(c,0);
            return;
        }

        addReplyArrayLen(c,raxSize(s->cgroups));
        raxIterator ri;
        raxStart(&ri,s->cgroups);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            streamCG *cg = ri.data;
            addReplyMapLen(c,4);
            addReplyBulkCString(c,"name");
            addReplyBulkCBuffer(c,ri.key,ri.key_len);
            addReplyBulkCString(c,"consumers");
            addReplyLongLong(c,raxSize(cg->consumers));
            addReplyBulkCString(c,"pending");
            addReplyLongLong(c,raxSize(cg->pel));
            addReplyBulkCString(c,"last-delivered-id");
            addReplyStreamID(c,&cg->last_id);
        }
        raxStop(&ri);
    } else if (!strcasecmp(opt,"STREAM")) {
        /* XINFO STREAM <key> [FULL [COUNT <count>]]. */
        xinfoReplyWithStreamInfo(c,s);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* Validate the integrity stream listpack entries structure. Both in term of a
 * valid listpack, but also that the structure of the entires matches a valid
 * stream. return 1 if valid 0 if not valid. */
int streamValidateListpackIntegrity(unsigned char *lp, size_t size, int deep) {
    int valid_record;
    unsigned char *p, *next;

    /* Since we don't want to run validation of all records twice, we'll
     * run the listpack validation of just the header and do the rest here. */
    if (!lpValidateIntegrity(lp, size, 0))
        return 0;

    /* In non-deep mode we just validated the listpack header (encoded size) */
    if (!deep) return 1;

    next = p = lpValidateFirst(lp);
    if (!lpValidateNext(lp, &next, size)) return 0;
    if (!p) return 0;

    /* entry count */
    int64_t entry_count = lpGetIntegerIfValid(p, &valid_record);
    if (!valid_record) return 0;
    p = next; if (!lpValidateNext(lp, &next, size)) return 0;

    /* deleted */
    int64_t deleted_count = lpGetIntegerIfValid(p, &valid_record);
    if (!valid_record) return 0;
    p = next; if (!lpValidateNext(lp, &next, size)) return 0;

    /* num-of-fields */
    int64_t master_fields = lpGetIntegerIfValid(p, &valid_record);
    if (!valid_record) return 0;
    p = next; if (!lpValidateNext(lp, &next, size)) return 0;

    /* the field names */
    for (int64_t j = 0; j < master_fields; j++) {
        p = next; if (!lpValidateNext(lp, &next, size)) return 0;
    }

    /* the zero master entry terminator. */
    int64_t zero = lpGetIntegerIfValid(p, &valid_record);
    if (!valid_record || zero != 0) return 0;
    p = next; if (!lpValidateNext(lp, &next, size)) return 0;

    entry_count += deleted_count;
    while (entry_count--) {
        if (!p) return 0;
        int64_t fields = master_fields, extra_fields = 3;
        int64_t flags = lpGetIntegerIfValid(p, &valid_record);
        if (!valid_record) return 0;
        p = next; if (!lpValidateNext(lp, &next, size)) return 0;

        /* entry id */
        lpGetIntegerIfValid(p, &valid_record);
        if (!valid_record) return 0;
        p = next; if (!lpValidateNext(lp, &next, size)) return 0;
        lpGetIntegerIfValid(p, &valid_record);
        if (!valid_record) return 0;
        p = next; if (!lpValidateNext(lp, &next, size)) return 0;

        if (!(flags & STREAM_ITEM_FLAG_SAMEFIELDS)) {
            /* num-of-fields */
            fields = lpGetIntegerIfValid(p, &valid_record);
            if (!valid_record) return 0;
            p = next; if (!lpValidateNext(lp, &next, size)) return 0;

            /* the field names */
            for (int64_t j = 0; j < fields; j++) {
                p = next; if (!lpValidateNext(lp, &next, size)) return 0;
            }

            extra_fields += fields + 1;
        }

        /* the values */
        for (int64_t j = 0; j < fields; j++) {
            p = next; if (!lpValidateNext(lp, &next, size)) return 0;
        }

        /* lp-count */
        int64_t lp_count = lpGetIntegerIfValid(p, &valid_record);
        if (!valid_record) return 0;
        if (lp_count != fields + extra_fields) return 0;
        p = next; if (!lpValidateNext(lp, &next, size)) return 0;
    }

    if (next)
        return 0;

    return 1;
}
