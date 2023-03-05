/* Copyright (c) 2022, ctrip.com
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

#include "ctrip_swap.h"

#define LOCK_LINKS_LINER_SIZE 4096

#define LINK_SIGNAL_PROCEEDED 0
#define LINK_SIGNAL_UNLOCK 1

#define LINK_TO_LOCK(link_ptr) ((struct lock*)((char*)(link_ptr)-offsetof(struct lock,link)))

/* callback when link target is ready. */
typedef void (*linkProceed)(struct lockLink *link, void *pd);

static size_t lock_memory_used;

static inline void *lock_malloc(size_t size) {
    void *ptr = zmalloc(size);
#ifdef LOCK_PRECISE_MMEORY_USED
#ifdef HAVE_MALLOC_SIZE
    if (ptr) lock_memory_used += zmalloc_size(ptr);
#endif
#endif
    return ptr;
}

static inline void *lock_realloc(void *oldptr, size_t size) {
#ifdef LOCK_PRECISE_MMEORY_USED
#ifdef HAVE_MALLOC_SIZE
    if (oldptr) lock_memory_used -= zmalloc_size(oldptr);
#endif
#endif
    void *ptr = zrealloc(oldptr,size);
#ifdef LOCK_PRECISE_MMEORY_USED
#ifdef HAVE_MALLOC_SIZE
    if (ptr) lock_memory_used += zmalloc_size(ptr);
#endif
#endif
    return ptr;
}

static inline void lock_free(void *ptr) {
#ifdef LOCK_PRECISE_MMEORY_USED
#ifdef HAVE_MALLOC_SIZE
    if (ptr) lock_memory_used -= zmalloc_size(ptr);
#endif
#endif
    zfree(ptr);
}


static void lockLinksInit(lockLinks *links) {
    memset(links->buf,0,sizeof(lock*)*LOCK_LINKS_BUF_SIZE);
    links->links = links->buf;
    links->capacity = LOCK_LINKS_BUF_SIZE;
    links->count = 0;
    links->proceeded = 0;
    links->unlocked = 0;
    links->reserved = 0;
}

static void lockLinksDeinit(lockLinks *links) {
    if (links->links && links->links != links->buf)
        lock_free(links->links);
    lockLinksInit(links);
}

static void lockLinksMakeRoomFor(lockLinks *links, int count) {
    if (count <= links->capacity) return;

    while (links->capacity < count &&
            links->capacity < LOCK_LINKS_LINER_SIZE) {
        links->capacity *= 2;
    }
    while (links->capacity < count &&
            links->capacity >= LOCK_LINKS_LINER_SIZE) {
        links->capacity += LOCK_LINKS_LINER_SIZE;
    }
    serverAssert(links->capacity >= count);

    if (links->links == links->buf) {
        links->links = lock_malloc(sizeof(lock*)*links->capacity);
        memcpy(links->links,links->buf,sizeof(lock*)*LOCK_LINKS_BUF_SIZE);
    } else {
        links->links = lock_realloc(links->links,sizeof(lock*)*links->capacity);
    }
}

static inline void lockLinksPush(lockLinks *links, void *target) {
    lockLinksMakeRoomFor(links,links->count+1);
    links->links[links->count++] = target;
}

static inline void lockLinkTargetInit(lockLinkTarget *target) {
    target->linked = 0;
    target->signaled = 0;
}

static inline void lockLinkTargetLinked(lockLinkTarget *target) {
    serverAssert(target->signaled <= target->linked);
    target->linked++;
}

static inline void lockLinkTargetSignaled(lockLinkTarget *target) {
    target->signaled++;
    serverAssert(target->signaled <= target->linked);
}

static inline int lockLinkTargetReady(lockLinkTarget *target) {
    serverAssert(target->signaled <= target->linked);
    return target->signaled == target->linked;
}

void lockLinkInit(lockLink *link, int64_t txid) {
    link->txid = txid;
    lockLinksInit(&link->links);
    lockLinkTargetInit(&link->target);
}

void lockLinkDeinit(lockLink *link) {
    link->txid = 0;
    lockLinksDeinit(&link->links);
    lockLinkTargetInit(&link->target);
}

void lockLinkLink(lockLink *from, lockLink *to, int *test_would_block) {
    serverAssert(from->txid <= to->txid);
    int wont_block = (from->links.proceeded && from->txid == to->txid) ||
            from->links.unlocked;

    if (test_would_block) {
       if (!wont_block) {
           *test_would_block = 1;
       }
       return;
    }

    lockLinksPush(&from->links,to);
    lockLinkTargetLinked(&to->target);
    if (wont_block) {
        lockLinkTargetSignaled(&to->target);
    }
}

static void lockLinkSignal(lockLink *link, int type, linkProceed cb,
        void *pd) {
    if (type == LINK_SIGNAL_PROCEEDED) {
        serverAssert(!link->links.proceeded && !link->links.unlocked);
        link->links.proceeded = 1;
    } else {
        serverAssert(type == LINK_SIGNAL_UNLOCK);
        serverAssert(link->links.proceeded);
        link->links.unlocked = 1;
    }

    for (int i = 0; i < link->links.count; i++) {
        lockLink *to = link->links.links[i];
        serverAssert(link->txid <= to->txid);
        if ((type == LINK_SIGNAL_PROCEEDED && link->txid == to->txid) ||
                (type == LINK_SIGNAL_UNLOCK && link->txid < to->txid)) {
            lockLinkTargetSignaled(&to->target);
            if (lockLinkTargetReady(&to->target)) {
                cb(to,pd);
            }
        }
    }
}

void lockLinkProceeded(lockLink *link, linkProceed cb, void *pd) {
    lockLinkSignal(link,LINK_SIGNAL_PROCEEDED,cb,pd);
}

void lockLinkUnlock(lockLink *link, linkProceed cb, void *pd) {
    lockLinkSignal(link,LINK_SIGNAL_UNLOCK,cb,pd);
}

dictType keyLevelLockDictType = {
    dictSdsHash,                    /* hash function */
    NULL,                           /* key dup */
    NULL,                           /* val dup */
    dictSdsKeyCompare,              /* key compare */
    dictSdsDestructor,              /* key destructor */
    NULL,                           /* val destructor */
    NULL                            /* allow to expand */
};

locks *locksCreate(int level, redisDb *db, robj *key, locks *parent) {
    locks *locks;

    locks = lock_malloc(sizeof(struct locks));
    locks->lock_list = listCreate();
    locks->level = level;
    locks->parent = parent;

    switch (level) {
    case REQUEST_LEVEL_SVR:
        serverAssert(parent == NULL);
        locks->svr.dbnum = server.dbnum;
        locks->svr.dbs = lock_malloc(locks->svr.dbnum*sizeof(struct locks));
        break;
    case REQUEST_LEVEL_DB:
        serverAssert(parent->level == REQUEST_LEVEL_SVR);
        serverAssert(db);
        locks->db.db = db;
        locks->db.keys = dictCreate(&keyLevelLockDictType, NULL);
        break;
    case REQUEST_LEVEL_KEY:
        serverAssert(parent->level == REQUEST_LEVEL_DB);
        serverAssert(db && key);
        incrRefCount(key);
        locks->key.key = key;
        dictAdd(parent->db.keys,sdsdup(key->ptr),locks);
        break;
    default:
        serverPanic("unexpected lock level");
        break;
    }

    return locks;
}

