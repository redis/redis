/*
 * Background Job Manager - submit jobs to a background thread.
 */
#include "fmacros.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#include "bjm.h"
#include "fifo.h"
#include "zmalloc.h"
#include "atomicvar.h"
#include "server.h"     // Gah!  Hate to pull this in.  CPU affinity & BIO config.

static const unsigned BJM_THREAD_STACK_SIZE = 4 * 1024 * 1024;
static const int INITIAL_FUNCTION_CAPACITY = 8;

static int functionsCount;
static int functionsCapacity;
static bjmJobFunc *functions;   // An array of function pointers.  Index matches job lists.

// A FIFO queue with a mutex to protect access
typedef struct {
    pthread_mutex_t mutex;
    Fifo *fifo;
} MutexFifo;

// A Joblist contains a specific function to be executed with a list of privdata
typedef struct {
    bjmJobFunc func;        // The callback function for the jobs
    MutexFifo jobs;         // The contained list of jobs (privdata values)
    redisAtomic long job_count;  // Might be greater than length(jobs). It includes in-progress.
} Joblist;

// This arrays hold a Joblist* for each known callback function.
static Joblist **jobsByFunc;    // Array indexed by index in functions[]

// This FIFO queue hold Joblists from the array above.  Each time one of those
//  Joblists becomes non-empty, it gets added to the active queue.
static MutexFifo activeJoblists;

static redisAtomic long queuedJobCount;
static redisAtomic long processedJobCount;

static pthread_cond_t wakeup_cond;  // Triggered when jobs are submitted

static int threadCount = 0;
static pthread_t *threads;      // Array of threads


static void mutexFifoInit(MutexFifo *q) {
    pthread_mutex_init(&q->mutex, NULL);
    q->fifo = fifoCreate();
}


static void mutexFifoLock(MutexFifo *q) {
    pthread_mutex_lock(&q->mutex);
}


static void mutexFifoUnlock(MutexFifo *q) {
    pthread_mutex_unlock(&q->mutex);
}


static void increaseFunctionCapacity() {
    assert(functionsCapacity > 0);  // was bjmInit called?
    functionsCapacity *= 2;
    functions = zrealloc(functions, functionsCapacity * sizeof(bjmJobFunc));
    jobsByFunc = zrealloc(jobsByFunc, functionsCapacity * sizeof(Joblist*));
}


// Find the function's index.  Adds the function if it's a new one.
static int getFuncIdx(bjmJobFunc func) {
    // It's expected that the function count is small, probably spanning only 1 or 2 cache lines.
    //  A simple linear search will be faster than a complex structure like hash.
    for (int i = 0;  i < functionsCount;  i++) {
        if (functions[i] == func) return i;
    }

    // At this point, we know that the function isn't in the list.  Insert at end.
    if (functionsCount == functionsCapacity) increaseFunctionCapacity();
    int idx = functionsCount++;
    functions[idx] = func;
    jobsByFunc[idx] = zmalloc(sizeof(Joblist));
    mutexFifoInit(&jobsByFunc[idx]->jobs);
    jobsByFunc[idx]->func = func;
    atomicSet(jobsByFunc[idx]->job_count, 0);
    return idx;
}


/* Pull one job from the active joblists.  Synchronously waits for a job if none available.
 *  privdata_ptr - returns the caller supplied privdata.
 *  joblist_ptr  - returns the joblist that the job was taken from.  This is needed by the caller
 *                 in order to (later) decrement the job_count.
 * Returns:
 *  Returns the bjmJobFunc to be called.
 */
static bjmJobFunc waitForJob(void **privdata_ptr, Joblist **joblist_ptr) {
    bjmJobFunc func = NULL;

    mutexFifoLock(&activeJoblists);
    while (fifoLength(activeJoblists.fifo) == 0) {
        pthread_cond_wait(&wakeup_cond, &activeJoblists.mutex);
    }

    Joblist *joblist = fifoPeek(activeJoblists.fifo);
    func = joblist->func;
    *joblist_ptr = joblist;

    mutexFifoLock(&joblist->jobs);
    *privdata_ptr = fifoPop(joblist->jobs.fifo);

    if (fifoLength(joblist->jobs.fifo) == 0) {
        // No jobs left for this function
        fifoPop(activeJoblists.fifo);
    } else if (fifoLength(activeJoblists.fifo) > 1) {
        // Rotate the joblist for this function to the end
        fifoPop(activeJoblists.fifo);
        fifoPush(activeJoblists.fifo, joblist);
    }
    // Keep the lock on the individual joblist until it is properly handled in
    //  the activeJobLists.  Can't have the size changing.
    mutexFifoUnlock(&joblist->jobs);
    mutexFifoUnlock(&activeJoblists);

    return func;
}


