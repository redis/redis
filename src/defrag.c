/* 
 * Active memory defragmentation
 * Try to find key / value allocations that need to be re-allocated in order 
 * to reduce external fragmentation.
 * We do that by scanning the keyspace and for each pointer we have, we can try to
 * ask the allocator if moving it to a new address will help reduce fragmentation.
 *
 * Copyright (c) 2020-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#include "server.h"
#include <stddef.h>

#ifdef HAVE_DEFRAG

typedef struct defragCtx {
    void *privdata;
    int slot;
} defragCtx;

typedef struct defragPubSubCtx {
    kvstore *pubsub_channels;
    dict *(*clientPubSubChannels)(client*);
} defragPubSubCtx;

/* this method was added to jemalloc in order to help us understand which
 * pointers are worthwhile moving and which aren't */
int je_get_defrag_hint(void* ptr);

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
    server.stat_active_defrag_hits++;
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

/* Defrag helper for robj and/or string objects with expected refcount.
 *
 * Like activeDefragStringOb, but it requires the caller to pass in the expected
 * reference count. In some cases, the caller needs to update a robj whose
 * reference count is not 1, in these cases, the caller must explicitly pass
 * in the reference count, otherwise defragmentation will not be performed.
 * Note that the caller is responsible for updating any other references to the robj. */
robj *activeDefragStringObEx(robj* ob, int expected_refcount) {
    robj *ret = NULL;
    if (ob->refcount!=expected_refcount)
        return NULL;

    /* try to defrag robj (only if not an EMBSTR type (handled below). */
    if (ob->type!=OBJ_STRING || ob->encoding!=OBJ_ENCODING_EMBSTR) {
        if ((ret = activeDefragAlloc(ob))) {
            ob = ret;
        }
    }

    /* try to defrag string object */
    if (ob->type == OBJ_STRING) {
        if(ob->encoding==OBJ_ENCODING_RAW) {
            sds newsds = activeDefragSds((sds)ob->ptr);
            if (newsds) {
                ob->ptr = newsds;
            }
        } else if (ob->encoding==OBJ_ENCODING_EMBSTR) {
            /* The sds is embedded in the object allocation, calculate the
             * offset and update the pointer in the new allocation. */
            long ofs = (intptr_t)ob->ptr - (intptr_t)ob;
            if ((ret = activeDefragAlloc(ob))) {
                ret->ptr = (void*)((intptr_t)ret + ofs);
            }
        } else if (ob->encoding!=OBJ_ENCODING_INT) {
            serverPanic("Unknown string encoding");
        }
    }
    return ret;
}

/* Defrag helper for robj and/or string objects
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
robj *activeDefragStringOb(robj* ob) {
    return activeDefragStringObEx(ob, 1);
}

/* Defrag helper for lua scripts
 *
 * returns NULL in case the allocation wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
luaScript *activeDefragLuaScript(luaScript *script) {
    luaScript *ret = NULL;

    /* try to defrag script struct */
    if ((ret = activeDefragAlloc(script))) {
        script = ret;
    }

    /* try to defrag actual script object */
    robj *ob = activeDefragStringOb(script->body);
    if (ob) script->body = ob;

    return ret;
}

/* Defrag helper for dict main allocations (dict struct, and hash tables).
 * Receives a pointer to the dict* and return a new dict* when the dict
 * struct itself was moved.
 * 
 * Returns NULL in case the allocation wasn't moved.
 * When it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
dict *dictDefragTables(dict *d) {
    dict *ret = NULL;
    dictEntry **newtable;
    /* handle the dict struct */
    if ((ret = activeDefragAlloc(d)))
        d = ret;
    /* handle the first hash table */
    if (!d->ht_table[0]) return ret; /* created but unused */
    newtable = activeDefragAlloc(d->ht_table[0]);
    if (newtable)
        d->ht_table[0] = newtable;
    /* handle the second hash table */
    if (d->ht_table[1]) {
        newtable = activeDefragAlloc(d->ht_table[1]);
        if (newtable)
            d->ht_table[1] = newtable;
    }
    return ret;
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
void activeDefragZsetEntry(zset *zs, dictEntry *de) {
    sds newsds;
    double* newscore;
    sds sdsele = dictGetKey(de);
    if ((newsds = activeDefragSds(sdsele)))
        dictSetKey(zs->dict, de, newsds);
    newscore = zslDefrag(zs->zsl, *(double*)dictGetVal(de), sdsele, newsds);
    if (newscore) {
        dictSetVal(zs->dict, de, newscore);
    }
}