static void locksRelease(locks *locks) {
    if (!locks) return;

    serverAssert(listLength(locks->lock_list) == 0);
    listRelease(locks->lock_list);
    locks->lock_list = NULL;

    switch (locks->level) {
    case REQUEST_LEVEL_SVR:
        lock_free(locks->svr.dbs);
        break;
    case REQUEST_LEVEL_DB:
        dictRelease(locks->db.keys);
        break;
    case REQUEST_LEVEL_KEY:
        serverAssert(locks->parent->level == REQUEST_LEVEL_DB);
        dictDelete(locks->parent->db.keys,locks->key.key->ptr);
        decrRefCount(locks->key.key);
        break;
    default:
        serverPanic("unexpected lock level");
        break;
    }

    lock_free(locks);
}

const char *lockDump(lock *lock);
sds locksDump(locks *locks) {
    listIter li;
    listNode *ln;
    sds result = sdsempty();
    char *key;
    redisDb *db;

    switch (locks->level) {
    case REQUEST_LEVEL_SVR:
        db = NULL;
        key = "<svr>";
        break;
    case REQUEST_LEVEL_DB:
        db = locks->db.db;
        key = "<db>";
        break;
    case REQUEST_LEVEL_KEY:
        db = locks->parent->db.db;
        key = locks->key.key->ptr;
        break;
    default:
        db = NULL;
        key = "?";
        break;
    }
    result = sdscatprintf(result,"(level=%s,db=%d,key=%s,lock_count=%ld):",
            requestLevelName(locks->level),
            db ? db->id : -1, key, listLength(locks->lock_list));

    result = sdscat(result, "[");
    listRewind(locks->lock_list,&li);
    while ((ln = listNext(&li))) {
        lock *lock = listNodeValue(ln);
        if (ln != listFirst(locks->lock_list))
            result = sdscat(result,",");
        result = sdscat(result,lockDump(lock));
    }
    result = sdscat(result, "]");
    return result;
}

static inline lock *locksLastLock(locks *locks) {
    if (locks == NULL) return NULL;
    listNode *ln = listLast(locks->lock_list);
    return ln ? listNodeValue(ln) : NULL;
}

/* create link with upper or current level lock (if exits). */
static inline void locksLinkLock(locks *locks, lock* lock, int *would_block) {
    struct lock *last;
    if ((last = locksLastLock(locks))) {
        lockLinkLink(&last->link,&lock->link,would_block);
    }
}

static void dbLocksChildrenLinkLock(locks *locks, lock* lock, int *would_block) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(locks->db.keys);
    serverAssert(locks->level == REQUEST_LEVEL_DB);
    while ((de = dictNext(di)) != NULL) {
        struct locks *keylocks = dictGetVal(de);
        locksLinkLock(keylocks,lock,would_block);
        if (would_block && *would_block) break;
    }
    dictReleaseIterator(di);
}

static void svrLocksChildrenLinkLock(locks *locks, lock* lock, int *would_block) {
    serverAssert(locks->level == REQUEST_LEVEL_SVR);
    for (int i = 0; i < locks->svr.dbnum; i++) {
        struct locks *dblocks = locks->svr.dbs[i];
        locksLinkLock(dblocks,lock,would_block);
        if (would_block && *would_block) break;
        dbLocksChildrenLinkLock(dblocks,lock,would_block);
        if (would_block && *would_block) break;
    }
}

/* create link with all children of current level locks. */
void locksChildrenLinksLock(locks *locks, lock* lock, int *would_block) {
    switch (locks->level) {
    case REQUEST_LEVEL_SVR:
        svrLocksChildrenLinkLock(locks,lock,would_block);
        break;
    case REQUEST_LEVEL_DB:
        dbLocksChildrenLinkLock(locks,lock,would_block);
        break;
    case REQUEST_LEVEL_KEY:
        break;
    default:
        serverPanic("unexpected locks level");
        break;
    }
}

/* create children link (with higher target level) of left to lock */
void lockMigrateChildrenLinks(lock *left, lock *lock, int *would_block) {
    int level = left->locks->level;
    for (int i = 0; i < left->link.links.count; i++) {
        struct lock *from = LINK_TO_LOCK(left->link.links.links[i]);
        if (from->locks == NULL || from->locks->level <= level) {
            /* skip lower level or current (locks is NULL) lock */
            continue;
        }
        lockLinkLink(&from->link,&lock->link,would_block);
        if (would_block && *would_block) break;
    }
}

static inline void locksChildrenLinkLock(locks* locks, lock *lock,
        int *would_block) {
    struct lock *last = locksLastLock(locks);
    if (last == NULL) {
        if (locks) {
            locksChildrenLinksLock(locks,lock,would_block);
        } else {
            /* Note locks could be NULL if test would block */
            serverAssert(would_block != NULL);
        }
    } else {
        /* last->links is the 'right' part of last children, but it's
         * equvilant to 'all' of last children, 'left' part of children
         * can be represented by last itself. */
        lockMigrateChildrenLinks(last,lock,would_block);
    }
}


lock *lockNew(int64_t txid, redisDb *db, robj *key, client *c,
        lockProceedCallback proceed, void *pd, freefunc pdfree,
        void *msgs) {
    lock *lock = lock_malloc(sizeof(struct lock));

    lockLinkInit(&lock->link,txid);

    lock->locks = NULL;
    lock->locks_ln = NULL;
    lock->db = db;
    if (key) incrRefCount(key);
    lock->key = key;
    lock->c = c;
    lock->proceed = proceed;
    lock->pd = pd;
    lock->pdfree = pdfree;
    lock->lock_timer = 0;
    lock->conflict = 0;
    lock->start_time = ustime();

    UNUSED(msgs);
#ifdef SWAP_DEBUG
    lock->msgs = msgs;
#endif

    return lock;
}

void lockFree(lock *lock) {
    serverAssert(lockLinkTargetReady(&lock->link.target));
    serverAssert(lock->locks_ln == NULL);
    serverAssert(lock->locks == NULL);

    lockLinkDeinit(&lock->link);
    if (lock->key) {
        decrRefCount(lock->key);
        lock->key = NULL;
    }
    if (lock->pdfree) {
        lock->pdfree(lock->pd);
    }
    lock->pd = NULL;
    lock->pdfree = NULL;

    lock_free(lock);
}

static inline const char *booleanRepr(int boolean) {
    return boolean ? "true" : "false";
}

const char *lockDump(lock *lock) {
    static char repr[256];
    char *ptr = repr, *end = repr + sizeof(repr) - 1;

    ptr += snprintf(ptr,end-ptr,
            "txid=%ld,target=(linked=%d,signaled=%d),links=(proceed=%s,unlocked=%s,[",
            lock->link.txid,lock->link.target.linked,lock->link.target.signaled,
            booleanRepr(lock->link.links.proceeded),booleanRepr(lock->link.links.unlocked));

    for (int i = 0; i < lock->link.links.count && ptr < end; i++) {
        struct lockLink *target_link = lock->link.links.links[i];
        struct lock *target = LINK_TO_LOCK(target_link);
        ptr += snprintf(ptr,end-ptr,"(txid=%ld,db=%d,key=%s),",
                target_link->txid,
                target->db ? target->db->id : -1,
                target->key ? (char*)target->key->ptr : "<nil>");
    }
    if (ptr < end) snprintf(ptr, end-ptr, "]");

    return repr;
}

