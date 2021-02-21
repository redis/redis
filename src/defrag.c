/* 
 * Active memory defragmentation
 * Try to find key / value allocations that need to be re-allocated in order 
 * to reduce external fragmentation.
 * We do that by scanning the keyspace and for each pointer we have, we can try to
 * ask the allocator if moving it to a new address will help reduce fragmentation.
 *
 * Copyright (c) 2020, Redis Labs, Inc
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
#include <time.h>
#include <assert.h>
#include <stddef.h>

#ifdef HAVE_DEFRAG

/* this method was added to jemalloc in order to help us understand which
 * pointers are worthwhile moving and which aren't */
int je_get_defrag_hint(void* ptr);

/* forward declarations*/
void defragDictBucketCallback(void *privdata, dictEntry **bucketref);
dictEntry* replaceSatelliteDictKeyPtrAndOrDefragDictEntry(dict *d, sds oldkey, sds newkey, uint64_t hash, long *defragged);

/* Defrag helper for generic allocations.
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
void* activeDefragAlloc(void *ptr) {
    size_t size;
    void *newptr;
    if(!je_get_defrag_hint(ptr)) {
        server.stat_active_defrag_misses++;
        return NULL;
    }
    /* move this allocation to a new allocation.
     * make sure not to use the thread cache. so that we don't get back the same
     * pointers we try to free */
    size = zmalloc_size(ptr);
    newptr = zmalloc_no_tcache(size);
    memcpy(newptr, ptr, size);
    zfree_no_tcache(ptr);
    return newptr;
}

/*Defrag helper for sds strings
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
sds activeDefragSds(sds sdsptr) {
    void* ptr = sdsAllocPtr(sdsptr);
    void* newptr = activeDefragAlloc(ptr);
    if (newptr) {
        size_t offset = sdsptr - (char*)ptr;
        sdsptr = (char*)newptr + offset;
        return sdsptr;
    }
    return NULL;
}

/* Defrag helper for robj and/or string objects
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
robj *activeDefragStringOb(robj* ob, long *defragged) {
    robj *ret = NULL;
    if (ob->refcount!=1)
        return NULL;

    /* try to defrag robj (only if not an EMBSTR type (handled below). */
    if (ob->type!=OBJ_STRING || ob->encoding!=OBJ_ENCODING_EMBSTR) {
        if ((ret = activeDefragAlloc(ob))) {
            ob = ret;
            (*defragged)++;
        }
    }

    /* try to defrag string object */
    if (ob->type == OBJ_STRING) {
        if(ob->encoding==OBJ_ENCODING_RAW) {
            sds newsds = activeDefragSds((sds)ob->ptr);
            if (newsds) {
                ob->ptr = newsds;
                (*defragged)++;
            }
        } else if (ob->encoding==OBJ_ENCODING_EMBSTR) {
            /* The sds is embedded in the object allocation, calculate the
             * offset and update the pointer in the new allocation. */
            long ofs = (intptr_t)ob->ptr - (intptr_t)ob;
            if ((ret = activeDefragAlloc(ob))) {
                ret->ptr = (void*)((intptr_t)ret + ofs);
                (*defragged)++;
            }
        } else if (ob->encoding!=OBJ_ENCODING_INT) {
            serverPanic("Unknown string encoding");
        }
    }
    return ret;
}

/* Defrag helper for dictEntries to be used during dict iteration (called on
 * each step). Returns a stat of how many pointers were moved. */
long dictIterDefragEntry(dictIterator *iter) {
    /* This function is a little bit dirty since it messes with the internals
     * of the dict and it's iterator, but the benefit is that it is very easy
     * to use, and require no other changes in the dict. */
    long defragged = 0;
    dictht *ht;
    /* Handle the next entry (if there is one), and update the pointer in the
     * current entry. */
    if (iter->nextEntry) {
        dictEntry *newde = activeDefragAlloc(iter->nextEntry);
        if (newde) {
            defragged++;
            iter->nextEntry = newde;
            iter->entry->next = newde;
        }
    }
    /* handle the case of the first entry in the hash bucket. */
    ht = &iter->d->ht[iter->table];
    if (ht->table[iter->index] == iter->entry) {
        dictEntry *newde = activeDefragAlloc(iter->entry);
        if (newde) {
            iter->entry = newde;
            ht->table[iter->index] = newde;
            defragged++;
        }
    }
    return defragged;
}

/* Defrag helper for dict main allocations (dict struct, and hash tables).
 * receives a pointer to the dict* and implicitly updates it when the dict
 * struct itself was moved. Returns a stat of how many pointers were moved. */
long dictDefragTables(dict* d) {
    dictEntry **newtable;
    long defragged = 0;
    /* handle the first hash table */
    newtable = activeDefragAlloc(d->ht[0].table);
    if (newtable)
        defragged++, d->ht[0].table = newtable;
    /* handle the second hash table */
    if (d->ht[1].table) {
        newtable = activeDefragAlloc(d->ht[1].table);
        if (newtable)
            defragged++, d->ht[1].table = newtable;
    }
    return defragged;
}

/* Internal function used by zslDefrag */
void zslUpdateNode(zskiplist *zsl, zskiplistNode *oldnode, zskiplistNode *newnode, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == oldnode)
            update[i]->level[i].forward = newnode;
    }
    serverAssert(zsl->header!=oldnode);
    if (newnode->level[0].forward) {
        serverAssert(newnode->level[0].forward->backward==oldnode);
        newnode->level[0].forward->backward = newnode;
    } else {
        serverAssert(zsl->tail==oldnode);
        zsl->tail = newnode;
    }
}

