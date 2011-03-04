#include "redis.h"

#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <signal.h>

/* dscache.c - Disk store cache for disk store backend.
 *
 * When Redis is configured for using disk as backend instead of memory, the
 * memory is used as a cache, so that recently accessed keys are taken in
 * memory for fast read and write operations.
 *
 * Modified keys are marked to be flushed on disk, and will be flushed
 * as long as the maxium configured flush time elapsed.
 *
 * This file implements the whole caching subsystem and contains further
 * documentation. */

/* TODO:
 *
 * WARNING: most of the following todo items and design issues are no
 * longer relevant with the new design. Here as a checklist to see if
 * some old ideas still apply.
 *
 * - What happens when an object is destroyed?
 *
 *   If the object is destroyed since semantically it was deleted or
 *   replaced with something new, we don't care if there was a SAVE
 *   job pending for it. Anyway when the IO JOb will be created we'll get
 *   the pointer of the current value.
 *
 *   If the object is already a REDIS_IO_SAVEINPROG object, then it is
 *   impossible that we get a decrRefCount() that will reach refcount of zero
 *   since the object is both in the dataset and in the io job entry.
 *
 * - What happens with MULTI/EXEC?
 *
 *   Good question. Without some kind of versioning with a global counter
 *   it is not possible to have trasactions on disk, but they are still
 *   useful since from the point of view of memory and client bugs it is
 *   a protection anyway. Also it's useful for WATCH.
 *
 *   Btw there is to check what happens when WATCH gets combined to keys
 *   that gets removed from the object cache. Should be save but better
 *   to check.
 *
 * - Check if/why INCR will not update the LRU info for the object.
 *
 * - Fix/Check the following race condition: a key gets a DEL so there is
 *   a write operation scheduled against this key. Later the same key will
 *   be the argument of a GET, but the write operation was still not
 *   completed (to delete the file). If the GET will be for some reason
 *   a blocking loading (via lookup) we can load the old value on memory.
 *
 *   This problems can be fixed with negative caching. We can use it
 *   to optimize the system, but also when a key is deleted we mark
 *   it as non existing on disk as well (in a way that this cache
 *   entry can't be evicted, setting time to 0), then we avoid looking at
 *   the disk at all if the key can't be there. When an IO Job complete
 *   a deletion, we set the time of the negative caching to a non zero
 *   value so it will be evicted later.
 *
 *   Are there other patterns like this where we load stale data?
 *
 *   Also, make sure that key preloading is ONLY done for keys that are
 *   not marked as cacheKeyDoesNotExist(), otherwise, again, we can load
 *   data from disk that should instead be deleted.
 *
 * - dsSet() should use rename(2) in order to avoid corruptions.
 *
 * - Don't add a LOAD if there is already a LOADINPROGRESS, or is this
 *   impossible since anyway the io_keys stuff will work as lock?
 *
 * - Serialize special encoded things in a raw form.
 *
 * - When putting IO read operations on top of the queue, do this only if
 *   the already-on-top operation is not a save or if it is a save that
 *   is scheduled for later execution. If there is a save that is ready to
 *   fire, let's insert the load operation just before the first save that
 *   is scheduled for later exection for instance.
 *
 * - Support MULTI/EXEC transactions via a journal file, that is played on
 *   startup to check if there is cleanup to do. This way we can implement
 *   transactions with our simple file based KV store.
 */

/* Virtual Memory is composed mainly of two subsystems:
 * - Blocking Virutal Memory
 * - Threaded Virtual Memory I/O
 * The two parts are not fully decoupled, but functions are split among two
 * different sections of the source code (delimited by comments) in order to
 * make more clear what functionality is about the blocking VM and what about
 * the threaded (not blocking) VM.
 *
 * Redis VM design:
 *
 * Redis VM is a blocking VM (one that blocks reading swapped values from
 * disk into memory when a value swapped out is needed in memory) that is made
 * unblocking by trying to examine the command argument vector in order to
 * load in background values that will likely be needed in order to exec
 * the command. The command is executed only once all the relevant keys
 * are loaded into memory.
 *
 * This basically is almost as simple of a blocking VM, but almost as parallel
 * as a fully non-blocking VM.
 */

void spawnIOThread(void);
int cacheScheduleIOPushJobs(int flags);
int processActiveIOJobs(int max);

/* =================== Virtual Memory - Blocking Side  ====================== */

