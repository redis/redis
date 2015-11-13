/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "..\redis.h"
#include "Win32_Portability.h"

void SetupRedisGlobals(LPVOID redisData, size_t redisDataSize, uint32_t dictHashSeed)
{
#ifndef NO_QFORKIMPL
    memcpy(&server, redisData, redisDataSize);
    dictSetHashFunctionSeed(dictHashSeed);
#endif
}

int do_rdbSave(char* filename)
{
#ifndef NO_QFORKIMPL
    server.rdb_child_pid = GetCurrentProcessId();
    if( rdbSave(filename) != REDIS_OK ) {
        redisLog(REDIS_WARNING,"rdbSave failed in qfork: %s", strerror(errno));
        return REDIS_ERR;
    }
#endif
    return REDIS_OK;
}

int do_aofSave(char* filename, int aof_pipe_read_ack, int aof_pipe_read_data, int aof_pipe_write_ack)
{
#ifndef NO_QFORKIMPL
    int rewriteAppendOnlyFile(char *filename);

    server.aof_child_pid = GetCurrentProcessId();
    server.aof_pipe_write_ack_to_parent = aof_pipe_write_ack;
    server.aof_pipe_read_ack_from_parent = aof_pipe_read_ack;
    server.aof_pipe_read_data_from_parent = aof_pipe_read_data;
    server.aof_pipe_read_ack_from_child = -1;
    server.aof_pipe_write_ack_to_child = -1;
    server.aof_pipe_write_data_to_child = -1;
    if (rewriteAppendOnlyFile(filename) != REDIS_OK) {
        redisLog(REDIS_WARNING, "rewriteAppendOnlyFile failed in qfork: %s", strerror(errno));
        return REDIS_ERR;
    }
#endif
    return REDIS_OK;
}

int rdbSaveRioWithEOFMark(rio *rdb, int *error);

// This function is meant to be an exact replica of the fork() child path in rdbSaveToSlavesSockets
int do_rdbSaveToSlavesSockets(int *fds, int numfds, uint64_t *clientids)
{
#ifndef NO_QFORKIMPL
    int retval;
    rio slave_sockets;

    server.rdb_child_pid = GetCurrentProcessId();

    rioInitWithFdset(&slave_sockets,fds,numfds);
    // On Windows we need to use the fds after do_socketSave2 has finished
    // so we don't free them here, moreover since we allocate the fds in
    // QFork.cpp it's better to use malloc instead of zmalloc.
    POSIX_ONLY(zfree(fds););

    // On Windows we haven't duplicated the listening sockets so we shouldn't close them
    POSIX_ONLY(closeListeningSockets(0);)

    redisSetProcTitle("redis-rdb-to-slaves");
    
    retval = rdbSaveRioWithEOFMark(&slave_sockets,NULL);
    if (retval == REDIS_OK && rioFlush(&slave_sockets) == 0)
        retval = REDIS_ERR;
    
    if (retval == REDIS_OK) {
        size_t private_dirty = zmalloc_get_private_dirty();
    
        if (private_dirty) {
            redisLog(REDIS_NOTICE,
                "RDB: %Iu MB of memory used by copy-on-write",                  WIN_PORT_FIX /* %zu -> %Iu */
                private_dirty/(1024*1024));
        }
    
        /* If we are returning OK, at least one slave was served
         * with the RDB file as expected, so we need to send a report
         * to the parent via the pipe. The format of the message is:
         *
         * <len> <slave[0].id> <slave[0].error> ...
         *
         * len, slave IDs, and slave errors, are all uint64_t integers,
         * so basically the reply is composed of 64 bits for the len field
         * plus 2 additional 64 bit integers for each entry, for a total
         * of 'len' entries.
         *
         * The 'id' represents the slave's client ID, so that the master
         * can match the report with a specific slave, and 'error' is
         * set to 0 if the replication process terminated with a success
         * or the error code if an error occurred. */
        void *msg = zmalloc(sizeof(uint64_t)*(1+2*numfds));
        uint64_t *len = msg;
        uint64_t *ids = len+1;
        int j, msglen;
    
        *len = numfds;
        for (j = 0; j < numfds; j++) {
            *ids++ = clientids[j];
            *ids++ = slave_sockets.io.fdset.state[j];
        }
    
        /* Write the message to the parent. If we have no good slaves or
         * we are unable to transfer the message to the parent, we exit
         * with an error so that the parent will abort the replication
         * process with all the childre that were waiting. */
        msglen = sizeof(uint64_t)*(1+2*numfds);
        if (*len == 0 ||
            write(server.rdb_pipe_write_result_to_parent,msg,msglen)
            != msglen)
        {
            retval = REDIS_ERR;
        }
    }
    return retval;
#endif
    return REDIS_OK;
}

int do_socketSave(int *fds, int numfds, uint64_t *clientids, int pipe_write_fd)
{
    server.rdb_pipe_write_result_to_parent = pipe_write_fd;
    return do_rdbSaveToSlavesSockets(fds, numfds, clientids);
}