/* Defrag helper for sorted set.
 * Update the robj pointer, defrag the skiplist struct and return the new score
 * reference. We may not access oldele pointer (not even the pointer stored in
 * the skiplist), as it was already freed. Newele may be null, in which case we
 * only need to defrag the skiplist, but not update the obj pointer.
 * When return value is non-NULL, it is the score reference that must be updated
 * in the dict record. */
double *zslDefrag(zskiplist *zsl, double score, sds oldele, sds newele) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x, *newx;
    int i;
    sds ele = newele? newele: oldele;

    /* find the skiplist node referring to the object that was moved,
     * and all pointers that need to be updated if we'll end up moving the skiplist node. */
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            x->level[i].forward->ele != oldele && /* make sure not to access the
                                                     ->obj pointer if it matches
                                                     oldele */
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                sdscmp(x->level[i].forward->ele,ele) < 0)))
            x = x->level[i].forward;
        update[i] = x;
    }

    /* update the robj pointer inside the skip list record. */
    x = x->level[0].forward;
    serverAssert(x && score == x->score && x->ele==oldele);
    if (newele)
        x->ele = newele;

    /* try to defrag the skiplist record itself */
    newx = activeDefragAlloc(x);
    if (newx) {
        zslUpdateNode(zsl, x, newx, update);
        return &newx->score;
    }
    return NULL;
}

/* Defrag helper for sorted set.
 * Defrag a single dict entry key name, and corresponding skiplist struct */
long activeDefragZsetEntry(zset *zs, dictEntry *de) {
    sds newsds;
    double* newscore;
    long defragged = 0;
    sds sdsele = dictGetKey(de);
    if ((newsds = activeDefragSds(sdsele)))
        defragged++, de->key = newsds;
    newscore = zslDefrag(zs->zsl, *(double*)dictGetVal(de), sdsele, newsds);
    if (newscore) {
        dictSetVal(zs->dict, de, newscore);
        defragged++;
    }
    return defragged;
}

#define DEFRAG_SDS_DICT_NO_VAL 0
#define DEFRAG_SDS_DICT_VAL_IS_SDS 1
#define DEFRAG_SDS_DICT_VAL_IS_STROB 2
#define DEFRAG_SDS_DICT_VAL_VOID_PTR 3

/* Defrag a dict with sds key and optional value (either ptr, sds or robj string) */
long activeDefragSdsDict(dict* d, int val_type) {
    dictIterator *di;
    dictEntry *de;
    long defragged = 0;
    di = dictGetIterator(d);
    while((de = dictNext(di)) != NULL) {
        sds sdsele = dictGetKey(de), newsds;
        if ((newsds = activeDefragSds(sdsele)))
            de->key = newsds, defragged++;
        /* defrag the value */
        if (val_type == DEFRAG_SDS_DICT_VAL_IS_SDS) {
            sdsele = dictGetVal(de);
            if ((newsds = activeDefragSds(sdsele)))
                de->v.val = newsds, defragged++;
        } else if (val_type == DEFRAG_SDS_DICT_VAL_IS_STROB) {
            robj *newele, *ele = dictGetVal(de);
            if ((newele = activeDefragStringOb(ele, &defragged)))
                de->v.val = newele;
        } else if (val_type == DEFRAG_SDS_DICT_VAL_VOID_PTR) {
            void *newptr, *ptr = dictGetVal(de);
            if ((newptr = activeDefragAlloc(ptr)))
                de->v.val = newptr, defragged++;
        }
        defragged += dictIterDefragEntry(di);
    }
    dictReleaseIterator(di);
    return defragged;
}

/* Defrag a list of ptr, sds or robj string values */
long activeDefragList(list *l, int val_type) {
    long defragged = 0;
    listNode *ln, *newln;
    for (ln = l->head; ln; ln = ln->next) {
        if ((newln = activeDefragAlloc(ln))) {
            if (newln->prev)
                newln->prev->next = newln;
            else
                l->head = newln;
            if (newln->next)
                newln->next->prev = newln;
            else
                l->tail = newln;
            ln = newln;
            defragged++;
        }
        if (val_type == DEFRAG_SDS_DICT_VAL_IS_SDS) {
            sds newsds, sdsele = ln->value;
            if ((newsds = activeDefragSds(sdsele)))
                ln->value = newsds, defragged++;
        } else if (val_type == DEFRAG_SDS_DICT_VAL_IS_STROB) {
            robj *newele, *ele = ln->value;
            if ((newele = activeDefragStringOb(ele, &defragged)))
                ln->value = newele;
        } else if (val_type == DEFRAG_SDS_DICT_VAL_VOID_PTR) {
            void *newptr, *ptr = ln->value;
            if ((newptr = activeDefragAlloc(ptr)))
                ln->value = newptr, defragged++;
        }
    }
    return defragged;
}