void dsInit(void) {
    int pipefds[2];
    size_t stacksize;

    zmalloc_enable_thread_safeness(); /* we need thread safe zmalloc() */

    redisLog(REDIS_NOTICE,"Opening Disk Store: %s", server.ds_path);
    /* Open Disk Store */
    if (dsOpen() != REDIS_OK) {
        redisLog(REDIS_WARNING,"Fatal error opening disk store. Exiting.");
        exit(1);
    };

    /* Initialize threaded I/O for Object Cache */
    server.io_newjobs = listCreate();
    server.io_processing = listCreate();
    server.io_processed = listCreate();
    server.io_ready_clients = listCreate();
    pthread_mutex_init(&server.io_mutex,NULL);
    pthread_cond_init(&server.io_condvar,NULL);
    pthread_mutex_init(&server.bgsavethread_mutex,NULL);
    server.io_active_threads = 0;
    if (pipe(pipefds) == -1) {
        redisLog(REDIS_WARNING,"Unable to intialized DS: pipe(2): %s. Exiting."
            ,strerror(errno));
        exit(1);
    }
    server.io_ready_pipe_read = pipefds[0];
    server.io_ready_pipe_write = pipefds[1];
    redisAssert(anetNonBlock(NULL,server.io_ready_pipe_read) != ANET_ERR);
    /* LZF requires a lot of stack */
    pthread_attr_init(&server.io_threads_attr);
    pthread_attr_getstacksize(&server.io_threads_attr, &stacksize);

    /* Solaris may report a stacksize of 0, let's set it to 1 otherwise
     * multiplying it by 2 in the while loop later will not really help ;) */
    if (!stacksize) stacksize = 1;

    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&server.io_threads_attr, stacksize);
    /* Listen for events in the threaded I/O pipe */
    if (aeCreateFileEvent(server.el, server.io_ready_pipe_read, AE_READABLE,
        vmThreadedIOCompletedJob, NULL) == AE_ERR)
        oom("creating file event");

    /* Spawn our I/O thread */
    spawnIOThread();
}

/* Compute how good candidate the specified object is for eviction.
 * An higher number means a better candidate. */
double computeObjectSwappability(robj *o) {
    /* actual age can be >= minage, but not < minage. As we use wrapping
     * 21 bit clocks with minutes resolution for the LRU. */
    return (double) estimateObjectIdleTime(o);
}

/* Try to free one entry from the diskstore object cache */
int cacheFreeOneEntry(void) {
    int j, i;
    struct dictEntry *best = NULL;
    double best_swappability = 0;
    redisDb *best_db = NULL;
    robj *val;
    sds key;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        /* Why maxtries is set to 100?
         * Because this way (usually) we'll find 1 object even if just 1% - 2%
         * are swappable objects */
        int maxtries = 100;

        for (i = 0; i < 5 && dictSize(db->dict); i++) {
            dictEntry *de;
            double swappability;
            robj keyobj;
            sds keystr;

            if (maxtries) maxtries--;
            de = dictGetRandomKey(db->dict);
            keystr = dictGetEntryKey(de);
            val = dictGetEntryVal(de);
            initStaticStringObject(keyobj,keystr);

            /* Don't remove objects that are currently target of a
             * read or write operation. */
            if (cacheScheduleIOGetFlags(db,&keyobj) != 0) {
                if (maxtries) i--; /* don't count this try */
                continue;
            }
            swappability = computeObjectSwappability(val);
            if (!best || swappability > best_swappability) {
                best = de;
                best_swappability = swappability;
                best_db = db;
            }
        }
    }
    if (best == NULL) {
        /* Was not able to fix a single object... we should check if our
         * IO queues have stuff in queue, and try to consume the queue
         * otherwise we'll use an infinite amount of memory if changes to
         * the dataset are faster than I/O */
        if (listLength(server.cache_io_queue) > 0) {
            redisLog(REDIS_DEBUG,"--- Busy waiting IO to reclaim memory");
            cacheScheduleIOPushJobs(REDIS_IO_ASAP);
            processActiveIOJobs(1);
            return REDIS_OK;
        }
        /* Nothing to free at all... */
        return REDIS_ERR;
    }
    key = dictGetEntryKey(best);
    val = dictGetEntryVal(best);

    redisLog(REDIS_DEBUG,"Key selected for cache eviction: %s swappability:%f",
        key, best_swappability);

    /* Delete this key from memory */
    {
        robj *kobj = createStringObject(key,sdslen(key));
        dbDelete(best_db,kobj);
        decrRefCount(kobj);
    }
    return REDIS_OK;
}

/* Return true if it's safe to swap out objects in a given moment.
 * Basically we don't want to swap objects out while there is a BGSAVE
 * or a BGAEOREWRITE running in backgroud. */
int dsCanTouchDiskStore(void) {
    return (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1);
}

/* ==================== Disk store negative caching  ========================
 *
 * When disk store is enabled, we need negative caching, that is, to remember
 * keys that are for sure *not* on the disk key-value store.
 *
 * This is usefuls because without negative caching cache misses will cost us
 * a disk lookup, even if the same non existing key is accessed again and again.
 *
 * With negative caching we remember that the key is not on disk, so if it's
 * not in memory and we have a negative cache entry, we don't try a disk
 * access at all.
 */

