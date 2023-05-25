/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* Check the length and size of a number of objects that will be added to list to see
 * if we need to convert a listpack to a quicklist. Note that we only check string
 * encoded objects as their string length can be queried in constant time.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertListpack(robj *o, robj **argv, int start, int end,
                                       beforeConvertCB fn, void *data)
{
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    size_t add_bytes = 0;
    size_t add_length = 0;

    if (argv) {
        for (int i = start; i <= end; i++) {
            if (!sdsEncodedObject(argv[i]))
                continue;
            add_bytes += sdslen(argv[i]->ptr);
        }
        add_length = end - start + 1;
    }

    if (quicklistNodeExceedsLimit(server.list_max_listpack_size,
            lpBytes(o->ptr) + add_bytes, lpLength(o->ptr) + add_length))
    {
        /* Invoke callback before conversion. */
        if (fn) fn(data);

        quicklist *ql = quicklistCreate();
        quicklistSetOptions(ql, server.list_max_listpack_size, server.list_compress_depth);

        /* Append listpack to quicklist if it's not empty, otherwise release it. */
        if (lpLength(o->ptr))
            quicklistAppendListpack(ql, o->ptr);
        else
            lpFree(o->ptr);
        o->ptr = ql;
        o->encoding = OBJ_ENCODING_QUICKLIST;
    }
}

/* Check the length and size of a quicklist to see if we need to convert it to listpack.
 *
 * 'shrinking' is 1 means that the conversion is due to a list shrinking, to avoid
 * frequent conversions of quicklist and listpack due to frequent insertion and
 * deletion, we don't convert quicklist to listpack until its length or size is
 * below half of the limit.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertQuicklist(robj *o, int shrinking, beforeConvertCB fn, void *data) {
    serverAssert(o->encoding == OBJ_ENCODING_QUICKLIST);

    size_t sz_limit;
    unsigned int count_limit;
    quicklist *ql = o->ptr;

    /* A quicklist can be converted to listpack only if it has only one packed node. */
    if (ql->len != 1 || ql->head->container != QUICKLIST_NODE_CONTAINER_PACKED)
        return;

    /* Check the length or size of the quicklist is below the limit. */
    quicklistNodeLimit(server.list_max_listpack_size, &sz_limit, &count_limit);
    if (shrinking) {
        sz_limit /= 2;
        count_limit /= 2;
    }
    if (ql->head->sz > sz_limit || ql->count > count_limit) return;

    /* Invoke callback before conversion. */
    if (fn) fn(data);

    /* Extract the listpack from the unique quicklist node,
     * then reset it and release the quicklist. */
    o->ptr = ql->head->entry;
    ql->head->entry = NULL;
    quicklistRelease(ql);
    o->encoding = OBJ_ENCODING_LISTPACK;
}

/* Check if the list needs to be converted to appropriate encoding due to
 * growing, shrinking or other cases.
 *
 * 'lct' can be one of the following values:
 * LIST_CONV_AUTO      - Used after we built a new list, and we want to let the
 *                       function decide on the best encoding for that list.
 * LIST_CONV_GROWING   - Used before or right after adding elements to the list,
 *                       in which case we are likely to only consider converting
 *                       from listpack to quicklist.
 *                       'argv' is only used in this case to calculate the size
 *                       of a number of objects that will be added to list.
 * LIST_CONV_SHRINKING - Used after removing an element from the list, in which case we
 *                       wanna consider converting from quicklist to listpack. When we
 *                       know we're shrinking, we use a lower (more strict) threshold in
 *                       order to avoid repeated conversions on every list change. */
