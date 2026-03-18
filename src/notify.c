/*
 * Copyright (c) 2013-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"

/* This file implements keyspace events notification via Pub/Sub and
 * described at https://redis.io/docs/latest/develop/use/keyspace-notifications/. */

/* Turn a string representing notification classes into an integer
 * representing notification classes flags xored.
 *
 * The function returns -1 if the input contains characters not mapping to
 * any class. */
int keyspaceEventsStringToFlags(char *classes) {
    char *p = classes;
    int c, flags = 0;

    while((c = *p++) != '\0') {
        switch(c) {
        case 'A': flags |= NOTIFY_ALL; break;
        case 'g': flags |= NOTIFY_GENERIC; break;
        case '$': flags |= NOTIFY_STRING; break;
        case 'l': flags |= NOTIFY_LIST; break;
        case 's': flags |= NOTIFY_SET; break;
        case 'h': flags |= NOTIFY_HASH; break;
        case 'z': flags |= NOTIFY_ZSET; break;
        case 'x': flags |= NOTIFY_EXPIRED; break;
        case 'e': flags |= NOTIFY_EVICTED; break;
        case 'K': flags |= NOTIFY_KEYSPACE; break;
        case 'E': flags |= NOTIFY_KEYEVENT; break;
        case 't': flags |= NOTIFY_STREAM; break;
        case 'm': flags |= NOTIFY_KEY_MISS; break;
        case 'd': flags |= NOTIFY_MODULE; break;
        case 'n': flags |= NOTIFY_NEW; break;
        case 'o': flags |= NOTIFY_OVERWRITTEN; break;
        case 'c': flags |= NOTIFY_TYPE_CHANGED; break;
        case 'S': flags |= NOTIFY_SUBKEYSPACE; break;
        case 'T': flags |= NOTIFY_SUBKEYEVENT; break;
        default: return -1;
        }
    }
    return flags;
}

/* This function does exactly the reverse of the function above: it gets
 * as input an integer with the xored flags and returns a string representing
 * the selected classes. The string returned is an sds string that needs to
 * be released with sdsfree(). */
sds keyspaceEventsFlagsToString(int flags) {
    sds res;

    res = sdsempty();
    if ((flags & NOTIFY_ALL) == NOTIFY_ALL) {
        res = sdscatlen(res,"A",1);
    } else {
        if (flags & NOTIFY_GENERIC) res = sdscatlen(res,"g",1);
        if (flags & NOTIFY_STRING) res = sdscatlen(res,"$",1);
        if (flags & NOTIFY_LIST) res = sdscatlen(res,"l",1);
        if (flags & NOTIFY_SET) res = sdscatlen(res,"s",1);
        if (flags & NOTIFY_HASH) res = sdscatlen(res,"h",1);
        if (flags & NOTIFY_ZSET) res = sdscatlen(res,"z",1);
        if (flags & NOTIFY_EXPIRED) res = sdscatlen(res,"x",1);
        if (flags & NOTIFY_EVICTED) res = sdscatlen(res,"e",1);
        if (flags & NOTIFY_STREAM) res = sdscatlen(res,"t",1);
        if (flags & NOTIFY_MODULE) res = sdscatlen(res,"d",1);
        if (flags & NOTIFY_NEW) res = sdscatlen(res,"n",1);
        if (flags & NOTIFY_OVERWRITTEN) res = sdscatlen(res,"o",1);
        if (flags & NOTIFY_TYPE_CHANGED) res = sdscatlen(res,"c",1);
    }
    if (flags & NOTIFY_KEYSPACE) res = sdscatlen(res,"K",1);
    if (flags & NOTIFY_KEYEVENT) res = sdscatlen(res,"E",1);
    if (flags & NOTIFY_KEY_MISS) res = sdscatlen(res,"m",1);
    if (flags & NOTIFY_SUBKEYSPACE) res = sdscatlen(res,"S",1);
    if (flags & NOTIFY_SUBKEYEVENT) res = sdscatlen(res,"T",1);
    return res;
}

/* Build the subkeys payload with length-prefix format.
 * Format: <len>:<subkey>[,<len>:<subkey>...]
 * Example: 3:abc,2:xx,5:hello */
static sds buildSubkeysPayload(robj **subkeys, int numsubkeys) {
    sds payload = sdsempty();
    char lenbuf[32];

    for (int i = 0; i < numsubkeys; i++) {
        if (i > 0) payload = sdscatlen(payload, ",", 1);
        size_t subkeylen = sdslen(subkeys[i]->ptr);
        int lenlen = ll2string(lenbuf, sizeof(lenbuf), subkeylen);
        payload = sdscatlen(payload, lenbuf, lenlen);
        payload = sdscatlen(payload, ":", 1);
        payload = sdscatsds(payload, subkeys[i]->ptr);
    }
    return payload;
}

