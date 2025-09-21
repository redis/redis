/* cmdpool.h - Object pool for parsedCommand structures
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __CMDPOOL_H__
#define __CMDPOOL_H__

#include "server.h"

/* Default pool configuration */
#define CMDPOOL_DEFAULT_INITIAL_SIZE 64
#define CMDPOOL_DEFAULT_MAX_SIZE 1024
#define CMDPOOL_DEFAULT_GROW_SIZE 32

/* Command pool structure */
typedef struct cmdPool {
    parsedCommand **pool;       /* Array of available parsedCommand pointers */
    int size;                   /* Current pool size */
    int capacity;               /* Maximum pool capacity */
    int max_size;               /* Maximum allowed pool size */
    int grow_size;              /* Number of objects to allocate when growing */
    
    /* Statistics */
    long long allocations;      /* Total allocations made */
    long long deallocations;    /* Total deallocations made */
    long long pool_hits;        /* Number of times pool provided an object */
    long long pool_misses;      /* Number of times pool was empty */
} cmdPool;

/* Global command pool instance */
extern cmdPool *global_cmd_pool;

/* Function prototypes */
cmdPool *cmdPoolCreate(int initial_size, int max_size, int grow_size);
void cmdPoolDestroy(cmdPool *pool);
parsedCommand *cmdPoolGet(cmdPool *pool);
void cmdPoolPut(cmdPool *pool, parsedCommand *cmd);
void cmdPoolShrink(cmdPool *pool);

/* Initialize and cleanup global pool */
void cmdPoolGlobalInit(void);
void cmdPoolGlobalCleanup(void);

#endif /* __CMDPOOL_H__ */
