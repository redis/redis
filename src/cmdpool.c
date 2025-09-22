/* cmdpool.c - Client-specific command pool for parsedCommand structures
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "zmalloc.h"
#include <string.h>

/* Initialize a client command queue with pool */
void cmdQueueInit(cmdQueue *queue) {
    if (!queue) return;
    
    queue->cmds = listCreate();
    queue->pool_size = 0;
    
    /* Initialize pool array to NULL */
    for (int i = 0; i < 16; i++) {
        queue->pool[i] = NULL;
    }
}

/* Cleanup a client command queue and its pool */
void cmdQueueCleanup(cmdQueue *queue) {
    if (!queue) return;
    
    /* Free all commands in the queue */
    if (queue->cmds) {
        listIter iter;
        listNode *node;
        listRewind(queue->cmds, &iter);
        while ((node = listNext(&iter)) != NULL) {
            parsedCommand *cmd = listNodeValue(node);
            if (cmd->argv) {
                for (int j = 0; j < cmd->argc; j++) {
                    decrRefCount(cmd->argv[j]);
                }
                zfree(cmd->argv);
            }
            zfree(cmd);
        }
        listRelease(queue->cmds);
    }
    
    /* Free all commands in the pool */
    for (int i = 0; i < queue->pool_size; i++) {
        if (queue->pool[i]) {
            if (queue->pool[i]->argv) {
                zfree(queue->pool[i]->argv);
            }
            zfree(queue->pool[i]);
        }
    }
}

/* Get a parsedCommand from the client's pool */
parsedCommand *cmdQueueGetCommand(cmdQueue *queue) {
    if (!queue) return NULL;
    
    parsedCommand *cmd = NULL;
    
    if (queue->pool_size > 0) {
        /* Get from pool */
        cmd = queue->pool[--queue->pool_size];
        queue->pool[queue->pool_size] = NULL;

        robj **argv = cmd->argv;
        int argv_len = cmd->argv_len;
        memset(cmd, 0, sizeof(parsedCommand));
        cmd->argv = argv;
        cmd->argv_len = argv_len;
    } else {
        /* Pool is empty, allocate new */
        cmd = zcalloc(sizeof(parsedCommand));
    }
    
    return cmd;
}

/* Return a parsedCommand to the client's pool */
void cmdQueuePutCommand(cmdQueue *queue, parsedCommand *cmd) {
    for (int j = 0; j < cmd->argc; j++)
        decrRefCount(cmd->argv[j]);

    /* If pool is not full, add to pool */
    if (queue->pool_size < 16) {
        queue->pool[queue->pool_size++] = cmd;
    } else {
        if (cmd->argv) {
            zfree(cmd->argv);
            cmd->argv = NULL;
        }

        /* Pool is full, free the command */
        zfree(cmd);
    }
}
