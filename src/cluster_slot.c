#include "server.h"
#include "cluster.h"
#include "cluster_slot.h"

static uint64_t getSingleSlotStat(int slot, int order_by) {
    serverAssert(order_by != ORDER_BY_INVALID);
    uint64_t singleSlotStat = 0;
    if (order_by == ORDER_BY_KEY_COUNT) {
        singleSlotStat = (*server.db->slots_to_keys).by_slot[slot].count;
    }
    return singleSlotStat;
}

static void addReplySingleSlotStat(client *c, int slot) {
    addReplyLongLong(c, slot);
    addReplyMapLen(c, 1);
    addReplyBulkCString(c, "key_count");
    addReplyLongLong(c, (*server.db->slots_to_keys).by_slot[slot].count);
}

static void addReplySortedSlotStats(client *c, sortedSlotStatEntry sorted[], int num_slots_assigned, int limit) {
    int len = min(limit, num_slots_assigned);
    addReplyMapLen(c, len);

    for (int i = 0; i < len; i++) {
        addReplySingleSlotStat(c, sorted[i].slot);
    }
}

static int countValidSlots(unsigned char *slots) {
    int count = 0;
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (slots[slot]) count++;
    }
    return count;
}

static int countAssignedSlots() {
    int count = 0;
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (server.cluster->slots[slot]) count++;
    }
    return count;
}

static int slotStatEntryAscCmp(const void *a, const void *b) {
    sortedSlotStatEntry entry_a = *((sortedSlotStatEntry *) a);
    sortedSlotStatEntry entry_b = *((sortedSlotStatEntry *) b);
    return entry_a.stat - entry_b.stat;
}

static int slotStatEntryDescCmp(const void *a, const void *b) {
    sortedSlotStatEntry entry_a = *((sortedSlotStatEntry *) a);
    sortedSlotStatEntry entry_b = *((sortedSlotStatEntry *) b);
    return entry_b.stat - entry_a.stat;
}

static int checkSlotAssignment(client *c, unsigned char *slots, int start_slot, int end_slot) {
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (server.cluster->slots[slot]) {
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",(int)slot);
                return C_ERR;
            }
        }
    }
    return C_OK;
}

void addReplySlotStats(client *c, unsigned char *slots) {
    int count = countValidSlots(slots);
    addReplyMapLen(c, count);

    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (!slots[slot]) continue;
        addReplySingleSlotStat(c, slot);
    }
}

int checkSlotAssignmentsOrReply(client *c, unsigned char *slots, int del, int start_slot, int end_slot) {
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (del && server.cluster->slots[slot] == NULL) {
            addReplyErrorFormat(c,"Slot %d is already unassigned", slot);
            return C_ERR;
        } else if (!del && server.cluster->slots[slot]) {
            addReplyErrorFormat(c,"Slot %d is already busy", slot);
            return C_ERR;
        }
        if (slots[slot]++ == 1) {
            addReplyErrorFormat(c,"Slot %d specified multiple times",(int)slot);
            return C_ERR;
        }
    }
    return C_OK;
}

int checkSlotStatsOrderByArgumentOrReply(client *c, int *order_by, int *limit, int *desc) {
    *limit = CLUSTER_SLOTS;
    *desc = 1;
    *order_by = ORDER_BY_INVALID;

    if (!strcasecmp(c->argv[3]->ptr, "key_count")) {
        *order_by = ORDER_BY_KEY_COUNT;
    }
    if (*order_by == ORDER_BY_INVALID) {
        addReplyError(c, "unrecognized sort column for ORDER BY. The supported columns are, 1) key_count.");
        return C_ERR;
    }

    int i = 4; /* Next argument index, following ORDERBY */
    while(i < c->argc) {
        int moreargs = c->argc > i+1;
        if (!strcasecmp(c->argv[i]->ptr,"limit") && moreargs) {
            long tmp;
            if (getRangeLongFromObjectOrReply(
                c, c->argv[i+1], 1, CLUSTER_SLOTS+1, &tmp, 
                "limit has to lie in between 1 and 16384 (maximum number of slots)") != C_OK)
                return C_ERR;
            *limit = tmp;
            i++;
        } else if (!strcasecmp(c->argv[i]->ptr,"asc")) {
            *desc = 0;
        } else if (!strcasecmp(c->argv[i]->ptr,"desc")) {
            *desc = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return C_ERR;
        }
        i++;
    }

    return C_OK;
}

int checkSlotStatsSlotsArgumentOrReply(client *c, unsigned char *slots) {
    int slot;
    for (int i = 3; i < c->argc; i++) {
        if ((slot = getSlotOrReply(c,c->argv[i])) == C_ERR) {
            return C_ERR;
        }
        if (checkSlotAssignment(c, slots, slot, slot) == C_ERR) {
            return C_ERR;
        }
    }
    return C_OK;
}

int checkSlotStatsSlotsRangeArgumentOrReply(client *c, unsigned char *slots) {
    int startslot, endslot;
    if (c->argc % 2 == 0) {
        addReplyErrorArity(c);
        return C_ERR;
    }

    for (int i = 3; i < c->argc; i += 2) {
        if ((startslot = getSlotOrReply(c,c->argv[i])) == C_ERR ||
            (endslot = getSlotOrReply(c,c->argv[i+1])) == C_ERR) {
            return C_ERR;
        }
        if (startslot > endslot) {
            addReplyErrorFormat(c,"start slot number %d is greater than end slot number %d", startslot, endslot);
            return C_ERR;
        }
        if (checkSlotAssignment(c, slots, startslot, endslot) == C_ERR) {
            return C_ERR;
        }
    }

    return C_OK;
}

int getSlotOrReply(client *c, robj *o) {
    long long slot;
    if (getLongLongFromObject(o,&slot) != C_OK ||
        slot < 0 || slot >= CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

void markAssignedSlots(unsigned char *slots) {
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (server.cluster->slots[slot]) {
            slots[slot]++;
        }
    }
}

void sortAndAddReplySlotStats(client *c, int order_by, int limit, int desc) {
    int num_slots_assigned = countAssignedSlots();
    sortedSlotStatEntry sorted[num_slots_assigned];
    
    sortSlotStats(sorted, num_slots_assigned, order_by, desc);
    addReplySortedSlotStats(c, sorted, num_slots_assigned, limit);
}

void sortSlotStats(sortedSlotStatEntry sorted[], int num_slots_assigned, int order_by, int desc) {
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (server.cluster->slots[slot]) {
            sorted[slot].slot = slot;
            sorted[slot].stat = getSingleSlotStat(slot, order_by);
        }
    }
    qsort(sorted, num_slots_assigned, sizeof(sortedSlotStatEntry), (desc) ? slotStatEntryDescCmp : slotStatEntryAscCmp);
}
