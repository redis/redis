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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "queue.h"
#include "zmalloc.h"

queue *queueCreate(void) {
    struct queue *queue;

    if ((queue = zmalloc(sizeof(*queue))) == NULL)
        return NULL;

    queue->len = 0;
    queue->startidx = 0;
    queue->head = NULL;
    queue->tail = NULL;
    queue->free = NULL;
    return queue;
}

queueEntry *queueAdd(queue *q, void *value) {
    queueEntry *entry = NULL;

    if ((entry = zmalloc(sizeof(*entry))) == NULL)
        return NULL;

    entry->value = value;
    entry->next = NULL;

    if (q->len == 0) {
        q->head = q->tail = entry;
    } else {
        q->tail->next = entry;
        q->tail = entry;
    }

    q->len++;

    return entry;
}

queueEntry *queueIndex(queue *q, long long index) {
    long long len = 0;
    if (index == q->len - 1 || index == -1) return q->tail;
    if (index == q->startidx || index == 0) return q->head;

    if (index < 0) len = (q->len - q->startidx) + index;
    else len = index - q->startidx;

    if (len < 0) return q->head;

    queueEntry *e = q->head;
    
    while (len-- && e) {
        e = e->next;
    }

    return e;
}

void queueRelease(queue *q) {
    queueEntry *current, *next;
    current = q->head;

    while (current) {
        next = current->next;

        if (q->free) q->free(current->value);
        zfree(current);

        current = next;
    }

    zfree(q);
}