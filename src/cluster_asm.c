/*
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "cluster_asm.h"
#include "cluster.h"
#include "cluster_legacy.h"

#define ASM_IMPORT  (1 << 1)
#define ASM_MIGRATE (1 << 2)

typedef struct asmTask {
    list *slot_ranges;            /* List of slot ranges for this migration operation */
    sds state;                    /* Dummy state str */
    char source[CLUSTER_NAMELEN]; /* Source node name */
    char dest[CLUSTER_NAMELEN];   /* Destination node name */
    int operation;                /* Either ASM_IMPORT or ASM_MIGRATE */
} asmTask;

/* Returns C_OK if there is no overlapping import operation in progress for the
 * given slot range. Otherwise, returns C_ERR */
static int checkOverlappingImport(SlotRange *req) {
    listIter li, sli;
    listNode *ln, *sln;

    listRewind(server.cluster->asm_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *link = ln->value;

        /* Only import operations on the destination side can be cancelled. */
        if (link->operation != ASM_IMPORT ||
            strcasecmp(link->state, "inprogress") != 0)
        {
            continue;
        }

        listRewind(link->slot_ranges, &sli);
        while ((sln = listNext(&sli)) != NULL) {
            SlotRange *sr = sln->value;
            /* Cancel if any slot range overlaps with the requested range. */
            if (sr->first <= req->last && sr->last >= req->first)
                return C_ERR;
        }
    }

    return C_OK;
}

/* Validates the given slot ranges for a migration operation:
 * - Ensures the current node is a master.
 * - Verifies all slots are in a STABLE state.
 * - Checks that slot ranges are well-formed and non-overlapping.
 * - Confirms all slots belong to a single source node.
 * - Confirms no ongoing import operation that overlaps with the slot ranges.
 *
 * Returns the source node if validation succeeds.
 * Otherwise, returns NULL and sets 'err' variable. */
static clusterNode *validateImportSlotRanges(list *slot_ranges, sds *err) {
    listNode *ln;
    listIter li;
    clusterNode *source = NULL;
    unsigned char *slots = zcalloc(CLUSTER_SLOTS);

    *err = NULL;

    /* Ensure this is a master node */
    if (!clusterNodeIsMaster(server.cluster->myself)) {
        *err = sdsnew("Slot migration not allowed on replica.");
        goto out;
    }

    /* Ensure no manual migration is in progress. */
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (server.cluster->importing_slots_from[i] != NULL ||
            server.cluster->migrating_slots_to[i] != NULL)
        {
            *err = sdsnew("Slot states must be STABLE to start a slot migration operation.");
            goto out;
        }
    }

    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li))) {
        SlotRange *sr = ln->value;

        /* Validate slot boundaries */
        if (sr->first >= CLUSTER_SLOTS || sr->last >= CLUSTER_SLOTS ||
            sr->first > sr->last)
        {
            *err = sdscatprintf(sdsempty(), "invalid slot range: %d-%d",
                                sr->first, sr->last);
            goto out;
        }

        /* Ensure no import operation overlaps with this slot range. */
        if (checkOverlappingImport(sr) != C_OK) {
            *err = sdscatprintf(sdsempty(),
                                "overlapping import exists for slot range: %d-%d",
                                sr->first, sr->last);
            goto out;
        }

        /* Validate if we can start migration operation for this slot range. */
        for (int i = sr->first; i <= sr->last; i++) {
            if (server.cluster->slots[i] == NULL) {
                *err = sdscatprintf(sdsempty(), "slot has no owner: %d", i);
                goto out;
            }

            if (!source) {
                source = server.cluster->slots[i];
                if (source == server.cluster->myself) {
                    *err = sdscatprintf(sdsempty(), "this node is already the owner of the slot: %d", i);
                    goto out;
                }
            } else if (source != server.cluster->slots[i]) {
                *err = sdsnew("slots belong to different source nodes");
                goto out;
            }

            if (slots[i]++ != 0) {
                *err = sdscatprintf(sdsempty(), "slot %d is given twice in different slot ranges", i);
                goto out;
            }
        }
    }

out:
    zfree(slots);
    return *err ? NULL : source;
}

/* CLUSTER MIGRATION IMPORT <start-slot end-slot [start-slot end-slot ...]>
 *
 * Sent by operator to the destination node to start the migration. */
static void clusterCommandMigrationImport(client *c) {
    /* Validate slot range arg count */
    int remaining = c->argc - 3;
    if (remaining == 0 || remaining % 2 != 0) {
        addReplyErrorArity(c);
        return;
    }

    /* Parse slot ranges */
    int first, last;
    list *slot_ranges = listCreate();
    listSetFreeMethod(slot_ranges, zfree);

    for (int i = 3; i < c->argc; i += 2) {
        if ((first = getSlotOrReply(c, c->argv[i])) == -1 ||
            (last = getSlotOrReply(c, c->argv[i + 1])) == -1)
        {
            listRelease(slot_ranges);
            return;
        }

        SlotRange *sr = zmalloc(sizeof(*sr));
        sr->first = first;
        sr->last = last;
        listAddNodeTail(slot_ranges, sr);
    }

    sds err = NULL;
    clusterNode *source;

    /* Validate that the slot ranges are valid and that migration can be
     * initiated for them. */
    source = validateImportSlotRanges(slot_ranges, &err);
    if (!source) {
        addReplyErrorSds(c, err);
        listRelease(slot_ranges);
        return;
    }

    /* Schedule slot migration operation */
    asmTask *link = zcalloc(sizeof(*link));
    link->slot_ranges = slot_ranges;
    link->state = sdsnew("inprogress");
    link->operation = ASM_IMPORT;
    memcpy(link->source, source->name, CLUSTER_NAMELEN);
    memcpy(link->dest, server.cluster->myself->name, CLUSTER_NAMELEN);
    listAddNodeTail(server.cluster->asm_links, link);

    addReply(c, shared.ok);
}

