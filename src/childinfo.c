/*
 * Copyright (c) 2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include <unistd.h>

typedef struct {
    int information_type;            /* Type of information */
    size_t info;               /* Information about the process. */
} child_info_data;

/* Open a child-parent channel used in order to move information about the
 * RDB / AOF saving process from the child to the parent (for instance
 * the amount of copy on write memory used) */
void openChildInfoPipe(void) {
    if (pipe(server.child_info_pipe) == -1) {
        /* On error our two file descriptors should be still set to -1,
         * but we call anyway closeChildInfoPipe() since can't hurt. */
        closeChildInfoPipe();
    } else if (anetNonBlock(NULL,server.child_info_pipe[0]) != ANET_OK) {
        closeChildInfoPipe();
    } else {
        server.child_info_nread = 0;
    }
}

/* Close the pipes opened with openChildInfoPipe(). */
void closeChildInfoPipe(void) {
    if (server.child_info_pipe[0] != -1 ||
        server.child_info_pipe[1] != -1)
    {
        close(server.child_info_pipe[0]);
        close(server.child_info_pipe[1]);
        server.child_info_pipe[0] = -1;
        server.child_info_pipe[1] = -1;
        server.child_info_nread = 0;
    }
}

/* Send save data to parent. */
void sendChildInfo(int information_type, size_t info) {
    if (server.child_info_pipe[1] == -1) return;

    child_info_data buffer = {.information_type = information_type, .info = info};
    ssize_t wlen = sizeof(buffer);

    if (write(server.child_info_pipe[1],&buffer,wlen) != wlen) {
        /* Nothing to do on error, this will be detected by the other side. */
    }
}

/* Update Child info. */
void updateChildInfo(int information_type, size_t info) {
    if (information_type == CHILD_INFO_TYPE_CURRENT_COW_SIZE) {
        server.stat_current_cow_bytes = info;
    } else if (information_type == CHILD_INFO_TYPE_CURRENT_KEYS_PROCESSED) {
        server.stat_current_processed_keys = info;
    } else if (information_type == CHILD_INFO_TYPE_AOF_COW_SIZE) {
        server.stat_aof_cow_bytes = info;
    } else if (information_type == CHILD_INFO_TYPE_RDB_COW_SIZE) {
        server.stat_rdb_cow_bytes = info;
    } else if (information_type == CHILD_INFO_TYPE_MODULE_COW_SIZE) {
        server.stat_module_cow_bytes = info;
    }
}

/* Read child info data from the pipe.
 * if complete data read into the buffer, process type, copy-on-write type and copy-on-write size
 * are stored into *information_type, and *info respectively and returns 1.
 * otherwise, the partial data is left in the buffer, waiting for the next read, and returns 0. */
int readChildInfo(int *information_type, size_t *info) {
    /* We are using here a static buffer in combination with the server.child_info_nread to handle short reads */
    static child_info_data buffer;
    ssize_t wlen = sizeof(buffer);

    /* Do not overlap */
    if (server.child_info_nread == wlen) server.child_info_nread = 0;

    int nread = read(server.child_info_pipe[0], (char *)&buffer + server.child_info_nread, wlen - server.child_info_nread);
    if (nread > 0) {
        server.child_info_nread += nread;
    }

    /* We have complete child info */
    if (server.child_info_nread == wlen) {
        *information_type = buffer.information_type;
        *info = buffer.info;
        return 1;
    } else {
        return 0;
    }
}

/* Receive info data from child. */
void receiveChildInfo(void) {
    if (server.child_info_pipe[0] == -1) return;

    int information_type;
    size_t info;

    /* Drain the pipe and update child info so that we get the final message. */
    while (readChildInfo(&information_type, &info)) {
        updateChildInfo(information_type, info);
    }
}