/* Returns true if the specified key may exists on disk, that is, we don't
 * have an entry in our negative cache for this key */
int cacheKeyMayExist(redisDb *db, robj *key) {
    return dictFind(db->io_negcache,key) == NULL;
}

/* Set the specified key as an entry that may possibily exist on disk, that is,
 * remove the negative cache entry for this key if any. */
void cacheSetKeyMayExist(redisDb *db, robj *key) {
    dictDelete(db->io_negcache,key);
}

/* Set the specified key as non existing on disk, that is, create a negative
 * cache entry for this key. */
void cacheSetKeyDoesNotExist(redisDb *db, robj *key) {
    if (dictReplace(db->io_negcache,key,(void*)time(NULL))) {
        incrRefCount(key);
    }
}

/* Remove one entry from negative cache using approximated LRU. */
int negativeCacheEvictOneEntry(void) {
    struct dictEntry *de;
    robj *best = NULL;
    redisDb *best_db = NULL;
    time_t time, best_time = 0;
    int j;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        int i;

        if (dictSize(db->io_negcache) == 0) continue;
        for (i = 0; i < 3; i++) {
            de = dictGetRandomKey(db->io_negcache);
            time = (time_t) dictGetEntryVal(de);

            if (best == NULL || time < best_time) {
                best = dictGetEntryKey(de);
                best_db = db;
                best_time = time;
            }
        }
    }
    if (best) {
        dictDelete(best_db->io_negcache,best);
        return REDIS_OK;
    } else {
        return REDIS_ERR;
    }
}

/* ================== Disk store cache - Threaded I/O  ====================== */

void freeIOJob(iojob *j) {
    decrRefCount(j->key);
    /* j->val can be NULL if the job is about deleting the key from disk. */
    if (j->val) decrRefCount(j->val);
    zfree(j);
}

/* Every time a thread finished a Job, it writes a byte into the write side
 * of an unix pipe in order to "awake" the main thread, and this function
 * is called.
 *
 * If privdata != NULL the function will try to put more jobs in the queue
 * of IO jobs to process as more room is made. */
void vmThreadedIOCompletedJob(aeEventLoop *el, int fd, void *privdata,
            int mask)
{
    char buf[1];
    int retval, processed = 0, toprocess = -1;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    /* For every byte we read in the read side of the pipe, there is one
     * I/O job completed to process. */
    while((retval = read(fd,buf,1)) == 1) {
        iojob *j;
        listNode *ln;

        redisLog(REDIS_DEBUG,"Processing I/O completed job");

        /* Get the processed element (the oldest one) */
        lockThreadedIO();
        redisAssert(listLength(server.io_processed) != 0);
        if (toprocess == -1) {
            toprocess = (listLength(server.io_processed)*REDIS_MAX_COMPLETED_JOBS_PROCESSED)/100;
            if (toprocess <= 0) toprocess = 1;
        }
        ln = listFirst(server.io_processed);
        j = ln->value;
        listDelNode(server.io_processed,ln);
        unlockThreadedIO();

        /* Post process it in the main thread, as there are things we
         * can do just here to avoid race conditions and/or invasive locks */
        redisLog(REDIS_DEBUG,"COMPLETED Job type %s, key: %s",
            (j->type == REDIS_IOJOB_LOAD) ? "load" : "save",
            (unsigned char*)j->key->ptr);
        if (j->type == REDIS_IOJOB_LOAD) {
            /* Create the key-value pair in the in-memory database */
            if (j->val != NULL) {
                /* Note: it's possible that the key is already in memory
                 * due to a blocking load operation. */
                if (dbAdd(j->db,j->key,j->val) == REDIS_OK) {
                    incrRefCount(j->val);
                    if (j->expire != -1) setExpire(j->db,j->key,j->expire);
                }
            } else {
                /* Key not found on disk. If it is also not in memory
                 * as a cached object, nor there is a job writing it
                 * in background, we are sure the key does not exist
                 * currently.
                 *
                 * So we set a negative cache entry avoiding that the
                 * resumed client will block load what does not exist... */
                if (dictFind(j->db->dict,j->key->ptr) == NULL &&
                    (cacheScheduleIOGetFlags(j->db,j->key) &
                      (REDIS_IO_SAVE|REDIS_IO_SAVEINPROG)) == 0)
                {
                    cacheSetKeyDoesNotExist(j->db,j->key);
                }
            }
            cacheScheduleIODelFlag(j->db,j->key,REDIS_IO_LOADINPROG);
            handleClientsBlockedOnSwappedKey(j->db,j->key);
            freeIOJob(j);
        } else if (j->type == REDIS_IOJOB_SAVE) {
            cacheScheduleIODelFlag(j->db,j->key,REDIS_IO_SAVEINPROG);
            freeIOJob(j);
        }
        processed++;
        if (privdata != NULL) cacheScheduleIOPushJobs(0);
        if (processed == toprocess) return;
    }
    if (retval < 0 && errno != EAGAIN) {
        redisLog(REDIS_WARNING,
            "WARNING: read(2) error in vmThreadedIOCompletedJob() %s",
            strerror(errno));
    }
}