/* Internal implementation for keyspace event notifications.
 *
 * The API provided to the rest of the Redis core is a simple function:
 *
 * notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
 *
 * 'type' is the notification class we define in `server.h`.
 * 'event' is a C string representing the event name.
 * 'key' is a Redis object representing the key name.
 * 'subkeys' is an array of Redis objects representing the subkey names (can be NULL).
 * 'numsubkeys' is the number of subkeys in the array.
 * 'dbid' is the database ID where the key lives.
 *
 * For subkey notifications:
 * - __subkeyspace@<db>__:<key> with payload: <event>|<len>:<subkey>[,<len>:<subkey>...]
 * - __subkeyevent@<db>__:<event> with payload: <key_len>:<key>|<len>:<subkey>[,<len>:<subkey>...]
 */
static void notifyKeyspaceEventImpl(int type, const char *event, robj *key,
                                    robj **subkeys, int numsubkeys, int dbid) {
    sds chan;
    robj *chanobj, *eventobj;
    int len = -1;
    char buf[24];

    /* If any modules are interested in events, notify the module system now.
     * This bypasses the notifications configuration, but the module engine
     * will only call event subscribers if the event type matches the types
     * they are interested in. */
    moduleNotifyKeyspaceEvent(type, event, key, dbid);

    /* If notifications for this class of events are off, return ASAP. */
    if (!(server.notify_keyspace_events & type)) return;

    eventobj = createStringObject(event, strlen(event));

    /* __keyspace@<db>__:<key> <event> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYSPACE) {
        chan = sdsnewlen("__keyspace@", 11);
        len = ll2string(buf, sizeof(buf), dbid);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, key->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, eventobj, 0);
        decrRefCount(chanobj);
    }

    /* __keyevent@<db>__:<event> <key> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYEVENT) {
        chan = sdsnewlen("__keyevent@", 11);
        if (len == -1) len = ll2string(buf, sizeof(buf), dbid);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, eventobj->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, key, 0);
        decrRefCount(chanobj);
    }

    /* Subkey-level notifications (only when subkeys are provided). */
    if (subkeys != NULL && numsubkeys > 0) {
        sds subkeysPayload = buildSubkeysPayload(subkeys, numsubkeys);

        /* __subkeyspace@<db>__:<key> <event>|<len>:<subkey>[,...] notifications. */
        if (server.notify_keyspace_events & NOTIFY_SUBKEYSPACE) {
            chan = sdsnewlen("__subkeyspace@", 14);
            if (len == -1) len = ll2string(buf, sizeof(buf), dbid);
            chan = sdscatlen(chan, buf, len);
            chan = sdscatlen(chan, "__:", 3);
            chan = sdscatsds(chan, key->ptr);
            chanobj = createObject(OBJ_STRING, chan);

            /* Build payload: <event>|<subkeys_payload> */
            sds payload = sdscatsds(sdsdup(eventobj->ptr), sdsnewlen("|", 1));
            payload = sdscatsds(payload, subkeysPayload);
            robj *payloadobj = createObject(OBJ_STRING, payload);
            pubsubPublishMessage(chanobj, payloadobj, 0);
            decrRefCount(chanobj);
            decrRefCount(payloadobj);
        }

        /* __subkeyevent@<db>__:<event> <key_len>:<key>|<len>:<subkey>[,...] notifications. */
        if (server.notify_keyspace_events & NOTIFY_SUBKEYEVENT) {
            chan = sdsnewlen("__subkeyevent@", 14);
            if (len == -1) len = ll2string(buf, sizeof(buf), dbid);
            chan = sdscatlen(chan, buf, len);
            chan = sdscatlen(chan, "__:", 3);
            chan = sdscatsds(chan, eventobj->ptr);
            chanobj = createObject(OBJ_STRING, chan);

            /* Build payload: <key_len>:<key>|<subkeys_payload> */
            size_t keylen = sdslen(key->ptr);
            char keylenbuf[32];
            int keylenlen = ll2string(keylenbuf, sizeof(keylenbuf), keylen);
            sds payload = sdsnewlen(keylenbuf, keylenlen);
            payload = sdscatlen(payload, ":", 1);
            payload = sdscatsds(payload, key->ptr);
            payload = sdscatlen(payload, "|", 1);
            payload = sdscatsds(payload, subkeysPayload);
            robj *payloadobj = createObject(OBJ_STRING, payload);
            pubsubPublishMessage(chanobj, payloadobj, 0);
            decrRefCount(chanobj);
            decrRefCount(payloadobj);
        }

        sdsfree(subkeysPayload);
    }

    decrRefCount(eventobj);
}

/* Public API for key-level notifications (backward compatible). */
void notifyKeyspaceEvent(int type, const char *event, robj *key, int dbid) {
    notifyKeyspaceEventImpl(type, event, key, NULL, 0, dbid);
}

/* Public API for notifications with subkeys (key-level + subkey-level). */
void notifyKeyspaceEventWithSubkeys(int type, const char *event, robj *key,
                                    robj **subkeys, int numsubkeys, int dbid) {
    notifyKeyspaceEventImpl(type, event, key, subkeys, numsubkeys, dbid);
}

/* Check if subkey-level notifications are enabled for the given event type. */
int isSubkeyNotifyEnabled(int type) {
    return (server.notify_keyspace_events & type) &&
           (server.notify_keyspace_events & (NOTIFY_SUBKEYSPACE | NOTIFY_SUBKEYEVENT));
}