#define DEFRAG_SDS_DICT_NO_VAL 0
#define DEFRAG_SDS_DICT_VAL_IS_SDS 1
#define DEFRAG_SDS_DICT_VAL_IS_STROB 2
#define DEFRAG_SDS_DICT_VAL_VOID_PTR 3
#define DEFRAG_SDS_DICT_VAL_LUA_SCRIPT 4

void activeDefragSdsDictCallback(void *privdata, const dictEntry *de) {
    UNUSED(privdata);
    UNUSED(de);
}

/* Defrag a dict with sds key and optional value (either ptr, sds or robj string) */
void activeDefragSdsDict(dict* d, int val_type) {
    unsigned long cursor = 0;
    dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = (dictDefragAllocFunction *)activeDefragSds,
        .defragVal = (val_type == DEFRAG_SDS_DICT_VAL_IS_SDS ? (dictDefragAllocFunction *)activeDefragSds :
                      val_type == DEFRAG_SDS_DICT_VAL_IS_STROB ? (dictDefragAllocFunction *)activeDefragStringOb :
                      val_type == DEFRAG_SDS_DICT_VAL_VOID_PTR ? (dictDefragAllocFunction *)activeDefragAlloc :
                      val_type == DEFRAG_SDS_DICT_VAL_LUA_SCRIPT ? (dictDefragAllocFunction *)activeDefragLuaScript :
                      NULL)
    };
    do {
        cursor = dictScanDefrag(d, cursor, activeDefragSdsDictCallback,
                                &defragfns, NULL);
    } while (cursor != 0);
}

/* Defrag a list of ptr, sds or robj string values */
void activeDefragList(list *l, int val_type) {
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
        }
        if (val_type == DEFRAG_SDS_DICT_VAL_IS_SDS) {
            sds newsds, sdsele = ln->value;
            if ((newsds = activeDefragSds(sdsele)))
                ln->value = newsds;
        } else if (val_type == DEFRAG_SDS_DICT_VAL_IS_STROB) {
            robj *newele, *ele = ln->value;
            if ((newele = activeDefragStringOb(ele)))
                ln->value = newele;
        } else if (val_type == DEFRAG_SDS_DICT_VAL_VOID_PTR) {
            void *newptr, *ptr = ln->value;
            if ((newptr = activeDefragAlloc(ptr)))
                ln->value = newptr;
        }
    }
}

void activeDefragQuickListNode(quicklist *ql, quicklistNode **node_ref) {
    quicklistNode *newnode, *node = *node_ref;
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
    }
    if ((newzl = activeDefragAlloc(node->entry)))
        node->entry = newzl;
}

void activeDefragQuickListNodes(quicklist *ql) {
    quicklistNode *node = ql->head;
    while (node) {
        activeDefragQuickListNode(ql, &node);
        node = node->next;
    }
}

/* when the value has lots of elements, we want to handle it later and not as
 * part of the main dictionary scan. this is needed in order to prevent latency
 * spikes when handling large items */
void defragLater(redisDb *db, dictEntry *kde) {
    sds key = sdsdup(dictGetKey(kde));
    listAddNodeTail(db->defrag_later, key);
}

/* returns 0 if no more work needs to be been done, and 1 if time is up and more work is needed. */
long scanLaterList(robj *ob, unsigned long *cursor, long long endtime) {
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
        activeDefragQuickListNode(ql, &node);
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
} scanLaterZsetData;

void scanLaterZsetCallback(void *privdata, const dictEntry *_de) {
    dictEntry *de = (dictEntry*)_de;
    scanLaterZsetData *data = privdata;
    activeDefragZsetEntry(data->zs, de);
    server.stat_active_defrag_scanned++;
}