void lockThreadedIO(void) {
    pthread_mutex_lock(&server.io_mutex);
}

void unlockThreadedIO(void) {
    pthread_mutex_unlock(&server.io_mutex);
}

void *IOThreadEntryPoint(void *arg) {
    iojob *j;
    listNode *ln;
    REDIS_NOTUSED(arg);
    long long start;

    pthread_detach(pthread_self());
    lockThreadedIO();
    while(1) {
        /* Get a new job to process */
        if (listLength(server.io_newjobs) == 0) {
            /* Wait for more work to do */
            redisLog(REDIS_DEBUG,"[T] wait for signal");
            pthread_cond_wait(&server.io_condvar,&server.io_mutex);
            redisLog(REDIS_DEBUG,"[T] signal received");
            continue;
        }
        start = ustime();
        redisLog(REDIS_DEBUG,"[T] %ld IO jobs to process",
            listLength(server.io_newjobs));
        ln = listFirst(server.io_newjobs);
        j = ln->value;
        listDelNode(server.io_newjobs,ln);
        /* Add the job in the processing queue */
        listAddNodeTail(server.io_processing,j);
        ln = listLast(server.io_processing); /* We use ln later to remove it */
        unlockThreadedIO();

        redisLog(REDIS_DEBUG,"[T] %ld: new job type %s: %p about key '%s'",
            (long) pthread_self(),
            (j->type == REDIS_IOJOB_LOAD) ? "load" : "save",
            (void*)j, (char*)j->key->ptr);

        /* Process the Job */
        if (j->type == REDIS_IOJOB_LOAD) {
            time_t expire;

            j->val = dsGet(j->db,j->key,&expire);
            if (j->val) j->expire = expire;
        } else if (j->type == REDIS_IOJOB_SAVE) {
            if (j->val) {
                dsSet(j->db,j->key,j->val,j->expire);
            } else {
                dsDel(j->db,j->key);
            }
        }

        /* Done: insert the job into the processed queue */
        redisLog(REDIS_DEBUG,"[T] %ld completed the job: %p (key %s)",
            (long) pthread_self(), (void*)j, (char*)j->key->ptr);

        redisLog(REDIS_DEBUG,"[T] lock IO");
        lockThreadedIO();
        redisLog(REDIS_DEBUG,"[T] IO locked");
        listDelNode(server.io_processing,ln);
        listAddNodeTail(server.io_processed,j);

        /* Signal the main thread there is new stuff to process */
        redisAssert(write(server.io_ready_pipe_write,"x",1) == 1);
        redisLog(REDIS_DEBUG,"TIME (%c): %lld\n", j->type == REDIS_IOJOB_LOAD ? 'L' : 'S', ustime()-start);
    }
    /* never reached, but that's the full pattern... */
    unlockThreadedIO();
    return NULL;
}

void spawnIOThread(void) {
    pthread_t thread;
    sigset_t mask, omask;
    int err;

    sigemptyset(&mask);
    sigaddset(&mask,SIGCHLD);
    sigaddset(&mask,SIGHUP);
    sigaddset(&mask,SIGPIPE);
    pthread_sigmask(SIG_SETMASK, &mask, &omask);
    while ((err = pthread_create(&thread,&server.io_threads_attr,IOThreadEntryPoint,NULL)) != 0) {
        redisLog(REDIS_WARNING,"Unable to spawn an I/O thread: %s",
            strerror(err));
        usleep(1000000);
    }
    pthread_sigmask(SIG_SETMASK, &omask, NULL);
    server.io_active_threads++;
}

/* Wait that up to 'max' pending IO Jobs are processed by the I/O thread.
 * From our point of view an IO job processed means that the count of
 * server.io_processed must increase by one.
 *
 * If max is -1, all the pending IO jobs will be processed.
 *
 * Returns the number of IO jobs processed.
 *
 * NOTE: while this may appear like a busy loop, we are actually blocked
 * by IO since we continuously acquire/release the IO lock. */
