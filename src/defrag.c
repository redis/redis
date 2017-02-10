/* 
 * Active memory defragmentation
 * Try to find key / value allocations that need to be re-allocated in order 
 * to reduce external fragmentation.
 * We do that by scanning the keyspace and for each pointer we have, we can try to
 * ask the allocator if moving it to a new address will help reduce fragmentation.
 *
 * Copyright (c) 2017, Oran Agra
 * Copyright (c) 2017, Redis Labs, Inc
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
int je_get_defrag_hint(void* ptr, int *bin_util, int *run_util);

/* Defrag helper for generic allocations.
 *
 * returns NULL in case the allocatoin wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
void* activeDefragAlloc(void *ptr) {
    int bin_util, run_util;
    size_t size;
    void *newptr;
    if(!je_get_defrag_hint(ptr, &bin_util, &run_util)) {
        server.stat_active_defrag_misses++;
        return NULL;
    }
    /* if this run is more utilized than the average utilization in this bin
     * (or it is full), skip it. This will eventually move all the allocations
     * from relatively empty runs into relatively full runs. */
    if (run_util > bin_util || run_util == 1<<16) {
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
 * returns NULL in case the allocatoin wasn't moved.
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
 * returns NULL in case the allocatoin wasn't moved.
 * when it returns a non-null value, the old pointer was already released
 * and should NOT be accessed. */
robj *activeDefragStringOb(robj* ob, int *defragged) {
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
 * each step). Teturns a stat of how many pointers were moved. */
int dictIterDefragEntry(dictIterator *iter) {
    /* This function is a little bit dirty since it messes with the internals
     * of the dict and it's iterator, but the benefit is that it is very easy
     * to use, and require no other chagnes in the dict. */
    int defragged = 0;
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
int dictDefragTables(dict** dictRef) {
    dict *d = *dictRef;
    dictEntry **newtable;
    int defragged = 0;
    /* handle the dict struct */
    dict *newd = activeDefragAlloc(d);
    if (newd)
        defragged++, *dictRef = d = newd;
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

/* Utility function that replaces an old key pointer in the dictionary with a
 * new pointer. Additionally, we try to defrag the dictEntry in that dict.
 * Oldkey mey be a dead pointer and should not be accessed (we get a
 * pre-calculated hash value). Newkey may be null if the key pointer wasn't
 * moved. Return value is the the dictEntry if found, or NULL if not found.
 * NOTE: this is very ugly code, but it let's us avoid the complication of
 * doing a scan on another dict. */
dictEntry* replaceSateliteDictKeyPtrAndOrDefragDictEntry(dict *d, sds oldkey, sds newkey, unsigned int hash, int *defragged) {
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

/* for each key we scan in the main dict, this function will attempt to defrag
 * all the various pointers it has. Returns a stat of how many pointers were
 * moved. */
int defragKey(redisDb *db, dictEntry *de) {
    sds keysds = dictGetKey(de);
    robj *newob, *ob;
    unsigned char *newzl;
    dict *d;
    dictIterator *di;
    int defragged = 0;
    sds newsds;

    /* Try to defrag the key name. */
    newsds = activeDefragSds(keysds);
    if (newsds)
        defragged++, de->key = newsds;
    if (dictSize(db->expires)) {
         /* Dirty code:
          * I can't search in db->expires for that key after i already released
          * the pointer it holds it won't be able to do the string compare */
        unsigned int hash = dictGetHash(db->dict, de->key);
        replaceSateliteDictKeyPtrAndOrDefragDictEntry(db->expires, keysds, newsds, hash, &defragged);
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
            quicklist *ql = ob->ptr, *newql;
            quicklistNode *node = ql->head, *newnode;
            if ((newql = activeDefragAlloc(ql)))
                defragged++, ob->ptr = ql = newql;
            while (node) {
                if ((newnode = activeDefragAlloc(node))) {
                    if (newnode->prev)
                        newnode->prev->next = newnode;
                    else
                        ql->head = newnode;
                    if (newnode->next)
                        newnode->next->prev = newnode;
                    else
                        ql->tail = newnode;
                    node = newnode;
                    defragged++;
                }
                if ((newzl = activeDefragAlloc(node->zl)))
                    defragged++, node->zl = newzl;
                node = node->next;
            }
        } else if (ob->encoding == OBJ_ENCODING_ZIPLIST) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                defragged++, ob->ptr = newzl;
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (ob->type == OBJ_SET) {
        if (ob->encoding == OBJ_ENCODING_HT) {
            d = ob->ptr;
            di = dictGetIterator(d);
            while((de = dictNext(di)) != NULL) {
                sds sdsele = dictGetKey(de);
                if ((newsds = activeDefragSds(sdsele)))
                    defragged++, de->key = newsds;
                defragged += dictIterDefragEntry(di);
            }
            dictReleaseIterator(di);
            dictDefragTables((dict**)&ob->ptr);
        } else if (ob->encoding == OBJ_ENCODING_INTSET) {
            intset *is = ob->ptr;
            intset *newis = activeDefragAlloc(is);
            if (newis)
                defragged++, ob->ptr = newis;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (ob->type == OBJ_ZSET) {
        if (ob->encoding == OBJ_ENCODING_ZIPLIST) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                defragged++, ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = (zset*)ob->ptr;
            zset *newzs;
            zskiplist *newzsl;
            struct zskiplistNode *newheader;
            if ((newzs = activeDefragAlloc(zs)))
                defragged++, ob->ptr = zs = newzs;
            if ((newzsl = activeDefragAlloc(zs->zsl)))
                defragged++, zs->zsl = newzsl;
            if ((newheader = activeDefragAlloc(zs->zsl->header)))
                defragged++, zs->zsl->header = newheader;
            d = zs->dict;
            di = dictGetIterator(d);
            while((de = dictNext(di)) != NULL) {
                double* newscore;
                sds sdsele = dictGetKey(de);
                if ((newsds = activeDefragSds(sdsele)))
                    defragged++, de->key = newsds;
                newscore = zslDefrag(zs->zsl, *(double*)dictGetVal(de), sdsele, newsds);
                if (newscore) {
                    dictSetVal(d, de, newscore);
                    defragged++;
                }
                defragged += dictIterDefragEntry(di);
            }
            dictReleaseIterator(di);
            dictDefragTables(&zs->dict);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (ob->type == OBJ_HASH) {
        if (ob->encoding == OBJ_ENCODING_ZIPLIST) {
            if ((newzl = activeDefragAlloc(ob->ptr)))
                defragged++, ob->ptr = newzl;
        } else if (ob->encoding == OBJ_ENCODING_HT) {
            d = ob->ptr;
            di = dictGetIterator(d);
            while((de = dictNext(di)) != NULL) {
                sds sdsele = dictGetKey(de);
                if ((newsds = activeDefragSds(sdsele)))
                    defragged++, de->key = newsds;
                sdsele = dictGetVal(de);
                if ((newsds = activeDefragSds(sdsele)))
                    defragged++, de->v.val = newsds;
                defragged += dictIterDefragEntry(di);
            }
            dictReleaseIterator(di);
            dictDefragTables((dict**)&ob->ptr);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (ob->type == OBJ_MODULE) {
        /* Currently defragmenting modules private data types
         * is not supported. */
    } else {
        serverPanic("Unknown object type");
    }
    return defragged;
}

/* Defrag scan callback for the main db dictionary. */
void defragScanCallback(void *privdata, const dictEntry *de) {
    int defragged = defragKey((redisDb*)privdata, (dictEntry*)de);
    server.stat_active_defrag_hits += defragged;
    if(defragged)
        server.stat_active_defrag_key_hits++;
    else
        server.stat_active_defrag_key_misses++;
}

/* Defrag scan callback for for each hash table bicket,
 * used in order to defrag the dictEntry allocations. */
void defragDictBucketCallback(void *privdata, dictEntry **bucketref) {
    UNUSED(privdata);
    while(*bucketref) {
        dictEntry *de = *bucketref, *newde;
        if ((newde = activeDefragAlloc(de))) {
            *bucketref = newde;
        }
        bucketref = &(*bucketref)->next;
    }
}

/* Utility function to get the fragmentation ratio from jemalloc.
 * It is critical to do that by comparing only heap maps that belown to
 * jemalloc, and skip ones the jemalloc keeps as spare. Since we use this
 * fragmentation ratio in order to decide if a defrag action should be taken
 * or not, a false detection can cause the defragmenter to waste a lot of CPU
 * without the possibility of getting any results. */
float getAllocatorFragmentation(size_t *out_frag_bytes) {
    size_t epoch = 1, allocated = 0, resident = 0, active = 0, sz = sizeof(size_t);
    /* Update the statistics cached by mallctl. */
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);
    /* Unlike RSS, this does not include RSS from shared libraries and other non
     * heap mappings. */
    je_mallctl("stats.resident", &resident, &sz, NULL, 0);
    /* Unlike resident, this doesn't not include the pages jemalloc reserves
     * for re-use (purge will clean that). */
    je_mallctl("stats.active", &active, &sz, NULL, 0);
    /* Unlike zmalloc_used_memory, this matches the stats.resident by taking
     * into account all allocations done by this process (not only zmalloc). */
    je_mallctl("stats.allocated", &allocated, &sz, NULL, 0);
    float frag_pct = ((float)active / allocated)*100 - 100;
    size_t frag_bytes = active - allocated;
    float rss_pct = ((float)resident / allocated)*100 - 100;
    size_t rss_bytes = resident - allocated;
    if(out_frag_bytes)
        *out_frag_bytes = frag_bytes;
    serverLog(LL_DEBUG,
        "allocated=%zu, active=%zu, resident=%zu, frag=%.0f%% (%.0f%% rss), frag_bytes=%zu (%zu%% rss)",
        allocated, active, resident, frag_pct, rss_pct, frag_bytes, rss_bytes);
    return frag_pct;
}

#define INTERPOLATE(x, x1, x2, y1, y2) ( (y1) + ((x)-(x1)) * ((y2)-(y1)) / ((x2)-(x1)) )
#define LIMIT(y, min, max) ((y)<(min)? min: ((y)>(max)? max: (y)))

/* Perform incremental defragmentation work from the serverCron.
 * This works in a similar way to activeExpireCycle, in the sense that
 * we do incremental work across calls. */
void activeDefragCycle(void) {
    static int current_db = -1;
    static unsigned long cursor = 0;
    static redisDb *db = NULL;
    static long long start_scan, start_stat;
    unsigned int iterations = 0;
    unsigned long long defragged = server.stat_active_defrag_hits;
    long long start, timelimit;

    if (server.aof_child_pid!=-1 || server.rdb_child_pid!=-1)
        return; /* Defragging memory while there's a fork will just do damage. */

    /* Once a second, check if we the fragmentation justfies starting a scan
     * or making it more aggressive. */
    run_with_period(1000) {
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
    if (!server.active_defrag_running)
        return;

    /* See activeExpireCycle for how timelimit is handled. */
    start = ustime();
    timelimit = 1000000*server.active_defrag_running/server.hz/100;
    if (timelimit <= 0) timelimit = 1;

    do {
        if (!cursor) {
            /* Move on to next database, and stop if we reached the last one. */
            if (++current_db >= server.dbnum) {
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
                return;
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
            cursor = dictScan(db->dict, cursor, defragScanCallback, defragDictBucketCallback, db);
            /* Once in 16 scan iterations, or 1000 pointer reallocations
             * (if we have a lot of pointers in one hash bucket), check if we
             * reached the tiem limit. */
            if (cursor && (++iterations > 16 || server.stat_active_defrag_hits - defragged > 1000)) {
                if ((ustime() - start) > timelimit) {
                    return;
                }
                iterations = 0;
                defragged = server.stat_active_defrag_hits;
            }
        } while(cursor);
    } while(1);
}

#else /* HAVE_DEFRAG */

void activeDefragCycle(void) {
    /* Not implemented yet. */
}

#endif