/* Defrag a list of sds values and a dict with the same sds keys */
long activeDefragSdsListAndDict(list *l, dict *d, int dict_val_type) {
    long defragged = 0;
    sds newsds, sdsele;
    listNode *ln, *newln;
    dictIterator *di;
    dictEntry *de;
    /* Defrag the list and it's sds values */
    for (ln = l->head; ln; ln = ln->next) {
        if ((newln = activeDefragAlloc(ln))) {
            if (newln->prev)
                newln->prev->next = newln;
            else
                l->head = newln;
            if (newln->next)
                newln->next->prev = newln;
            else
                l->tail = newln;
            ln = newln;
            defragged++;
        }
        sdsele = ln->value;
        if ((newsds = activeDefragSds(sdsele))) {
            /* When defragging an sds value, we need to update the dict key */
            uint64_t hash = dictGetHash(d, newsds);
            replaceSatelliteDictKeyPtrAndOrDefragDictEntry(d, sdsele, newsds, hash, &defragged);
            ln->value = newsds;
            defragged++;
        }
    }

    /* Defrag the dict values (keys were already handled) */
    di = dictGetIterator(d);
    while((de = dictNext(di)) != NULL) {
        if (dict_val_type == DEFRAG_SDS_DICT_VAL_IS_SDS) {
            sds newsds, sdsele = dictGetVal(de);
            if ((newsds = activeDefragSds(sdsele)))
                de->v.val = newsds, defragged++;
        } else if (dict_val_type == DEFRAG_SDS_DICT_VAL_IS_STROB) {
            robj *newele, *ele = dictGetVal(de);
            if ((newele = activeDefragStringOb(ele, &defragged)))
                de->v.val = newele, defragged++;
        } else if (dict_val_type == DEFRAG_SDS_DICT_VAL_VOID_PTR) {
            void *newptr, *ptr = dictGetVal(de);
            if ((newptr = activeDefragAlloc(ptr)))
                de->v.val = newptr, defragged++;
        }
        defragged += dictIterDefragEntry(di);
    }
    dictReleaseIterator(di);

    return defragged;
}

/* Utility function that replaces an old key pointer in the dictionary with a
 * new pointer. Additionally, we try to defrag the dictEntry in that dict.
 * Oldkey mey be a dead pointer and should not be accessed (we get a
 * pre-calculated hash value). Newkey may be null if the key pointer wasn't
 * moved. Return value is the the dictEntry if found, or NULL if not found.
 * NOTE: this is very ugly code, but it let's us avoid the complication of
 * doing a scan on another dict. */
dictEntry* replaceSatelliteDictKeyPtrAndOrDefragDictEntry(dict *d, sds oldkey, sds newkey, uint64_t hash, long *defragged) {
    dictEntry **deref = dictFindEntryRefByPtrAndHash(d, oldkey, hash);
    if (deref) {
        dictEntry *de = *deref;
        dictEntry *newde = activeDefragAlloc(de);
        if (newde) {
            de = *deref = newde;
            (*defragged)++;
        }
        if (newkey)
            de->key = newkey;
        return de;
    }
    return NULL;
}

long activeDefragQuickListNode(quicklist *ql, quicklistNode **node_ref) {
    quicklistNode *newnode, *node = *node_ref;
    long defragged = 0;
    unsigned char *newzl;
    if ((newnode = activeDefragAlloc(node))) {
        if (newnode->prev)
            newnode->prev->next = newnode;
        else
            ql->head = newnode;
        if (newnode->next)
            newnode->next->prev = newnode;
        else
            ql->tail = newnode;
        *node_ref = node = newnode;
        defragged++;
    }
    if ((newzl = activeDefragAlloc(node->zl)))
        defragged++, node->zl = newzl;
    return defragged;
}

long activeDefragQuickListNodes(quicklist *ql) {
    quicklistNode *node = ql->head;
    long defragged = 0;
    while (node) {
        defragged += activeDefragQuickListNode(ql, &node);
        node = node->next;
    }
    return defragged;
}

/* when the value has lots of elements, we want to handle it later and not as
 * part of the main dictionary scan. this is needed in order to prevent latency
 * spikes when handling large items */
void defragLater(redisDb *db, dictEntry *kde) {
    sds key = sdsdup(dictGetKey(kde));
    listAddNodeTail(db->defrag_later, key);
}

/* returns 0 if no more work needs to be been done, and 1 if time is up and more work is needed. */
long scanLaterList(robj *ob, unsigned long *cursor, long long endtime, long long *defragged) {
    quicklist *ql = ob->ptr;
    quicklistNode *node;
    long iterations = 0;
    int bookmark_failed = 0;
    if (ob->type != OBJ_LIST || ob->encoding != OBJ_ENCODING_QUICKLIST)
        return 0;

    if (*cursor == 0) {
        /* if cursor is 0, we start new iteration */
        node = ql->head;
    } else {
        node = quicklistBookmarkFind(ql, "_AD");
        if (!node) {
            /* if the bookmark was deleted, it means we reached the end. */
            *cursor = 0;
            return 0;
        }
        node = node->next;
    }

    (*cursor)++;
    while (node) {
        (*defragged) += activeDefragQuickListNode(ql, &node);
        server.stat_active_defrag_scanned++;
        if (++iterations > 128 && !bookmark_failed) {
            if (ustime() > endtime) {
                if (!quicklistBookmarkCreate(&ql, "_AD", node)) {
                    bookmark_failed = 1;
                } else {
                    ob->ptr = ql; /* bookmark creation may have re-allocated the quicklist */
                    return 1;
                }
            }
            iterations = 0;
        }
        node = node->next;
    }
    quicklistBookmarkDelete(ql, "_AD");
    *cursor = 0;
    return bookmark_failed? 1: 0;
}

typedef struct {
    zset *zs;
    long defragged;
} scanLaterZsetData;

void scanLaterZsetCallback(void *privdata, const dictEntry *_de) {
    dictEntry *de = (dictEntry*)_de;
    scanLaterZsetData *data = privdata;
    data->defragged += activeDefragZsetEntry(data->zs, de);
    server.stat_active_defrag_scanned++;
}