int processActiveIOJobs(int max) {
    int processed = 0;

    while(max == -1 || max > 0) {
        int io_processed_len;

        redisLog(REDIS_DEBUG,"[P] lock IO");
        lockThreadedIO();
        redisLog(REDIS_DEBUG,"Waiting IO jobs processing: new:%d proessing:%d processed:%d",listLength(server.io_newjobs),listLength(server.io_processing),listLength(server.io_processed));

        if (listLength(server.io_newjobs) == 0 &&
            listLength(server.io_processing) == 0)
        {
            /* There is nothing more to process */
            redisLog(REDIS_DEBUG,"[P] Nothing to process, unlock IO, return");
            unlockThreadedIO();
            break;
        }

#if 1
        /* If there are new jobs we need to signal the thread to
         * process the next one. FIXME: drop this if useless. */
        redisLog(REDIS_DEBUG,"[P] waitEmptyIOJobsQueue: new %d, processing %d, processed %d",
            listLength(server.io_newjobs),
            listLength(server.io_processing),
            listLength(server.io_processed));

        if (listLength(server.io_newjobs)) {
            redisLog(REDIS_DEBUG,"[P] There are new jobs, signal");
            pthread_cond_signal(&server.io_condvar);
        }
#endif

        /* Check if we can process some finished job */
        io_processed_len = listLength(server.io_processed);
        redisLog(REDIS_DEBUG,"[P] Unblock IO");
        unlockThreadedIO();
        redisLog(REDIS_DEBUG,"[P] Wait");
        usleep(10000);
        if (io_processed_len) {
            vmThreadedIOCompletedJob(NULL,server.io_ready_pipe_read,
                                                        (void*)0xdeadbeef,0);
            processed++;
            if (max != -1) max--;
        }
    }
    return processed;
}

void waitEmptyIOJobsQueue(void) {
    processActiveIOJobs(-1);
}

/* Process up to 'max' IO Jobs already completed by threads but still waiting
 * processing from the main thread.
 *
 * If max == -1 all the pending jobs are processed.
 *
 * The number of processed jobs is returned. */
int processPendingIOJobs(int max) {
    int processed = 0;

    while(max == -1 || max > 0) {
        int io_processed_len;

        lockThreadedIO();
        io_processed_len = listLength(server.io_processed);
        unlockThreadedIO();
        if (io_processed_len == 0) break;
        vmThreadedIOCompletedJob(NULL,server.io_ready_pipe_read,
                                                    (void*)0xdeadbeef,0);
        if (max != -1) max--;
        processed++;
    }
    return processed;
}

void processAllPendingIOJobs(void) {
    processPendingIOJobs(-1);
}

/* This function must be called while with threaded IO locked */
void queueIOJob(iojob *j) {
    redisLog(REDIS_DEBUG,"Queued IO Job %p type %d about key '%s'\n",
        (void*)j, j->type, (char*)j->key->ptr);
    listAddNodeTail(server.io_newjobs,j);
    if (server.io_active_threads < server.vm_max_threads)
        spawnIOThread();
}

/* Consume all the IO scheduled operations, and all the thread IO jobs
 * so that eventually the state of diskstore is a point-in-time snapshot.
 *
 * This is useful when we need to BGSAVE with diskstore enabled. */
void cacheForcePointInTime(void) {
    redisLog(REDIS_NOTICE,"Diskstore: synching on disk to reach point-in-time state.");
    while (listLength(server.cache_io_queue) != 0) {
        cacheScheduleIOPushJobs(REDIS_IO_ASAP);
        processActiveIOJobs(1);
    }
    waitEmptyIOJobsQueue();
    processAllPendingIOJobs();
}

void cacheCreateIOJob(int type, redisDb *db, robj *key, robj *val, time_t expire) {
    iojob *j;

    j = zmalloc(sizeof(*j));
    j->type = type;
    j->db = db;
    j->key = key;
    incrRefCount(key);
    j->val = val;
    if (val) incrRefCount(val);
    j->expire = expire;

    lockThreadedIO();
    queueIOJob(j);
    pthread_cond_signal(&server.io_condvar);
    unlockThreadedIO();
}

/* ============= Disk store cache - Scheduling of IO operations ============= 
 *
 * We use a queue and an hash table to hold the state of IO operations
 * so that's fast to lookup if there is already an IO operation in queue
 * for a given key.
 *
 * There are two types of IO operations for a given key:
 * REDIS_IO_LOAD and REDIS_IO_SAVE.
 *
 * The function cacheScheduleIO() function pushes the specified IO operation
 * in the queue, but avoid adding the same key for the same operation
 * multiple times, thanks to the associated hash table.
 *
 * We take a set of flags per every key, so when the scheduled IO operation
 * gets moved from the scheduled queue to the actual IO Jobs queue that
 * is processed by the IO thread, we flag it as IO_LOADINPROG or
 * IO_SAVEINPROG.
 *
 * So for every given key we always know if there is some IO operation
 * scheduled, or in progress, for this key.
 *
 * NOTE: all this is very important in order to guarantee correctness of
 * the Disk Store Cache. Jobs are always queued here. Load jobs are
 * queued at the head for faster execution only in the case there is not
 * already a write operation of some kind for this job.
 *
 * So we have ordering, but can do exceptions when there are no already
 * operations for a given key. Also when we need to block load a given
 * key, for an immediate lookup operation, we can check if the key can
 * be accessed synchronously without race conditions (no IN PROGRESS
 * operations for this key), otherwise we blocking wait for completion. */