/* Cancel atomic slot migration operations that overlap with the given slot
 * range. Returns the number of cancelled operations. */
static int cancelLinksForSlotRange(SlotRange *req_range) {
    int num_cancelled = 0;
    listIter li, sli;
    listNode *ln, *sln;

    listRewind(server.cluster->asm_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *link = ln->value;

        /* Only import operations on the destination side can be cancelled. */
        if (link->operation != ASM_IMPORT ||
            strcasecmp(link->state, "inprogress") != 0)
        {
            continue;
        }

        listRewind(link->slot_ranges, &sli);
        while ((sln = listNext(&sli)) != NULL) {
            SlotRange *sr = sln->value;
            /* Cancel if any slot range overlaps with the requested range. */
            if (sr->first <= req_range->last && sr->last >= req_range->first) {
                link->state = sdscpy(link->state, "cancelled");
                num_cancelled++;
                break;
            }
        }
    }
    return num_cancelled;
}

/* CLUSTER MIGRATION CANCEL <start-slot end-slot [start-slot end-slot ...]>
 *   - Reply: Number of cancelled operations
 *
 * Cancels import operations that overlap with the specified slot ranges.
 * Multiple operations may be cancelled. */
static void clusterCommandMigrationCancel(client *c) {
    int remaining, first, last;
    int num_cancelled = 0;
    listIter li;
    listNode *ln;
    list *slot_ranges;

    /* Validate slot range arg count */
    remaining = c->argc - 3;
    if (remaining == 0 || remaining % 2 != 0) {
        addReplyErrorArity(c);
        return;
    }

    slot_ranges = listCreate();
    listSetFreeMethod(slot_ranges, zfree);

    /* Parse slot ranges into the list */
    for (int i = 3; i < c->argc; i += 2) {
        if ((first = getSlotOrReply(c, c->argv[i])) == -1 ||
            (last = getSlotOrReply(c, c->argv[i + 1])) == -1)
        {
            listRelease(slot_ranges);
            return;
        }

        if (first > last) {
            addReplyErrorFormat(c, "invalid slot range: %d-%d", first, last);
            listRelease(slot_ranges);
            return;
        }

        SlotRange *sr = zmalloc(sizeof(*sr));
        sr->first = first;
        sr->last = last;
        listAddNodeTail(slot_ranges, sr);
    }

    /* Cancel asm operations that overlaps with the slot ranges. */
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        num_cancelled += cancelLinksForSlotRange(ln->value);
    }

    addReplyLongLong(c, num_cancelled);
    listRelease(slot_ranges);
}

/* Create a slot range string in the format of: "1000-2000 3000-4000 ..." */
static sds createSlotRangesStr(list *slot_ranges) {
    listNode *ln;
    listIter li;
    sds s = sdsempty();

    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        SlotRange *range = ln->value;
        s = sdscatprintf(s, "%d-%d ", range->first, range->last);
    }
    sdssetlen(s, sdslen(s) - 1);
    s[sdslen(s)] = '\0';

    return s;
}

/* CLUSTER MIGRATION STATUS
 *  - Reply: Array of atomic slot migration links */
static void clusterCommandMigrationStatus(client *c) {
    listIter li;
    listNode *ln;

    addReplyArrayLen(c, listLength(server.cluster->asm_links));

    listRewind(server.cluster->asm_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *link = ln->value;

        addReplyMapLen(c, 5);
        addReplyBulkCString(c, "slots_range");
        addReplyBulkSds(c, createSlotRangesStr(link->slot_ranges));
        addReplyBulkCString(c, "source");
        addReplyBulkCBuffer(c, link->source, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "dest");
        addReplyBulkCBuffer(c, link->dest, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "operation");
        addReplyBulkCString(c, link->operation == ASM_IMPORT ? "importing" : "migrating");
        addReplyBulkCString(c, "state");
        addReplyBulkCString(c, link->state);
    }
}

/* CLUSTER MIGRATION
 *      <IMPORT start-slot end-slot [start-slot end-slot ...] |
 *       STATUS |
 *       CANCEL start-slot end-slot [start-slot end-slot ...]>
 * */
void clusterMigrationCommand(client *c) {
    if (c->argc < 3) {
        addReplyErrorArity(c);
        return;
    }

    if (strcasecmp(c->argv[2]->ptr, "import") == 0) {
        clusterCommandMigrationImport(c);
    } else if (strcasecmp(c->argv[2]->ptr, "status") == 0) {
        clusterCommandMigrationStatus(c);
    } else if (strcasecmp(c->argv[2]->ptr, "cancel") == 0) {
        clusterCommandMigrationCancel(c);
    } else {
        addReplyError(c, "unknown argument");
    }
}
