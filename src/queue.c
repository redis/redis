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
    queue->head = NULL;
    queue->free = NULL;
    return queue;
}

static queueNode *queueNodeCreate(void) {
    struct queueNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->len = 0;
    node->startidx = 0;
    node->head = node->tail = NULL;
    node->next = NULL;
    return node;
}

queueEntry *queueAdd(queue *q, void *value) {
    queueNode *headNode, *nextNode;
    headNode = q->head;

    if (headNode == NULL || headNode->len > QUEUE_NODE_MAX_SIZE) {
        nextNode = headNode;
        if ((headNode = queueNodeCreate()) == NULL) {
            return NULL;
        }

        q->head = headNode;
        headNode->next = nextNode;
        headNode->startidx = q->len;
    }

    queueEntry *entry = NULL;

    if ((entry = zmalloc(sizeof(*entry))) == NULL)
        return NULL;

    entry->value = value;

    if (headNode->len == 0) {
        headNode->head = headNode->tail = entry;
    } else {
        headNode->tail->next = entry;
        headNode->tail = entry;
    }

    headNode->len++;
    q->len++;

    return entry;
}

queueEntry *queueFind(queue *q, long long index) {
    long long len;
    queueNode *node, *nextNode;
    queueEntry *entry, *nextEntry;

    node = q->head;
    
    while(node) {
        if (index >= node->startidx) {
            if (index + 1 == node->startidx + node->len) {
                return node->tail;
            } else if (index + 1 > node->startidx + node->len) {
                return NULL;
            } else {
                len = index - node->startidx;
                entry = node->head;
                while (len--) {
                    nextEntry = entry->next;
                    entry = nextEntry;
                }

                return entry;
            }
        }

        nextNode = node->next;
        node = nextNode;
    }

    return NULL;
}

void queueRelease(queue *q) {
    queueNode *node, *next;
    node = q->head;

    while(node) {
        next = node->next;
        queueNodeRelease(node, q);
        node = next;
    }

    zfree(q);
}

void queueNodeRelease(queueNode *n, queue *q) {
    queueEntry *entry, *next;
    entry = n->head;

    while(entry) {
        next = entry->next;
        queueEntryRelease(entry, q);
        entry = next;
    }

    zfree(n);
}

void queueEntryRelease(queueEntry *e, queue *q) {
    if (q->free) q->free(e->value);
    zfree(e);
}