long scanLaterZset(robj *ob, unsigned long *cursor) {
    if (ob->type != OBJ_ZSET || ob->encoding != OBJ_ENCODING_SKIPLIST)
        return 0;
    zset *zs = (zset*)ob->ptr;
    dict *d = zs->dict;
    scanLaterZsetData data = {zs, 0};
    *cursor = dictScan(d, *cursor, scanLaterZsetCallback, defragDictBucketCallback, &data);
    return data.defragged;
}

void scanLaterSetCallback(void *privdata, const dictEntry *_de) {
    dictEntry *de = (dictEntry*)_de;
    long *defragged = privdata;
    sds sdsele = dictGetKey(de), newsds;
    if ((newsds = activeDefragSds(sdsele)))
        (*defragged)++, de->key = newsds;
    server.stat_active_defrag_scanned++;
}

long scanLaterSet(robj *ob, unsigned long *cursor) {
    long defragged = 0;
    if (ob->type != OBJ_SET || ob->encoding != OBJ_ENCODING_HT)
        return 0;
    dict *d = ob->ptr;
    *cursor = dictScan(d, *cursor, scanLaterSetCallback, defragDictBucketCallback, &defragged);
    return defragged;
}

void scanLaterHashCallback(void *privdata, const dictEntry *_de) {
    dictEntry *de = (dictEntry*)_de;
    long *defragged = privdata;
    sds sdsele = dictGetKey(de), newsds;
    if ((newsds = activeDefragSds(sdsele)))
        (*defragged)++, de->key = newsds;
    sdsele = dictGetVal(de);
    if ((newsds = activeDefragSds(sdsele)))
        (*defragged)++, de->v.val = newsds;
    server.stat_active_defrag_scanned++;
}

long scanLaterHash(robj *ob, unsigned long *cursor) {
    long defragged = 0;
    if (ob->type != OBJ_HASH || ob->encoding != OBJ_ENCODING_HT)
        return 0;
    dict *d = ob->ptr;
    *cursor = dictScan(d, *cursor, scanLaterHashCallback, defragDictBucketCallback, &defragged);
    return defragged;
}

