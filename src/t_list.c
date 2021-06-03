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

#define LIST_MAX_ITEM_SIZE ((1ull<<32)-1024)

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

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
    } else {
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, unsigned int sz) {
    return createStringObject((char*)data,sz);
}

robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;

    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);
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
    int iter_direction =
        direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else {
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li->iter);
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
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {
            value = createStringObject((char *)entry->entry.value,
                                       entry->entry.sz);
        } else {
            value = createStringObjectFromLongLong(entry->entry.longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        value = getDecodedObject(value);
        sds str = value->ptr;
        size_t len = sdslen(str);
        if (where == LIST_TAIL) {
            quicklistInsertAfter((quicklist *)entry->entry.quicklist,
                                 &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore((quicklist *)entry->entry.quicklist,
                                  &entry->entry, str, len);
        }
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
int listTypeEqual(listTypeEntry *entry, robj *o) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
        return quicklistCompare(entry->entry.zi,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelEntry(iter->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Create a quicklist from a single ziplist */
void listTypeConvert(robj *subject, int enc) {
    serverAssertWithInfo(NULL,subject,subject->type==OBJ_LIST);
    serverAssertWithInfo(NULL,subject,subject->encoding==OBJ_ENCODING_ZIPLIST);

    if (enc == OBJ_ENCODING_QUICKLIST) {
        size_t zlen = server.list_max_ziplist_size;
        int depth = server.list_compress_depth;
        subject->ptr = quicklistCreateFromZiplist(zlen, depth, subject->ptr);
        subject->encoding = OBJ_ENCODING_QUICKLIST;
    } else {
        serverPanic("Unsupported list conversion");
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
        case OBJ_ENCODING_QUICKLIST:
            lobj = createObject(OBJ_LIST, quicklistDup(o->ptr));
            lobj->encoding = OBJ_ENCODING_QUICKLIST;
            break;
        default:
            serverPanic("Unknown list encoding");
            break;
    }
    return lobj;
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

/* Implements LPUSH/RPUSH/LPUSHX/RPUSHX. 
 * 'xx': push if key exists. */
void pushGenericCommand(client *c, int where, int xx) {
    int j;

    for (j = 2; j < c->argc; j++) {
        if (sdslen(c->argv[j]->ptr) > LIST_MAX_ITEM_SIZE) {
            addReplyError(c, "Element too large");
            return;
        }
    }

    robj *lobj = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c,lobj,OBJ_LIST)) return;
    if (!lobj) {
        if (xx) {
            addReply(c, shared.czero);
            return;
        }

        lobj = createQuicklistObject();
        quicklistSetOptions(lobj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        dbAdd(c->db,c->argv[1],lobj);
    }

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

/* RPUSH <key> <element> [<element> ...] */
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

    if (sdslen(c->argv[4]->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

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

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistEntry entry;
        if (quicklistIndex(o->ptr, index, &entry)) {
            if (entry.value) {
                addReplyBulkCBuffer(c, entry.value, entry.sz);
            } else {
                addReplyBulkLongLong(c, entry.longval);
            }
        } else {
            addReplyNull(c);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* LSET <key> <index> <element> */
void lsetCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->argv[3];

    if (sdslen(value->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        int replaced = quicklistReplaceAtIndex(ql, index,
                                               value->ptr, sdslen(value->ptr));
        if (!replaced) {
            addReplyErrorObject(c,shared.outofrangeerr);
        } else {
            addReply(c,shared.ok);
            signalModifiedKey(c,c->db,c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
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

    /* Return the result in form of a multi-bulk reply */
    addReplyArrayLen(c,rangelen);
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        int from = reverse ? end : start;
        int direction = reverse ? LIST_HEAD : LIST_TAIL;
        listTypeIterator *iter = listTypeInitIterator(o,from,direction);

        while(rangelen--) {
            listTypeEntry entry;
            listTypeNext(iter, &entry);
            quicklistEntry *qe = &entry.entry;
            if (qe->value) {
                addReplyBulkCBuffer(c,qe->value,qe->sz);
            } else {
                addReplyBulkLongLong(c,qe->longval);
            }
        }
        listTypeReleaseIterator(iter);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* A housekeeping helper for list elements popping tasks. */
void listElementsRemoved(client *c, robj *key, int where, robj *o, long count) {
    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";

    notifyKeyspaceEvent(NOTIFY_LIST, event, key, c->db->id);
    if (listTypeLength(o) == 0) {
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
        dbDelete(c->db, key);
    }
    signalModifiedKey(c, c->db, key);
    server.dirty += count;
}

/* Implements the generic list pop operation for LPOP/RPOP.
 * The where argument specifies which end of the list is operated on. An
 * optional count may be provided as the third argument of the client's
 * command. */
void popGenericCommand(client *c, int where) {
    long count = 0;
    robj *value;

    if (c->argc > 3) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
                            c->cmd->name);
        return;
    } else if (c->argc == 3) {
        /* Parse the optional count argument. */
        if (getPositiveLongFromObjectOrReply(c,c->argv[2],&count,NULL) != C_OK) 
            return;
        if (count == 0) {
            /* Fast exit path. */
            addReplyNullArray(c);
            return;
        }
    }

    robj *o = lookupKeyWriteOrReply(c, c->argv[1], shared.null[c->resp]);
    if (o == NULL || checkType(c, o, OBJ_LIST))
        return;

    if (!count) {
        /* Pop a single element. This is POP's original behavior that replies
         * with a bulk string. */
        value = listTypePop(o,where);
        serverAssert(value != NULL);
        addReplyBulk(c,value);
        decrRefCount(value);
        listElementsRemoved(c,c->argv[1],where,o,1);
    } else {
        /* Pop a range of elements. An addition to the original POP command,
         *  which replies with a multi-bulk. */
        long llen = listTypeLength(o);
        long rangelen = (count > llen) ? llen : count;
        long rangestart = (where == LIST_HEAD) ? 0 : -rangelen;
        long rangeend = (where == LIST_HEAD) ? rangelen - 1 : -1;
        int reverse = (where == LIST_HEAD) ? 0 : 1;

        addListRangeReply(c,o,rangestart,rangeend,reverse);
        quicklistDelRange(o->ptr,rangestart,rangelen);
        listElementsRemoved(c,c->argv[1],where,o,rangelen);
    }
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
    } else {
        serverPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
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
 * be combiled with RANK in order to returning only the element starting
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

    if (sdslen(ele->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

    /* Parse the optional arguments. */
    for (int j = 3; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1)-j;

        if (!strcasecmp(opt,"RANK") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &rank, NULL) != C_OK)
                return;
            if (rank == 0) {
                addReplyError(c,"RANK can't be zero: use 1 to start from "
                                "the first match, 2 from the second, ...");
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

    if (sdslen(obj->ptr) > LIST_MAX_ITEM_SIZE) {
        addReplyError(c, "Element too large");
        return;
    }

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
    }

    addReplyLongLong(c,removed);
}

void lmoveHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value,
                     int where) {
    /* Create the list if the key does not exist */
    if (!dstobj) {
        dstobj = createQuicklistObject();
        quicklistSetOptions(dstobj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        dbAdd(c->db,dstkey,dstobj);
    }
    signalModifiedKey(c,c->db,dstkey);
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

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        notifyKeyspaceEvent(NOTIFY_LIST,
                            wherefrom == LIST_HEAD ? "lpop" : "rpop",
                            touchedkey,
                            c->db->id);
        if (listTypeLength(sobj) == 0) {
            dbDelete(c->db,touchedkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        signalModifiedKey(c,c->db,touchedkey);
        server.dirty++;
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

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is a helper function for handleClientsBlockedOnKeys(). Its work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BLMOVE) also push the
 *    'value' element on the destination list (the "push" side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional xPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'wherefrom' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped from the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The argument 'whereto' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element is to be pushed to the head or tail so that we can
 * propagate the command properly.
 *
 * The function returns C_OK if we are able to serve the client, otherwise
 * C_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BLMOVE that fails to push the value to the destination key as it is
 * of the wrong type. */
int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int wherefrom, int whereto)
{
    robj *argv[5];

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        argv[0] = (wherefrom == LIST_HEAD) ? shared.lpop :
                                             shared.rpop;
        argv[1] = key;
        propagate((wherefrom == LIST_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        addReplyArrayLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);

        /* Notify event. */
        char *event = (wherefrom == LIST_HEAD) ? "lpop" : "rpop";
        notifyKeyspaceEvent(NOTIFY_LIST,event,key,receiver->db->id);
    } else {
        /* BLMOVE */
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            lmoveHandlePush(receiver,dstkey,dstobj,value,whereto);
            /* Propagate the LMOVE/RPOPLPUSH operation. */
            int isbrpoplpush = (receiver->lastcmd->proc == brpoplpushCommand);
            argv[0] = isbrpoplpush ? shared.rpoplpush : shared.lmove;
            argv[1] = key;
            argv[2] = dstkey;
            argv[3] = getStringObjectFromListPosition(wherefrom);
            argv[4] = getStringObjectFromListPosition(whereto);
            propagate(isbrpoplpush ? server.rpoplpushCommand : server.lmoveCommand,
                db->id,argv,(isbrpoplpush ? 3 : 5),
                PROPAGATE_AOF|
                PROPAGATE_REPL);

            /* Notify event ("lpush" or "rpush" was notified by lmoveHandlePush). */
            notifyKeyspaceEvent(NOTIFY_LIST,wherefrom == LIST_TAIL ? "rpop" : "lpop",
                                key,receiver->db->id);
        } else {
            /* BLMOVE failed because of wrong
             * destination type. */
            return C_ERR;
        }
    }
    return C_OK;
}

/* Blocking RPOP/LPOP */
void blockingPopGenericCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;

    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (checkType(c,o,OBJ_LIST)) {
                return;
            } else {
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a normal [LR]POP. */
                    robj *value = listTypePop(o,where);
                    serverAssert(value != NULL);

                    addReplyArrayLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
                    decrRefCount(value);
                    listElementsRemoved(c,c->argv[j],where,o,1);

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    rewriteClientCommandVector(c,2,
                        (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are not allowed to block the client, the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & CLIENT_DENY_BLOCKING) {
        addReplyNullArray(c);
        return;
    }

    /* If the keys do not exist we must block */
    struct listPos pos = {where};
    blockForKeys(c,BLOCKED_LIST,c->argv + 1,c->argc - 2,timeout,NULL,&pos,NULL);
}

/* BLPOP <key> [<key> ...] <timeout> */
void blpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_HEAD);
}

/* BLPOP <key> [<key> ...] <timeout> */
void brpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_TAIL);
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
            struct listPos pos = {wherefrom, whereto};
            blockForKeys(c,BLOCKED_LIST,c->argv + 1,1,timeout,c->argv[2],&pos,NULL);
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