static void listTypeTryConversionRaw(robj *o, list_conv_type lct,
                                     robj **argv, int start, int end,
                                     beforeConvertCB fn, void *data)
{
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        if (lct == LIST_CONV_GROWING) return; /* Growing has nothing to do with quicklist */
        listTypeTryConvertQuicklist(o, lct == LIST_CONV_SHRINKING, fn, data);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (lct == LIST_CONV_SHRINKING) return; /* Shrinking has nothing to do with listpack */
        listTypeTryConvertListpack(o, argv, start, end, fn, data);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion without passing 'argv'. */
void listTypeTryConversion(robj *o, list_conv_type lct, beforeConvertCB fn, void *data) {
    listTypeTryConversionRaw(o, lct, NULL, 0, 0, fn, data);
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion before adding elements to the list. */
void listTypeTryConversionAppend(robj *o, robj **argv, int start, int end,
                                 beforeConvertCB fn, void *data)
{
    listTypeTryConversionRaw(o, LIST_CONV_GROWING, argv, start, end, fn, data);
}

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        if (value->encoding == OBJ_ENCODING_INT) {
            char buf[32];
            ll2string(buf, 32, (long)value->ptr);
            quicklistPush(subject->ptr, buf, strlen(buf), pos);
        } else {
            quicklistPush(subject->ptr, value->ptr, sdslen(value->ptr), pos);
        }
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        if (value->encoding == OBJ_ENCODING_INT) {
            subject->ptr = (where == LIST_HEAD) ?
                lpPrependInteger(subject->ptr, (long)value->ptr) :
                lpAppendInteger(subject->ptr, (long)value->ptr);
        } else {
            subject->ptr = (where == LIST_HEAD) ?
                lpPrepend(subject->ptr, value->ptr, sdslen(value->ptr)) :
                lpAppend(subject->ptr, value->ptr, sdslen(value->ptr));
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, size_t sz) {
    return createStringObject((char*)data,sz);
}

robj *listTypePop(robj *subject, int where) {
    robj *value = NULL;

    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        long long vlong;
        int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p;
        unsigned char *vstr;
        int64_t vlen;
        unsigned char intbuf[LP_INTBUF_SIZE];

        p = (where == LIST_HEAD) ? lpFirst(subject->ptr) : lpLast(subject->ptr);
        if (p) {
            vstr = lpGet(p, &vlen, intbuf);
            value = createStringObject((char*)vstr, vlen);
            subject->ptr = lpDelete(subject->ptr, p, NULL);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength(subject->ptr);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. */
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        int iter_direction = direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        li->lpi = lpSeek(subject->ptr, index);
    } else {
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Sets the direction of an iterator. */
void listTypeSetIteratorDirection(listTypeIterator *li, listTypeEntry *entry, unsigned char direction) {
    if (li->direction == direction) return;

    li->direction = direction;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        int dir = direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
        quicklistSetDirection(li->iter, dir);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = li->subject->ptr;
        /* Note that the iterator for listpack always points to the next of the current entry,
         * so we need to update position of the iterator depending on the direction. */
        li->lpi = (direction == LIST_TAIL) ? lpNext(lp, entry->lpe) : lpPrev(lp, entry->lpe);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    if (li->encoding == OBJ_ENCODING_QUICKLIST)
        quicklistReleaseIterator(li->iter);
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    serverAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistNext(li->iter, &entry->entry);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        entry->lpe = li->lpi;
        if (entry->lpe != NULL) {
            li->lpi = (li->direction == LIST_TAIL) ?
                lpNext(li->subject->ptr,li->lpi) : lpPrev(li->subject->ptr,li->lpi);
            return 1;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Get entry value at the current position of the iterator.
 * When the function returns NULL, it populates the integer value by
 * reference in 'lval'. Otherwise a pointer to the string is returned,
 * and 'vlen' is set to the length of the string. */
unsigned char *listTypeGetValue(listTypeEntry *entry, size_t *vlen, long long *lval) {
    unsigned char *vstr = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {
            vstr = entry->entry.value;
            *vlen = entry->entry.sz;
        } else {
            *lval = entry->entry.longval;
        }
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned int slen;
        vstr = lpGetValue(entry->lpe, &slen, lval);
        *vlen = slen;
    } else {
        serverPanic("Unknown list encoding");
    }
    return vstr;
}

/* Return entry or NULL at the current position of the iterator. */
robj *listTypeGet(listTypeEntry *entry) {
    unsigned char *vstr;
    size_t vlen;
    long long lval;

    vstr = listTypeGetValue(entry, &vlen, &lval);
    if (vstr) 
        return createStringObject((char *)vstr, vlen);
    else
        return createStringObjectFromLongLong(lval);
}

void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    robj *subject = entry->li->subject;
    value = getDecodedObject(value);
    sds str = value->ptr;
    size_t len = sdslen(str);

    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (where == LIST_TAIL) {
            quicklistInsertAfter(entry->li->iter, &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore(entry->li->iter, &entry->entry, str, len);
        }
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        int lpw = (where == LIST_TAIL) ? LP_AFTER : LP_BEFORE;
        subject->ptr = lpInsertString(subject->ptr, (unsigned char *)str,
                                      len, entry->lpe, lpw, &entry->lpe);
    } else {
        serverPanic("Unknown list encoding");
    }
    decrRefCount(value);
}

/* Replaces entry at the current position of the iterator. */
void listTypeReplace(listTypeEntry *entry, robj *value) {
    robj *subject = entry->li->subject;
    value = getDecodedObject(value);
    sds str = value->ptr;
    size_t len = sdslen(str);

    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistReplaceEntry(entry->li->iter, &entry->entry, str, len);
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        subject->ptr = lpReplace(subject->ptr, &entry->lpe, (unsigned char *)str, len);
    } else {
        serverPanic("Unknown list encoding");
    }

    decrRefCount(value);
}

/* Replace entry at offset 'index' by 'value'.
 *
 * Returns 1 if replace happened.
 * Returns 0 if replace failed and no changes happened. */
int listTypeReplaceAtIndex(robj *o, int index, robj *value) {
    value = getDecodedObject(value);
    sds vstr = value->ptr;
    size_t vlen = sdslen(vstr);
    int replaced = 0;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        replaced = quicklistReplaceAtIndex(ql, index, vstr, vlen);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p = lpSeek(o->ptr,index);
        if (p) {
            o->ptr = lpReplace(o->ptr, &p, (unsigned char *)vstr, vlen);
            replaced = 1;
        }
    } else {
        serverPanic("Unknown list encoding");
    }

    decrRefCount(value);
    return replaced;
}

/* Compare the given object with the entry at the current position. */
int listTypeEqual(listTypeEntry *entry, robj *o) {
    serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCompare(&entry->entry,o->ptr,sdslen(o->ptr));
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        return lpCompare(entry->lpe,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelEntry(iter->iter, &entry->entry);
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p = entry->lpe;
        iter->subject->ptr = lpDelete(iter->subject->ptr,p,&p);

        /* Update position of the iterator depending on the direction */
        if (iter->direction == LIST_TAIL)
            iter->lpi = p;
        else {
            if (p) {
                iter->lpi = lpPrev(iter->subject->ptr,p);
            } else {
                /* We deleted the last element, so we need to set the
                 * iterator to the last element. */
                iter->lpi = lpLast(iter->subject->ptr);
            }
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a list object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *listTypeDup(robj *o) {
    robj *lobj;

    serverAssert(o->type == OBJ_LIST);

    switch (o->encoding) {
        case OBJ_ENCODING_LISTPACK:
            lobj = createObject(OBJ_LIST, lpDup(o->ptr));
            break;
        case OBJ_ENCODING_QUICKLIST:
            lobj = createObject(OBJ_LIST, quicklistDup(o->ptr));
            break;
        default:
            serverPanic("Unknown list encoding");
            break;
    }
    lobj->encoding = o->encoding;
    return lobj;
}

/* Delete a range of elements from the list. */
void listTypeDelRange(robj *subject, long start, long count) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(subject->ptr, start, count);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        subject->ptr = lpDeleteRange(subject->ptr, start, count);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

/* Implements LPUSH/RPUSH/LPUSHX/RPUSHX. 
 * 'xx': push if key exists. */
void pushGenericCommand(client *c, int where, int xx) {
    int j;

    robj *lobj = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c,lobj,OBJ_LIST)) return;
    if (!lobj) {
        if (xx) {
            addReply(c, shared.czero);
            return;
        }

        lobj = createListListpackObject();
        dbAdd(c->db,c->argv[1],lobj);
    }

    listTypeTryConversionAppend(lobj,c->argv,2,c->argc-1,NULL,NULL);
    for (j = 2; j < c->argc; j++) {
        listTypePush(lobj,c->argv[j],where);
        server.dirty++;
    }

    addReplyLongLong(c, listTypeLength(lobj));

    char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
}

/* LPUSH <key> <element> [<element> ...] */
void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD,0);
}

/* RPUSH <key> <element> [<element> ...] */
void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL,0);
}