#define REDIS_IO_LOAD 1
#define REDIS_IO_SAVE 2
#define REDIS_IO_LOADINPROG 4
#define REDIS_IO_SAVEINPROG 8

void cacheScheduleIOAddFlag(redisDb *db, robj *key, long flag) {
    struct dictEntry *de = dictFind(db->io_queued,key);

    if (!de) {
        dictAdd(db->io_queued,key,(void*)flag);
        incrRefCount(key);
        return;
    } else {
        long flags = (long) dictGetEntryVal(de);

        if (flags & flag) {
            redisLog(REDIS_WARNING,"Adding the same flag again: was: %ld, addede: %ld",flags,flag);
            redisAssert(!(flags & flag));
        }
        flags |= flag;
        dictGetEntryVal(de) = (void*) flags;
    }
}

void cacheScheduleIODelFlag(redisDb *db, robj *key, long flag) {
    struct dictEntry *de = dictFind(db->io_queued,key);
    long flags;

    redisAssert(de != NULL);
    flags = (long) dictGetEntryVal(de);
    redisAssert(flags & flag);
    flags &= ~flag;
    if (flags == 0) {
        dictDelete(db->io_queued,key);
    } else {
        dictGetEntryVal(de) = (void*) flags;
    }
}

int cacheScheduleIOGetFlags(redisDb *db, robj *key) {
    struct dictEntry *de = dictFind(db->io_queued,key);

    return (de == NULL) ? 0 : ((long) dictGetEntryVal(de));
}

void cacheScheduleIO(redisDb *db, robj *key, int type) {
    ioop *op;
    long flags;

    if ((flags = cacheScheduleIOGetFlags(db,key)) & type) return;
    
    redisLog(REDIS_DEBUG,"Scheduling key %s for %s",
        key->ptr, type == REDIS_IO_LOAD ? "loading" : "saving");
    cacheScheduleIOAddFlag(db,key,type);
    op = zmalloc(sizeof(*op));
    op->type = type;
    op->db = db;
    op->key = key;
    incrRefCount(key);
    op->ctime = time(NULL);

    /* Give priority to load operations if there are no save already
     * in queue for the same key. */
    if (type == REDIS_IO_LOAD && !(flags & REDIS_IO_SAVE)) {
        listAddNodeHead(server.cache_io_queue, op);
        cacheScheduleIOPushJobs(REDIS_IO_ONLYLOADS);
    } else {
        /* FIXME: probably when this happens we want to at least move
         * the write job about this queue on top, and set the creation time
         * to a value that will force processing ASAP. */
        listAddNodeTail(server.cache_io_queue, op);
    }
}

/* Push scheduled IO operations into IO Jobs that the IO thread can process.
 *
 * If flags include REDIS_IO_ONLYLOADS only load jobs are processed:this is
 * useful since it's safe to push LOAD IO jobs from any place of the code, while
 * SAVE io jobs should never be pushed while we are processing a command
 * (not protected by lookupKey() that will block on keys in IO_SAVEINPROG
 * state.
 *
 * The REDIS_IO_ASAP flag tells the function to don't wait for the IO job
 * scheduled completion time, but just do the operation ASAP. This is useful
 * when we need to reclaim memory from the IO queue.
 */