static void lockStatUpdateLocked(lock *lock) {
    int level = lock->locks->level;
    lockStat *stat = server.swap_lock->stat;
    lockInstantaneouStat *inst_stat = stat->instant+level;
    lockCumulativeStat *cumu_stat = &stat->cumulative;

    cumu_stat->request_count++;
    inst_stat->request_count++;
    if (lock->conflict) {
        cumu_stat->conflict_count++;
        inst_stat->conflict_count++;
    }
}

static void lockStatUpdateUnlocked(lock *lock) {
    lockCumulativeStat *cumu_stat = &server.swap_lock->stat->cumulative;
    cumu_stat->request_count--;
    if (lock->conflict) cumu_stat->conflict_count--;
}

static void lockUpdateWaitTime(lock *lock) {
    long long wait_time = ustime() - lock->start_time;
    int level;
    if (lock->key != NULL) {
        level = REQUEST_LEVEL_KEY;
    } else if (lock->db != NULL) {
        level = REQUEST_LEVEL_DB;
    } else {
        level = REQUEST_LEVEL_SVR;
    }
    lockInstantaneouStat* stat = server.swap_lock->stat->instant+level;
    atomicIncr(stat->wait_time , wait_time);
    atomicIncr(stat->proceed_count, 1);
    if (stat->wait_time_maxs[stat->wait_time_max_index] < wait_time) {
        stat->wait_time_maxs[stat->wait_time_max_index] = wait_time;
    }
    
}

static inline void lockStartLatencyTraceIfNeeded(lock *lock) {
    if (lock->conflict && server.swap_debug_trace_latency) {
        elapsedStart(&lock->lock_timer);
    } else {
        lock->lock_timer = 0;
    }
}

static inline void lockEndLatencyTraceIfNeeded(lock *lock) {
    if (lock->lock_timer) {
        metricDebugInfo(SWAP_DEBUG_LOCK_WAIT, elapsedUs(lock->lock_timer));
    }
}

static inline int lockShouldFlushAfterProceed(lock *lock) {
    return lock->link.links.count > 0;
}

static void lockProceed(lock *lock) {
    int flush = lockShouldFlushAfterProceed(lock);
    serverAssert(lockLinkTargetReady(&lock->link.target));
    lockEndLatencyTraceIfNeeded(lock);
    lockUpdateWaitTime(lock);
    lock->proceed(lock,flush,lock->db,lock->key,lock->c,lock->pd);
}

void lockProceedByLink(lockLink *link, void *pd) {
    lock *lock = LINK_TO_LOCK(link);
    UNUSED(pd);
    lockProceed(lock);
}

void lockProceeded(void *lock_) {
    lock *lock = lock_;
    lockLinkProceeded(&lock->link,lockProceedByLink,NULL);
}

static inline void lockAttachToLocks(lock *lock, locks *locks) {
    listAddNodeTail(locks->lock_list,lock);
    lock->locks = locks;
    lock->locks_ln = listLast(locks->lock_list);
}

static inline void lockDetachFromLocks(lock *lock) {
    locks *locks = lock->locks;
    lock->locks = NULL;
    listDelNode(locks->lock_list,lock->locks_ln);
    lock->locks_ln = NULL;
}

static inline void locksFreeIfEmptyKeyLevel(locks *locks) {
    if (locks->level == REQUEST_LEVEL_KEY &&
            listLength(locks->lock_list) == 0) {
        locksRelease(locks);
    }
}

void lockUnlock(void *lock_) {
    lock *lock = lock_;
    locks *locks = lock->locks;
    lockDetachFromLocks(lock);
    locksFreeIfEmptyKeyLevel(locks);
    lockLinkUnlock(&lock->link,lockProceedByLink,NULL);
    lockStatUpdateUnlocked(lock);
    lockFree(lock);
}

/* return 1 if lock proceeded */
static inline int lockProceedIfReady(lock *lock) {
    lock->conflict = !lockLinkTargetReady(&lock->link.target);
    lockStatUpdateLocked(lock);
    lockStartLatencyTraceIfNeeded(lock);
    if (!lock->conflict) {
        lockProceed(lock);
        return 1;
    } else {
        return 0;
    }
}

static int _lockLock(int *would_block,
        int64_t txid, redisDb *db, robj *key, lockProceedCallback cb,
        client *c, void *pd, freefunc pdfree, void *msgs) {
    lock *lock = lockNew(txid,db,key,c,cb,pd,pdfree,msgs);
    locks *svrlocks = server.swap_lock->svrlocks, *dblocks, *keylocks, *locks;

    locksLinkLock(svrlocks,lock,would_block);
    if (db == NULL) {
        locks = svrlocks;
        goto end;
    }

    dblocks = svrlocks->svr.dbs[db->id];
    locksLinkLock(dblocks,lock,would_block);
    if (key == NULL) {
        locks = dblocks;
        goto end;
    }

    keylocks = dictFetchValue(dblocks->db.keys,key->ptr);
    if (keylocks == NULL) {
        if (would_block == NULL) {
            keylocks = locksCreate(REQUEST_LEVEL_KEY,db,key,dblocks);
        } else {
            /* keylocks will remain NULL if testing would block. */
        }
    } else {
        serverAssert(locksLastLock(keylocks)!= NULL);
    }
    locksLinkLock(keylocks,lock,would_block);
    locks = keylocks;

end:
    locksChildrenLinkLock(locks,lock,would_block);

    if (would_block == NULL) {
        lockAttachToLocks(lock, locks);
#ifdef SWAP_DEBUG
        sds dump = locksDump(locks);
        int conflict = !lockLinkTargetReady(&lock->link.target);
        DEBUG_MSGS_APPEND(msgs,"lock","locks = %s, conflict=%d",dump,conflict);
        sdsfree(dump);
#endif
        return lockProceedIfReady(lock);
    } else {
        lockFree(lock);
        return 0;
    }
}

/* return 1 if lock proceeded */
int lockLock(int64_t txid, redisDb *db, robj *key, lockProceedCallback cb,
        client *c, void *pd, freefunc pdfree, void *msgs) {
    return _lockLock(NULL,txid,db,key,cb,c,pd,pdfree,msgs);
}

int lockWouldBlock(int64_t txid, redisDb *db, robj *key) {
    int would_block = 0;
    _lockLock(&would_block,txid,db,key,NULL,NULL,NULL,NULL,NULL);
    return would_block;
}

static lockInstantaneouStat *lockStatCreateInstantaneou() {
    int i, metric_offset, j;
    lockInstantaneouStat *inst_stats = lock_malloc(REQUEST_LEVEL_TYPES*sizeof(lockInstantaneouStat));
    for (i = 0; i < REQUEST_LEVEL_TYPES; i++) {
        metric_offset = SWAP_LOCK_STATS_METRIC_OFFSET + i * SWAP_LOCK_METRIC_SIZE;
        inst_stats[i].name = requestLevelName(i);
        inst_stats[i].request_count = 0;
        inst_stats[i].conflict_count = 0;
        inst_stats[i].proceed_count = 0;
        inst_stats[i].wait_time = 0;
        inst_stats[i].wait_time_max_index = 0;
        for (j = 0;j < STATS_METRIC_SAMPLES;j++) {
            inst_stats[i].wait_time_maxs[j] = 0;
        }
        inst_stats[i].stats_metric_idx_request = metric_offset+SWAP_LOCK_METRIC_REQUEST;
        inst_stats[i].stats_metric_idx_conflict = metric_offset+SWAP_LOCK_METRIC_CONFLICT;
        inst_stats[i].stats_metric_idx_wait_time = metric_offset+SWAP_LOCK_METRIC_WAIT_TIME;
        inst_stats[i].stats_metric_idx_proceed_count= metric_offset+SWAP_LOCK_METRIC_PROCEED_COUNT;
    }
    return inst_stats;
}

