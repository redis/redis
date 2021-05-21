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
    size_t keys;
    size_t cow;
    monotime cow_updated;
    double progress;
    childInfoType information_type; /* Type of information */
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
void sendChildInfoGeneric(childInfoType info_type, size_t keys, double progress, char *pname) {
    if (server.child_info_pipe[1] == -1) return;
    static size_t peak_cow = 0;
    static monotime cow_updated = 0;
    static uint64_t cow_update_cost = 0;
    static size_t cow = 0;

    child_info_data data = {0}; /* zero everything, including padding to satisfy valgrind */

    /* When called to report current info, we need to throttle down CoW updates as they
     * can be very expensive. To do that, we measure the time it takes to get a reading
     * and schedule the next reading to happen not before time*CHILD_COW_COST_FACTOR
     * passes. */

    monotime now = getMonotonicUs();
    if (info_type != CHILD_INFO_TYPE_CURRENT_INFO ||
        !cow_updated ||
        now - cow_updated > cow_update_cost * CHILD_COW_DUTY_CYCLE)
    {
        cow = zmalloc_get_private_dirty(-1);
        cow_updated = getMonotonicUs();
        cow_update_cost = cow_updated - now;
        if (cow > peak_cow) peak_cow = cow;

        int cow_info = (info_type != CHILD_INFO_TYPE_CURRENT_INFO);
        if (cow) {
            serverLog(cow_info ? LL_NOTICE : LL_VERBOSE,
                      "%s: %zu MB of memory used by copy-on-write",
                      pname, (cow_info ? peak_cow : cow) / (1024 * 1024));
        }
    }

    data.information_type = info_type;
    data.keys = keys;
    data.cow = cow;
    data.cow_updated = cow_updated;
    data.progress = progress;

    ssize_t wlen = sizeof(data);

    if (write(server.child_info_pipe[1], &data, wlen) != wlen) {
        /* Nothing to do on error, this will be detected by the other side. */
    }
}

/* Update Child info. */
void updateChildInfo(childInfoType information_type, size_t cow, monotime cow_updated, size_t keys, double progress) {
    static size_t peak_cow = 0;
    if (cow > peak_cow) peak_cow = cow;

    if (information_type == CHILD_INFO_TYPE_CURRENT_INFO) {
        server.stat_current_cow_bytes = cow;
        server.stat_current_cow_updated = cow_updated;
        server.stat_current_save_keys_processed = keys;
        if (progress != -1) server.stat_module_progress = progress;
    } else if (information_type == CHILD_INFO_TYPE_AOF_COW_SIZE) {
        server.stat_aof_cow_bytes = peak_cow;
    } else if (information_type == CHILD_INFO_TYPE_RDB_COW_SIZE) {
        server.stat_rdb_cow_bytes = peak_cow;
    } else if (information_type == CHILD_INFO_TYPE_MODULE_COW_SIZE) {
        server.stat_module_cow_bytes = peak_cow;
    }

    /* Reset peak_cow. */
    if (information_type != CHILD_INFO_TYPE_CURRENT_INFO) peak_cow = 0;
}

/* Read child info data from the pipe.
 * if complete data read into the buffer, 
 * data is stored into *buffer, and returns 1.
 * otherwise, the partial data is left in the buffer, waiting for the next read, and returns 0. */
int readChildInfo(childInfoType *information_type, size_t *cow, monotime *cow_updated, size_t *keys, double* progress) {
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
        *cow = buffer.cow;
        *cow_updated = buffer.cow_updated;
        *keys = buffer.keys;
        *progress = buffer.progress;
        return 1;
    } else {
        return 0;
    }
}

/* Receive info data from child. */
void receiveChildInfo(void) {
    if (server.child_info_pipe[0] == -1) return;

    size_t cow;
    monotime cow_updated;
    size_t keys;
    double progress;
    childInfoType information_type;

    /* Drain the pipe and update child info so that we get the final message. */
    while (readChildInfo(&information_type, &cow, &cow_updated, &keys, &progress)) {
        updateChildInfo(information_type, cow, cow_updated, keys, progress);
    }
}