/* LPUSHX <key> <element> [<element> ...] */
void lpushxCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD,1);
}

/* RPUSHX <key> <element> [<element> ...] */
void rpushxCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL,1);
}

/* LINSERT <key> (BEFORE|AFTER) <pivot> <element> */
void linsertCommand(client *c) {
    int where;
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        where = LIST_TAIL;
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        where = LIST_HEAD;
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    /* We're not sure if this value can be inserted yet, but we cannot
     * convert the list inside the iterator. We don't want to loop over
     * the list twice (once to see if the value can be inserted and once
     * to do the actual insert), so we assume this value can be inserted
     * and convert the listpack to a regular list if necessary. */
    listTypeTryConversionAppend(subject,c->argv,4,4,NULL,NULL);

    /* Seek pivot from head to tail */
    iter = listTypeInitIterator(subject,0,LIST_TAIL);
    while (listTypeNext(iter,&entry)) {
        if (listTypeEqual(&entry,c->argv[3])) {
            listTypeInsert(&entry,c->argv[4],where);
            inserted = 1;
            break;
        }
    }
    listTypeReleaseIterator(iter);

    if (inserted) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"linsert",
                            c->argv[1],c->db->id);
        server.dirty++;
    } else {
        /* Notify client of a failed insert */
        addReplyLongLong(c,-1);
        return;
    }

    addReplyLongLong(c,listTypeLength(subject));
}

