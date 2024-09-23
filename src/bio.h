/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef __BIO_H
#define __BIO_H

typedef void lazy_free_fn(void *args[]);
typedef void comp_fn(uint64_t user_data, void *user_ptr);

typedef enum bio_worker_t {
    BIO_WORKER_CLOSE_FILE = 0,
    BIO_WORKER_AOF_FSYNC,
    BIO_WORKER_LAZY_FREE,
    BIO_WORKER_NUM
} bio_worker_t;

/* Background job opcodes */
typedef enum bio_job_type_t {
    BIO_CLOSE_FILE = 0,     /* Deferred close(2) syscall. */
    BIO_AOF_FSYNC,          /* Deferred AOF fsync. */
    BIO_LAZY_FREE,          /* Deferred objects freeing. */
    BIO_CLOSE_AOF,
    BIO_COMP_RQ_CLOSE_FILE,  /* Job completion request, registered on close-file worker's queue */
    BIO_COMP_RQ_AOF_FSYNC,  /* Job completion request, registered on aof-fsync worker's queue */
    BIO_COMP_RQ_LAZY_FREE,  /* Job completion request, registered on lazy-free worker's queue */
    BIO_NUM_OPS
} bio_job_type_t;

/* Exported API */
void bioInit(void);
unsigned long bioPendingJobsOfType(int type);
void bioDrainWorker(int job_type);
void bioKillThreads(void);
void bioCreateCloseJob(int fd, int need_fsync, int need_reclaim_cache);
void bioCreateCloseAofJob(int fd, long long offset, int need_reclaim_cache);
void bioCreateFsyncJob(int fd, long long offset, int need_reclaim_cache);
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...);
void bioCreateCompRq(bio_worker_t assigned_worker, comp_fn *func, uint64_t user_data, void *user_ptr);


#endif
