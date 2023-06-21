
#include "server.h"
#include "cluster.h"

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing between {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * Slot to Key API
 * -------------------------------------------------------------------------- */

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot. */

/* Links to the next and previous entries for keys in the same slot are stored
 * in the dict entry metadata. See Slot to Key API below. */
#define dictEntryNextInSlot(de) \
    (((clusterDictEntryMetadata *)dictEntryMetadata(de))->next)
#define dictEntryPrevInSlot(de) \
    (((clusterDictEntryMetadata *)dictEntryMetadata(de))->prev)

void slotToKeyAddEntry(dictEntry *entry, redisDb *db) {
    sds key = dictGetKey(entry);
    unsigned int hashslot = keyHashSlot(key, sdslen(key));
    slotToKeys *slot_to_keys = &(*db->slots_to_keys).by_slot[hashslot];
    slot_to_keys->count++;

    /* Insert entry before the first element in the list. */
    dictEntry *first = slot_to_keys->head;
    dictEntryNextInSlot(entry) = first;
    if (first != NULL) {
        serverAssert(dictEntryPrevInSlot(first) == NULL);
        dictEntryPrevInSlot(first) = entry;
    }
    serverAssert(dictEntryPrevInSlot(entry) == NULL);
    slot_to_keys->head = entry;
}

void slotToKeyDelEntry(dictEntry *entry, redisDb *db) {
    sds key = dictGetKey(entry);
    unsigned int hashslot = keyHashSlot(key, sdslen(key));
    slotToKeys *slot_to_keys = &(*db->slots_to_keys).by_slot[hashslot];
    slot_to_keys->count--;

    /* Connect previous and next entries to each other. */
    dictEntry *next = dictEntryNextInSlot(entry);
    dictEntry *prev = dictEntryPrevInSlot(entry);
    if (next != NULL) {
        dictEntryPrevInSlot(next) = prev;
    }
    if (prev != NULL) {
        dictEntryNextInSlot(prev) = next;
    } else {
        /* The removed entry was the first in the list. */
        serverAssert(slot_to_keys->head == entry);
        slot_to_keys->head = next;
    }
}

/* Updates neighbour entries when an entry has been replaced (e.g. reallocated
 * during active defrag). */
void slotToKeyReplaceEntry(dict *d, dictEntry *entry) {
    dictEntry *next = dictEntryNextInSlot(entry);
    dictEntry *prev = dictEntryPrevInSlot(entry);
    if (next != NULL) {
        dictEntryPrevInSlot(next) = entry;
    }
    if (prev != NULL) {
        dictEntryNextInSlot(prev) = entry;
    } else {
        /* The replaced entry was the first in the list. */
        sds key = dictGetKey(entry);
        unsigned int hashslot = keyHashSlot(key, sdslen(key));
        clusterDictMetadata *dictmeta = dictMetadata(d);
        redisDb *db = dictmeta->db;
        slotToKeys *slot_to_keys = &(*db->slots_to_keys).by_slot[hashslot];
        slot_to_keys->head = entry;
    }
}

/* Initialize slots-keys map of given db. */
void slotToKeyInit(redisDb *db) {
    db->slots_to_keys = zcalloc(sizeof(clusterSlotToKeyMapping));
    clusterDictMetadata *dictmeta = dictMetadata(db->dict);
    dictmeta->db = db;
}

/* Empty slots-keys map of given db. */
void slotToKeyFlush(redisDb *db) {
    memset(db->slots_to_keys, 0,
           sizeof(clusterSlotToKeyMapping));
}

/* Free slots-keys map of given db. */
void slotToKeyDestroy(redisDb *db) {
    zfree(db->slots_to_keys);
    db->slots_to_keys = NULL;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
unsigned int delKeysInSlot(unsigned int hashslot) {
    unsigned int j = 0;

    dictEntry *de = (*server.db->slots_to_keys).by_slot[hashslot].head;
    while (de != NULL) {
        sds sdskey = dictGetKey(de);
        de = dictEntryNextInSlot(de);
        robj *key = createStringObject(sdskey, sdslen(sdskey));
        dbDelete(&server.db[0], key);
        propagateDeletion(&server.db[0], key, server.lazyfree_lazy_server_del);
        signalModifiedKey(NULL, &server.db[0], key);
        moduleNotifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, server.db[0].id);
        postExecutionUnitOperations();
        decrRefCount(key);
        j++;
        server.dirty++;
    }

    return j;
}

unsigned int countKeysInSlot(unsigned int hashslot) {
    return (*server.db->slots_to_keys).by_slot[hashslot].count;
}

#ifdef REDIS_CLUSTER_FLOTILLA
#include "cluster_flotilla.c"
#else
#include "cluster_legacy.c"
#endif