static void lockStatFreeInstantaneou(lockInstantaneouStat *stat) {
    lock_free(stat);
}

static void lockStatInitCumulative(lockCumulativeStat *cumu_stat) {
    cumu_stat->request_count = 0;
    cumu_stat->conflict_count = 0;
}

void lockStatInit(lockStat *stat) {
    lockStatInitCumulative(&stat->cumulative);
    stat->instant = lockStatCreateInstantaneou();
}

void lockStatDeinit(lockStat *stat) {
    lockStatFreeInstantaneou(stat->instant);
}

void trackSwapLockInstantaneousMetrics() {
    lockInstantaneouStat *inst_stats = server.swap_lock->stat->instant;
    for (int i = 0; i < REQUEST_LEVEL_TYPES; i++) {
        long long request, conflict, wait_time, proceed_count;
        lockInstantaneouStat *inst_stat = inst_stats + i;
        atomicGet(inst_stat->request_count,request);
        trackInstantaneousMetric(inst_stat->stats_metric_idx_request,request);
        atomicGet(inst_stat->conflict_count,conflict);
        trackInstantaneousMetric(inst_stat->stats_metric_idx_conflict,conflict);
        atomicGet(inst_stat->wait_time,wait_time);
        trackInstantaneousMetric(inst_stat->stats_metric_idx_wait_time,wait_time);
        atomicGet(inst_stat->proceed_count,proceed_count);
        trackInstantaneousMetric(inst_stat->stats_metric_idx_proceed_count,proceed_count);
        run_with_period(4000) {
            //4000ms * 16 > 60s
            inst_stat->wait_time_max_index++;
            inst_stat->wait_time_max_index %= STATS_METRIC_SAMPLES;
            inst_stat->wait_time_maxs[inst_stat->wait_time_max_index] = 0;
        }
        
    }
}

void resetSwapLockInstantaneousMetrics() {
    for (int i = 0; i < REQUEST_LEVEL_TYPES; i++) {
        lockInstantaneouStat *inst_stat = server.swap_lock->stat->instant+i;
        inst_stat->request_count = 0;
        inst_stat->conflict_count = 0;
    }
}

sds genSwapLockInfoString(sds info) {
    int j;
    size_t memory_used = lock_memory_used;

    lockCumulativeStat *cumu_stat = &server.swap_lock->stat->cumulative;

#ifdef LOCK_PRECISE_MMEORY_USED
    memory_used = lock_memory_used;
#else
    memory_used = cumu_stat->request_count*(
            sizeof(locks)+sizeof(lock)+sizeof(list));
#endif

    info = sdscatprintf(info,
            "swap_lock_used_memory:%lu\r\n"
            "swap_lock_request:%ld\r\n"
            "swap_lock_conflict:%ld\r\n",
            memory_used,
            cumu_stat->request_count,
            cumu_stat->conflict_count);

    for (j = 0; j < REQUEST_LEVEL_TYPES; j++) {
        long long request, conflict, rps, cps;
        long long wait_time_ps, proceed_count_ps, max_wait_time = 0;
        lockInstantaneouStat *lock_stat = server.swap_lock->stat->instant+j;
        atomicGet(lock_stat->request_count,request);
        atomicGet(lock_stat->conflict_count,conflict);
        rps = getInstantaneousMetric(lock_stat->stats_metric_idx_request);
        cps = getInstantaneousMetric(lock_stat->stats_metric_idx_conflict);
        wait_time_ps = getInstantaneousMetric(lock_stat->stats_metric_idx_wait_time);
        proceed_count_ps = getInstantaneousMetric(lock_stat->stats_metric_idx_proceed_count);
        for(int k = 0; k < STATS_METRIC_SAMPLES; k++) {
            if (max_wait_time < lock_stat->wait_time_maxs[k]) {
                max_wait_time = lock_stat->wait_time_maxs[k];
            }
        }
        info = sdscatprintf(info,
                "swap_lock_%s:request=%lld,conflict=%lld,request_ps=%lld,conflict_ps=%lld,avg_wait_time=%lld,max_wait_time=%lld\r\n",
                lock_stat->name,request,conflict,rps,cps,proceed_count_ps != 0? (wait_time_ps/proceed_count_ps):0, max_wait_time);
    }
    return info;
}

void swapLockCreate() {
    int i;

    locks *svrlocks = locksCreate(REQUEST_LEVEL_SVR,NULL,NULL,NULL);
    for (i = 0; i < svrlocks->svr.dbnum; i++) {
        redisDb *db = server.db + i;
        svrlocks->svr.dbs[i] = locksCreate(REQUEST_LEVEL_DB,db,NULL,svrlocks);
    }

    lockStat *stat = lock_malloc(sizeof(lockStat));
    lockStatInit(stat);

    server.swap_lock = lock_malloc(sizeof(struct swapLock));
    server.swap_lock->svrlocks = svrlocks;
    server.swap_lock->stat = stat;
}

void swapLockDestroy() {
    int i;

    locks *svrlocks = server.swap_lock->svrlocks;
    for (i = 0; i < svrlocks->svr.dbnum; i++) {
        locks *dblocks = svrlocks->svr.dbs[i];
        serverAssert(dictSize(dblocks->db.keys) == 0);
        locksRelease(dblocks);
    }
    locksRelease(svrlocks);
    lock_free(svrlocks);

    lock_free(server.swap_lock->stat);

    lock_free(server.swap_lock);
}


#ifdef REDIS_TEST

static int blocked;

void proceedLater(void *lock, int flush, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(flush), UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = lock;
    blocked--;
    lockProceeded(lock);
}

#define wait_init_suite() do {  \
    if (server.hz != 10) {  \
        server.hz = 10; \
        server.dbnum = 4;   \
        server.db = zmalloc(sizeof(redisDb)*server.dbnum);  \
        for (int i = 0; i < server.dbnum; i++) server.db[i].id = i; \
        swapLockCreate(); \
    }   \
} while (0)

int swapLockTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    redisDb *db, *db2;
    robj *key1, *key2, *key3;
    void *handle1, *handle2, *handle3, *handledb, *handledb2, *handlesvr;
    int64_t txid = 0;

    wait_init_suite();

    TEST("lock: init") {
        db = server.db, db2 = server.db+1;
        key1 = createStringObject("key-1",5);
        key2 = createStringObject("key-2",5);
        key3 = createStringObject("key-3",5);
    }

   TEST("lock: parallel key") {
       handle1 = NULL, handle2 = NULL, handle3 = NULL, handlesvr = NULL;
       lockLock(txid++,db,key1,proceedLater,NULL,&handle1,NULL,NULL), blocked++;
       lockLock(txid++,db,key2,proceedLater,NULL,&handle2,NULL,NULL), blocked++;
       lockLock(txid++,db,key3,proceedLater,NULL,&handle3,NULL,NULL), blocked++;
       test_assert(!blocked);
       test_assert(lockWouldBlock(txid++,db,key1));
       test_assert(lockWouldBlock(txid++,db,key2));
       test_assert(lockWouldBlock(txid++,db,key3));
       test_assert(lockWouldBlock(txid++,db,NULL));
       lockUnlock(handle1);
       test_assert(!lockWouldBlock(txid++,db,key1));
       lockUnlock(handle2);
       test_assert(!lockWouldBlock(txid++,db,key2));
       lockUnlock(handle3);
       test_assert(!lockWouldBlock(txid++,db,key3));
       test_assert(!lockWouldBlock(txid++,NULL,NULL));
   }

   TEST("lock: pipelined key") {
       int i, COUNT = 3, proceeded;
       void *handles[COUNT];
       for (i = 0; i < COUNT; i++) {
           blocked++;
           proceeded = lockLock(txid++,db,key1,proceedLater,NULL,&handles[i],NULL,NULL);
           test_assert((i == 0 && proceeded == 1) || (proceeded == 0));
       }
       test_assert(lockWouldBlock(txid++,db,key1));
       /* first one proceeded, others blocked */
       test_assert(blocked == 2);
       for (i = 0; i < COUNT-1; i++) {
           lockUnlock(handles[i]);
           test_assert(lockWouldBlock(txid++,db,key1));
       }
       test_assert(blocked == 0);
       lockUnlock(handles[COUNT-1]);
       test_assert(!lockWouldBlock(txid++,db,key1));
   }

   TEST("lock: parallel db") {
       int proceeded;
       proceeded = lockLock(txid++,db,NULL,proceedLater,NULL,&handledb,NULL,NULL), blocked++;
       test_assert(proceeded == 1);
       proceeded = lockLock(txid++,db2,NULL,proceedLater,NULL,&handledb2,NULL,NULL), blocked++;
       test_assert(proceeded == 1);
       test_assert(!blocked);
       test_assert(lockWouldBlock(txid++,db,NULL));
       test_assert(lockWouldBlock(txid++,db2,NULL));
       lockUnlock(handledb);
       lockUnlock(handledb2);
       test_assert(!lockWouldBlock(txid++,db,NULL));
       test_assert(!lockWouldBlock(txid++,db2,NULL));
   }

    TEST("lock: mixed parallel-key/db/parallel-key") {
        handle1 = NULL, handle2 = NULL, handle3 = NULL, handledb = NULL;
        lockLock(txid++,db,key1,proceedLater,NULL,&handle1,NULL,NULL),blocked++;
        lockLock(txid++,db,key2,proceedLater,NULL,&handle2,NULL,NULL),blocked++;
        lockLock(txid++,db,NULL,proceedLater,NULL,&handledb,NULL,NULL),blocked++;
        lockLock(txid++,db,key3,proceedLater,NULL,&handle3,NULL,NULL),blocked++;
        /* key1/key2 proceeded, db/key3 blocked */
        test_assert(lockWouldBlock(txid++,db,NULL));
        test_assert(blocked == 2);
        /* key1/key2 notify */
        lockUnlock(handle1);
        test_assert(lockWouldBlock(txid++,db,NULL));
        lockUnlock(handle2);
        test_assert(lockWouldBlock(txid++,db,NULL));
        /* db proceeded, key3 still blocked. */
        test_assert(blocked == 1);
        test_assert(handle3 == NULL);
        /* db notified, key3 proceeds but still blocked */
        lockUnlock(handledb);
        test_assert(!blocked);
        test_assert(lockWouldBlock(txid++,db,NULL));
        /* db3 proceed, noting would block */
        lockUnlock(handle3);
        test_assert(!lockWouldBlock(txid++,db,NULL));
    }

    TEST("lock: mixed parallel-key/server/parallel-key") {
        handle1 = NULL, handle2 = NULL, handle3 = NULL, handlesvr = NULL;
        lockLock(txid++,db,key1,proceedLater,NULL,&handle1,NULL,NULL),blocked++;
        lockLock(txid++,db,key2,proceedLater,NULL,&handle2,NULL,NULL),blocked++;
        lockLock(txid++,NULL,NULL,proceedLater,NULL,&handlesvr,NULL,NULL),blocked++;
        lockLock(txid++,db,key3,proceedLater,NULL,&handle3,NULL,NULL),blocked++;
        /* key1/key2 proceeded, svr/key3 blocked */
        test_assert(lockWouldBlock(txid++,NULL,NULL));
        test_assert(lockWouldBlock(txid++,db,NULL));
        test_assert(blocked == 2);
        /* key1/key2 notify */
        lockUnlock(handle1);
        test_assert(lockWouldBlock(txid++,NULL,NULL));
        lockUnlock(handle2);
        test_assert(lockWouldBlock(txid++,NULL,NULL));
        /* svr proceeded, key3 still blocked. */
        test_assert(blocked == 1);
        test_assert(handle3 == NULL);
        /* svr notified, db3 proceeds but still would block */
        lockUnlock(handlesvr);
        test_assert(!blocked);
        test_assert(lockWouldBlock(txid++,NULL,NULL));
        /* db3 proceed, noting would block */
        lockUnlock(handle3);
        test_assert(!lockWouldBlock(txid++,NULL,NULL));
    }

    TEST("lock: deinit") {
        decrRefCount(key1), decrRefCount(key2), decrRefCount(key3);
    }

    return error;
}


static int proceeded = 0;
void proceededCounter(void *lock, int flush, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(flush), UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = lock;
    proceeded++;
    lockProceeded(lock);
}

#define reentrant_case_reset() do { \
    proceeded = 0; \
    handle1 = NULL, handle2 = NULL, handle3 = NULL, handle4 = NULL; \
    handle5 = NULL, handle6 = NULL, handle7 = NULL, handle8 = NULL;\
} while (0)

locks *searchLocks(redisDb *db, robj *key) {
    locks *svrlocks = server.swap_lock->svrlocks, *dblocks, *keylocks;
    if (db == NULL) return svrlocks;
    dblocks = svrlocks->svr.dbs[db->id];
    if (key == NULL) return dblocks;
    keylocks = dictFetchValue(dblocks->db.keys,key->ptr);
    return keylocks;
}

int swapLockReentrantTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    redisDb *db, *db2;
    robj *key1, *key2;
    void *handle1 = NULL, *handle2 = NULL, *handle3 = NULL, *handle4 = NULL, *handle5 = NULL, *handle6 = NULL,
         *handle7 = NULL, *handle8 = NULL;

    wait_init_suite();

    TEST("lock-reentrant: init") {
        db = server.db, db2 = server.db+1;
        key1 = createStringObject("key-1",5);
        key2 = createStringObject("key-2",5);
    }

   TEST("lock-reentrant: key (without prceeding listener)") {
       test_assert(!lockWouldBlock(10,db,key1));
       lockLock(10,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(!lockWouldBlock(10,db,key1));
       lockLock(10,db,key1,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle1 != NULL && handle2 != NULL);
       test_assert(lockWouldBlock(11,db,key1));
       lockLock(11,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       lockUnlock(handle1);
       lockUnlock(handle2);
       test_assert(proceeded == 3);
       test_assert(searchLocks(db,key1) != NULL);
       lockUnlock(handle3);
       test_assert(searchLocks(db,key1) == NULL);
       test_assert(proceeded == 3);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: key (with prceeding listener)") {
       test_assert(!lockWouldBlock(20,db,key1));
       lockLock(20,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(handle1 != NULL);
       test_assert(proceeded == 1);
       test_assert(lockWouldBlock(21,db,key1));
       lockLock(21,db,key1,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(lockWouldBlock(21,db,key1));
       lockLock(21,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(lockWouldBlock(22,db,key1));
       lockLock(22,db,key1,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 1);
       lockUnlock(handle1);
       test_assert(proceeded == 3);
       test_assert(handle2 != NULL);
       test_assert(handle3 != NULL);
       lockUnlock(handle2);
       test_assert(proceeded == 3);
       lockUnlock(handle3);
       test_assert(proceeded == 4);
       test_assert(handle4 != NULL);
       test_assert(searchLocks(db,key1) != NULL);
       lockUnlock(handle4);
       test_assert(searchLocks(db,key1) == NULL);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: db listener") {
       lockLock(30,db,NULL,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       lockLock(30,db,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle2 != NULL);
       lockLock(31,db,NULL,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       lockLock(31,db2,NULL,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 3);
       test_assert(handle4 != NULL);
       lockUnlock(handle1);
       lockUnlock(handle2);
       test_assert(proceeded == 4);
       test_assert(handle3 != NULL);
       lockUnlock(handle3);
       lockUnlock(handle4);
       test_assert(proceeded == 4);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: svr listener") {
       lockLock(40,NULL,NULL,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       lockLock(40,NULL,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle2 != NULL);
       lockLock(41,NULL,NULL,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       lockLock(41,NULL,NULL,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 2);
       lockUnlock(handle1);
       test_assert(proceeded == 2);
       lockUnlock(handle2);
       test_assert(proceeded == 4);
       test_assert(handle3 != NULL);
       test_assert(handle4 != NULL);
       lockUnlock(handle3);
       test_assert(proceeded == 4);
       lockUnlock(handle4);
       test_assert(proceeded == 4);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: db and svr listener") {
       lockLock(50,db,NULL,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       lockLock(51,db,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle2 == NULL);
       lockLock(51,db2,NULL,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle3 != NULL);
       lockLock(51,NULL,NULL,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(handle4 == NULL);
       test_assert(proceeded == 2);
       lockUnlock(handle1);
       test_assert(handle2 != NULL);
       test_assert(handle4 != NULL);
       test_assert(proceeded == 4);
       lockUnlock(handle2);
       lockUnlock(handle3);
       lockUnlock(handle4);
       test_assert(proceeded == 4);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: multi-level (with key & db listener)") {
       lockLock(60,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1);
       test_assert(handle1 != NULL);
       lockLock(61,db,key1,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 1);
       lockLock(61,db,key2,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       test_assert(handle3 != NULL);
       lockLock(61,db,key1,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 2);
       lockLock(61,db,NULL,proceededCounter,NULL,&handle5,NULL,NULL);
       test_assert(proceeded == 2);
       lockLock(61,db,key1,proceededCounter,NULL,&handle6,NULL,NULL);
       test_assert(proceeded == 2);
       lockLock(62,db,key2,proceededCounter,NULL,&handle7,NULL,NULL);
       test_assert(proceeded == 2);
       lockUnlock(handle1);
       test_assert(proceeded == 6);
       test_assert(handle2 != NULL);
       test_assert(handle3 != NULL);
       test_assert(handle4 != NULL);
       test_assert(handle5 != NULL);
       test_assert(handle6 != NULL);
       lockUnlock(handle2);
       lockUnlock(handle3);
       lockUnlock(handle4);
       lockUnlock(handle5);
       test_assert(proceeded == 7);
       lockUnlock(handle6);
       test_assert(proceeded == 7);
       test_assert(handle7 != NULL);
       lockUnlock(handle7);
       test_assert(proceeded == 7);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: multi-level (with key & svr listener)") {
       lockLock(70,db,key1,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(proceeded == 1 && handle1 != NULL);
       lockLock(70,db,key2,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(proceeded == 2 && handle2 != NULL);
       test_assert(handle1 != handle2);
       lockLock(71,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(proceeded == 2);
       lockLock(71,db,key2,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 2);
       lockLock(71,NULL,NULL,proceededCounter,NULL,&handle5,NULL,NULL);
       test_assert(proceeded == 2 && handle5 == NULL);
       lockLock(72,NULL,NULL,proceededCounter,NULL,&handle6,NULL,NULL);
       test_assert(proceeded == 2);
       lockUnlock(handle1);
       test_assert(proceeded == 3);
       test_assert(handle3 != NULL);
       lockUnlock(handle2);
       test_assert(proceeded == 5);
       test_assert(handle4 != NULL);
       test_assert(handle5 != NULL);
       lockUnlock(handle3);
       lockUnlock(handle4);
       test_assert(proceeded == 5);
       lockUnlock(handle5);
       test_assert(proceeded == 6);
       test_assert(handle6 != NULL);
       lockUnlock(handle6);
       test_assert(proceeded == 6);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: multi-level (with key & db & svr listener)") {
       lockLock(80,db,key2,proceededCounter,NULL,&handle1,NULL,NULL);
       test_assert(handle1 != NULL);
       lockLock(80,db2,NULL,proceededCounter,NULL,&handle2,NULL,NULL);
       test_assert(handle2 != NULL);
       test_assert(proceeded == 2);
       lockLock(81,db,key1,proceededCounter,NULL,&handle3,NULL,NULL);
       test_assert(handle3 != NULL);
       test_assert(handle2 != NULL );
       test_assert(proceeded == 3);
       lockLock(81,db,key2,proceededCounter,NULL,&handle4,NULL,NULL);
       test_assert(proceeded == 3);
       lockLock(81,db,NULL,proceededCounter,NULL,&handle5,NULL,NULL);
       test_assert(proceeded == 3);
       lockLock(81,db2,NULL,proceededCounter,NULL,&handle6,NULL,NULL);
       test_assert(proceeded == 3);
       lockLock(81,NULL,NULL,proceededCounter,NULL,&handle7,NULL,NULL);
       test_assert(proceeded == 3);
       lockLock(82,NULL,NULL,proceededCounter,NULL,&handle8,NULL,NULL);
       test_assert(proceeded == 3);
       lockUnlock(handle1);
       test_assert(proceeded == 5);
       test_assert(handle4 != NULL);
       test_assert(handle5 != NULL);
       lockUnlock(handle2);
       test_assert(proceeded == 7);
       test_assert(handle6 != NULL);
       test_assert(handle7 != NULL);
       lockUnlock(handle3);
       lockUnlock(handle4);
       lockUnlock(handle5);
       lockUnlock(handle6);
       test_assert(proceeded == 7);
       lockUnlock(handle7);
       test_assert(proceeded == 8);
       test_assert(handle8 != NULL);
       lockUnlock(handle8);
       test_assert(proceeded == 8);
       reentrant_case_reset();
   }

   TEST("lock-reentrant: expand links buf") {
       int i, COUNT = LOCK_LINKS_BUF_SIZE*4;
       void *handles[COUNT];

       for (i = 0; i < COUNT; i++) {
           lockLock(90,db,key1,proceededCounter,NULL,&handles[i],NULL,NULL);
       }
       test_assert(proceeded == COUNT);
       for (i = 0; i < COUNT; i++) {
           lockUnlock(handles[i]);
       }
       test_assert(proceeded == COUNT);
       reentrant_case_reset();
   }

    TEST("lock-reentrant: deinit") {
        decrRefCount(key1), decrRefCount(key2);
    }

    return error;
}

void proceedWithoutAck(void *listeners, int flush, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(flush), UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = listeners;
    proceeded++;
}

void proceedRightaway(void *listeners, int flush, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(flush), UNUSED(db), UNUSED(key), UNUSED(c);
    void **pd = pd_;
    *pd = listeners;
    proceeded++;
    lockProceeded(listeners);
    lockUnlock(listeners);
}

typedef struct lockAndFlush {
    void *locker;
    int flush;
} lockAndFlush;

void proceedLaterGetLockAndFlush(void *locker, int flush, redisDb *db, robj *key, client *c, void *pd_) {
    UNUSED(flush), UNUSED(db), UNUSED(key), UNUSED(c);
    lockAndFlush *lock_and_flush = pd_;
    lock_and_flush->flush = flush;
    lock_and_flush->locker = locker;
}

#define ack_case_reset() do { \
    proceeded = 0; \
    handle1 = NULL, handle2 = NULL, handle3 = NULL, handle4 = NULL; \
    handle5 = NULL, handle6 = NULL, handle7 = NULL, handle8 = NULL; \
    handle9 = NULL, handle10 = NULL; \
} while (0)

int swapLockProceedTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    redisDb *db, *db2;
    robj *key1, *key2;
    void *handle1, *handle2, *handle3, *handle4, *handle5, *handle6,
         *handle7, *handle8, *handle9, *handle10;

    wait_init_suite();

    TEST("lock-proceeded: init") {
        db = server.db, db2 = server.db+1;
        key1 = createStringObject("key-1",5);
        key2 = createStringObject("key-2",5);
        ack_case_reset();
        UNUSED(db2);
    }

    TEST("lock-proceeded: multi-level (db & svr)") {
        test_assert(searchLocks(db,NULL) != NULL);
        test_assert(!lockWouldBlock(10,db,NULL));
        lockLock(10,db,NULL,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        test_assert(lockWouldBlock(10,db,NULL));
        lockLock(10,db,NULL,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle2 == NULL && proceeded == 1);
        lockLock(10,db,key1,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        test_assert(handle3 == NULL && proceeded == 1);
        lockLock(10,db2,NULL,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 != NULL && proceeded == 2);
        lockLock(10,db2,NULL,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 2);
        lockLock(10,NULL,NULL,proceedWithoutAck,NULL,&handle6,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 2);
        lockLock(10,db2,key2,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle7 == NULL && proceeded == 2);
        lockLock(11,db,key1,proceedWithoutAck,NULL,&handle8,NULL,NULL);
        test_assert(handle8 == NULL && proceeded == 2);

        lockProceeded(handle1);
        test_assert(handle2 !=NULL && proceeded == 3);
        lockProceeded(handle4);
        test_assert(handle5 != NULL && proceeded == 4);
        lockProceeded(handle5);
        test_assert(handle6 == NULL && proceeded == 4);
        lockProceeded(handle2);
        test_assert(handle3 != NULL && proceeded == 5);
        lockProceeded(handle3);
        test_assert(handle6 != NULL && proceeded == 6);
        lockProceeded(handle6);
        test_assert(handle7 != NULL && proceeded == 7);
        lockProceeded(handle7);
        test_assert(handle8 == NULL && proceeded == 7);

        lockUnlock(handle1), lockUnlock(handle2), lockUnlock(handle3),
            lockUnlock(handle4), lockUnlock(handle5), lockUnlock(handle6),
            lockUnlock(handle7);

        test_assert(handle8 != NULL && proceeded == 8);
        lockProceeded(handle8);
        lockUnlock(handle8);

        ack_case_reset();
    }

    TEST("lock-proceeded: multi-level (key & svr)") {
        lockLock(20,db,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        lockLock(20,db,key1,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle2 == NULL && proceeded == 1);
        lockLock(20,db,key2,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        test_assert(handle3 != NULL && proceeded == 2);
        lockLock(20,db,key2,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 == NULL && proceeded == 2);
        lockLock(20,NULL,NULL,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 2);
        lockProceeded(handle3);
        test_assert(handle4 != NULL && proceeded == 3);
        lockProceeded(handle1);
        test_assert(handle2 != NULL && handle5 == NULL && proceeded == 4);
        lockProceeded(handle2);
        test_assert(handle5 == NULL && proceeded == 4);
        lockProceeded(handle4);
        test_assert(handle5 != NULL && proceeded == 5);
        lockProceeded(handle5);
        lockUnlock(handle1), lockUnlock(handle2), lockUnlock(handle3),
            lockUnlock(handle4), lockUnlock(handle5);
        test_assert(searchLocks(db,key1) == NULL);
        ack_case_reset();
    }

    TEST("lock-proceeded: multi-level (key & db & svr)") {
        lockLock(30,db,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        lockLock(30,db,key2,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle1 != handle2 && handle1 && handle2 && proceeded == 2);
        lockLock(30,db2,key1,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        lockLock(30,db2,key2,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle3 != handle4 && handle3 && handle4 && proceeded == 4);
        lockLock(30,db,NULL,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 4);
        lockLock(30,NULL,NULL,proceedWithoutAck,NULL,&handle6,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 4);
        lockLock(30,db,key1,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 4);

        lockProceeded(handle4), lockProceeded(handle3), lockProceeded(handle2), lockProceeded(handle1);
        test_assert(handle5 != NULL && proceeded == 5);
        lockProceeded(handle5);
        test_assert(handle6 != NULL && handle6 != handle5 && proceeded == 6);
        lockProceeded(handle6);

        lockUnlock(handle1), lockUnlock(handle2), lockUnlock(handle3),
            lockUnlock(handle4), lockUnlock(handle5), lockUnlock(handle6);
        lockProceeded(handle7);
        lockUnlock(handle7);

        ack_case_reset();
    }

    TEST("lock-proceeded: proceed ack disorder") {
        test_assert(!lockWouldBlock(40,db,key1));
        lockLock(40,db,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        test_assert(lockWouldBlock(40,db,key1));
        lockLock(40,db,key1,proceedWithoutAck,NULL,&handle2,NULL,NULL);
        test_assert(handle2 == NULL && proceeded == 1);
        lockProceeded(handle1);
        test_assert(handle2 != NULL && proceeded == 2);
        test_assert(lockWouldBlock(41,db,key1));
        lockProceeded(handle2);
        test_assert(proceeded == 2);
        test_assert(lockWouldBlock(41,db,key1));
        lockUnlock(handle1);
        test_assert(lockWouldBlock(41,db,key1));
        lockLock(41,db,key1,proceedWithoutAck,NULL,&handle3,NULL,NULL);
        test_assert(handle3 == NULL && proceeded == 2);
        /* proceed iff previous tx finished. */
        lockUnlock(handle2);
        test_assert(handle3 != NULL && proceeded == 3);
        lockLock(41,db,key1,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 == NULL && proceeded == 3);
        lockLock(41,db,key2,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 != NULL && proceeded == 4);
        lockLock(41,db,NULL,proceedWithoutAck,NULL,&handle6,NULL,NULL);
        test_assert(handle6 == NULL && proceeded == 4);
        lockLock(41,db,key1,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle7 == NULL && proceeded == 4);
        lockLock(42,db,key2,proceedWithoutAck,NULL,&handle8,NULL,NULL);
        test_assert(handle8 == NULL && proceeded == 4);

        lockProceeded(handle3);
        test_assert(handle4 != NULL && handle6 == NULL && proceeded == 5);
        lockProceeded(handle5);
        test_assert(handle6 == NULL && proceeded == 5);
        lockProceeded(handle4);
        test_assert(handle6 != NULL && proceeded == 6);
        lockProceeded(handle6);
        test_assert(handle7 != NULL && handle8 == NULL && proceeded == 7);
        lockProceeded(handle7);
        lockUnlock(handle3), lockUnlock(handle4), lockUnlock(handle5),
            lockUnlock(handle6), lockUnlock(handle7);
        test_assert(handle8 != NULL && proceeded == 8);
        lockProceeded(handle8);
        lockUnlock(handle8);
        test_assert(proceeded == 8);
        ack_case_reset();
    }

    TEST("lock-proceeded: proceed rightaway") {
        test_assert(!lockWouldBlock(50,db,key1));
        lockLock(50,db,key1,proceedRightaway,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        test_assert(!lockWouldBlock(50,db,key1));
        lockLock(50,db,key1,proceedRightaway,NULL,&handle2,NULL,NULL);
        test_assert(handle2 != NULL && proceeded == 2);
        test_assert(!lockWouldBlock(51,db,key1));
        lockLock(51,db,key1,proceedRightaway,NULL,&handle3,NULL,NULL);
        test_assert(handle3 != NULL && proceeded == 3);
        test_assert(!lockWouldBlock(51,db,NULL));
        lockLock(51,db,NULL,proceedRightaway,NULL,&handle4,NULL,NULL);
        test_assert(handle4 != NULL && proceeded == 4);
        test_assert(!lockWouldBlock(51,NULL,NULL));
        lockLock(51,NULL,NULL,proceedRightaway,NULL,&handle5,NULL,NULL);
        test_assert(handle5 != NULL && proceeded == 5);
        ack_case_reset();
    }

    TEST("lock-proceeded: proceed mixed later & rightaway") {
        lockLock(60,db2,key1,proceedWithoutAck,NULL,&handle1,NULL,NULL);
        test_assert(handle1 != NULL && proceeded == 1);
        lockLock(60,db2,key2,proceedRightaway,NULL,&handle2,NULL,NULL);
        test_assert(handle2 != NULL && proceeded == 2);

        lockLock(61,db,key1,proceedRightaway,NULL,&handle3,NULL,NULL);
        test_assert(handle3 != NULL && proceeded == 3);
        lockLock(61,db,key1,proceedWithoutAck,NULL,&handle4,NULL,NULL);
        test_assert(handle4 != NULL && proceeded == 4);

        lockLock(61,db,key1,proceedWithoutAck,NULL,&handle5,NULL,NULL);
        test_assert(handle5 == NULL && proceeded == 4);
        lockLock(61,db,key2,proceedRightaway,NULL,&handle6,NULL,NULL);
        test_assert(handle6 != NULL && proceeded == 5);

        lockLock(61,db2,key1,proceedWithoutAck,NULL,&handle7,NULL,NULL);
        test_assert(handle7 == NULL && proceeded == 5);

        lockLock(61,db,NULL,proceedWithoutAck,NULL,&handle8,NULL,NULL);
        test_assert(handle8 == NULL && proceeded == 5);

        lockLock(61,NULL,NULL,proceedWithoutAck,NULL,&handle9,NULL,NULL);
        test_assert(handle9 == NULL && proceeded == 5);

        lockLock(61,db,key1,proceedWithoutAck,NULL,&handle10,NULL,NULL);
        test_assert(handle10 == NULL && proceeded == 5);

        lockProceeded(handle4);
        test_assert(handle5 != NULL && proceeded == 6);
        lockProceeded(handle5);
        test_assert(handle8 != NULL && handle9 == NULL && proceeded == 7);

        lockProceeded(handle1);
        test_assert(handle7 == NULL && proceeded == 7);
        lockUnlock(handle1);
        test_assert(handle7 != NULL && proceeded == 8);

        lockProceeded(handle7), lockProceeded(handle8);

        test_assert(handle9 != NULL && proceeded == 9);
        lockProceeded(handle9);
        test_assert(handle10 != NULL && proceeded == 10);
        lockProceeded(handle10);

        lockUnlock(handle4), lockUnlock(handle5), lockUnlock(handle7),
            lockUnlock(handle8), lockUnlock(handle9), lockUnlock(handle10);

        test_assert(!lockWouldBlock(62,db,key1));
        ack_case_reset();
    }

    TEST("lock-proceeded: flush if lock blocked or blocking") {
        int proceeded;
        flushAndBlock fnb1 = {0}, fnb2 = {0}, fnb3 = {0}, fnb4 = {0}, fnb5 = {0};

        proceeded = lockLock(70,db,key1,proceedLaterGetLockAndFlush,NULL,&fnb1,NULL,NULL);
        test_assert(proceeded == 1);
        test_assert(fnb1.flush == 0);
        test_assert(fnb1.locker != NULL);

        proceeded = lockLock(71,db,key1,proceedLaterGetLockAndFlush,NULL,&fnb2,NULL,NULL);
        test_assert(proceeded == 0);

        proceeded = lockLock(71,db,key2,proceedLaterGetLockAndFlush,NULL,&fnb3,NULL,NULL);
        test_assert(proceeded == 1);
        test_assert(fnb3.flush == 0);
        test_assert(fnb3.locker != NULL);

        proceeded = lockLock(72,db,NULL,proceedLaterGetLockAndFlush,NULL,&fnb4,NULL,NULL);
        test_assert(proceeded == 0);

        proceeded = lockLock(72,db,key2,proceedLaterGetLockAndFlush,NULL,&fnb5,NULL,NULL);
        test_assert(proceeded == 0);

        lockProceeded(fnb3.locker);
        test_assert(fnb4.locker == NULL);

        lockProceeded(fnb1.locker);
        test_assert(fnb2.locker == NULL);

        lockUnlock(fnb1.locker);
        test_assert(fnb2.locker != NULL);
        test_assert(fnb2.flush == 1); /* fnb4 depends on fnb2, so fnb2 should flush. */

        lockProceeded(fnb2.locker);

        lockUnlock(fnb3.locker);
        test_assert(fnb4.locker == NULL);

        lockUnlock(fnb2.locker);
        test_assert(fnb4.locker != NULL);
        test_assert(fnb4.flush == 1); /* fnb5 depends on fnb4, so fnb4 should flush. */

        lockProceeded(fnb4.locker);
        test_assert(fnb5.locker);
        test_assert(fnb5.flush == 0);

        ack_case_reset();
    }

    TEST("lock-proceeded: deinit") {
        decrRefCount(key1), decrRefCount(key2);
    }

   return error;
}

#endif