void scanLaterZset(robj *ob, unsigned long *cursor) {
    if (ob->type != OBJ_ZSET || ob->encoding != OBJ_ENCODING_SKIPLIST)
        return;
    zset *zs = (zset*)ob->ptr;
    dict *d = zs->dict;
    scanLaterZsetData data = {zs};
    dictDefragFunctions defragfns = {.defragAlloc = activeDefragAlloc};
    *cursor = dictScanDefrag(d, *cursor, scanLaterZsetCallback, &defragfns, &data);
}

/* Used as scan callback when all the work is done in the dictDefragFunctions. */
void scanCallbackCountScanned(void *privdata, const dictEntry *de) {
    UNUSED(privdata);
    UNUSED(de);
    server.stat_active_defrag_scanned++;
}

void scanLaterSet(robj *ob, unsigned long *cursor) {
    if (ob->type != OBJ_SET || ob->encoding != OBJ_ENCODING_HT)
        return;
    dict *d = ob->ptr;
    dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = (dictDefragAllocFunction *)activeDefragSds
    };
    *cursor = dictScanDefrag(d, *cursor, scanCallbackCountScanned, &defragfns, NULL);
}

void scanLaterHash(robj *ob, unsigned long *cursor) {
    if (ob->type != OBJ_HASH || ob->encoding != OBJ_ENCODING_HT)
        return;
    dict *d = ob->ptr;
    dictDefragFunctions defragfns = {
        .defragAlloc = activeDefragAlloc,
        .defragKey = (dictDefragAllocFunction *)activeDefragSds,
        .defragVal = (dictDefragAllocFunction *)activeDefragSds
    };
    *cursor = dictScanDefrag(d, *cursor, scanCallbackCountScanned, &defragfns, NULL);
}

