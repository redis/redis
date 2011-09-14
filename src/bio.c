/* Background I/O service for Redis.
 *
 * This file implements operations that we need to perform in the background.
 * Currently there is only a single operation, that is a background close(2)
 * system call. This is needed as when the process is the last owner of a
 * reference to a file closing it means unlinking it, and the deletion of the
 * file is slow, blocking the server.
 *
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put here Redis specific background tasks (for instance
 * it is not impossible that we'll need a non blocking FLUSHDB/FLUSHALL
 * implementation).
 *
 * DESIGN
 * ------
 *
 * The design is trivial, we have a structure representing a job to perform
 * and a single thread performing all the I/O operations in the queue.
 * Currently there is no way for the creator of the job to be notified about
 * the completion of the operation, this will only be added when/if needed.
 */

#include "redis.h"
#include "bio.h"

static pthread_mutex_t bio_mutex;
static pthread_cond_t bio_condvar;
static list *bio_jobs;
/* The following array is used to hold the number of pending jobs for every
 * OP type. This allows us to export the bioPendingJobsOfType() API that is
 * useful when the main thread wants to perform some operation that may involve
 * objects shared with the background thread. The main thread will just wait
 * that there are no longer jobs of this type to be executed before performing
 * the sensible operation. This data is also useful for reporting. */
static unsigned long long *bio_pending;

/* This structure represents a background Job. It is only used locally to this
 * file as the API deos not expose the internals at all. */
struct bio_job {
    int type;       /* Job type, for instance BIO_JOB_CLOSE */
    void *data;     /* Job specific arguments pointer. */
};

void *bioProcessBackgroundJobs(void *arg);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/* Initialize the background system, spawning the thread. */
void bioInit(void) {
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;
    int j;

    /* Initialization of state vars and objects */
    pthread_mutex_init(&bio_mutex,NULL);
    pthread_cond_init(&bio_condvar,NULL);
    bio_jobs = listCreate();
    bio_pending = zmalloc(sizeof(*bio_pending)*REDIS_BIO_MAX_OP_ID);
    for (j = 0; j < REDIS_BIO_MAX_OP_ID; j++) bio_pending[j] = 0;

    /* Set the stack size as by default it may be small in some system */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr,&stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes */
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our thread */
    if (pthread_create(&thread,&attr,bioProcessBackgroundJobs,NULL) != 0) {
        redisLog(REDIS_WARNING,"Fatal: Can't initialize Background Jobs.");
        exit(1);
    }
}

void bioCreateBackgroundJob(int type, void *data) {
    struct bio_job *job = zmalloc(sizeof(*job));

    job->type = type;
    job->data = data;
    pthread_mutex_lock(&bio_mutex);
    listAddNodeTail(bio_jobs,job);
    bio_pending[type]++;
    pthread_cond_signal(&bio_condvar);
    pthread_mutex_unlock(&bio_mutex);
}

void *bioProcessBackgroundJobs(void *arg) {
    struct bio_job *job;
    REDIS_NOTUSED(arg);

    pthread_detach(pthread_self());
    pthread_mutex_lock(&bio_mutex);
    while(1) {
        listNode *ln;
        int type;

        /* The loop always starts with the lock hold. */
        if (listLength(bio_jobs) == 0) {
            pthread_cond_wait(&bio_condvar,&bio_mutex);
            continue;
        }
        /* Pop the job from the queue. */
        ln = listFirst(bio_jobs);
        job = ln->value;
        type = job->type;
        listDelNode(bio_jobs,ln);
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
        pthread_mutex_unlock(&bio_mutex);

        /* Process the job accordingly to its type. */
        if (type == REDIS_BIO_CLOSE_FILE) {
            close((long)job->data);
        } else {
            redisPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex);
        bio_pending[type]--;
    }
}

/* Return the number of pending jobs of the specified type. */
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex);
    return val;
}

/* Wait until the number of pending jobs of the specified type are
 * less or equal to the specified number.
 *
 * This function may block for long time, it should only be used to perform
 * special tasks like AOF rewriting or alike. */
void bioWaitPendingJobsLE(int type, unsigned long long num) {
    unsigned long long iteration = 0;

    /* We poll the jobs queue aggressively to start, and gradually relax
     * the polling speed if it is going to take too much time. */
    while(1) {
        iteration++;
        if (iteration > 1000 && iteration <= 10000) {
            usleep(100);
        } else if (iteration > 10000) {
            usleep(1000);
        }
        if (bioPendingJobsOfType(type) <= num) break;
    }
}
