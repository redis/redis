/* Background I/O service for Redis.
 *
 * This file implements operations that we need to perform in the background.
 * Currently there are 3 operations:
 * 1) a background close(2) system call.
 * 2) AOF fsync
 * 3) lazyfree of memory
 *
 * For full copyright and license info, see Redis source.
 */

#include "server.h"     /* Includes adlist.h and others */
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>

#define BIO_NUM_OPS 3 /* Number of different BIO operations */

/* Background job structure */
typedef struct bio_job {
    void *arg1, *arg2, *arg3;
    time_t time;
} bio_job;

/* Worker structure */
typedef struct BioWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condvar;
    list *jobs;
} BioWorker;

/* Array of workers, one per operation type */
static BioWorker bio_workers[BIO_NUM_OPS];

/* Forward declarations */
static void *bioProcessBackgroundJobs(void *arg);
void bioCreateBackgroundJob(int type, void *arg1, void *arg2, void *arg3);

/* Initialize background workers */
void bioInit(void) {
    for (int j = 0; j < BIO_NUM_OPS; j++) {
        bio_workers[j].jobs = listCreate();
        pthread_mutex_init(&bio_workers[j].mutex, NULL);
        pthread_cond_init(&bio_workers[j].condvar, NULL);
        pthread_create(&bio_workers[j].thread, NULL, bioProcessBackgroundJobs, (void *)(intptr_t)j);
    }
}

/* Create a background job and notify the worker */
void bioCreateBackgroundJob(int type, void *arg1, void *arg2, void *arg3) {
    if (type >= BIO_NUM_OPS) return;

    bio_job *job = zmalloc(sizeof(*job));
    job->arg1 = arg1;
    job->arg2 = arg2;
    job->arg3 = arg3;
    job->time = time(NULL);

    BioWorker *worker = &bio_workers[type];
    pthread_mutex_lock(&worker->mutex);
    listAddNodeTail(worker->jobs, job);
    pthread_cond_signal(&worker->condvar);
    pthread_mutex_unlock(&worker->mutex);
}

/* Worker thread function to process jobs */
static void *bioProcessBackgroundJobs(void *arg) {
    int type = (intptr_t)arg;
    bio_job *job;
    BioWorker *worker = &bio_workers[type];

    while (1) {
        pthread_mutex_lock(&worker->mutex);
        while (listLength(worker->jobs) == 0) {
            pthread_cond_wait(&worker->condvar, &worker->mutex);
        }

        listNode *ln = listFirst(worker->jobs);
        job = listNodeValue(ln);
        listDelNode(worker->jobs, ln);
        pthread_mutex_unlock(&worker->mutex);

        /* Actual processing depending on job type */
        switch (type) {
            case 0: // Background close job
                if (job->arg1) {
                    int fd = (intptr_t)job->arg1;
                    close(fd);
                    // printf("Closed fd %d in background\n", fd);
                }
                break;

            case 1: // AOF fsync job
                if (job->arg1) {
                    int fd = (intptr_t)job->arg1;
                    fsync(fd);
                    // printf("Fsync on fd %d done in background\n", fd);
                }
                break;

            case 2: // Lazy free job
                if (job->arg1) {
                    void (*freeFunc)(void *) = (void (*)(void *))job->arg2;
                    if (freeFunc) freeFunc(job->arg1);
                    // printf("Lazy free job done\n");
                }
                break;

            default:
                // Unknown job type
                break;
        }

        zfree(job);
    }

    return NULL;
}

/* Returns number of pending jobs for a given type */
int bioPendingJobsOfType(int type) {
    if (type >= BIO_NUM_OPS) return 0;
    BioWorker *worker = &bio_workers[type];
    pthread_mutex_lock(&worker->mutex);
    int len = listLength(worker->jobs);
    pthread_mutex_unlock(&worker->mutex);
    return len;
}

/* Create a lazy free job */
void bioCreateLazyFreeJob(void *ptr) {
    /* Use type 2 for lazyfree, arg2 can be free function */
    bioCreateBackgroundJob(2, ptr, (void *)zfree, NULL);
}

/* Create a background close job */
void bioCreateCloseJob(int fd) {
    bioCreateBackgroundJob(0, (void *)(intptr_t)fd, NULL, NULL);
}

/* Create a background fsync job */
void bioCreateFsyncJob(int fd) {
    bioCreateBackgroundJob(1, (void *)(intptr_t)fd, NULL, NULL);
}

/* Create a background close job for AOF file */
void bioCreateCloseAofJob(int fd) {
    bioCreateBackgroundJob(0, (void *)(intptr_t)fd, NULL, NULL);
}

/* Wait for all jobs in a worker to finish */
void bioDrainWorker(int type) {
    if (type >= BIO_NUM_OPS) return;
    BioWorker *worker = &bio_workers[type];
    while (1) {
        pthread_mutex_lock(&worker->mutex);
        int pending = listLength(worker->jobs);
        pthread_mutex_unlock(&worker->mutex);
        if (pending == 0) break;
        sched_yield(); /* Yield CPU briefly */
    }
}

/* Placeholder to stop all worker threads gracefully if needed */
void bioKillThreads(void) {
    /* Implement if Redis wants to terminate threads cleanly */
}

/* Create a completion request job (stub example) */
void bioCreateCompRq(void *comp) {
    /* For now, treat as lazyfree job */
    bioCreateBackgroundJob(2, comp, (void *)zfree, NULL);
}