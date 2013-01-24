/*
 * Copyright (c) 2013, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "redis.h"

/* This file implements keyspace events notification via Pub/Sub ad
 * described at http://redis.io/topics/keyspace-events.
 *
 * The API provided to the rest of the Redis core is a simple function:
 *
 * notifyKeyspaceEvent(char *event, robj *key, int dbid);
 *
 * 'event' is a C string representing the event name.
 * 'key' is a Redis object representing the key name.
 * 'dbid' is the database ID where the key lives.
 */

void notifyKeyspaceEvent(char *event, robj *key, int dbid) {
    sds keyspace_chan, keyevent_chan;
    int len;
    char buf[24];
    robj *chan1, *chan2, *eventobj;

    if (!server.notify_keyspace_events) return;

    /* The prefix of the two channels is identical if not for
     * 'keyspace' that is 'keyevent' in the event channel name, so
     * we build a single prefix and overwrite 'event' with 'space'. */
    keyspace_chan = sdsnewlen("__keyspace@",11);
    len = ll2string(buf,sizeof(buf),dbid);
    keyspace_chan = sdscatlen(keyspace_chan, buf, len);
    keyspace_chan = sdscatlen(keyspace_chan, "__:", 3);
    keyevent_chan = sdsdup(keyspace_chan); /* Dup the prefix. */
    memcpy(keyevent_chan+5,"event",5); /* Fix it. */

    eventobj = createStringObject(event,strlen(event));

    /* The keyspace channel name has a trailing key name, while
     * the keyevent channel name has a trailing event name. */
    keyspace_chan = sdscatsds(keyspace_chan, key->ptr);
    keyevent_chan = sdscatsds(keyevent_chan, eventobj->ptr);
    chan1 = createObject(REDIS_STRING, keyspace_chan);
    chan2 = createObject(REDIS_STRING, keyevent_chan);

    /* Finally publish the two notifications. */
    pubsubPublishMessage(chan1, eventobj);
    pubsubPublishMessage(chan2, key);

    /* Release objects. */
    decrRefCount(eventobj);
    decrRefCount(chan1);
    decrRefCount(chan2);
}