/* LLEN <key> */
void llenCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    addReplyLongLong(c,listTypeLength(o));
}

/* LINDEX <key> <index> */
void lindexCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    listTypeIterator *iter = listTypeInitIterator(o,index,LIST_TAIL);
    listTypeEntry entry;
    unsigned char *vstr;
    size_t vlen;
    long long lval;

    if (listTypeNext(iter,&entry)) {
        vstr = listTypeGetValue(&entry,&vlen,&lval);
        if (vstr) {
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {
            addReplyBulkLongLong(c, lval);
        }
    } else {
        addReplyNull(c);
    }

    listTypeReleaseIterator(iter);
}

/* LSET <key> <index> <element> */
void lsetCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->argv[3];

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    listTypeTryConversionAppend(o,c->argv,3,3,NULL,NULL);
    if (listTypeReplaceAtIndex(o,index,value)) {
        addReply(c,shared.ok);
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
        server.dirty++;

        /* We might replace a big item with a small one or vice versa, but we've
         * already handled the growing case in listTypeTryConversionAppend()
         * above, so here we just need to try the conversion for shrinking. */
        listTypeTryConversion(o,LIST_CONV_SHRINKING,NULL,NULL);
    } else {
        addReplyErrorObject(c,shared.outofrangeerr);
    }
}

/* A helper function like addListRangeReply, more details see below.
 * The difference is that here we are returning nested arrays, like:
 * 1) keyname
 * 2) 1) element1
 *    2) element2
 *
 * And also actually pop out from the list by calling listElementsRemoved.
 * We maintain the server.dirty and notifications there.
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function. */
void listPopRangeAndReplyWithKey(client *c, robj *o, robj *key, int where, long count, int signal, int *deleted) {
    long llen = listTypeLength(o);
    long rangelen = (count > llen) ? llen : count;
    long rangestart = (where == LIST_HEAD) ? 0 : -rangelen;
    long rangeend = (where == LIST_HEAD) ? rangelen - 1 : -1;
    int reverse = (where == LIST_HEAD) ? 0 : 1;

    /* We return key-name just once, and an array of elements */
    addReplyArrayLen(c, 2);
    addReplyBulk(c, key);
    addListRangeReply(c, o, rangestart, rangeend, reverse);

    /* Pop these elements. */
    listTypeDelRange(o, rangestart, rangelen);
    /* Maintain the notifications and dirty. */
    listElementsRemoved(c, key, where, o, rangelen, signal, deleted);
}

/* Extracted from `addListRangeReply()` to reply with a quicklist list.
 * Note that the purpose is to make the methods small so that the
 * code in the loop can be inlined better to improve performance. */
void addListQuicklistRangeReply(client *c, robj *o, int from, int rangelen, int reverse) {
    /* Return the result in form of a multi-bulk reply */
    addReplyArrayLen(c,rangelen);

    int direction = reverse ? AL_START_TAIL : AL_START_HEAD;
    quicklistIter *iter = quicklistGetIteratorAtIdx(o->ptr, direction, from);
    while(rangelen--) {
        quicklistEntry qe;
        serverAssert(quicklistNext(iter, &qe)); /* fail on corrupt data */
        if (qe.value) {
            addReplyBulkCBuffer(c,qe.value,qe.sz);
        } else {
            addReplyBulkLongLong(c,qe.longval);
        }
    }
    quicklistReleaseIterator(iter);
}

/* Extracted from `addListRangeReply()` to reply with a listpack list.
 * Note that the purpose is to make the methods small so that the
 * code in the loop can be inlined better to improve performance. */
void addListListpackRangeReply(client *c, robj *o, int from, int rangelen, int reverse) {
    unsigned char *p = lpSeek(o->ptr, from);
    unsigned char *vstr;
    unsigned int vlen;
    long long lval;

    /* Return the result in form of a multi-bulk reply */
    addReplyArrayLen(c,rangelen);

    while(rangelen--) {
        serverAssert(p); /* fail on corrupt data */
        vstr = lpGetValue(p, &vlen, &lval);
        if (vstr) {
            addReplyBulkCBuffer(c,vstr,vlen);
        } else {
            addReplyBulkLongLong(c,lval);
        }
        p = reverse ? lpPrev(o->ptr,p) : lpNext(o->ptr,p);
    }
}

/* A helper for replying with a list's range between the inclusive start and end
 * indexes as multi-bulk, with support for negative indexes. Note that start
 * must be less than end or an empty array is returned. When the reverse
 * argument is set to a non-zero value, the reply is reversed so that elements
 * are returned from end to start. */
