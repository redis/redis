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

/* Exported API */
void bioInit(void);
unsigned long bioPendingJobsOfType(int type);
void bioDrainWorker(int job_type);
void bioKillThreads(void);
void bioCreateCloseJob(int fd, int need_fsync, int need_reclaim_cache);
void bioCreateCloseAofJob(int fd, long long offset, int need_reclaim_cache);
void bioCreateFsyncJob(int fd, long long offset, int need_reclaim_cache);
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...);

/* Background job opcodes */
enum {
    BIO_CLOSE_FILE = 0, /* Deferred close(2) syscall. */
    BIO_AOF_FSYNC,      /* Deferred AOF fsync. */
    BIO_LAZY_FREE,      /* Deferred objects freeing. */
    BIO_CLOSE_AOF,      /* Deferred close for AOF files. */
    BIO_NUM_OPS
};

#endif
