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

#define DEFAULT_SLOT -1
#define DEFAULT_STAT 0
#define UNASSIGNED_SLOT 0

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

static int countAssignedSlotsFromEntireShard() {
    int count = 0;
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (doesSlotBelongToCurrentShard(slot)) count++;
    }
    return count;
}

static int checkSlotAssignment(client *c, unsigned char *slots, int start_slot, int end_slot) {
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (doesSlotBelongToCurrentShard(slot)) {
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",(int)slot);
                return C_ERR;
            }
        }
    }
    return C_OK;
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
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (doesSlotBelongToCurrentShard(slot)) {
            sorted[slot].slot = slot;
            sorted[slot].stat = getSingleSlotStat(slot, order_by);
        } else {
            /* Even if the slot does not belong to the current shard,
             * we should fill the entry with default values so that qsort() does not segfault.
             * These entries will be filtered and ignored upon addReplySortedSlotStats(). */
            sorted[slot].slot = DEFAULT_SLOT;
            sorted[slot].stat = DEFAULT_STAT;
        }
    }
    qsort(sorted, CLUSTER_SLOTS, sizeof(sortedSlotStatEntry), (desc) ? slotStatEntryDescCmp : slotStatEntryAscCmp);
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
        if (!slots[slot]) continue;
        addReplySingleSlotStat(c, slot);
    }
}

static void addReplySortedSlotStats(client *c, sortedSlotStatEntry sorted[], int limit) {
    int num_slots_assigned = countAssignedSlotsFromEntireShard();
    int len = min(limit, num_slots_assigned);
    addReplyMapLen(c, len);

    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (sorted[i].slot == DEFAULT_SLOT) continue;
        if (len <= 0) break;

        addReplySingleSlotStat(c, sorted[i].slot);
        len--;
    }
}

static void sortAndAddReplySlotStats(client *c, int order_by, int limit, int desc) {
    sortedSlotStatEntry sorted[CLUSTER_SLOTS];
    sortSlotStats(sorted, order_by, desc);
    addReplySortedSlotStats(c, sorted, limit);
}

static int checkSlotStatsOrderByArgumentOrReply(client *c, int *order_by, int *limit, int *desc) {
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

static int checkSlotStatsSlotsRangeArgumentOrReply(client *c, unsigned char *slots) {
    if (c->argc % 2 == 0) {
        addReplyErrorArity(c);
        return C_ERR;
    }
    
    int startslot, endslot;
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
    for (int i = 2; i < c->argc; i++) {
        if (!strcasecmp(c->argv[2]->ptr,"slotsrange")) {
            /* CLUSTER SLOT-STATS SLOTSRANGE start-slot end-slot [start-slot end-slot ...] */
            if (checkSlotStatsSlotsRangeArgumentOrReply(c, slots) == C_ERR) {
                return;
            }
            addReplySlotStats(c, slots);
            return;
        } else if (!strcasecmp(c->argv[2]->ptr,"orderby")) {
            /* CLUSTER SLOT-STATS ORDERBY column [LIMIT limit] [ASC | DESC] */
            int limit, desc, order_by;
            if (checkSlotStatsOrderByArgumentOrReply(c, &order_by, &limit, &desc) == C_ERR) {
                return;
            }
            sortAndAddReplySlotStats(c, order_by, limit, desc);
            return;
        } else {
            addReplySubcommandSyntaxError(c);
            return;
        }
    }
}