#define MAX_IO_JOBS_QUEUE 10
int cacheScheduleIOPushJobs(int flags) {
    time_t now = time(NULL);
    listNode *ln;
    int jobs, topush = 0, pushed = 0;

    /* Don't push new jobs if there is a threaded BGSAVE in progress. */
    if (server.bgsavethread != (pthread_t) -1) return 0;

    /* Sync stuff on disk, but only if we have less
     * than MAX_IO_JOBS_QUEUE IO jobs. */
    lockThreadedIO();
    jobs = listLength(server.io_newjobs);
    unlockThreadedIO();

    topush = MAX_IO_JOBS_QUEUE-jobs;
    if (topush < 0) topush = 0;
    if (topush > (signed)listLength(server.cache_io_queue))
        topush = listLength(server.cache_io_queue);

    while((ln = listFirst(server.cache_io_queue)) != NULL) {
        ioop *op = ln->value;
        struct dictEntry *de;
        robj *val;

        if (!topush) break;
        topush--;

        if (op->type != REDIS_IO_LOAD && flags & REDIS_IO_ONLYLOADS) break;

        /* Don't execute SAVE before the scheduled time for completion */
        if (op->type == REDIS_IO_SAVE && !(flags & REDIS_IO_ASAP) &&
              (now - op->ctime) < server.cache_flush_delay) break;

        /* Don't add a SAVE job in the IO thread queue if there is already
         * a save in progress for the same key. */
        if (op->type == REDIS_IO_SAVE && 
            cacheScheduleIOGetFlags(op->db,op->key) & REDIS_IO_SAVEINPROG)
        {
            /* Move the operation at the end of the list if there
             * are other operations, so we can try to process the next one.
             * Otherwise break, nothing to do here. */
            if (listLength(server.cache_io_queue) > 1) {
                listDelNode(server.cache_io_queue,ln);
                listAddNodeTail(server.cache_io_queue,op);
                continue;
            } else {
                break;
            }
        }

        redisLog(REDIS_DEBUG,"Creating IO %s Job for key %s",
            op->type == REDIS_IO_LOAD ? "load" : "save", op->key->ptr);

        if (op->type == REDIS_IO_LOAD) {
            cacheCreateIOJob(REDIS_IOJOB_LOAD,op->db,op->key,NULL,0);
        } else {
            time_t expire = -1;

            /* Lookup the key, in order to put the current value in the IO
             * Job. Otherwise if the key does not exists we schedule a disk
             * store delete operation, setting the value to NULL. */
            de = dictFind(op->db->dict,op->key->ptr);
            if (de) {
                val = dictGetEntryVal(de);
                expire = getExpire(op->db,op->key);
            } else {
                /* Setting the value to NULL tells the IO thread to delete
                 * the key on disk. */
                val = NULL;
            }
            cacheCreateIOJob(REDIS_IOJOB_SAVE,op->db,op->key,val,expire);
        }
        /* Mark the operation as in progress. */
        cacheScheduleIODelFlag(op->db,op->key,op->type);
        cacheScheduleIOAddFlag(op->db,op->key,
            (op->type == REDIS_IO_LOAD) ? REDIS_IO_LOADINPROG :
                                          REDIS_IO_SAVEINPROG);
        /* Finally remove the operation from the queue.
         * But we'll have trace of it in the hash table. */
        listDelNode(server.cache_io_queue,ln);
        decrRefCount(op->key);
        zfree(op);
        pushed++;
    }
    return pushed;
}

void cacheCron(void) {
    /* Push jobs */
    cacheScheduleIOPushJobs(0);

    /* Reclaim memory from the object cache */
    while (server.ds_enabled && zmalloc_used_memory() >
            server.cache_max_memory)
    {
        int done = 0;

        if (cacheFreeOneEntry() == REDIS_OK) done++;
        if (negativeCacheEvictOneEntry() == REDIS_OK) done++;
        if (done == 0) break; /* nothing more to free */
    }
}

/* ========== Disk store cache - Blocking clients on missing keys =========== */

/* This function makes the clinet 'c' waiting for the key 'key' to be loaded.
 * If the key is already in memory we don't need to block.
 *
 *   FIXME: we should try if it's actually better to suspend the client
 *   accessing an object that is being saved, and awake it only when
 *   the saving was completed.
 *
 * Otherwise if the key is not in memory, we block the client and start
 * an IO Job to load it:
 *
 * the key is added to the io_keys list in the client structure, and also
 * in the hash table mapping swapped keys to waiting clients, that is,
 * server.io_waited_keys. */
int waitForSwappedKey(redisClient *c, robj *key) {
    struct dictEntry *de;
    list *l;

    /* Return ASAP if the key is in memory */
    de = dictFind(c->db->dict,key->ptr);
    if (de != NULL) return 0;

    /* Don't wait for keys we are sure are not on disk either */
    if (!cacheKeyMayExist(c->db,key)) return 0;

    /* Add the key to the list of keys this client is waiting for.
     * This maps clients to keys they are waiting for. */
    listAddNodeTail(c->io_keys,key);
    incrRefCount(key);

    /* Add the client to the swapped keys => clients waiting map. */
    de = dictFind(c->db->io_keys,key);
    if (de == NULL) {
        int retval;

        /* For every key we take a list of clients blocked for it */
        l = listCreate();
        retval = dictAdd(c->db->io_keys,key,l);
        incrRefCount(key);
        redisAssert(retval == DICT_OK);
    } else {
        l = dictGetEntryVal(de);
    }
    listAddNodeTail(l,c);

    /* Are we already loading the key from disk? If not create a job */
    if (de == NULL)
        cacheScheduleIO(c->db,key,REDIS_IO_LOAD);
    return 1;
}

/* Preload keys for any command with first, last and step values for
 * the command keys prototype, as defined in the command table. */
void waitForMultipleSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int j, last;
    if (cmd->vm_firstkey == 0) return;
    last = cmd->vm_lastkey;
    if (last < 0) last = argc+last;
    for (j = cmd->vm_firstkey; j <= last; j += cmd->vm_keystep) {
        redisAssert(j < argc);
        waitForSwappedKey(c,argv[j]);
    }
}

/* Preload keys needed for the ZUNIONSTORE and ZINTERSTORE commands.
 * Note that the number of keys to preload is user-defined, so we need to
 * apply a sanity check against argc. */
void zunionInterBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int i, num;
    REDIS_NOTUSED(cmd);

    num = atoi(argv[2]->ptr);
    if (num > (argc-3)) return;
    for (i = 0; i < num; i++) {
        waitForSwappedKey(c,argv[3+i]);
    }
}

/* Preload keys needed to execute the entire MULTI/EXEC block.
 *
 * This function is called by blockClientOnSwappedKeys when EXEC is issued,
 * and will block the client when any command requires a swapped out value. */
void execBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int i, margc;
    struct redisCommand *mcmd;
    robj **margv;
    REDIS_NOTUSED(cmd);
    REDIS_NOTUSED(argc);
    REDIS_NOTUSED(argv);

    if (!(c->flags & REDIS_MULTI)) return;
    for (i = 0; i < c->mstate.count; i++) {
        mcmd = c->mstate.commands[i].cmd;
        margc = c->mstate.commands[i].argc;
        margv = c->mstate.commands[i].argv;

        if (mcmd->vm_preload_proc != NULL) {
            mcmd->vm_preload_proc(c,mcmd,margc,margv);
        } else {
            waitForMultipleSwappedKeys(c,mcmd,margc,margv);
        }
    }
}

/* Is this client attempting to run a command against swapped keys?
 * If so, block it ASAP, load the keys in background, then resume it.
 *
 * The important idea about this function is that it can fail! If keys will
 * still be swapped when the client is resumed, this key lookups will
 * just block loading keys from disk. In practical terms this should only
 * happen with SORT BY command or if there is a bug in this function.
 *
 * Return 1 if the client is marked as blocked, 0 if the client can
 * continue as the keys it is going to access appear to be in memory. */
int blockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd) {
    if (cmd->vm_preload_proc != NULL) {
        cmd->vm_preload_proc(c,cmd,c->argc,c->argv);
    } else {
        waitForMultipleSwappedKeys(c,cmd,c->argc,c->argv);
    }

    /* If the client was blocked for at least one key, mark it as blocked. */
    if (listLength(c->io_keys)) {
        c->flags |= REDIS_IO_WAIT;
        aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
        server.cache_blocked_clients++;
        return 1;
    } else {
        return 0;
    }
}

/* Remove the 'key' from the list of blocked keys for a given client.
 *
 * The function returns 1 when there are no longer blocking keys after
 * the current one was removed (and the client can be unblocked). */
int dontWaitForSwappedKey(redisClient *c, robj *key) {
    list *l;
    listNode *ln;
    listIter li;
    struct dictEntry *de;

    /* The key object might be destroyed when deleted from the c->io_keys
     * list (and the "key" argument is physically the same object as the
     * object inside the list), so we need to protect it. */
    incrRefCount(key);

    /* Remove the key from the list of keys this client is waiting for. */
    listRewind(c->io_keys,&li);
    while ((ln = listNext(&li)) != NULL) {
        if (equalStringObjects(ln->value,key)) {
            listDelNode(c->io_keys,ln);
            break;
        }
    }
    redisAssert(ln != NULL);

    /* Remove the client form the key => waiting clients map. */
    de = dictFind(c->db->io_keys,key);
    redisAssert(de != NULL);
    l = dictGetEntryVal(de);
    ln = listSearchKey(l,c);
    redisAssert(ln != NULL);
    listDelNode(l,ln);
    if (listLength(l) == 0)
        dictDelete(c->db->io_keys,key);

    decrRefCount(key);
    return listLength(c->io_keys) == 0;
}

/* Every time we now a key was loaded back in memory, we handle clients
 * waiting for this key if any. */
void handleClientsBlockedOnSwappedKey(redisDb *db, robj *key) {
    struct dictEntry *de;
    list *l;
    listNode *ln;
    int len;

    de = dictFind(db->io_keys,key);
    if (!de) return;

    l = dictGetEntryVal(de);
    len = listLength(l);
    /* Note: we can't use something like while(listLength(l)) as the list
     * can be freed by the calling function when we remove the last element. */
    while (len--) {
        ln = listFirst(l);
        redisClient *c = ln->value;

        if (dontWaitForSwappedKey(c,key)) {
            /* Put the client in the list of clients ready to go as we
             * loaded all the keys about it. */
            listAddNodeTail(server.io_ready_clients,c);
        }
    }
}