long defragQuicklist(redisDb *db, dictEntry *kde) {
    robj *ob = dictGetVal(kde);
    long defragged = 0;
    quicklist *ql = ob->ptr, *newql;
    serverAssert(ob->type == OBJ_LIST && ob->encoding == OBJ_ENCODING_QUICKLIST);
    if ((newql = activeDefragAlloc(ql)))
        defragged++, ob->ptr = ql = newql;
    if (ql->len > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else
        defragged += activeDefragQuickListNodes(ql);
    return defragged;
}

long defragZsetSkiplist(redisDb *db, dictEntry *kde) {
    robj *ob = dictGetVal(kde);
    long defragged = 0;
    zset *zs = (zset*)ob->ptr;
    zset *newzs;
    zskiplist *newzsl;
    dict *newdict;
    dictEntry *de;
    struct zskiplistNode *newheader;
    serverAssert(ob->type == OBJ_ZSET && ob->encoding == OBJ_ENCODING_SKIPLIST);
    if ((newzs = activeDefragAlloc(zs)))
        defragged++, ob->ptr = zs = newzs;
    if ((newzsl = activeDefragAlloc(zs->zsl)))
        defragged++, zs->zsl = newzsl;
    if ((newheader = activeDefragAlloc(zs->zsl->header)))
        defragged++, zs->zsl->header = newheader;
    if (dictSize(zs->dict) > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else {
        dictIterator *di = dictGetIterator(zs->dict);
        while((de = dictNext(di)) != NULL) {
            defragged += activeDefragZsetEntry(zs, de);
        }
        dictReleaseIterator(di);
    }
    /* handle the dict struct */
    if ((newdict = activeDefragAlloc(zs->dict)))
        defragged++, zs->dict = newdict;
    /* defrag the dict tables */
    defragged += dictDefragTables(zs->dict);
    return defragged;
}

long defragHash(redisDb *db, dictEntry *kde) {
    long defragged = 0;
    robj *ob = dictGetVal(kde);
    dict *d, *newd;
    serverAssert(ob->type == OBJ_HASH && ob->encoding == OBJ_ENCODING_HT);
    d = ob->ptr;
    if (dictSize(d) > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else
        defragged += activeDefragSdsDict(d, DEFRAG_SDS_DICT_VAL_IS_SDS);
    /* handle the dict struct */
    if ((newd = activeDefragAlloc(ob->ptr)))
        defragged++, ob->ptr = newd;
    /* defrag the dict tables */
    defragged += dictDefragTables(ob->ptr);
    return defragged;
}

long defragSet(redisDb *db, dictEntry *kde) {
    long defragged = 0;
    robj *ob = dictGetVal(kde);
    dict *d, *newd;
    serverAssert(ob->type == OBJ_SET && ob->encoding == OBJ_ENCODING_HT);
    d = ob->ptr;
    if (dictSize(d) > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else
        defragged += activeDefragSdsDict(d, DEFRAG_SDS_DICT_NO_VAL);
    /* handle the dict struct */
    if ((newd = activeDefragAlloc(ob->ptr)))
        defragged++, ob->ptr = newd;
    /* defrag the dict tables */
    defragged += dictDefragTables(ob->ptr);
    return defragged;
}

/* Defrag callback for radix tree iterator, called for each node,
 * used in order to defrag the nodes allocations. */
int defragRaxNode(raxNode **noderef) {
    raxNode *newnode = activeDefragAlloc(*noderef);
    if (newnode) {
        *noderef = newnode;
        return 1;
    }
    return 0;
}

/* returns 0 if no more work needs to be been done, and 1 if time is up and more work is needed. */
int scanLaterStreamListpacks(robj *ob, unsigned long *cursor, long long endtime, long long *defragged) {
    static unsigned char last[sizeof(streamID)];
    raxIterator ri;
    long iterations = 0;
    if (ob->type != OBJ_STREAM || ob->encoding != OBJ_ENCODING_STREAM) {
        *cursor = 0;
        return 0;
    }

    stream *s = ob->ptr;
    raxStart(&ri,s->rax);
    if (*cursor == 0) {
        /* if cursor is 0, we start new iteration */
        defragRaxNode(&s->rax->head);
        /* assign the iterator node callback before the seek, so that the
         * initial nodes that are processed till the first item are covered */
        ri.node_cb = defragRaxNode;
        raxSeek(&ri,"^",NULL,0);
    } else {
        /* if cursor is non-zero, we seek to the static 'last' */
        if (!raxSeek(&ri,">", last, sizeof(last))) {
            *cursor = 0;
            raxStop(&ri);
            return 0;
        }
        /* assign the iterator node callback after the seek, so that the
         * initial nodes that are processed till now aren't covered */
        ri.node_cb = defragRaxNode;
    }

    (*cursor)++;
    while (raxNext(&ri)) {
        void *newdata = activeDefragAlloc(ri.data);
        if (newdata)
            raxSetData(ri.node, ri.data=newdata), (*defragged)++;
        server.stat_active_defrag_scanned++;
        if (++iterations > 128) {
            if (ustime() > endtime) {
                serverAssert(ri.key_len==sizeof(last));
                memcpy(last,ri.key,ri.key_len);
                raxStop(&ri);
                return 1;
            }
            iterations = 0;
        }
    }
    raxStop(&ri);
    *cursor = 0;
    return 0;
}

/* optional callback used defrag each rax element (not including the element pointer itself) */
typedef void *(raxDefragFunction)(raxIterator *ri, void *privdata, long *defragged);

/* defrag radix tree including:
 * 1) rax struct
 * 2) rax nodes
 * 3) rax entry data (only if defrag_data is specified)
 * 4) call a callback per element, and allow the callback to return a new pointer for the element */
long defragRadixTree(rax **raxref, int defrag_data, raxDefragFunction *element_cb, void *element_cb_data) {
    long defragged = 0;
    raxIterator ri;
    rax* rax;
    if ((rax = activeDefragAlloc(*raxref)))
        defragged++, *raxref = rax;
    rax = *raxref;
    raxStart(&ri,rax);
    ri.node_cb = defragRaxNode;
    defragRaxNode(&rax->head);
    raxSeek(&ri,"^",NULL,0);
    while (raxNext(&ri)) {
        void *newdata = NULL;
        if (element_cb)
            newdata = element_cb(&ri, element_cb_data, &defragged);
        if (defrag_data && !newdata)
            newdata = activeDefragAlloc(ri.data);
        if (newdata)
            raxSetData(ri.node, ri.data=newdata), defragged++;
    }
    raxStop(&ri);
    return defragged;
}

typedef struct {
    streamCG *cg;
    streamConsumer *c;
} PendingEntryContext;

void* defragStreamConsumerPendingEntry(raxIterator *ri, void *privdata, long *defragged) {
    UNUSED(defragged);
    PendingEntryContext *ctx = privdata;
    streamNACK *nack = ri->data, *newnack;
    nack->consumer = ctx->c; /* update nack pointer to consumer */
    newnack = activeDefragAlloc(nack);
    if (newnack) {
        /* update consumer group pointer to the nack */
        void *prev;
        raxInsert(ctx->cg->pel, ri->key, ri->key_len, newnack, &prev);
        serverAssert(prev==nack);
        /* note: we don't increment 'defragged' that's done by the caller */
    }
    return newnack;
}

void* defragStreamConsumer(raxIterator *ri, void *privdata, long *defragged) {
    streamConsumer *c = ri->data;
    streamCG *cg = privdata;
    void *newc = activeDefragAlloc(c);
    if (newc) {
        /* note: we don't increment 'defragged' that's done by the caller */
        c = newc;
    }
    sds newsds = activeDefragSds(c->name);
    if (newsds)
        (*defragged)++, c->name = newsds;
    if (c->pel) {
        PendingEntryContext pel_ctx = {cg, c};
        *defragged += defragRadixTree(&c->pel, 0, defragStreamConsumerPendingEntry, &pel_ctx);
    }
    return newc; /* returns NULL if c was not defragged */
}

void* defragStreamConsumerGroup(raxIterator *ri, void *privdata, long *defragged) {
    streamCG *cg = ri->data;
    UNUSED(privdata);
    if (cg->consumers)
        *defragged += defragRadixTree(&cg->consumers, 0, defragStreamConsumer, cg);
    if (cg->pel)
        *defragged += defragRadixTree(&cg->pel, 0, NULL, NULL);
    return NULL;
}

long defragStream(redisDb *db, dictEntry *kde) {
    long defragged = 0;
    robj *ob = dictGetVal(kde);
    serverAssert(ob->type == OBJ_STREAM && ob->encoding == OBJ_ENCODING_STREAM);
    stream *s = ob->ptr, *news;

    /* handle the main struct */
    if ((news = activeDefragAlloc(s)))
        defragged++, ob->ptr = s = news;

    if (raxSize(s->rax) > server.active_defrag_max_scan_fields) {
        rax *newrax = activeDefragAlloc(s->rax);
        if (newrax)
            defragged++, s->rax = newrax;
        defragLater(db, kde);
    } else
        defragged += defragRadixTree(&s->rax, 1, NULL, NULL);

    if (s->cgroups)
        defragged += defragRadixTree(&s->cgroups, 1, defragStreamConsumerGroup, NULL);
    return defragged;
}

/* Defrag a module key. This is either done immediately or scheduled
 * for later. Returns then number of pointers defragged.
 */
long defragModule(redisDb *db, dictEntry *kde) {
    robj *obj = dictGetVal(kde);
    serverAssert(obj->type == OBJ_MODULE);
    long defragged = 0;

    if (!moduleDefragValue(dictGetKey(kde), obj, &defragged))
        defragLater(db, kde);

    return defragged;
}

/* for each key we scan in the main dict, this function will attempt to defrag
 * all the various pointers it has. Returns a stat of how many pointers were
 * moved. */
long defragKey(redisDb *db, dictEntry *de) {
    sds keysds = dictGetKey(de);
    robj *newob, *ob;
    unsigned char *newzl;
    long defragged = 0;
    sds newsds;

    /* Try to defrag the key name. */
    newsds = activeDefragSds(keysds);
    if (newsds)
        defragged++, de->key = newsds;
    if (dictSize(db->expires)) {
         /* Dirty code:
          * I can't search in db->expires for that key after i already released
          * the pointer it holds it won't be able to do the string compare */
        uint64_t hash = dictGetHash(db->dict, de->key);
        replaceSatelliteDictKeyPtrAndOrDefragDictEntry(db->expires, keysds, newsds, hash, &defragged);
    }

    /* Try to defrag robj and / or string value. */
    ob = dictGetVal(de);
    if ((newob = activeDefragStringOb(ob, &defragged))) {
        de->v.val = newob;
        ob = newob;
    }

    if (ob->type == OBJ_STRING) {
        /* Already handled in activeDefragStringOb. */
    } else if (ob->type == OBJ_LIST) {
        if (ob->encoding == OBJ_ENCODING_QUICKLIST) {
            defragged += defragQuicklist(db, de);
        } else if (ob->encoding == OBJ_ENCODING_ZIPLIST) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                defragged++, ob->ptr = newzl;
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (ob->type == OBJ_SET) {
        if (ob->encoding == OBJ_ENCODING_HT) {
            defragged += defragSet(db, de);
        } else if (ob->encoding == OBJ_ENCODING_INTSET) {
            intset *newis, *is = ob->ptr;
            if ((newis = activeDefragAlloc(is)))
                defragged++, ob->ptr = newis;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (ob->type == OBJ_ZSET) {
        if (ob->encoding == OBJ_ENCODING_ZIPLIST) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                defragged++, ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_SKIPLIST) {
            defragged += defragZsetSkiplist(db, de);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (ob->type == OBJ_HASH) {
        if (ob->encoding == OBJ_ENCODING_ZIPLIST) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                defragged++, ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_HT) {
            defragged += defragHash(db, de);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (ob->type == OBJ_STREAM) {
        defragged += defragStream(db, de);
    } else if (ob->type == OBJ_MODULE) {
        defragged += defragModule(db, de);
    } else {
        serverPanic("Unknown object type");
    }
    return defragged;
}

/* Defrag scan callback for the main db dictionary. */
void defragScanCallback(void *privdata, const dictEntry *de) {
    long defragged = defragKey((redisDb*)privdata, (dictEntry*)de);
    server.stat_active_defrag_hits += defragged;
    if(defragged)
        server.stat_active_defrag_key_hits++;
    else
        server.stat_active_defrag_key_misses++;
    server.stat_active_defrag_scanned++;
}

/* Defrag scan callback for each hash table bucket,
 * used in order to defrag the dictEntry allocations. */
void defragDictBucketCallback(void *privdata, dictEntry **bucketref) {
    UNUSED(privdata); /* NOTE: this function is also used by both activeDefragCycle and scanLaterHash, etc. don't use privdata */
    while(*bucketref) {
        dictEntry *de = *bucketref, *newde;
        if ((newde = activeDefragAlloc(de))) {
            *bucketref = newde;
        }
        bucketref = &(*bucketref)->next;
    }
}

/* Utility function to get the fragmentation ratio from jemalloc.
 * It is critical to do that by comparing only heap maps that belong to
 * jemalloc, and skip ones the jemalloc keeps as spare. Since we use this
 * fragmentation ratio in order to decide if a defrag action should be taken
 * or not, a false detection can cause the defragmenter to waste a lot of CPU
 * without the possibility of getting any results. */
float getAllocatorFragmentation(size_t *out_frag_bytes) {
    size_t resident, active, allocated;
    zmalloc_get_allocator_info(&allocated, &active, &resident);
    float frag_pct = ((float)active / allocated)*100 - 100;
    size_t frag_bytes = active - allocated;
    float rss_pct = ((float)resident / allocated)*100 - 100;
    size_t rss_bytes = resident - allocated;
    if(out_frag_bytes)
        *out_frag_bytes = frag_bytes;
    serverLog(LL_DEBUG,
        "allocated=%zu, active=%zu, resident=%zu, frag=%.0f%% (%.0f%% rss), frag_bytes=%zu (%zu rss)",
        allocated, active, resident, frag_pct, rss_pct, frag_bytes, rss_bytes);
    return frag_pct;
}

/* We may need to defrag other globals, one small allocation can hold a full allocator run.
 * so although small, it is still important to defrag these */
long defragOtherGlobals() {
    long defragged = 0;

    /* there are many more pointers to defrag (e.g. client argv, output / aof buffers, etc.
     * but we assume most of these are short lived, we only need to defrag allocations
     * that remain static for a long time */
    defragged += activeDefragSdsDict(server.lua_scripts, DEFRAG_SDS_DICT_VAL_IS_STROB);
    defragged += activeDefragSdsListAndDict(server.repl_scriptcache_fifo, server.repl_scriptcache_dict, DEFRAG_SDS_DICT_NO_VAL);
    defragged += moduleDefragGlobals();
    return defragged;
}

/* returns 0 more work may or may not be needed (see non-zero cursor),
 * and 1 if time is up and more work is needed. */
int defragLaterItem(dictEntry *de, unsigned long *cursor, long long endtime) {
    if (de) {
        robj *ob = dictGetVal(de);
        if (ob->type == OBJ_LIST) {
            return scanLaterList(ob, cursor, endtime, &server.stat_active_defrag_hits);
        } else if (ob->type == OBJ_SET) {
            server.stat_active_defrag_hits += scanLaterSet(ob, cursor);
        } else if (ob->type == OBJ_ZSET) {
            server.stat_active_defrag_hits += scanLaterZset(ob, cursor);
        } else if (ob->type == OBJ_HASH) {
            server.stat_active_defrag_hits += scanLaterHash(ob, cursor);
        } else if (ob->type == OBJ_STREAM) {
            return scanLaterStreamListpacks(ob, cursor, endtime, &server.stat_active_defrag_hits);
        } else if (ob->type == OBJ_MODULE) {
            return moduleLateDefrag(dictGetKey(de), ob, cursor, endtime, &server.stat_active_defrag_hits);
        } else {
            *cursor = 0; /* object type may have changed since we schedule it for later */
        }
    } else {
        *cursor = 0; /* object may have been deleted already */
    }
    return 0;
}

/* static variables serving defragLaterStep to continue scanning a key from were we stopped last time. */
static sds defrag_later_current_key = NULL;
static unsigned long defrag_later_cursor = 0;

/* returns 0 if no more work needs to be been done, and 1 if time is up and more work is needed. */
int defragLaterStep(redisDb *db, long long endtime) {
    unsigned int iterations = 0;
    unsigned long long prev_defragged = server.stat_active_defrag_hits;
    unsigned long long prev_scanned = server.stat_active_defrag_scanned;
    long long key_defragged;

    do {
        /* if we're not continuing a scan from the last call or loop, start a new one */
        if (!defrag_later_cursor) {
            listNode *head = listFirst(db->defrag_later);

            /* Move on to next key */
            if (defrag_later_current_key) {
                serverAssert(defrag_later_current_key == head->value);
                listDelNode(db->defrag_later, head);
                defrag_later_cursor = 0;
                defrag_later_current_key = NULL;
            }

            /* stop if we reached the last one. */
            head = listFirst(db->defrag_later);
            if (!head)
                return 0;

            /* start a new key */
            defrag_later_current_key = head->value;
            defrag_later_cursor = 0;
        }

        /* each time we enter this function we need to fetch the key from the dict again (if it still exists) */
        dictEntry *de = dictFind(db->dict, defrag_later_current_key);
        key_defragged = server.stat_active_defrag_hits;
        do {
            int quit = 0;
            if (defragLaterItem(de, &defrag_later_cursor, endtime))
                quit = 1; /* time is up, we didn't finish all the work */

            /* Once in 16 scan iterations, 512 pointer reallocations, or 64 fields
             * (if we have a lot of pointers in one hash bucket, or rehashing),
             * check if we reached the time limit. */
            if (quit || (++iterations > 16 ||
                            server.stat_active_defrag_hits - prev_defragged > 512 ||
                            server.stat_active_defrag_scanned - prev_scanned > 64)) {
                if (quit || ustime() > endtime) {
                    if(key_defragged != server.stat_active_defrag_hits)
                        server.stat_active_defrag_key_hits++;
                    else
                        server.stat_active_defrag_key_misses++;
                    return 1;
                }
                iterations = 0;
                prev_defragged = server.stat_active_defrag_hits;
                prev_scanned = server.stat_active_defrag_scanned;
            }
        } while(defrag_later_cursor);
        if(key_defragged != server.stat_active_defrag_hits)
            server.stat_active_defrag_key_hits++;
        else
            server.stat_active_defrag_key_misses++;
    } while(1);
}

#define INTERPOLATE(x, x1, x2, y1, y2) ( (y1) + ((x)-(x1)) * ((y2)-(y1)) / ((x2)-(x1)) )
#define LIMIT(y, min, max) ((y)<(min)? min: ((y)>(max)? max: (y)))

/* decide if defrag is needed, and at what CPU effort to invest in it */
void computeDefragCycles() {
    size_t frag_bytes;
    float frag_pct = getAllocatorFragmentation(&frag_bytes);
    /* If we're not already running, and below the threshold, exit. */
    if (!server.active_defrag_running) {
        if(frag_pct < server.active_defrag_threshold_lower || frag_bytes < server.active_defrag_ignore_bytes)
            return;
    }

    /* Calculate the adaptive aggressiveness of the defrag */
    int cpu_pct = INTERPOLATE(frag_pct,
            server.active_defrag_threshold_lower,
            server.active_defrag_threshold_upper,
            server.active_defrag_cycle_min,
            server.active_defrag_cycle_max);
    cpu_pct = LIMIT(cpu_pct,
            server.active_defrag_cycle_min,
            server.active_defrag_cycle_max);
     /* We allow increasing the aggressiveness during a scan, but don't
      * reduce it. */
    if (!server.active_defrag_running ||
        cpu_pct > server.active_defrag_running)
    {
        server.active_defrag_running = cpu_pct;
        serverLog(LL_VERBOSE,
            "Starting active defrag, frag=%.0f%%, frag_bytes=%zu, cpu=%d%%",
            frag_pct, frag_bytes, cpu_pct);
    }
}

/* Perform incremental defragmentation work from the serverCron.
 * This works in a similar way to activeExpireCycle, in the sense that
 * we do incremental work across calls. */
void activeDefragCycle(void) {
    static int current_db = -1;
    static unsigned long cursor = 0;
    static redisDb *db = NULL;
    static long long start_scan, start_stat;
    unsigned int iterations = 0;
    unsigned long long prev_defragged = server.stat_active_defrag_hits;
    unsigned long long prev_scanned = server.stat_active_defrag_scanned;
    long long start, timelimit, endtime;
    mstime_t latency;
    int quit = 0;

    if (!server.active_defrag_enabled) {
        if (server.active_defrag_running) {
            /* if active defrag was disabled mid-run, start from fresh next time. */
            server.active_defrag_running = 0;
            if (db)
                listEmpty(db->defrag_later);
            defrag_later_current_key = NULL;
            defrag_later_cursor = 0;
            current_db = -1;
            cursor = 0;
            db = NULL;
        }
        return;
    }

    if (hasActiveChildProcess())
        return; /* Defragging memory while there's a fork will just do damage. */

    /* Once a second, check if the fragmentation justfies starting a scan
     * or making it more aggressive. */
    run_with_period(1000) {
        computeDefragCycles();
    }
    if (!server.active_defrag_running)
        return;

    /* See activeExpireCycle for how timelimit is handled. */
    start = ustime();
    timelimit = 1000000*server.active_defrag_running/server.hz/100;
    if (timelimit <= 0) timelimit = 1;
    endtime = start + timelimit;
    latencyStartMonitor(latency);

    do {
        /* if we're not continuing a scan from the last call or loop, start a new one */
        if (!cursor) {
            /* finish any leftovers from previous db before moving to the next one */
            if (db && defragLaterStep(db, endtime)) {
                quit = 1; /* time is up, we didn't finish all the work */
                break; /* this will exit the function and we'll continue on the next cycle */
            }

            /* Move on to next database, and stop if we reached the last one. */
            if (++current_db >= server.dbnum) {
                /* defrag other items not part of the db / keys */
                defragOtherGlobals();

                long long now = ustime();
                size_t frag_bytes;
                float frag_pct = getAllocatorFragmentation(&frag_bytes);
                serverLog(LL_VERBOSE,
                    "Active defrag done in %dms, reallocated=%d, frag=%.0f%%, frag_bytes=%zu",
                    (int)((now - start_scan)/1000), (int)(server.stat_active_defrag_hits - start_stat), frag_pct, frag_bytes);

                start_scan = now;
                current_db = -1;
                cursor = 0;
                db = NULL;
                server.active_defrag_running = 0;

                computeDefragCycles(); /* if another scan is needed, start it right away */
                if (server.active_defrag_running != 0 && ustime() < endtime)
                    continue;
                break;
            }
            else if (current_db==0) {
                /* Start a scan from the first database. */
                start_scan = ustime();
                start_stat = server.stat_active_defrag_hits;
            }

            db = &server.db[current_db];
            cursor = 0;
        }

        do {
            /* before scanning the next bucket, see if we have big keys left from the previous bucket to scan */
            if (defragLaterStep(db, endtime)) {
                quit = 1; /* time is up, we didn't finish all the work */
                break; /* this will exit the function and we'll continue on the next cycle */
            }

            cursor = dictScan(db->dict, cursor, defragScanCallback, defragDictBucketCallback, db);

            /* Once in 16 scan iterations, 512 pointer reallocations. or 64 keys
             * (if we have a lot of pointers in one hash bucket or rehasing),
             * check if we reached the time limit.
             * But regardless, don't start a new db in this loop, this is because after
             * the last db we call defragOtherGlobals, which must be done in one cycle */
            if (!cursor || (++iterations > 16 ||
                            server.stat_active_defrag_hits - prev_defragged > 512 ||
                            server.stat_active_defrag_scanned - prev_scanned > 64)) {
                if (!cursor || ustime() > endtime) {
                    quit = 1;
                    break;
                }
                iterations = 0;
                prev_defragged = server.stat_active_defrag_hits;
                prev_scanned = server.stat_active_defrag_scanned;
            }
        } while(cursor && !quit);
    } while(!quit);

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("active-defrag-cycle",latency);
}

#else /* HAVE_DEFRAG */

void activeDefragCycle(void) {
    /* Not implemented yet. */
}

void *activeDefragAlloc(void *ptr) {
    UNUSED(ptr);
    return NULL;
}

robj *activeDefragStringOb(robj *ob, long *defragged) {
    UNUSED(ob);
    UNUSED(defragged);
    return NULL;
}

#endif