static void *pthreadFunction(void *arg) {
    int threadNum = (intptr_t)arg;

    const int MAX_THREAD_NAME = 16;
    char thread_name[MAX_THREAD_NAME];
    snprintf(thread_name, MAX_THREAD_NAME, "bjm thread %d", threadNum);
    redis_set_thread_title(thread_name);

    redisSetCpuAffinity(server.bio_cpulist);

    makeThreadKillable();

    /* Block SIGALRM so only the main thread will receive the watchdog signal. */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL)) {
        serverLog(LL_WARNING, "Warning: can't mask SIGALRM in BJM thread: %s", strerror(errno));
    }

    while (1) {
        void *privdata;
        Joblist *joblist;
        bjmJobFunc func = waitForJob(&privdata, &joblist);

        func(privdata);                     // Execute the callback
        atomicDecr(joblist->job_count, 1);   // Decrement count AFTER callback finishes
        atomicDecr(queuedJobCount, 1);
        atomicIncr(processedJobCount, 1);
    }

    return NULL;
}


// API
void bjmInit(int numThreads) {
    if (threadCount == numThreads) return;  // Silently skip to support testing
    assert(threadCount == 0);   // But don't allow changing the number of threads

    functionsCount = 0;
    functionsCapacity = INITIAL_FUNCTION_CAPACITY;

    functions = zmalloc(functionsCapacity * sizeof(bjmJobFunc));
    jobsByFunc = zmalloc(functionsCapacity * sizeof(Joblist*));

    atomicSet(queuedJobCount, 0);
    atomicSet(processedJobCount, 0);

    mutexFifoInit(&activeJoblists);

    pthread_cond_init(&wakeup_cond, NULL);

    threadCount = numThreads;
    threads = zmalloc(threadCount * sizeof(pthread_t));

    pthread_attr_t attr;
    size_t stacksize;
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stacksize);
    if (stacksize < BJM_THREAD_STACK_SIZE) stacksize = BJM_THREAD_STACK_SIZE;
    pthread_attr_setstacksize(&attr, stacksize);

    for (int i = 0;  i < threadCount;  i++) {
        void *arg = (void*)(intptr_t)i;
        if (pthread_create(&threads[i], &attr, pthreadFunction, arg) != 0) {
            serverLog(LL_WARNING, "Fatal: Can't initialize background jobs.");
            exit(1);
        }
    }
}


// API
bjmJobFuncHandle bjmRegisterJobFunc(bjmJobFunc func) {
    return getFuncIdx(func) + 1;    // +1 to avoid 0 (uninitialized static) being a valid value
}


// API
void bjmSubmitJob(bjmJobFuncHandle funcHandle, void *privdata) {
    Joblist *joblist = jobsByFunc[funcHandle - 1];

    mutexFifoLock(&joblist->jobs);
    fifoPush(joblist->jobs.fifo, privdata);
    atomicIncr(joblist->job_count, 1);
    atomicIncr(queuedJobCount, 1);
    if (fifoLength(joblist->jobs.fifo) == 1) {
        // Reader threads take the activeJobists lock before the joblist lock.  But this can't
        //  cause deadlock because this joblist isn't in the active joblist yet.
        mutexFifoLock(&activeJoblists);
        fifoPush(activeJoblists.fifo, joblist);
        mutexFifoUnlock(&activeJoblists);
    }
    pthread_cond_signal(&wakeup_cond);
    mutexFifoUnlock(&joblist->jobs);
}


// API
void bjmKillThreads(void) {
    for (int i = 0;  i < threadCount;  i++) {
        if (threads[i] == pthread_self()) continue;
        if (pthread_cancel(threads[i]) == 0) {
            int err = pthread_join(threads[i], NULL);
            if (err == 0) {
                serverLog(LL_WARNING, "BJM thread #%d terminated", i);
            } else {
                serverLog(LL_WARNING, "BJM thread #%d can not be joined: %s", i, strerror(err));
            }
        }
    }
}


// API
long bjmPendingJobsOfType(bjmJobFuncHandle funcHandle) {
    if (funcHandle == 0) return 0;  // func not registered (yet)
    long jobCount;
    atomicGet(jobsByFunc[funcHandle - 1]->job_count, jobCount);
    return jobCount;
}


// API
sds bjmCatInfo(sds info) {
    long queuedJobs, processedJobs;
    atomicGet(queuedJobCount, queuedJobs);
    atomicGet(processedJobCount, processedJobs);

    info = sdscatprintf(info,
        "# BackgroundJobManager\r\n"
        "bjm_num_threads:%d\r\n"
        "bjm_num_callbacks:%d\r\n"
        "bjm_jobs_in_queue:%ld\r\n"
        "bjm_processed_jobs:%ld\r\n",
        threadCount,
        functionsCount,
        queuedJobs,
        processedJobs
    );
    return info;
}