void addListRangeReply(client *c, robj *o, long start, long end, int reverse) {
    long rangelen, llen = listTypeLength(o);

    /* Convert negative indexes. */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        addReply(c,shared.emptyarray);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    int from = reverse ? end : start;
    if (o->encoding == OBJ_ENCODING_QUICKLIST)
        addListQuicklistRangeReply(c, o, from, rangelen, reverse);
    else if (o->encoding == OBJ_ENCODING_LISTPACK)
        addListListpackRangeReply(c, o, from, rangelen, reverse);
    else
        serverPanic("Unknown list encoding");
}

/* A housekeeping helper for list elements popping tasks.
 *
 * If 'signal' is 0, skip calling signalModifiedKey().
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function. */
void listElementsRemoved(client *c, robj *key, int where, robj *o, long count, int signal, int *deleted) {
    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";

    notifyKeyspaceEvent(NOTIFY_LIST, event, key, c->db->id);
    if (listTypeLength(o) == 0) {
        if (deleted) *deleted = 1;

        dbDelete(c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    } else {
        listTypeTryConversion(o, LIST_CONV_SHRINKING, NULL, NULL);
        if (deleted) *deleted = 0;
    }
    if (signal) signalModifiedKey(c, c->db, key);
    server.dirty += count;
}

/* Implements the generic list pop operation for LPOP/RPOP.
 * The where argument specifies which end of the list is operated on. An
 * optional count may be provided as the third argument of the client's
 * command. */
void popGenericCommand(client *c, int where) {
    int hascount = (c->argc == 3);
    long count = 0;
    robj *value;

    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    } else if (hascount) {
        /* Parse the optional count argument. */
        if (getPositiveLongFromObjectOrReply(c,c->argv[2],&count,NULL) != C_OK) 
            return;
    }

    robj *o = lookupKeyWriteOrReply(c, c->argv[1], hascount ? shared.nullarray[c->resp]: shared.null[c->resp]);
    if (o == NULL || checkType(c, o, OBJ_LIST))
        return;

    if (hascount && !count) {
        /* Fast exit path. */
        addReply(c,shared.emptyarray);
        return;
    }

    if (!count) {
        /* Pop a single element. This is POP's original behavior that replies
         * with a bulk string. */
        value = listTypePop(o,where);
        serverAssert(value != NULL);
        addReplyBulk(c,value);
        decrRefCount(value);
        listElementsRemoved(c,c->argv[1],where,o,1,1,NULL);
    } else {
        /* Pop a range of elements. An addition to the original POP command,
         *  which replies with a multi-bulk. */
        long llen = listTypeLength(o);
        long rangelen = (count > llen) ? llen : count;
        long rangestart = (where == LIST_HEAD) ? 0 : -rangelen;
        long rangeend = (where == LIST_HEAD) ? rangelen - 1 : -1;
        int reverse = (where == LIST_HEAD) ? 0 : 1;

        addListRangeReply(c,o,rangestart,rangeend,reverse);
        listTypeDelRange(o,rangestart,rangelen);
        listElementsRemoved(c,c->argv[1],where,o,rangelen,1,NULL);
    }
}

/* Like popGenericCommand but work with multiple keys.
 * Take multiple keys and return multiple elements from just one key.
 *
 * 'numkeys' the number of keys.
 * 'count' is the number of elements requested to pop.
 *
 * Always reply with array. */
void mpopGenericCommand(client *c, robj **keys, int numkeys, int where, long count) {
    int j;
    robj *o;
    robj *key;

    for (j = 0; j < numkeys; j++) {
        key = keys[j];
        o = lookupKeyWrite(c->db, key);

        /* Non-existing key, move to next key. */
        if (o == NULL) continue;

        if (checkType(c, o, OBJ_LIST)) return;

        long llen = listTypeLength(o);
        /* Empty list, move to next key. */
        if (llen == 0) continue;

        /* Pop a range of elements in a nested arrays way. */
        listPopRangeAndReplyWithKey(c, o, key, where, count, 1, NULL);

        /* Replicate it as [LR]POP COUNT. */
        robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
        rewriteClientCommandVector(c, 3,
                                   (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                                   key, count_obj);
        decrRefCount(count_obj);
        return;
    }

    /* Look like we are not able to pop up any elements. */
    addReplyNullArray(c);
}

/* LPOP <key> [count] */
void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

/* RPOP <key> [count] */
void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

/* LRANGE <key> <start> <stop> */
void lrangeCommand(client *c) {
    robj *o;
    long start, end;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray)) == NULL
         || checkType(c,o,OBJ_LIST)) return;

    addListRangeReply(c,o,start,end,0);
}

