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
list *bio_jobs;

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

    pthread_mutex_init(&bio_mutex,NULL);
    pthread_cond_init(&bio_condvar,NULL);
    bio_jobs = listCreate();

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

        /* The loop always starts with the lock hold. */
        if (listLength(bio_jobs) == 0) {
            pthread_cond_wait(&bio_condvar,&bio_mutex);
            continue;
        }
        /* Pop the job from the queue. */
        ln = listFirst(bio_jobs);
        job = ln->value;
        listDelNode(bio_jobs,ln);
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
        pthread_mutex_unlock(&bio_mutex);

        /* Process the job accordingly to its type. */
        if (job->type == REDIS_BIO_CLOSE_FILE) {
            close((long)job->data);
        } else {
            redisPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex);
    }
}
