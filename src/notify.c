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
        case 'r': flags |= NOTIFY_RATE_LIMIT; break;
        case 'S': flags |= NOTIFY_SUBKEYSPACE; break;
        case 'T': flags |= NOTIFY_SUBKEYEVENT; break;
        case 'I': flags |= NOTIFY_SUBKEYSPACEITEM; break;
        case 'V': flags |= NOTIFY_SUBKEYSPACEEVENT; break;
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
        if (flags & NOTIFY_RATE_LIMIT) res = sdscatlen(res,"r",1);
    }
    if (flags & NOTIFY_KEYSPACE) res = sdscatlen(res,"K",1);
    if (flags & NOTIFY_KEYEVENT) res = sdscatlen(res,"E",1);
    if (flags & NOTIFY_KEY_MISS) res = sdscatlen(res,"m",1);
    if (flags & NOTIFY_SUBKEYSPACE) res = sdscatlen(res,"S",1);
    if (flags & NOTIFY_SUBKEYEVENT) res = sdscatlen(res,"T",1);
    if (flags & NOTIFY_SUBKEYSPACEITEM) res = sdscatlen(res,"I",1);
    if (flags & NOTIFY_SUBKEYSPACEEVENT) res = sdscatlen(res,"V",1);
    return res;
}

/* Append subkeys in length-prefixed format to 'dst'.
 * If 'dst' is NULL, a new sds is created.
 * Format: <len>:<subkey>[,<len>:<subkey>...]
 * Example: 3:abc,2:xx,5:hello */
static sds catSubkeysPayload(sds dst, robj **subkeys, int count) {
    if (dst == NULL) dst = sdsempty();
    char lenbuf[32];

    for (int i = 0; i < count; i++) {
        serverAssert(sdsEncodedObject(subkeys[i]));
        if (i > 0) dst = sdscatlen(dst, ",", 1);
        size_t subkeylen = sdslen(subkeys[i]->ptr);
        int lenlen = ll2string(lenbuf, sizeof(lenbuf), subkeylen);
        dst = sdscatlen(dst, lenbuf, lenlen);
        dst = sdscatlen(dst, ":", 1);
        dst = sdscatsds(dst, subkeys[i]->ptr);
    }
    return dst;
}

/* Internal implementation for keyspace event notifications.
 *
 * The API provided to the rest of the Redis core is:
 *
 * notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
 * notifyKeyspaceEventWithSubkeys(int type, char *event, robj *key, int dbid,
 *                                robj **subkeys, int count);
 *
 * 'type' is the notification class we define in `server.h`.
 * 'event' is a C string representing the event name.
 * 'key' is a Redis object representing the key name.
 * 'dbid' is the database ID where the key lives.
 * 'subkeys' is an array of Redis objects representing the subkey names (can be NULL).
 * 'count' is the number of subkeys in the array.
 *
 * For subkey notifications (4 channel types):
 * - __subkeyspace@<db>__:<key>                  payload: <event>|<subkeys>
 * - __subkeyevent@<db>__:<event>                payload: <key_len>:<key>|<subkeys>
 * - __subkeyspaceitem@<db>__:<key>\n<subkey>    payload: <event>
 * - __subkeyspaceevent@<db>__:<event>|<key>     payload: <subkeys>
 *
 * Where <subkeys> is in length-prefixed format: <len>:<subkey>[,<len>:<subkey>...]
 * Example: 3:foo,5:hello
 *
 * NOTE: This function may invoke module notification callbacks, which may
 * cause the key's kvobj to be reallocated. */
