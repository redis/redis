/* Cluster slot APIs and commands - to retrieve, update and process slot level data 
 * in association with Redis cluster.
 *
 * Copyright (c) 2022, Kyle Kim <kimkyle at amazon dot com>, Amazon Web Services.
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
#include "cluster.h"
#include "cluster_slot.h"

#define DEFAULT_SLOT -1
#define DEFAULT_STAT 0
#define UNASSIGNED_SLOT 0
#define ORDER_BY_KEY_COUNT 1
#define ORDER_BY_INVALID -1

/* -----------------------------------------------------------------------------
 * Cluster slot public APIs.
 * More to be added as part of cluster.c/h refactoring task. See issue #8948.
 * -------------------------------------------------------------------------- */

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

/* -----------------------------------------------------------------------------
 * CLUSTER SLOT-STATS command
 * -------------------------------------------------------------------------- */

typedef struct sortedSlotStatEntry {
    int slot;
    uint64_t stat;
} sortedSlotStatEntry;

static bool doesSlotBelongToCurrentShard(int slot) {
    clusterNode *n = server.cluster->myself;
    return (server.cluster->slots[slot] == n) || (nodeIsSlave(n) && (server.cluster->slots[slot] == n->slaveof));
}

static void markAssignedSlots(unsigned char *slots) {
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (doesSlotBelongToCurrentShard(slot)) slots[slot]++;
    }
}

static int countAssignedSlotsFromSlotsArray(unsigned char *slots) {
    int count = 0;
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (slots[slot]) count++;
    }
    return count;
}

static int countAssignedSlotsFromCurrentShard() {
    int count = 0;
    clusterNode *n = server.cluster->myself;
    if (nodeIsSlave(n)) {
        count = n->slaveof->numslots;
    } else {
        count = server.cluster->myself->numslots;
    }
    return count;
}

static void checkSlotAssignment(unsigned char *slots, int start_slot, int end_slot) {
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (doesSlotBelongToCurrentShard(slot)) {
            slots[slot]++;
        }
    }
}

static uint64_t getSingleSlotStat(int slot, int order_by) {
    serverAssert(order_by != ORDER_BY_INVALID);
    uint64_t singleSlotStat = 0;
    if (order_by == ORDER_BY_KEY_COUNT) {
        singleSlotStat = (*server.db->slots_to_keys).by_slot[slot].count;
    }
    return singleSlotStat;
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

static void sortSlotStats(sortedSlotStatEntry sorted[], int order_by, int desc) {
    int i = 0;
    
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (doesSlotBelongToCurrentShard(slot)) {
            sorted[i].slot = slot;
            sorted[i].stat = getSingleSlotStat(slot, order_by);
            i++;
        }
    }
    qsort(sorted, i, sizeof(sortedSlotStatEntry), (desc) ? slotStatEntryDescCmp : slotStatEntryAscCmp);
}

static void addReplySingleSlotStat(client *c, int slot) {
    addReplyLongLong(c, slot);
    addReplyMapLen(c, 1);
    addReplyBulkCString(c, "key_count");
    addReplyLongLong(c, (*server.db->slots_to_keys).by_slot[slot].count);
}

static void addReplySlotStats(client *c, unsigned char *slots) {
    int num_slots_assigned = countAssignedSlotsFromSlotsArray(slots);
    addReplyMapLen(c, num_slots_assigned);

    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (slots[slot]) addReplySingleSlotStat(c, slot);
    }
}

static void addReplySortedSlotStats(client *c, sortedSlotStatEntry sorted[], long limit) {
    int num_slots_assigned = countAssignedSlotsFromCurrentShard();
    int len = min(limit, num_slots_assigned);
    addReplyMapLen(c, len);

    for (int i = 0; i < len; i++) {
        addReplySingleSlotStat(c, sorted[i].slot);
    }
}

static void sortAndAddReplySlotStats(client *c, int order_by, long limit, int desc) {
    sortedSlotStatEntry sorted[CLUSTER_SLOTS];
    sortSlotStats(sorted, order_by, desc);
    addReplySortedSlotStats(c, sorted, limit);
}

void clusterSlotStatsCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    /* Initialize slot assignment array. */
    unsigned char slots[CLUSTER_SLOTS]= {UNASSIGNED_SLOT};

    /* No further arguments. */
    if (c->argc == 2) {
        /* CLUSTER SLOT-STATS */
        markAssignedSlots(slots);
        addReplySlotStats(c, slots);
        return;
    }

    /* Parse additional arguments. */
    if (!strcasecmp(c->argv[2]->ptr,"slotsrange") && c->argc == 5) {
        /* CLUSTER SLOT-STATS SLOTSRANGE start-slot end-slot */
        int startslot, endslot;
        if ((startslot = getSlotOrReply(c,c->argv[3])) == C_ERR ||
            (endslot = getSlotOrReply(c,c->argv[4])) == C_ERR) {
            return;
        }
        if (startslot > endslot) {
            addReplyErrorFormat(c,"start slot number %d is greater than end slot number %d", startslot, endslot);
            return;
        }
        checkSlotAssignment(slots, startslot, endslot);
        addReplySlotStats(c, slots);
    } else if (!strcasecmp(c->argv[2]->ptr,"orderby") && c->argc >= 4) {
        /* CLUSTER SLOT-STATS ORDERBY column [LIMIT limit] [ASC | DESC] */
        int desc = 1, order_by = ORDER_BY_INVALID;
        if (!strcasecmp(c->argv[3]->ptr, "key_count")) {
            order_by = ORDER_BY_KEY_COUNT;
        } else {
            addReplyError(c, "unrecognized sort column for ORDER BY. The supported columns are: key_count.");
            return;
        }
        int i = 4; /* Next argument index, following ORDERBY */
        int limit_counter = 0, asc_desc_counter = 0;
        long limit;
        while(i < c->argc) {
            int moreargs = c->argc > i+1;
            if (!strcasecmp(c->argv[i]->ptr,"limit") && moreargs) {
                if (getRangeLongFromObjectOrReply(
                    c, c->argv[i+1], 1, CLUSTER_SLOTS, &limit,
                    "limit has to lie in between 1 and 16384 (maximum number of slots)") != C_OK)
                    return;
                i++;
                limit_counter++;
            } else if (!strcasecmp(c->argv[i]->ptr,"asc")) {
                desc = 0;
                asc_desc_counter++;
            } else if (!strcasecmp(c->argv[i]->ptr,"desc")) {
                desc = 1;
                asc_desc_counter++;
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }

            if (limit_counter > 1 || asc_desc_counter > 1) {
                addReplyError(c, "you cannot provide multiple filters of the same type.");
                return;
            }
            i++;
        }
        sortAndAddReplySlotStats(c, order_by, limit, desc);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