/* LTRIM <key> <start> <stop> */
void ltrimCommand(client *c) {
    robj *o;
    long start, end, llen, ltrim, rtrim;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(o->ptr,0,ltrim);
        quicklistDelRange(o->ptr,-rtrim,rtrim);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        o->ptr = lpDeleteRange(o->ptr,0,ltrim);
        o->ptr = lpDeleteRange(o->ptr,-rtrim,rtrim);
    } else {
        serverPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    } else {
        listTypeTryConversion(o,LIST_CONV_SHRINKING,NULL,NULL);
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty += (ltrim + rtrim);
    addReply(c,shared.ok);
}

/* LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len]
 *
 * The "rank" is the position of the match, so if it is 1, the first match
 * is returned, if it is 2 the second match is returned and so forth.
 * It is 1 by default. If negative has the same meaning but the search is
 * performed starting from the end of the list.
 *
 * If COUNT is given, instead of returning the single element, a list of
 * all the matching elements up to "num-matches" are returned. COUNT can
 * be combined with RANK in order to returning only the element starting
 * from the Nth. If COUNT is zero, all the matching elements are returned.
 *
 * MAXLEN tells the command to scan a max of len elements. If zero (the
 * default), all the elements in the list are scanned if needed.
 *
 * The returned elements indexes are always referring to what LINDEX
 * would return. So first element from head is 0, and so forth. */
void lposCommand(client *c) {
    robj *o, *ele;
    ele = c->argv[2];
    int direction = LIST_TAIL;
    long rank = 1, count = -1, maxlen = 0; /* Count -1: option not given. */

    /* Parse the optional arguments. */
    for (int j = 3; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1)-j;

        if (!strcasecmp(opt,"RANK") && moreargs) {
            j++;
            if (getRangeLongFromObjectOrReply(c, c->argv[j], -LONG_MAX, LONG_MAX, &rank, NULL) != C_OK)
                return;
            if (rank == 0) {
                addReplyError(c,"RANK can't be zero: use 1 to start from "
                                "the first match, 2 from the second ... "
                                "or use negative to start from the end of the list");
                return;
            }
        } else if (!strcasecmp(opt,"COUNT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &count,
              "COUNT can't be negative") != C_OK)
                return;
        } else if (!strcasecmp(opt,"MAXLEN") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &maxlen, 
              "MAXLEN can't be negative") != C_OK)
                return;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* A negative rank means start from the tail. */
    if (rank < 0) {
        rank = -rank;
        direction = LIST_HEAD;
    }

    /* We return NULL or an empty array if there is no such key (or
     * if we find no matches, depending on the presence of the COUNT option. */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        if (count != -1)
            addReply(c,shared.emptyarray);
        else
            addReply(c,shared.null[c->resp]);
        return;
    }
    if (checkType(c,o,OBJ_LIST)) return;

    /* If we got the COUNT option, prepare to emit an array. */
    void *arraylenptr = NULL;
    if (count != -1) arraylenptr = addReplyDeferredLen(c);

    /* Seek the element. */
    listTypeIterator *li;
    li = listTypeInitIterator(o,direction == LIST_HEAD ? -1 : 0,direction);
    listTypeEntry entry;
    long llen = listTypeLength(o);
    long index = 0, matches = 0, matchindex = -1, arraylen = 0;
    while (listTypeNext(li,&entry) && (maxlen == 0 || index < maxlen)) {
        if (listTypeEqual(&entry,ele)) {
            matches++;
            matchindex = (direction == LIST_TAIL) ? index : llen - index - 1;
            if (matches >= rank) {
                if (arraylenptr) {
                    arraylen++;
                    addReplyLongLong(c,matchindex);
                    if (count && matches-rank+1 >= count) break;
                } else {
                    break;
                }
            }
        }
        index++;
        matchindex = -1; /* Remember if we exit the loop without a match. */
    }
    listTypeReleaseIterator(li);

    /* Reply to the client. Note that arraylenptr is not NULL only if
     * the COUNT option was selected. */
    if (arraylenptr != NULL) {
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else {
        if (matchindex != -1)
            addReplyLongLong(c,matchindex);
        else
            addReply(c,shared.null[c->resp]);
    }
}

/* LREM <key> <count> <element> */
void lremCommand(client *c) {
    robj *subject, *obj;
    obj = c->argv[3];
    long toremove;
    long removed = 0;

    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != C_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,OBJ_LIST)) return;

    listTypeIterator *li;
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,LIST_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,LIST_TAIL);
    }

    listTypeEntry entry;
    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(li, &entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    if (removed) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"lrem",c->argv[1],c->db->id);
    }

    if (listTypeLength(subject) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    } else if (removed) {
        listTypeTryConversion(subject,LIST_CONV_SHRINKING,NULL,NULL);
    }

    addReplyLongLong(c,removed);
}

void lmoveHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value,
                     int where) {
    /* Create the list if the key does not exist */
    if (!dstobj) {
        dstobj = createListListpackObject();
        dbAdd(c->db,dstkey,dstobj);
    }
    signalModifiedKey(c,c->db,dstkey);
    listTypeTryConversionAppend(dstobj,&value,0,0,NULL,NULL);
    listTypePush(dstobj,value,where);
    notifyKeyspaceEvent(NOTIFY_LIST,
                        where == LIST_HEAD ? "lpush" : "rpush",
                        dstkey,
                        c->db->id);
    /* Always send the pushed value to the client. */
    addReplyBulk(c,value);
}

int getListPositionFromObjectOrReply(client *c, robj *arg, int *position) {
    if (strcasecmp(arg->ptr,"right") == 0) {
        *position = LIST_TAIL;
    } else if (strcasecmp(arg->ptr,"left") == 0) {
        *position = LIST_HEAD;
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
        return C_ERR;
    }
    return C_OK;
}

robj *getStringObjectFromListPosition(int position) {
    if (position == LIST_HEAD) {
        return shared.left;
    } else {
        // LIST_TAIL
        return shared.right;
    }
}

void lmoveGenericCommand(client *c, int wherefrom, int whereto) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,sobj,OBJ_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        addReplyNull(c);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        if (checkType(c,dobj,OBJ_LIST)) return;
        value = listTypePop(sobj,wherefrom);
        serverAssert(value); /* assertion for valgrind (avoid NPD) */
        lmoveHandlePush(c,c->argv[2],dobj,value,whereto);
        listElementsRemoved(c,touchedkey,wherefrom,sobj,1,1,NULL);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        if (c->cmd->proc == blmoveCommand) {
            rewriteClientCommandVector(c,5,shared.lmove,
                                       c->argv[1],c->argv[2],c->argv[3],c->argv[4]);
        } else if (c->cmd->proc == brpoplpushCommand) {
            rewriteClientCommandVector(c,3,shared.rpoplpush,
                                       c->argv[1],c->argv[2]);
        }
    }
}

/* LMOVE <source> <destination> (LEFT|RIGHT) (LEFT|RIGHT) */
void lmoveCommand(client *c) {
    int wherefrom, whereto;
    if (getListPositionFromObjectOrReply(c,c->argv[3],&wherefrom)
        != C_OK) return;
    if (getListPositionFromObjectOrReply(c,c->argv[4],&whereto)
        != C_OK) return;
    lmoveGenericCommand(c, wherefrom, whereto);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */
void rpoplpushCommand(client *c) {
    lmoveGenericCommand(c, LIST_TAIL, LIST_HEAD);
}

/* Blocking RPOP/LPOP/LMPOP
 *
 * 'numkeys' is the number of keys.
 * 'timeout_idx' parameter position of block timeout.
 * 'where' LIST_HEAD for LEFT, LIST_TAIL for RIGHT.
 * 'count' is the number of elements requested to pop, or -1 for plain single pop.
 *
 * When count is -1, a reply of a single bulk-string will be used.
 * When count > 0, an array reply will be used. */
void blockingPopGenericCommand(client *c, robj **keys, int numkeys, int where, int timeout_idx, long count) {
    robj *o;
    robj *key;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c,c->argv[timeout_idx],&timeout,UNIT_SECONDS)
        != C_OK) return;

    /* Traverse all input keys, we take action only based on one key. */
    for (j = 0; j < numkeys; j++) {
        key = keys[j];
        o = lookupKeyWrite(c->db, key);

        /* Non-existing key, move to next key. */
        if (o == NULL) continue;

        if (checkType(c, o, OBJ_LIST)) return;

        long llen = listTypeLength(o);
        /* Empty list, move to next key. */
        if (llen == 0) continue;

        if (count != -1) {
            /* BLMPOP, non empty list, like a normal [LR]POP with count option.
             * The difference here we pop a range of elements in a nested arrays way. */
            listPopRangeAndReplyWithKey(c, o, key, where, count, 1, NULL);

            /* Replicate it as [LR]POP COUNT. */
            robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
            rewriteClientCommandVector(c, 3,
                                       (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                                       key, count_obj);
            decrRefCount(count_obj);
            return;
        }

        /* Non empty list, this is like a normal [LR]POP. */
        robj *value = listTypePop(o,where);
        serverAssert(value != NULL);

        addReplyArrayLen(c,2);
        addReplyBulk(c,key);
        addReplyBulk(c,value);
        decrRefCount(value);
        listElementsRemoved(c,key,where,o,1,1,NULL);

        /* Replicate it as an [LR]POP instead of B[LR]POP. */
        rewriteClientCommandVector(c,2,
            (where == LIST_HEAD) ? shared.lpop : shared.rpop,
            key);
        return;
    }

    /* If we are not allowed to block the client, the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & CLIENT_DENY_BLOCKING) {
        addReplyNullArray(c);
        return;
    }

    /* If the keys do not exist we must block */
    blockForKeys(c,BLOCKED_LIST,keys,numkeys,timeout,0);
}