void defragQuicklist(redisDb *db, dictEntry *kde) {
    robj *ob = dictGetVal(kde);
    quicklist *ql = ob->ptr, *newql;
    serverAssert(ob->type == OBJ_LIST && ob->encoding == OBJ_ENCODING_QUICKLIST);
    if ((newql = activeDefragAlloc(ql)))
        ob->ptr = ql = newql;
    if (ql->len > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else
        activeDefragQuickListNodes(ql);
}

void defragZsetSkiplist(redisDb *db, dictEntry *kde) {
    robj *ob = dictGetVal(kde);
    zset *zs = (zset*)ob->ptr;
    zset *newzs;
    zskiplist *newzsl;
    dict *newdict;
    dictEntry *de;
    struct zskiplistNode *newheader;
    serverAssert(ob->type == OBJ_ZSET && ob->encoding == OBJ_ENCODING_SKIPLIST);
    if ((newzs = activeDefragAlloc(zs)))
        ob->ptr = zs = newzs;
    if ((newzsl = activeDefragAlloc(zs->zsl)))
        zs->zsl = newzsl;
    if ((newheader = activeDefragAlloc(zs->zsl->header)))
        zs->zsl->header = newheader;
    if (dictSize(zs->dict) > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else {
        dictIterator *di = dictGetIterator(zs->dict);
        while((de = dictNext(di)) != NULL) {
            activeDefragZsetEntry(zs, de);
        }
        dictReleaseIterator(di);
    }
    /* defrag the dict struct and tables */
    if ((newdict = dictDefragTables(zs->dict)))
        zs->dict = newdict;
}

void defragHash(redisDb *db, dictEntry *kde) {
    robj *ob = dictGetVal(kde);
    dict *d, *newd;
    serverAssert(ob->type == OBJ_HASH && ob->encoding == OBJ_ENCODING_HT);
    d = ob->ptr;
    if (dictSize(d) > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else
        activeDefragSdsDict(d, DEFRAG_SDS_DICT_VAL_IS_SDS);
    /* defrag the dict struct and tables */
    if ((newd = dictDefragTables(ob->ptr)))
        ob->ptr = newd;
}

void defragSet(redisDb *db, dictEntry *kde) {
    robj *ob = dictGetVal(kde);
    dict *d, *newd;
    serverAssert(ob->type == OBJ_SET && ob->encoding == OBJ_ENCODING_HT);
    d = ob->ptr;
    if (dictSize(d) > server.active_defrag_max_scan_fields)
        defragLater(db, kde);
    else
        activeDefragSdsDict(d, DEFRAG_SDS_DICT_NO_VAL);
    /* defrag the dict struct and tables */
    if ((newd = dictDefragTables(ob->ptr)))
        ob->ptr = newd;
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
int scanLaterStreamListpacks(robj *ob, unsigned long *cursor, long long endtime) {
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
            raxSetData(ri.node, ri.data=newdata);
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
typedef void *(raxDefragFunction)(raxIterator *ri, void *privdata);

/* defrag radix tree including:
 * 1) rax struct
 * 2) rax nodes
 * 3) rax entry data (only if defrag_data is specified)
 * 4) call a callback per element, and allow the callback to return a new pointer for the element */
void defragRadixTree(rax **raxref, int defrag_data, raxDefragFunction *element_cb, void *element_cb_data) {
    raxIterator ri;
    rax* rax;
    if ((rax = activeDefragAlloc(*raxref)))
        *raxref = rax;
    rax = *raxref;
    raxStart(&ri,rax);
    ri.node_cb = defragRaxNode;
    defragRaxNode(&rax->head);
    raxSeek(&ri,"^",NULL,0);
    while (raxNext(&ri)) {
        void *newdata = NULL;
        if (element_cb)
            newdata = element_cb(&ri, element_cb_data);
        if (defrag_data && !newdata)
            newdata = activeDefragAlloc(ri.data);
        if (newdata)
            raxSetData(ri.node, ri.data=newdata);
    }
    raxStop(&ri);
}

typedef struct {
    streamCG *cg;
    streamConsumer *c;
} PendingEntryContext;

void* defragStreamConsumerPendingEntry(raxIterator *ri, void *privdata) {
    PendingEntryContext *ctx = privdata;
    streamNACK *nack = ri->data, *newnack;
    nack->consumer = ctx->c; /* update nack pointer to consumer */
    newnack = activeDefragAlloc(nack);
    if (newnack) {
        /* update consumer group pointer to the nack */
        void *prev;
        raxInsert(ctx->cg->pel, ri->key, ri->key_len, newnack, &prev);
        serverAssert(prev==nack);
    }
    return newnack;
}

void* defragStreamConsumer(raxIterator *ri, void *privdata) {
    streamConsumer *c = ri->data;
    streamCG *cg = privdata;
    void *newc = activeDefragAlloc(c);
    if (newc) {
        c = newc;
    }
    sds newsds = activeDefragSds(c->name);
    if (newsds)
        c->name = newsds;
    if (c->pel) {
        PendingEntryContext pel_ctx = {cg, c};
        defragRadixTree(&c->pel, 0, defragStreamConsumerPendingEntry, &pel_ctx);
    }
    return newc; /* returns NULL if c was not defragged */
}

void* defragStreamConsumerGroup(raxIterator *ri, void *privdata) {
    streamCG *cg = ri->data;
    UNUSED(privdata);
    if (cg->consumers)
        defragRadixTree(&cg->consumers, 0, defragStreamConsumer, cg);
    if (cg->pel)
        defragRadixTree(&cg->pel, 0, NULL, NULL);
    return NULL;
}

void defragStream(redisDb *db, dictEntry *kde) {
    robj *ob = dictGetVal(kde);
    serverAssert(ob->type == OBJ_STREAM && ob->encoding == OBJ_ENCODING_STREAM);
    stream *s = ob->ptr, *news;

    /* handle the main struct */
    if ((news = activeDefragAlloc(s)))
        ob->ptr = s = news;

    if (raxSize(s->rax) > server.active_defrag_max_scan_fields) {
        rax *newrax = activeDefragAlloc(s->rax);
        if (newrax)
            s->rax = newrax;
        defragLater(db, kde);
    } else
        defragRadixTree(&s->rax, 1, NULL, NULL);

    if (s->cgroups)
        defragRadixTree(&s->cgroups, 1, defragStreamConsumerGroup, NULL);
}

/* Defrag a module key. This is either done immediately or scheduled
 * for later. Returns then number of pointers defragged.
 */
void defragModule(redisDb *db, dictEntry *kde) {
    robj *obj = dictGetVal(kde);
    serverAssert(obj->type == OBJ_MODULE);

    if (!moduleDefragValue(dictGetKey(kde), obj, db->id))
        defragLater(db, kde);
}

/* for each key we scan in the main dict, this function will attempt to defrag
 * all the various pointers it has. */
void defragKey(defragCtx *ctx, dictEntry *de) {
    sds keysds = dictGetKey(de);
    robj *newob, *ob;
    unsigned char *newzl;
    sds newsds;
    redisDb *db = ctx->privdata;
    int slot = ctx->slot;
    /* Try to defrag the key name. */
    newsds = activeDefragSds(keysds);
    if (newsds) {
        kvstoreDictSetKey(db->keys, slot, de, newsds);
        if (kvstoreDictSize(db->expires, slot)) {
            /* We can't search in db->expires for that key after we've released
             * the pointer it holds, since it won't be able to do the string
             * compare, but we can find the entry using key hash and pointer. */
            uint64_t hash = kvstoreGetHash(db->expires, newsds);
            dictEntry *expire_de = kvstoreDictFindEntryByPtrAndHash(db->expires, slot, keysds, hash);
            if (expire_de) kvstoreDictSetKey(db->expires, slot, expire_de, newsds);
        }
    }

    /* Try to defrag robj and / or string value. */
    ob = dictGetVal(de);
    if ((newob = activeDefragStringOb(ob))) {
        kvstoreDictSetVal(db->keys, slot, de, newob);
        ob = newob;
    }

    if (ob->type == OBJ_STRING) {
        /* Already handled in activeDefragStringOb. */
    } else if (ob->type == OBJ_LIST) {
        if (ob->encoding == OBJ_ENCODING_QUICKLIST) {
            defragQuicklist(db, de);
        } else if (ob->encoding == OBJ_ENCODING_LISTPACK) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                ob->ptr = newzl;
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (ob->type == OBJ_SET) {
        if (ob->encoding == OBJ_ENCODING_HT) {
            defragSet(db, de);
        } else if (ob->encoding == OBJ_ENCODING_INTSET ||
                   ob->encoding == OBJ_ENCODING_LISTPACK)
        {
            void *newptr, *ptr = ob->ptr;
            if ((newptr = activeDefragAlloc(ptr)))
                ob->ptr = newptr;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (ob->type == OBJ_ZSET) {
        if (ob->encoding == OBJ_ENCODING_LISTPACK) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_SKIPLIST) {
            defragZsetSkiplist(db, de);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (ob->type == OBJ_HASH) {
        if (ob->encoding == OBJ_ENCODING_LISTPACK) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_HT) {
            defragHash(db, de);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (ob->type == OBJ_STREAM) {
        defragStream(db, de);
    } else if (ob->type == OBJ_MODULE) {
        defragModule(db, de);
    } else {
        serverPanic("Unknown object type");
    }
}

/* Defrag scan callback for the main db dictionary. */
void defragScanCallback(void *privdata, const dictEntry *de) {
    long long hits_before = server.stat_active_defrag_hits;
    defragKey((defragCtx*)privdata, (dictEntry*)de);
    if (server.stat_active_defrag_hits != hits_before)
        server.stat_active_defrag_key_hits++;
    else
        server.stat_active_defrag_key_misses++;
    server.stat_active_defrag_scanned++;
}

/* Utility function to get the fragmentation ratio from jemalloc.
 * It is critical to do that by comparing only heap maps that belong to
 * jemalloc, and skip ones the jemalloc keeps as spare. Since we use this
 * fragmentation ratio in order to decide if a defrag action should be taken
 * or not, a false detection can cause the defragmenter to waste a lot of CPU
 * without the possibility of getting any results. */
float getAllocatorFragmentation(size_t *out_frag_bytes) {
    size_t resident, active, allocated, frag_smallbins_bytes;
    zmalloc_get_allocator_info(&allocated, &active, &resident, NULL, NULL, &frag_smallbins_bytes);

    /* Calculate the fragmentation ratio as the proportion of wasted memory in small
     * bins (which are defraggable) relative to the total allocated memory (including large bins).
     * This is because otherwise, if most of the memory usage is large bins, we may show high percentage,
     * despite the fact it's not a lot of memory for the user. */
    float frag_pct = (float)frag_smallbins_bytes / allocated * 100;
    float rss_pct = ((float)resident / allocated)*100 - 100;
    size_t rss_bytes = resident - allocated;
    if(out_frag_bytes)
        *out_frag_bytes = frag_smallbins_bytes;
    serverLog(LL_DEBUG,
        "allocated=%zu, active=%zu, resident=%zu, frag=%.2f%% (%.2f%% rss), frag_bytes=%zu (%zu rss)",
        allocated, active, resident, frag_pct, rss_pct, frag_smallbins_bytes, rss_bytes);
    return frag_pct;
}

/* Defrag scan callback for the pubsub dictionary. */
void defragPubsubScanCallback(void *privdata, const dictEntry *de) {
    defragCtx *ctx = privdata;
    defragPubSubCtx *pubsub_ctx = ctx->privdata;
    kvstore *pubsub_channels = pubsub_ctx->pubsub_channels;
    robj *newchannel, *channel = dictGetKey(de);
    dict *newclients, *clients = dictGetVal(de);

    /* Try to defrag the channel name. */
    serverAssert(channel->refcount == (int)dictSize(clients) + 1);
    newchannel = activeDefragStringObEx(channel, dictSize(clients) + 1);
    if (newchannel) {
        kvstoreDictSetKey(pubsub_channels, ctx->slot, (dictEntry*)de, newchannel);

        /* The channel name is shared by the client's pubsub(shard) and server's
         * pubsub(shard), after defraging the channel name, we need to update
         * the reference in the clients' dictionary. */
        dictIterator *di = dictGetIterator(clients);
        dictEntry *clientde;
        while((clientde = dictNext(di)) != NULL) {
            client *c = dictGetKey(clientde);
            dictEntry *pubsub_channel = dictFind(pubsub_ctx->clientPubSubChannels(c), newchannel);
            serverAssert(pubsub_channel);
            dictSetKey(pubsub_ctx->clientPubSubChannels(c), pubsub_channel, newchannel);
        }
        dictReleaseIterator(di);
    }

    /* Try to defrag the dictionary of clients that is stored as the value part. */
    if ((newclients = dictDefragTables(clients)))
        kvstoreDictSetVal(pubsub_channels, ctx->slot, (dictEntry*)de, newclients);

    server.stat_active_defrag_scanned++;
}

/* We may need to defrag other globals, one small allocation can hold a full allocator run.
 * so although small, it is still important to defrag these */
void defragOtherGlobals(void) {

    /* there are many more pointers to defrag (e.g. client argv, output / aof buffers, etc.
     * but we assume most of these are short lived, we only need to defrag allocations
     * that remain static for a long time */
    activeDefragSdsDict(evalScriptsDict(), DEFRAG_SDS_DICT_VAL_LUA_SCRIPT);
    moduleDefragGlobals();
    kvstoreDictLUTDefrag(server.pubsub_channels, dictDefragTables);
    kvstoreDictLUTDefrag(server.pubsubshard_channels, dictDefragTables);
}

/* returns 0 more work may or may not be needed (see non-zero cursor),
 * and 1 if time is up and more work is needed. */
int defragLaterItem(dictEntry *de, unsigned long *cursor, long long endtime, int dbid) {
    if (de) {
        robj *ob = dictGetVal(de);
        if (ob->type == OBJ_LIST) {
            return scanLaterList(ob, cursor, endtime);
        } else if (ob->type == OBJ_SET) {
            scanLaterSet(ob, cursor);
        } else if (ob->type == OBJ_ZSET) {
            scanLaterZset(ob, cursor);
        } else if (ob->type == OBJ_HASH) {
            scanLaterHash(ob, cursor);
        } else if (ob->type == OBJ_STREAM) {
            return scanLaterStreamListpacks(ob, cursor, endtime);
        } else if (ob->type == OBJ_MODULE) {
            return moduleLateDefrag(dictGetKey(de), ob, cursor, endtime, dbid);
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
int defragLaterStep(redisDb *db, int slot, long long endtime) {
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
        dictEntry *de = kvstoreDictFind(db->keys, slot, defrag_later_current_key);
        key_defragged = server.stat_active_defrag_hits;
        do {
            int quit = 0;
            if (defragLaterItem(de, &defrag_later_cursor, endtime,db->id))
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
void computeDefragCycles(void) {
    size_t frag_bytes;
    float frag_pct = getAllocatorFragmentation(&frag_bytes);
    /* If we're not already running, and below the threshold, exit. */
    if (!server.active_defrag_running) {
        if(frag_pct < server.active_defrag_threshold_lower || frag_bytes < server.active_defrag_ignore_bytes)
            return;
    }

    /* Calculate the adaptive aggressiveness of the defrag based on the current
     * fragmentation and configurations. */
    int cpu_pct = INTERPOLATE(frag_pct,
            server.active_defrag_threshold_lower,
            server.active_defrag_threshold_upper,
            server.active_defrag_cycle_min,
            server.active_defrag_cycle_max);
    cpu_pct = LIMIT(cpu_pct,
            server.active_defrag_cycle_min,
            server.active_defrag_cycle_max);

    /* Normally we allow increasing the aggressiveness during a scan, but don't
     * reduce it, since we should not lower the aggressiveness when fragmentation
     * drops. But when a configuration is made, we should reconsider it. */
    if (cpu_pct > server.active_defrag_running ||
        server.active_defrag_configuration_changed)
    {
        server.active_defrag_running = cpu_pct;
        server.active_defrag_configuration_changed = 0;
        serverLog(LL_VERBOSE,
            "Starting active defrag, frag=%.0f%%, frag_bytes=%zu, cpu=%d%%",
            frag_pct, frag_bytes, cpu_pct);
    }
}

/* Perform incremental defragmentation work from the serverCron.
 * This works in a similar way to activeExpireCycle, in the sense that
 * we do incremental work across calls. */
void activeDefragCycle(void) {
    static int slot = -1;
    static int current_db = -1;
    static int defrag_later_item_in_progress = 0;
    static int defrag_stage = 0;
    static unsigned long defrag_cursor = 0;
    static redisDb *db = NULL;
    static long long start_scan, start_stat;
    unsigned int iterations = 0;
    unsigned long long prev_defragged = server.stat_active_defrag_hits;
    unsigned long long prev_scanned = server.stat_active_defrag_scanned;
    long long start, timelimit, endtime;
    mstime_t latency;
    int all_stages_finished = 0;
    int quit = 0;

    if (!server.active_defrag_enabled) {
        if (server.active_defrag_running) {
            /* if active defrag was disabled mid-run, start from fresh next time. */
            server.active_defrag_running = 0;
            server.active_defrag_configuration_changed = 0;
            if (db)
                listEmpty(db->defrag_later);
            defrag_later_current_key = NULL;
            defrag_later_cursor = 0;
            current_db = -1;
            defrag_stage = 0;
            defrag_cursor = 0;
            slot = -1;
            defrag_later_item_in_progress = 0;
            db = NULL;
            goto update_metrics;
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

    /* Normally it is checked once a second, but when there is a configuration
     * change, we want to check it as soon as possible. */
    if (server.active_defrag_configuration_changed) {
        computeDefragCycles();
        server.active_defrag_configuration_changed = 0;
    }

    if (!server.active_defrag_running)
        return;

    /* See activeExpireCycle for how timelimit is handled. */
    start = ustime();
    timelimit = 1000000*server.active_defrag_running/server.hz/100;
    if (timelimit <= 0) timelimit = 1;
    endtime = start + timelimit;
    latencyStartMonitor(latency);

    dictDefragFunctions defragfns = {.defragAlloc = activeDefragAlloc};
    do {
        /* if we're not continuing a scan from the last call or loop, start a new one */
        if (!defrag_stage && !defrag_cursor && (slot < 0)) {
            /* finish any leftovers from previous db before moving to the next one */
            if (db && defragLaterStep(db, slot, endtime)) {
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
                defrag_stage = 0;
                defrag_cursor = 0;
                slot = -1;
                defrag_later_item_in_progress = 0;
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
            kvstoreDictLUTDefrag(db->keys, dictDefragTables);
            kvstoreDictLUTDefrag(db->expires, dictDefragTables);
            defrag_stage = 0;
            defrag_cursor = 0;
            slot = -1;
            defrag_later_item_in_progress = 0;
        }

        /* This array of structures holds the parameters for all defragmentation stages. */
        typedef struct defragStage {
            kvstore *kvs;
            dictScanFunction *scanfn;
            void *privdata;
        } defragStage;
        defragStage defrag_stages[] = {
            {db->keys, defragScanCallback, db},
            {db->expires, scanCallbackCountScanned, NULL},
            {server.pubsub_channels, defragPubsubScanCallback,
                &(defragPubSubCtx){server.pubsub_channels, getClientPubSubChannels}},
            {server.pubsubshard_channels, defragPubsubScanCallback,
                &(defragPubSubCtx){server.pubsubshard_channels, getClientPubSubShardChannels}},
        };
        do {
            int num_stages = sizeof(defrag_stages) / sizeof(defrag_stages[0]);
            serverAssert(defrag_stage < num_stages);
            defragStage *current_stage = &defrag_stages[defrag_stage];

            /* before scanning the next bucket, see if we have big keys left from the previous bucket to scan */
            if (defragLaterStep(db, slot, endtime)) {
                quit = 1; /* time is up, we didn't finish all the work */
                break; /* this will exit the function and we'll continue on the next cycle */
            }

            if (!defrag_later_item_in_progress) {
                /* Continue defragmentation from the previous stage.
                 * If slot is -1, it means this stage starts from the first non-empty slot. */
                if (slot == -1) slot = kvstoreGetFirstNonEmptyDictIndex(current_stage->kvs);
                defrag_cursor = kvstoreDictScanDefrag(current_stage->kvs, slot, defrag_cursor,
                    current_stage->scanfn, &defragfns, &(defragCtx){current_stage->privdata, slot});
            }

            if (!defrag_cursor) {
                /* Move to the next slot only if regular and large item scanning has been completed. */
                if (listLength(db->defrag_later) > 0) {
                    defrag_later_item_in_progress = 1;
                    continue;
                }

                /* Move to the next slot in the current stage. If we've reached the end, move to the next stage. */
                if ((slot = kvstoreGetNextNonEmptyDictIndex(current_stage->kvs, slot)) == -1)
                    defrag_stage++;
                defrag_later_item_in_progress = 0;
            }

            /* Check if all defragmentation stages have been processed.
             * If so, mark as finished and reset the stage counter to move on to next database. */
            if (defrag_stage == num_stages) {
                all_stages_finished = 1;
                defrag_stage = 0;
            }
    
            /* Once in 16 scan iterations, 512 pointer reallocations. or 64 keys
             * (if we have a lot of pointers in one hash bucket or rehashing),
             * check if we reached the time limit.
             * But regardless, don't start a new db in this loop, this is because after
             * the last db we call defragOtherGlobals, which must be done in one cycle */
            if (all_stages_finished ||
                ++iterations > 16 ||
                server.stat_active_defrag_hits - prev_defragged > 512 ||
                server.stat_active_defrag_scanned - prev_scanned > 64)
            {
                /* Quit if all stages were finished or timeout. */
                if (all_stages_finished || ustime() > endtime) {
                    quit = 1;
                    break;
                }
                iterations = 0;
                prev_defragged = server.stat_active_defrag_hits;
                prev_scanned = server.stat_active_defrag_scanned;
            }
        } while(!all_stages_finished && !quit);
    } while(!quit);

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("active-defrag-cycle",latency);

update_metrics:
    if (server.active_defrag_running > 0) {
        if (server.stat_last_active_defrag_time == 0)
            elapsedStart(&server.stat_last_active_defrag_time);
    } else if (server.stat_last_active_defrag_time != 0) {
        server.stat_total_active_defrag_time += elapsedUs(server.stat_last_active_defrag_time);
        server.stat_last_active_defrag_time = 0;
    }
}

#else /* HAVE_DEFRAG */

void activeDefragCycle(void) {
    /* Not implemented yet. */
}

void *activeDefragAlloc(void *ptr) {
    UNUSED(ptr);
    return NULL;
}

robj *activeDefragStringOb(robj *ob) {
    UNUSED(ob);
    return NULL;
}

#endif
