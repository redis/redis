/*
 * Copyright (c) 2014-2015, pythias <pythias at gmail dot com>
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

#ifndef __QUEUE_H
#define __QUEUE_H
#include <stdint.h>

/*Queue, QueueNode, QueueEntry data structures*/

typedef struct queueEntry {
    void *value;
    struct queueEntry *next;
} queueEntry;

typedef struct queue {
    long long len;
    long long startidx;
    queueEntry *head;
    queueEntry *tail;
    void (*free)(void *ptr); 
} queue;

/* Macros */
#define queueLength(q) ((q)->len)
#define queueHead(q) ((q)->head)
#define queueTail(q) ((q)->tail)
#define queueSetFreeMethod(q, m) ((q)->free = (m))
#define queueStartIndex(q) ((q)->startidx)
#define queueEntryValue(e) ((e)->value)
#define queueEntryNext(e) ((e)->next)

/* API */
queue *queueCreate(void);
queueEntry *queueAdd(queue *q, void *value);
queueEntry *queueIndex(queue *q, long long index);

void queueRelease(queue *q);

#define QUEUE_SEND_MAX_SIZE 1000

#endif // __INTSET_H