static void notifyKeyspaceEventImpl(int type, const char *event, robj *key, int dbid,
                                    robj **subkeys, int count)
{
    sds chan;
    robj *chanobj, *eventobj;
    char buf[24];
    serverAssert(sdsEncodedObject(key));

    /* If any modules are interested in events, notify the module system now.
     * This bypasses the notifications configuration, but the module engine
     * will only call event subscribers if the event type matches the types
     * they are interested in. Subkeys are passed through so that subscribers
     * with a subkey callback receive them. */
    moduleNotifyKeyspaceEvent(type, event, key, dbid, subkeys, count);

    /* If notifications for this class of events are off, return ASAP. */
    if (!(server.notify_keyspace_events & type)) return;

    /* If there are no Pub/Sub subscribers (neither pattern nor channel),
     * skip the remaining notification work since nobody would receive it. */
    if (dictSize(server.pubsub_patterns) == 0 && kvstoreSize(server.pubsub_channels) == 0)
        return;

    eventobj = createStringObject(event,strlen(event));
    int len = ll2string(buf,sizeof(buf),dbid);

    /* __keyspace@<db>__:<key> <event> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYSPACE) {
        chan = sdsnewlen("__keyspace@",11);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, key->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, eventobj, 0);
        decrRefCount(chanobj);
    }

    /* __keyevent@<db>__:<event> <key> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYEVENT) {
        chan = sdsnewlen("__keyevent@",11);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, eventobj->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, key, 0);
        decrRefCount(chanobj);
    }

    /* Subkey-level notifications (only when subkeys are provided). */
    if (subkeys != NULL && count > 0) {
        /* __subkeyspace@<db>__:<key> <event>|<len>:<subkey>[,...] notifications.
         * Skip if the event contains '|' to avoid parsing ambiguity since '|'
         * is used as a separator between event and subkeys in the payload. */
        if (server.notify_keyspace_events & NOTIFY_SUBKEYSPACE && !strchr(event, '|')) {
            chan = sdsnewlen("__subkeyspace@", 14);
            chan = sdscatlen(chan, buf, len);
            chan = sdscatlen(chan, "__:", 3);
            chan = sdscatsds(chan, key->ptr);
            chanobj = createObject(OBJ_STRING, chan);

            /* Build payload: <event>|<subkeys_payload> */
            sds payload = sdsdup(eventobj->ptr);
            payload = sdscatlen(payload, "|", 1);
            payload = catSubkeysPayload(payload, subkeys, count);
            robj *payloadobj = createObject(OBJ_STRING, payload);
            pubsubPublishMessage(chanobj, payloadobj, 0);
            decrRefCount(chanobj);
            decrRefCount(payloadobj);
        }

        /* __subkeyevent@<db>__:<event> <key_len>:<key>|<len>:<subkey>[,...] notifications. */
        if (server.notify_keyspace_events & NOTIFY_SUBKEYEVENT) {
            chan = sdsnewlen("__subkeyevent@", 14);
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
            payload = catSubkeysPayload(payload, subkeys, count);
            robj *payloadobj = createObject(OBJ_STRING, payload);
            pubsubPublishMessage(chanobj, payloadobj, 0);
            decrRefCount(chanobj);
            decrRefCount(payloadobj);
        }

        /* __subkeyspaceitem@<db>__:<key>\n<subkey> <event> notifications (per subkey).
         * Skip if the key contains '\n' to avoid parsing ambiguity in the channel name. */
        if (server.notify_keyspace_events & NOTIFY_SUBKEYSPACEITEM &&
            memchr(key->ptr, '\n', sdslen(key->ptr)) == NULL)
        {
            for (int i = 0; i < count; i++) {
                serverAssert(sdsEncodedObject(subkeys[i]));
                chan = sdsnewlen("__subkeyspaceitem@", 18);
                chan = sdscatlen(chan, buf, len);
                chan = sdscatlen(chan, "__:", 3);
                chan = sdscatsds(chan, key->ptr);
                chan = sdscatlen(chan, "\n", 1);
                chan = sdscatsds(chan, subkeys[i]->ptr);
                chanobj = createObject(OBJ_STRING, chan);
                pubsubPublishMessage(chanobj, eventobj, 0);
                decrRefCount(chanobj);
            }
        }

        /* __subkeyspaceevent@<db>__:<event>|<key> <subkeys> notifications.
         * Skip if the event contains '|' to avoid parsing ambiguity since '|'
         * is used as a separator between event and key in the channel name. */
        if (server.notify_keyspace_events & NOTIFY_SUBKEYSPACEEVENT && !strchr(event, '|')) {
            chan = sdsnewlen("__subkeyspaceevent@", 19);
            chan = sdscatlen(chan, buf, len);
            chan = sdscatlen(chan, "__:", 3);
            chan = sdscatsds(chan, eventobj->ptr);
            chan = sdscatlen(chan, "|", 1);
            chan = sdscatsds(chan, key->ptr);
            chanobj = createObject(OBJ_STRING, chan);
            robj *payloadobj = createObject(OBJ_STRING, catSubkeysPayload(NULL, subkeys, count));
            pubsubPublishMessage(chanobj, payloadobj, 0);
            decrRefCount(chanobj);
            decrRefCount(payloadobj);
        }
    }

    decrRefCount(eventobj);
}

/* Public API for key-level notifications (backward compatible). */
void notifyKeyspaceEvent(int type, const char *event, robj *key, int dbid) {
    notifyKeyspaceEventImpl(type, event, key, dbid, NULL, 0);
}

/* Public API for notifications with subkeys (key-level + subkey-level). */
void notifyKeyspaceEventWithSubkeys(int type, const char *event, robj *key, int dbid,
                                    robj **subkeys, int count) {
    notifyKeyspaceEventImpl(type, event, key, dbid, subkeys, count);
}

/* Check if subkey information should be collected for the given event type.
 * Returns true if any module subscribed to this event with subkeys, or if
 * there are Pub/Sub subscribers and any subkey-level notification channel is
 * enabled for this event type. */
int isSubkeyNotifyEnabled(int type) {
    if (moduleHasSubscribersForKeyspaceEventWithSubkeys(type)) return 1;
    if (dictSize(server.pubsub_patterns) == 0 && kvstoreSize(server.pubsub_channels) == 0)
        return 0;
    return (server.notify_keyspace_events & type) &&
           (server.notify_keyspace_events & (NOTIFY_SUBKEYSPACE | NOTIFY_SUBKEYEVENT |
                                             NOTIFY_SUBKEYSPACEITEM | NOTIFY_SUBKEYSPACEEVENT));
}