/* BLPOP <key> [<key> ...] <timeout> */
void blpopCommand(client *c) {
    blockingPopGenericCommand(c,c->argv+1,c->argc-2,LIST_HEAD,c->argc-1,-1);
}

/* BRPOP <key> [<key> ...] <timeout> */
void brpopCommand(client *c) {
    blockingPopGenericCommand(c,c->argv+1,c->argc-2,LIST_TAIL,c->argc-1,-1);
}

void blmoveGenericCommand(client *c, int wherefrom, int whereto, mstime_t timeout) {
    robj *key = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c,key,OBJ_LIST)) return;

    if (key == NULL) {
        if (c->flags & CLIENT_DENY_BLOCKING) {
            /* Blocking against an empty list when blocking is not allowed
             * returns immediately. */
            addReplyNull(c);
        } else {
            /* The list is empty and the client blocks. */
            blockForKeys(c,BLOCKED_LIST,c->argv + 1,1,timeout,0);
        }
    } else {
        /* The list exists and has elements, so
         * the regular lmoveCommand is executed. */
        serverAssertWithInfo(c,key,listTypeLength(key) > 0);
        lmoveGenericCommand(c,wherefrom,whereto);
    }
}

/* BLMOVE <source> <destination> (LEFT|RIGHT) (LEFT|RIGHT) <timeout> */
void blmoveCommand(client *c) {
    mstime_t timeout;
    int wherefrom, whereto;
    if (getListPositionFromObjectOrReply(c,c->argv[3],&wherefrom)
        != C_OK) return;
    if (getListPositionFromObjectOrReply(c,c->argv[4],&whereto)
        != C_OK) return;
    if (getTimeoutFromObjectOrReply(c,c->argv[5],&timeout,UNIT_SECONDS)
        != C_OK) return;
    blmoveGenericCommand(c,wherefrom,whereto,timeout);
}

/* BRPOPLPUSH <source> <destination> <timeout> */
void brpoplpushCommand(client *c) {
    mstime_t timeout;
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;
    blmoveGenericCommand(c, LIST_TAIL, LIST_HEAD, timeout);
}

/* LMPOP/BLMPOP
 *
 * 'numkeys_idx' parameter position of key number.
 * 'is_block' this indicates whether it is a blocking variant. */
void lmpopGenericCommand(client *c, int numkeys_idx, int is_block) {
    long j;
    long numkeys = 0;      /* Number of keys. */
    int where = 0;         /* HEAD for LEFT, TAIL for RIGHT. */
    long count = -1;       /* Reply will consist of up to count elements, depending on the list's length. */

    /* Parse the numkeys. */
    if (getRangeLongFromObjectOrReply(c, c->argv[numkeys_idx], 1, LONG_MAX,
                                      &numkeys, "numkeys should be greater than 0") != C_OK)
        return;

    /* Parse the where. where_idx: the index of where in the c->argv. */
    long where_idx = numkeys_idx + numkeys + 1;
    if (where_idx >= c->argc) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (getListPositionFromObjectOrReply(c, c->argv[where_idx], &where) != C_OK)
        return;

    /* Parse the optional arguments. */
    for (j = where_idx + 1; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (count == -1 && !strcasecmp(opt, "COUNT") && moreargs) {
            j++;
            if (getRangeLongFromObjectOrReply(c, c->argv[j], 1, LONG_MAX,
                                              &count,"count should be greater than 0") != C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    if (count == -1) count = 1;

    if (is_block) {
        /* BLOCK. We will handle CLIENT_DENY_BLOCKING flag in blockingPopGenericCommand. */
        blockingPopGenericCommand(c, c->argv+numkeys_idx+1, numkeys, where, 1, count);
    } else {
        /* NON-BLOCK */
        mpopGenericCommand(c, c->argv+numkeys_idx+1, numkeys, where, count);
    }
}

/* LMPOP numkeys <key> [<key> ...] (LEFT|RIGHT) [COUNT count] */
void lmpopCommand(client *c) {
    lmpopGenericCommand(c, 1, 0);
}

/* BLMPOP timeout numkeys <key> [<key> ...] (LEFT|RIGHT) [COUNT count] */
void blmpopCommand(client *c) {
    lmpopGenericCommand(c, 2, 1);
}
