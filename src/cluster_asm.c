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

#define ASM_MAX_DONE_TASKS 32 /* Maximum number of completed tasks to keep in memory. */
#define ASM_PAUSE_WRITE_MAX_GAP_BYTES (1 * 1024 * 1024) /* 1MB, TODO: a new config */

typedef struct asmTask {
    sds id;                             /* Task ID */
    int operation;                      /* Either ASM_IMPORT or ASM_MIGRATE */
    slotRangeArray *slot_ranges;        /* List of slot ranges for this migration task */
    int state;                          /* Current state of the task */
    char source[CLUSTER_NAMELEN];       /* Source node name */
    char dest[CLUSTER_NAMELEN];         /* Destination node name */
    clusterNode *source_node;           /* Source node */
    connection *main_channel_conn;      /* Main channel connection */
    connection *rdb_channel_conn;       /* RDB channel connection */
    int rdb_channel_state;              /* State of the RDB channel */
    unsigned long long dest_offset;     /* Destination offset */
    unsigned long long source_offset;   /* Source offset */
    int stream_eof_during_streaming;    /* If STREAM-EOF is received during streaming buffer */
    replDataBuf sync_buffer;            /* Buffer for the stream */
    client *main_channel_client;        /* Client for the main channel on the source side */
    client *rdb_channel_client;         /* Client for the RDB channel on the source side */
    long long retry_count;              /* Number of retries for this task */
    mstime_t create_time;               /* Task creation time */
    mstime_t start_time;                /* Task start time */
    mstime_t done_time;                 /* Task completion time */
    mstime_t paused_time;               /* The time when the slot writes were paused */
    sds error;                          /* Error message for this task */
} asmTask;

struct asmManager {
    list *tasks;                  /* List of asmTask to be processed */
    list *done_tasks;             /* List of completed asmTask */
    size_t sync_buffer_peak;      /* Peak size of sync buffer */
    long long total_done_tasks;   /* Total number of completed tasks */

    /* Fail point injection for debugging */
    int debug_failed_channel;     /* Channel where the task failed */
    int debug_failed_state;       /* State where the task failed */
};

enum asmState {
    /* Common state */
    ASM_NONE = 0,
    ASM_CONNECTING,
    ASM_AUTH_REPLY,
    ASM_CANCELED,
    ASM_FAILED,
    ASM_DONE,

    /* Import state */
    ASM_SEND_HANDSHAKE,
    ASM_HANDSHAKE_REPLY,
    ASM_SEND_SYNCSLOTS,
    ASM_SYNCSLOTS_REPLY,
    ASM_INIT_RDBCHANNEL,
    ASM_ACCUMULATE_BUF,
    ASM_STREAMING_BUF,
    ASM_WAIT_STREAM_EOF,
    ASM_TAKEOVER,

    /* Migrate state */
    ASM_WAIT_RDBCHANNEL,
    ASM_WAIT_BGSAVE_START,
    ASM_SEND_BULK_AND_STREAM,
    ASM_HANDOFF_PREP,
    ASM_HANDOFF,
    ASM_STREAM_DONE,

    /* RDB channel state */
    ASM_RDBCHANNEL_REQUEST,
    ASM_RDBCHANNEL_REPLY,
    ASM_RDBCHANNEL_TRANSFER,
};

enum asmChannel {
    ASM_IMPORT_MAIN_CHANNEL = 1,   /* Main channel for the import task */
    ASM_IMPORT_RDB_CHANNEL,        /* RDB channel for the import task */
    ASM_MIGRATE_MAIN_CHANNEL,      /* Main channel for the migrate task */
    ASM_MIGRATE_RDB_CHANNEL        /* RDB channel for the migrate task */
};

/* Global ASM manager */
struct asmManager *asmManager = NULL;

void asmStartImportTask(asmTask *task);

char *sendCommand(connection *conn, ...);
char *sendCommandArgv(connection *conn, int argc, char **argv, size_t *argv_lens);
char *receiveSynchronousResponse(connection *conn);
ConnectionType *connTypeOfReplication(void);
void createDumpPayload(rio *payload, robj *o, robj *key, int dbid);
int startBgsaveForReplication(int mincapa, int req);
void createReplicationBacklogIfNeeded(void);
static void asmSyncBufferReadFromConn(connection *conn);
void asmTaskSetFailed(asmTask *task, const char *fmt, ...);
int clusterAsmCancel(const char *task_id);

void clusterAsmInit(void) {
    asmManager = zcalloc(sizeof(*asmManager));
    asmManager->tasks = listCreate();
    asmManager->done_tasks = listCreate();
    asmManager->sync_buffer_peak = 0;
    asmManager->total_done_tasks = 0;
    asmManager->debug_failed_channel = 0;
    asmManager->debug_failed_state = 0;
}

char *asmTaskStateToString(int state) {
    switch (state) {
        case ASM_NONE: return "none";
        case ASM_CONNECTING: return "connecting";
        case ASM_AUTH_REPLY: return "auth-reply";
        case ASM_CANCELED: return "canceled";
        case ASM_FAILED: return "failed";
        case ASM_DONE: return "done";

        /* Import state */
        case ASM_SEND_HANDSHAKE: return "send-handshake";
        case ASM_HANDSHAKE_REPLY: return "handshake-reply";
        case ASM_SEND_SYNCSLOTS: return "send-syncslots";
        case ASM_SYNCSLOTS_REPLY: return "syncslots-reply";
        case ASM_INIT_RDBCHANNEL: return "init-rdbchannel";
        case ASM_ACCUMULATE_BUF: return "accumulate-buffer";
        case ASM_STREAMING_BUF: return "streaming-buffer";
        case ASM_WAIT_STREAM_EOF: return "wait-stream-eof";
        case ASM_TAKEOVER: return "takeover";
    
        /* Migrate state */
        case ASM_WAIT_RDBCHANNEL: return "wait-rdbchannel";
        case ASM_WAIT_BGSAVE_START: return "wait-bgsave-start";
        case ASM_SEND_BULK_AND_STREAM: return "send-bulk-and-stream";
        case ASM_HANDOFF_PREP: return "handoff-prep";
        case ASM_HANDOFF: return "handoff";
        case ASM_STREAM_DONE: return "stream-done";

        /* RDB channel state */
        case ASM_RDBCHANNEL_REQUEST: return "rdbchannel-request";
        case ASM_RDBCHANNEL_REPLY: return "rdbchannel-reply";
        case ASM_RDBCHANNEL_TRANSFER: return "rdbchannel-transfer";

        default: return "unknown";
    }
    serverAssert(0); /* Unreachable */
}

int asmDebugSetFailPoint(char * channel, char *state) {
    if (!asmManager) {
        serverLog(LL_WARNING, "ASM manager is not initialized");
        return C_ERR;
    }
    asmManager->debug_failed_channel = 0;
    asmManager->debug_failed_state = 0;
    if (!channel && !state) return C_ERR;
    if (sdslen(channel) == 0 && sdslen(state) == 0) {
        serverLog(LL_WARNING, "ASM fail point is cleared");
        return C_OK;
    }

    if (!strcasecmp(channel, "import-main-channel")) asmManager->debug_failed_channel = ASM_IMPORT_MAIN_CHANNEL;
    else if (!strcasecmp(channel, "import-rdb-channel")) asmManager->debug_failed_channel = ASM_IMPORT_RDB_CHANNEL;
    else if (!strcasecmp(channel, "migrate-main-channel")) asmManager->debug_failed_channel = ASM_MIGRATE_MAIN_CHANNEL;
    else if (!strcasecmp(channel, "migrate-rdb-channel")) asmManager->debug_failed_channel = ASM_MIGRATE_RDB_CHANNEL;
    else return C_ERR;

    if (!strcasecmp(state, "connecting")) asmManager->debug_failed_state = ASM_CONNECTING;
    else if (!strcasecmp(state, "auth-reply")) asmManager->debug_failed_state = ASM_AUTH_REPLY;
    else if (!strcasecmp(state, "handshake-reply")) asmManager->debug_failed_state = ASM_HANDSHAKE_REPLY;
    else if (!strcasecmp(state, "syncslots-reply")) asmManager->debug_failed_state = ASM_SYNCSLOTS_REPLY;
    else if (!strcasecmp(state, "accumulate-buffer")) asmManager->debug_failed_state = ASM_ACCUMULATE_BUF;
    else if (!strcasecmp(state, "streaming-buffer")) asmManager->debug_failed_state = ASM_STREAMING_BUF;
    else if (!strcasecmp(state, "wait-stream-eof")) asmManager->debug_failed_state = ASM_WAIT_STREAM_EOF;
    else if (!strcasecmp(state, "wait-rdbchannel")) asmManager->debug_failed_state = ASM_WAIT_RDBCHANNEL;
    else if (!strcasecmp(state, "wait-bgsave-start")) asmManager->debug_failed_state = ASM_WAIT_BGSAVE_START;
    else if (!strcasecmp(state, "send-bulk-and-stream")) asmManager->debug_failed_state = ASM_SEND_BULK_AND_STREAM;
    else if (!strcasecmp(state, "handoff")) asmManager->debug_failed_state = ASM_HANDOFF;
    else if (!strcasecmp(state, "rdbchannel-reply")) asmManager->debug_failed_state = ASM_RDBCHANNEL_REPLY;
    else if (!strcasecmp(state, "rdbchannel-transfer")) asmManager->debug_failed_state = ASM_RDBCHANNEL_TRANSFER;
    else return C_ERR;

    serverLog(LL_NOTICE, "ASM fail point set: channel=%s, state=%s", channel, state);
    return C_OK;
}

const char *asmChannelToString(int channel) {
    switch (channel) {
        case ASM_IMPORT_MAIN_CHANNEL: return "import-main-channel";
        case ASM_IMPORT_RDB_CHANNEL: return "import-rdb-channel";
        case ASM_MIGRATE_MAIN_CHANNEL: return "migrate-main-channel";
        case ASM_MIGRATE_RDB_CHANNEL: return "migrate-rdb-channel";
        default: return "unknown";
    }
}

int asmDebugIsFailPointActive(int channel, int state) {
    if (!asmManager) return 0; /* ASM manager not initialized */
    if (asmManager->debug_failed_channel == channel && asmManager->debug_failed_state == state) {
        serverLog(LL_NOTICE, "ASM fail point active: channel=%s, state=%s",
                  asmChannelToString(channel), asmTaskStateToString(state));
        return 1;
    }
    return 0;
}

void asmTaskReset(asmTask *task) {
    task->state = ASM_NONE;
    task->rdb_channel_state = ASM_NONE;
    task->main_channel_conn = NULL;
    task->rdb_channel_conn = NULL;
    task->dest_offset = 0;
    task->source_offset = 0;
    task->stream_eof_during_streaming = 0;
    replDataBufInit(&task->sync_buffer);
    sdsfree(task->error);
    task->error = sdsempty();
    task->main_channel_client = NULL;
    task->rdb_channel_client = NULL;
}

asmTask *asmTaskCreate(const char *task_id) {
    asmTask *task = zcalloc(sizeof(*task));
    task->error = sdsempty();
    asmTaskReset(task);
    task->slot_ranges = NULL;
    task->source_node = NULL;
    task->retry_count = 0;
    task->create_time = server.mstime;
    task->start_time = -1;
    task->done_time = -1;
    if (task_id) {
        task->id = sdsnew(task_id);
    } else {
        task->id = sdsnewlen(NULL, CLUSTER_NAMELEN);
        getRandomHexChars(task->id, CLUSTER_NAMELEN);
    }

    return task;
}

void asmTaskFree(asmTask *task) {
    replDataBufClear(&task->sync_buffer);
    sdsfree(task->id);
    zfree(task->slot_ranges);
    sdsfree(task->error);
    zfree(task);
}

size_t asmGetPeakSyncBufferSize(void) {
    /* Compute peak sync buffer usage. The current task's peak may not
     * reflect in asmManager->sync_buffer_peak immediately. */
    size_t peak = asmManager->sync_buffer_peak;
    asmTask *task = listFirst(asmManager->tasks) ?
                    listNodeValue(listFirst(asmManager->tasks)) : NULL;
    if (task && task->operation == ASM_IMPORT)
        peak = max(task->sync_buffer.peak, asmManager->sync_buffer_peak);
    
    return peak;
}

static inline int asmIsSlotImporting(void) {
    if (!asmManager || listLength(asmManager->tasks) == 0) return 0;

    /* Only support a single task at a time now, so only check the first task */
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    /* We only check the destination side, the source side `pauseActions` will
     * pause the write traffic (including expire/evict). */
    if ((task->operation == ASM_IMPORT && task->state != ASM_NONE)) {
        return 1;
    }

    return 0;
}

/* Returns 1 if the key belongs to the current node, 0 otherwise.
 * Check if there is a s lot import task in progress, and if so,
 * check if the key belongs to the current node, to avoid the
 * overhead of calculating the key’s hash slot. */
int asmKeyBelongsToCurrentNode(kvobj *kv) {
    if (asmIsSlotImporting()) {
        sds key = kvobjGetKey(kv);
        int slot = keyHashSlot((char*)key, sdslen(key));
        return clusterNodeCoversSlot(getMyClusterNode(), slot);
    }
    /* Not importing, all keys belong to the current node. TODO: make sure? */
    return 1;
}

static int compareSlotRange(const void *a, const void *b) {
    const slotRange *sa = a;
    const slotRange *sb = b;
    if (sa->start < sb->start) return -1;
    if (sa->start > sb->start) return 1;
    return 0;
}

/* Compare two slot range arrays, return 1 if equal, 0 otherwise */
static int slotRangeArrayIsEqual(slotRangeArray *sra1, slotRangeArray *sra2) {
    if (sra1->num_ranges != sra2->num_ranges) return 0;

    /* Sort slot ranges first */
    qsort(sra1->ranges, sra1->num_ranges, sizeof(slotRange), compareSlotRange);
    qsort(sra2->ranges, sra2->num_ranges, sizeof(slotRange), compareSlotRange);

    for (int i = 0; i < sra1->num_ranges; i++) {
        if (sra1->ranges[i].start != sra2->ranges[i].start ||
            sra1->ranges[i].end != sra2->ranges[i].end) {
            return 0;
        }
    }
    return 1;
}

/* Returns the ASM task with the given ID, or NULL if no such task exists. */
asmTask *lookupAsmTaskById(const char *id) {
    listIter li;
    listNode *ln;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (!strcmp(task->id, id)) return task;
    }
    return NULL;
}

/* Returns the ASM task that is identical to the given slot range array, or NULL
 * if no such task exists. */
asmTask *lookupAsmTaskBySlotRangeArray(slotRangeArray *sra) {
    listIter li;
    listNode *ln;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (slotRangeArrayIsEqual(task->slot_ranges, sra))
            return task;
    }
    return NULL;
}

/* Returns the slot range array for the given task ID */
slotRangeArray *asmTaskGetSlotRanges(const char *task_id) {
    asmTask *task = NULL;
    if (!task_id || (task = lookupAsmTaskById(task_id)) == NULL) return NULL;

    return task->slot_ranges;
}

/* Returns 1 if the slot range array overlaps with the given slot range. */
static int slotRangeArrayOverlaps(slotRangeArray *sra, slotRange *req) {
    for (int i = 0; i < sra->num_ranges; i++) {
        slotRange *sr = &sra->ranges[i];
        if (sr->start <= req->end && sr->end >= req->start)
            return 1;
    }
    return 0;
}

/* Returns the ASM task that overlaps with the given slot range, or NULL if
 * no such task exists. */
static asmTask *lookupAsmTaskBySlotRange(slotRange *req) {
    listIter li;
    listNode *ln;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (slotRangeArrayOverlaps(task->slot_ranges, req))
            return task;
    }
    return NULL;
}

/* Validates the given slot ranges for a migration task:
 * - Ensures the current node is a master.
 * - Verifies all slots are in a STABLE state.
 * - Checks that slot ranges are well-formed and non-overlapping.
 * - Confirms all slots belong to a single source node.
 * - Confirms no ongoing import task that overlaps with the slot ranges.
 *
 * Returns the source node if validation succeeds.
 * Otherwise, returns NULL and sets 'err' variable. */
static clusterNode *validateImportSlotRanges(slotRangeArray *slot_ranges, sds *err) {
    clusterNode *source = NULL;
    unsigned char *slots = zcalloc(CLUSTER_SLOTS);

    *err = NULL;

    /* Ensure this is a master node */
    if (!clusterNodeIsMaster(getMyClusterNode())) {
        *err = sdsnew("Slot migration not allowed on replica.");
        goto out;
    }

    /* Ensure no manual migration is in progress. */
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (getImportingSlotSource(i) != NULL ||
            getMigratingSlotDest(i) != NULL)
        {
            *err = sdsnew("All slot states must be STABLE to start a slot migration task.");
            goto out;
        }
    }

    for (int i = 0; i < slot_ranges->num_ranges; i++) {
        slotRange *sr = &slot_ranges->ranges[i];

        /* Ensure no import task overlaps with this slot range. */
        asmTask *task = lookupAsmTaskBySlotRange(sr);
        if (task && task->operation == ASM_IMPORT) {
            *err = sdscatprintf(sdsempty(),
                                "overlapping import exists for slot range: %d-%d",
                                sr->start, sr->end);
            goto out;
        }

        /* Validate if we can start migration task for this slot range. */
        for (int j = sr->start; j <= sr->end; j++) {
            clusterNode *node = getNodeBySlot(j);
            if (node == NULL) {
                *err = sdscatprintf(sdsempty(), "slot has no owner: %d", j);
                goto out;
            }

            if (!source) {
                source = node;
            } else if (source != node) {
                *err = sdsnew("slots belong to different source nodes");
                goto out;
            }
        }
    }

out:
    zfree(slots);
    return *err ? NULL : source;
}

/* Returns 1 if a migrate task is in progress, 0 otherwise. */
int asmMigrateInProgress(void) {
    listIter li;
    listNode *ln;

    if (!server.cluster_enabled || listLength(asmManager->tasks) == 0)
        return 0;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (task->operation == ASM_MIGRATE) return 1;
    }
    return 0;
}

/* Returns 1 if the task is in a state where it can receive replication stream
*  for the slot range, 0 otherwise. */
int asmCanFeedMigrationClient(asmTask *task) {
    return task->operation == ASM_MIGRATE &&
             (task->state == ASM_SEND_BULK_AND_STREAM ||
              task->state == ASM_HANDOFF_PREP);
}

/* Feed the migration client with the replication stream for the slot range. */
void asmFeedMigrationClient(robj **argv, int argc) {
    asmTask *task = NULL;

    if (server.cluster_enabled == 0 || listLength(asmManager->tasks) == 0)
        return;

    /* Quick check if there is a migrate task in progress. */
    task = listNodeValue(listFirst(asmManager->tasks));
    if (task->operation != ASM_MIGRATE) return;

    /* Check if the command belongs to the slot range. */
    struct redisCommand *cmd = lookupCommandBySds(argv[0]->ptr);
    serverAssert(cmd);

    int slot = getSlotFromCommand(cmd, argv, argc);
    /* If the command does not have keys, or has crossslot keys, skip it.
     * TODO: revisit this to see if we are okay with this. */
    if (slot == GETSLOT_NOKEYS || slot == GETSLOT_CROSSSLOT) return;

    /* Check if the slot belongs to the task's slot range. */
    slotRange sr = {slot, slot};
    if (!slotRangeArrayOverlaps(task->slot_ranges, &sr) ||
        !asmCanFeedMigrationClient(task))
    {
        return;
    }

    if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_MAIN_CHANNEL, task->state)))
        freeClientAsync(task->main_channel_client);

    /* Feed main channel with the command. */
    client *c = task->main_channel_client;
    size_t prev_bytes = getNormalClientPendingReplyBytes(c);

    addReplyArrayLen(c, argc);
    for (int i = 0; i < argc; i++)
        addReplyBulk(c, argv[i]);

    /* Update the task's source offset to reflect the bytes sent. */
    task->source_offset += (getNormalClientPendingReplyBytes(c) - prev_bytes);
}

asmTask *asmCreateImportTask(const char *task_id, slotRangeArray *slot_ranges, sds *err) {
    clusterNode *source;

    *err = NULL;
    /* Validate that the slot ranges are valid and that migration can be
     * initiated for them. */
    source = validateImportSlotRanges(slot_ranges, err);
    if (!source)
        return NULL;

    if (source == getMyClusterNode()) {
        *err = sdsnew("this node is already the owner of the slot range");
        return NULL;
    }

    if (listLength(asmManager->tasks) != 0) {
        *err = sdsnew("another ASM task is already in progress");
        return NULL;
    }

    /* Create a slot migration task */
    asmTask *task = asmTaskCreate(task_id);
    task->slot_ranges = slotRangeArrayDup(slot_ranges);
    task->state = ASM_NONE;
    task->operation = ASM_IMPORT;
    task->source_node = source;
    memcpy(task->source, source->name, CLUSTER_NAMELEN);
    memcpy(task->dest, getMyClusterId(), CLUSTER_NAMELEN);

    listAddNodeTail(asmManager->tasks, task);
    sds slot_ranges_str = slotRangeArrayToString(slot_ranges);
    serverLog(LL_NOTICE, "Import task created: src=%.40s, dest=%.40s, slots=%s",
                         task->source, task->dest, slot_ranges_str);
    sdsfree(slot_ranges_str);

    /* Don't start the task here since now we are in the context of executing a
     * command, otherwise, asmStartImportTask() may delete some keys belonging
     * to the slot range, and generate a big MULTI-EXEC, even this command itself
     * also be part of the MULTI-EXEC. So we will do it in beforeSleep(). */

    return task;
}

/* CLUSTER MIGRATION IMPORT <start-slot end-slot [start-slot end-slot ...]>
 *
 * Sent by operator to the destination node to start the migration. */
static void clusterMigrationCommandImport(client *c) {
    /* Validate slot range arg count */
    int remaining = c->argc - 3;
    if (remaining == 0 || remaining % 2 != 0) {
        addReplyErrorArity(c);
        return;
    }

    slotRangeArray *slot_ranges = parseSlotRangesOrReply(c, c->argc, 3);
    if (!slot_ranges) return;

    sds err = NULL;
    asmTask *task = asmCreateImportTask(NULL, slot_ranges, &err);
    zfree(slot_ranges);
    if (!task) {
        addReplyErrorSds(c, err);
        return;
    }

    addReplyBulkCString(c, task->id);
}

/* CLUSTER MIGRATION CANCEL [ID <id> | ALL]
 *   - Reply: Number of cancelled tasks
 *
 * Cancels import tasks that overlap with the specified slot ranges.
 * Multiple tasks may be cancelled. */
static void clusterMigrationCommandCancel(client *c) {
    sds task_id = NULL;
    int num_cancelled = 0;

    /* Validate slot range arg count */
    if (c->argc < 3) {
        addReplyErrorArity(c);
        return;
    }

    if (!strcasecmp(c->argv[3]->ptr, "id")) {
        if (c->argc != 5) {
            addReplyErrorArity(c);
            return;
        }
        task_id = c->argv[4]->ptr;
    } else if (!strcasecmp(c->argv[3]->ptr, "all")) {
        if (c->argc != 4) {
            addReplyErrorArity(c);
            return;
        }
    } else {
        addReplyError(c, "Missing ID or ALL argument");
        return;
    }

    num_cancelled = clusterAsmCancel(task_id);
    addReplyLongLong(c, num_cancelled);
}

/* Reply with the status of the task. */
static void replyTaskStatus(client *c, asmTask *task) {
    mstime_t p = 0;

    addReplyMapLen(c, 12);
    addReplyBulkCString(c, "id");
    addReplyBulkCString(c, task->id);
    addReplyBulkCString(c, "slots_range");
    addReplyBulkSds(c, slotRangeArrayToString(task->slot_ranges));
    addReplyBulkCString(c, "source");
    addReplyBulkCBuffer(c, task->source, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "dest");
    addReplyBulkCBuffer(c, task->dest, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "operation");
    addReplyBulkCString(c, task->operation == ASM_IMPORT ? "importing" : "migrating");
    addReplyBulkCString(c, "state");
    addReplyBulkCString(c, asmTaskStateToString(task->state));
    addReplyBulkCString(c, "last_error");
    addReplyBulkCBuffer(c, task->error, sdslen(task->error));
    addReplyBulkCString(c, "retries");
    addReplyBulkLongLong(c, task->retry_count);
    addReplyBulkCString(c, "create_time");
    addReplyBulkLongLong(c, task->create_time);
    addReplyBulkCString(c, "start_time");
    addReplyBulkLongLong(c, task->start_time);
    addReplyBulkCString(c, "done_time");
    addReplyBulkLongLong(c, task->done_time);

    if (task->operation == ASM_MIGRATE && task->state == ASM_DONE)
        p = task->done_time - task->paused_time;
    addReplyBulkCString(c, "write_pause_ms");
    addReplyBulkLongLong(c, p);
}

/* CLUSTER MIGRATION STATUS
 *  - Reply: Array of atomic slot migration tasks */
static void clusterMigrationCommandStatus(client *c) {
    listIter li;
    listNode *ln;

    addReplyArrayLen(c, listLength(asmManager->tasks) + listLength(asmManager->done_tasks));

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL)
        replyTaskStatus(c, listNodeValue(ln));

    listRewind(asmManager->done_tasks, &li);
    while ((ln = listNext(&li)) != NULL)
        replyTaskStatus(c, listNodeValue(ln));
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
        clusterMigrationCommandImport(c);
    } else if (strcasecmp(c->argv[2]->ptr, "status") == 0) {
        clusterMigrationCommandStatus(c);
    } else if (strcasecmp(c->argv[2]->ptr, "cancel") == 0) {
        clusterMigrationCommandCancel(c);
    } else {
        addReplyError(c, "unknown argument");
    }
}

void asmImportSetFailed(asmTask *task) {
    serverAssert(task->operation == ASM_IMPORT);
    if (task->state == ASM_FAILED) return;

    /* If we are in the RDB channel transfer state, we need to
     * close the client that was created for the RDB channel. */
    if (task->rdb_channel_conn && task->rdb_channel_state == ASM_RDBCHANNEL_TRANSFER) {
        client *c = connGetPrivateData(task->rdb_channel_conn);
        serverAssert(c->task == task);
        task->rdb_channel_conn = NULL;
        c->task = NULL;
        c->flags &= ~CLIENT_MASTER;
        freeClientAsync(c);
    }

    /* If we are in the wait stream EOF state, we need to close the
     * client that was created for the main channel. */
    if (task->main_channel_conn && task->state == ASM_WAIT_STREAM_EOF) {
        client *c = connGetPrivateData(task->main_channel_conn);
        serverAssert(c->task == task);
        task->main_channel_conn = NULL;
        c->task = NULL;
        c->flags &= ~CLIENT_MASTER;
        freeClientAsync(c);
    }

    /* Close the connections */
    if (task->rdb_channel_conn) connClose(task->rdb_channel_conn);
    if (task->main_channel_conn) connClose(task->main_channel_conn);
    task->rdb_channel_conn = NULL;
    task->main_channel_conn = NULL;

    /* Clear the replication data buffer */
    asmManager->sync_buffer_peak = max(asmManager->sync_buffer_peak, task->sync_buffer.peak);
    replDataBufClear(&task->sync_buffer);

    /* Mark the task as failed and notify the cluster */
    task->state = ASM_FAILED;
    clusterAsmOnEvent(task->id, ASM_EVENT_IMPORT_FAILED, NULL);
}

void asmMigrateSetFailed(asmTask *task) {
    serverAssert(task->operation == ASM_MIGRATE);
    if (task->state == ASM_FAILED) return;

    /* Close the RDB and main channel clients*/
    if (task->rdb_channel_client) {
        freeClientAsync(task->rdb_channel_client);
        task->rdb_channel_client->task = NULL;
        task->rdb_channel_client = NULL;
    }
    if (task->main_channel_client) {
        freeClientAsync(task->main_channel_client);
        task->main_channel_client->task = NULL;
        task->main_channel_client = NULL;
    }

    /* Actually it is not necessary to clear the sync buffer here,
     * to make asmTaskReset work properly after migrate task failed  */
    replDataBufClear(&task->sync_buffer);

    /* Mark the task as failed and notify the cluster */
    task->state = ASM_FAILED;
    clusterAsmOnEvent(task->id, ASM_EVENT_MIGRATE_FAILED, NULL);
}

void asmTaskSetFailed(asmTask *task, const char *fmt, ...) {
    va_list ap;
    sds error = sdsempty();

    /* Set the error message */
    va_start(ap, fmt);
    error = sdscatvprintf(error, fmt, ap);
    va_end(ap);
    error = sdscatprintf(error, " (state: %s, rdb_channel_state: %s)",
                         asmTaskStateToString(task->state),
                         asmTaskStateToString(task->rdb_channel_state));
    sdsfree(task->error);
    task->error = error;

    /* Log the error */
    sds slot_ranges_str = slotRangeArrayToString(task->slot_ranges);
    serverLog(LL_WARNING, "%s task failed. slots=%s, err=%s",
              task->operation == ASM_IMPORT ? "Import" : "Migrate",
              slot_ranges_str, task->error);
    sdsfree(slot_ranges_str);

    if (task->operation == ASM_IMPORT)
        asmImportSetFailed(task);
    else
        asmMigrateSetFailed(task);
}

/* The task is done or canceled and won't be retried. Update stats and
 * move it to the completed list, trim if necessary. */
void asmTaskComplete(asmTask *task) {
    listNode *ln = listFirst(asmManager->tasks);
    serverAssert(ln->value == task);

    task->done_time = server.mstime;
    asmManager->total_done_tasks++;

    if (task->operation == ASM_IMPORT) {
        asmManager->sync_buffer_peak = max(asmManager->sync_buffer_peak,
                                           task->sync_buffer.peak);
        replDataBufClear(&task->sync_buffer); /* Not used, so save memory */
    }

    listUnlinkNode(asmManager->tasks, ln);
    listLinkNodeHead(asmManager->done_tasks, ln);

    /* Trim the done tasks list if it grows too large */
    if (listLength(asmManager->done_tasks) > ASM_MAX_DONE_TASKS) {
        asmTask *oldest = listNodeValue(listLast(asmManager->done_tasks));
        asmTaskFree(oldest);
        listDelNode(asmManager->done_tasks, listLast(asmManager->done_tasks));
    }
}

void asmTaskCancel(asmTask *task) {
    if (task->state == ASM_CANCELED) return;

    asmTaskSetFailed(task, "Cancelled by user");
    task->state = ASM_CANCELED;
    asmTaskComplete(task);
}

void asmImportTakeover(asmTask *task) {
    serverAssert(task->state == ASM_WAIT_STREAM_EOF ||
                 task->state == ASM_STREAMING_BUF);

    /* Free the main channel connection since it is no longer needed. */
    serverAssert(task->main_channel_conn != NULL);
    client *c = connGetPrivateData(task->main_channel_conn);
    c->task = NULL;
    c->flags &= ~CLIENT_MASTER;
    freeClientAsync(c);
    task->main_channel_conn = NULL;

    task->state = ASM_TAKEOVER;
    clusterAsmOnEvent(task->id, ASM_EVENT_TAKEOVER, NULL);
}

void asmCallbackOnFreeClient(client *c) {
    asmTask *task = c->task;
    if (!task) return;

    /* If the RDB channel connection is closed, mark the task as failed. */
    if (c->conn && task->rdb_channel_conn == c->conn) {
        /* We create the client only when transferring data on the RDB channel */
        serverAssert(task->rdb_channel_state == ASM_RDBCHANNEL_TRANSFER);
        task->rdb_channel_conn = NULL; /* Will be freed by freeClient */
        c->flags &= ~CLIENT_MASTER;
        asmTaskSetFailed(task, "RDB channel - Connection is closed");
        return;
    }

    if (c->conn && task->main_channel_conn == c->conn) {
        /* After streaming buffer to DB, the client has the chance to close */
        serverAssert(task->state == ASM_WAIT_STREAM_EOF);
        task->main_channel_conn = NULL; /* Will be freed by freeClient */
        c->flags &= ~CLIENT_MASTER;
        asmTaskSetFailed(task, "Main channel - Connection is closed");
        return;
    }

    if (c == task->rdb_channel_client) {
        /* TODO: Detect whether the bgsave is completed successfully and
         * update the state properly. */
        task->rdb_channel_state = ASM_DONE;
        /* We may not have detected whether the child process has exited yet,
         * so we can’t determine whether the client has completed the slots
         * snapshot transfer. If the RDB channel is interrupted unexpectedly,
         * the destination side will also close the main channel.
         * So here we just reset the RDB channel client of task. */
        task->rdb_channel_client = NULL;
        return;
    }

    /* If the main channel client is closed, we need to mark the task as failed
     * and clean up the RDB channel client if it exists. */
    if (c == task->main_channel_client) {
        task->main_channel_client = NULL;
        /* The rdb channel client will be cleaned up */
        asmTaskSetFailed(task, "Main and RDB channel clients are disconnected.");
        return;
    }
}

/* Sends an AUTH command to the source node using the internal secret.
 * Returns an error string if the command fails, or NULL on success. */
char *asmSendInternalAuth(connection *conn) {
    size_t len = 0;
    const char *internal_secret = clusterGetSecret(&len);
    serverAssert(internal_secret != NULL);

    sds secret = sdsnewlen(internal_secret, len);
    char *err = sendCommand(conn, "AUTH", "internal connection", secret, NULL);
    sdsfree(secret);
    return err;
}

/* Handles the RDB channel sync with the source node.
 * This function is called when the RDB channel is established
 * and ready to sync with the source node. */
void asmRdbChannelSyncWithSource(connection *conn) {
    asmTask *task = connGetPrivateData(conn);
    char *err = NULL;
    sds task_error_msg = NULL;

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (connGetState(conn) != CONN_STATE_CONNECTED)
        goto error;

    /* Check if the task is in a fail point state */
    if (unlikely(asmDebugIsFailPointActive(ASM_IMPORT_RDB_CHANNEL, task->rdb_channel_state))) {
        char buf[1];
        /* Simulate a failure by shutting down the connection. On some operating
         * systems (e.g. Linux), the socket’s receive buffer is not flushed
         * immediately, so we issue a dummy read to drain any pending data and
         * surface the error condition. */
        connShutdown(conn);
        connRead(conn, buf, 1);
    }

    if (task->rdb_channel_state == ASM_CONNECTING) {
        connSetReadHandler(conn, asmRdbChannelSyncWithSource);
        connSetWriteHandler(conn, NULL);

        /* Send AUTH command to source node using internal auth */
        err = asmSendInternalAuth(conn);
        if (err) goto write_error;
        task->rdb_channel_state = ASM_AUTH_REPLY;
        return;
    }

    if (task->rdb_channel_state == ASM_AUTH_REPLY) {
        err = receiveSynchronousResponse(conn);
        /* The source node did not reply */
        if (err == NULL) goto no_response_error;

        /* Check `+OK` reply */
        if (!strcmp(err, "+OK")) {
            sdsfree(err);
            err = NULL;
            task->rdb_channel_state = ASM_RDBCHANNEL_REQUEST;
            serverLog(LL_NOTICE, "Source node replied to AUTH command, syncslots rdb channel operation can continue...");
        } else {
            task_error_msg = sdscatprintf(sdsempty(),
                "Error reply to AUTH from source: %s", err);
            sdsfree(err);
            goto error;
        }
    }

    if (task->rdb_channel_state == ASM_RDBCHANNEL_REQUEST) {
        err = sendCommand(conn, "CLUSTER", "SYNCSLOTS", "RDBCHANNEL", task->id, NULL);
        if (err) goto write_error;
        task->rdb_channel_state = ASM_RDBCHANNEL_REPLY;
        return;
    }

    if (task->rdb_channel_state == ASM_RDBCHANNEL_REPLY) {
        err = receiveSynchronousResponse(conn);
        /* The destination node did not reply */
        if (err == NULL) goto no_response_error;

        /* Check `+SLOTSSNAPSHOT` reply */
        if (!strncmp(err, "+SLOTSSNAPSHOT", strlen("+SLOTSSNAPSHOT"))) {
            sdsfree(err);
            err = NULL;
            task->state = ASM_ACCUMULATE_BUF;
            /* The main channel buffers pending commands. */
            connSetReadHandler(task->main_channel_conn, asmSyncBufferReadFromConn);

            task->rdb_channel_state = ASM_RDBCHANNEL_TRANSFER;
            client *c = createClient(conn);
            c->flags |= (CLIENT_MASTER | CLIENT_INTERNAL);
            c->querybuf = sdsempty();
            c->authenticated = 1;
            c->user = NULL;
            c->task = task;
            serverLog(LL_NOTICE,
                "Source node replied to SLOTSSNAPSHOT, syncslots snapshot can continue...");
        } else {
            task_error_msg = sdscatprintf(sdsempty(),
                "Error reply to CLUSTER SYNCSLOTS RDBCHANNEL from the source: %s", err);
            sdsfree(err);
            goto error;
        }
        return;
    }
    return;

no_response_error:
    task_error_msg = sdsnew("Source node did not respond to command during RDBCHANNELSYNCSLOTS handshake");
    /* Fall through to regular error handling */

error:
    asmTaskSetFailed(task, "RDB channel - Failed to sync with the source node: %s",
                     task_error_msg ? task_error_msg : connGetLastError(conn));
    sdsfree(task_error_msg);
    return;

write_error: /* Handle sendCommand() errors. */
    task_error_msg = sdscatprintf(sdsempty(), "Failed to send command to the source node: %s", err);
    sdsfree(err);
    goto error;
}

char *asmSendSlotRangesSync(connection *conn, asmTask *task) {
    /* Prepare CLUSTER SYNCSLOTS RANGES command */
    serverAssert(task->slot_ranges->num_ranges <= CLUSTER_SLOTS);
    int argc = task->slot_ranges->num_ranges*2 + 4;
    char **args = zcalloc(sizeof(char*) * argc);
    size_t *lens = zcalloc(sizeof(size_t) * argc);

    args[0] = "CLUSTER";
    args[1] = "SYNCSLOTS";
    args[2] = "RANGES";
    args[3] = task->id;
    lens[0] = strlen("CLUSTER");
    lens[1] = strlen("SYNCSLOTS");
    lens[2] = strlen("RANGES");
    lens[3] = sdslen(task->id);

    int i = 4;
    for (int j = 0; j < task->slot_ranges->num_ranges; j++) {
        slotRange *sr = &task->slot_ranges->ranges[j];
        args[i] = sdscatprintf(sdsempty(), "%d", sr->start);
        lens[i] = sdslen(args[i]);
        args[i+1] = sdscatprintf(sdsempty(), "%d", sr->end);
        lens[i+1] = sdslen(args[i+1]);
        i += 2;
    }
    serverAssert(i == argc);

    /* Send command to source node */
    char *err = sendCommandArgv(conn, argc, args, lens);

    /* Free allocated memory */
    for (int j = 4; j < argc; j++) {
        sdsfree(args[j]);
    }
    zfree(args);
    zfree(lens);

    return err;
}

void asmSyncWithSource(connection *conn) {
    asmTask *task = connGetPrivateData(conn);
    char *err = NULL;

    /* Some task errors are not network issues, we record them explicitly. */
    sds task_error_msg = NULL;

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (connGetState(conn) != CONN_STATE_CONNECTED)
        goto error;

    /* Check if the fail point is active for this channel and state */
    if (unlikely(asmDebugIsFailPointActive(ASM_IMPORT_MAIN_CHANNEL, task->state))) {
        char buf[1];
        /* Simulate a failure by shutting down the connection. On some operating
         * systems (e.g. Linux), the socket’s receive buffer is not flushed
         * immediately, so we issue a dummy read to drain any pending data and
         * surface the error condition. */
        connShutdown(conn);
        connRead(conn, buf, 1);
    }

    if (task->state == ASM_CONNECTING) {
        connSetReadHandler(conn, asmSyncWithSource);
        connSetWriteHandler(conn, NULL);
        /* Send AUTH command to source node using internal auth */
        err = asmSendInternalAuth(conn);
        if (err) goto write_error;
        task->state = ASM_AUTH_REPLY;
        return;
    }

    if (task->state == ASM_AUTH_REPLY) {
        err = receiveSynchronousResponse(conn);
        /* The source node did not reply */
        if (err == NULL) goto no_response_error;

        /* Check `+OK` reply */
        if (!strcmp(err, "+OK")) {
            sdsfree(err);
            err = NULL;
            task->state = ASM_SEND_HANDSHAKE;
            serverLog(LL_NOTICE, "Source node replied to AUTH command, syncslots can continue...");
        } else {
            task_error_msg = sdscatprintf(sdsempty(),
                "Error reply to AUTH from the source: %s", err);
            sdsfree(err);
            goto error;
        }
    }

    if (task->state == ASM_SEND_HANDSHAKE) {
        sds node_id = sdsnewlen(getMyClusterNode()->name, CLUSTER_NAMELEN);
        err = sendCommand(conn, "CLUSTER", "SYNCSLOTS", "CONF", "NODE-ID", node_id, NULL);
        sdsfree(node_id);
        if (err) goto write_error;
        task->state = ASM_HANDSHAKE_REPLY;
        return;
    }

    if (task->state == ASM_HANDSHAKE_REPLY) {
        err = receiveSynchronousResponse(conn);
        /* The source node did not reply */
        if (err == NULL) goto no_response_error;

        /* Check `+OK` reply */
        if (!strcmp(err, "+OK")) {
            sdsfree(err);
            err = NULL;
            task->state = ASM_SEND_SYNCSLOTS;
            serverLog(LL_NOTICE, "Source node replied to SYNCSLOTS CONF command, syncslots can continue...");
        } else {
            task_error_msg = sdscatprintf(sdsempty(),
                "Error reply to CLUSTER SYNCSLOTS CONF from the source: %s", err);
            sdsfree(err);
            goto error;
        }
    }

    if (task->state == ASM_SEND_SYNCSLOTS) {
        err = asmSendSlotRangesSync(conn, task);
        if (err) goto write_error;

        task->state = ASM_SYNCSLOTS_REPLY;
        return;
    }

    if (task->state == ASM_SYNCSLOTS_REPLY) {
        err = receiveSynchronousResponse(conn);
        /* The source node did not reply */
        if (err == NULL) goto no_response_error;

        /* Check `+RDBCHANNELSYNCSLOTS` reply */
        if (!strncmp(err, "+RDBCHANNELSYNCSLOTS", strlen("+RDBCHANNELSYNCSLOTS"))) {
            sdsfree(err);
            err = NULL;
            task->state = ASM_INIT_RDBCHANNEL;
            serverLog(LL_NOTICE,
                "Source node replied to RDBCHANNELSYNCSLOTS, syncslots can continue...");
        } else {
            task_error_msg = sdscatprintf(sdsempty(),
                "Error reply to CLUSTER SYNCSLOTS RANGES from the source: %s", err);
            sdsfree(err);
            goto error;
        }
    }

    if (task->state == ASM_INIT_RDBCHANNEL) {
        /* Create RDB channel connection */
        char *ip = clusterNodeIp(task->source_node);
        int port = server.tls_replication ? clusterNodeTlsPort(task->source_node) :
                                            clusterNodeTcpPort(task->source_node);
        task->rdb_channel_conn = connCreate(server.el, connTypeOfReplication());
        if (connConnect(task->rdb_channel_conn, ip, port,
                        server.bind_source_addr, asmRdbChannelSyncWithSource) == C_ERR)
        {
            serverLog(LL_WARNING, "Unable to connect to the source node: %s",
                      connGetLastError(task->rdb_channel_conn));
            goto error;
        }
        task->rdb_channel_state  = ASM_CONNECTING;
        connSetPrivateData(task->rdb_channel_conn, task);
        serverLog(LL_NOTICE,
            "RDB channel connection to source node %.40s established, waiting for AUTH reply...",
            clusterNodeGetName(task->source_node));

        /* Main channel waits for the new event */
        connSetReadHandler(conn, NULL);
        return;
    }
    return;

no_response_error:
    serverLog(LL_WARNING, "Source node did not respond to command during SYNCSLOTS handshake");
    /* Fall through to regular error handling */

error:
    asmTaskSetFailed(task, "Main channel - Failed to sync with source node: %s",
                     task_error_msg ? task_error_msg : connGetLastError(conn));
    sdsfree(task_error_msg);
    return;

write_error: /* Handle sendCommand() errors. */
    serverLog(LL_WARNING, "Failed to send command to source node: %s", err);
    sdsfree(err);
    goto error;
}

void asmImportSendACK(asmTask *task) {
    serverAssert(task->operation == ASM_IMPORT && task->state == ASM_WAIT_STREAM_EOF);
    serverLog(LL_DEBUG, "Destination node applied offset is %lld", task->dest_offset);

    char offset[64];
    ull2string(offset, sizeof(offset), task->dest_offset);

    char *err = sendCommand(task->main_channel_conn, "CLUSTER", "SYNCSLOTS", "ACK", offset, NULL);
    if (err) {
        asmTaskSetFailed(task, "Main channel - Failed to send ACK: %s", err);
        sdsfree(err);
    }
}

void asmStartSendBulkAndStream(struct asmTask *task) {
    serverAssert(task->state == ASM_WAIT_BGSAVE_START);

    if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_RDB_CHANNEL, task->state))) {
        connShutdown(task->rdb_channel_client->conn);
        return;
    }
    task->main_channel_client->replstate = SLAVE_STATE_SEND_BULK_AND_STREAM;

    task->state = ASM_SEND_BULK_AND_STREAM;
    task->rdb_channel_state = ASM_RDBCHANNEL_TRANSFER;
}

/* CLUSTER SYNCSLOTS SNAPSHOT-EOF
 *
 * This command is sent by the source node to the destination node to indicate
 * that the slots snapshot has ended. */
void clusterSyncSlotsSnapshotEOF(client *c) {
    /* This client is RDB channel connection. */
    asmTask *task = c->task;
    if (!task || task->rdb_channel_state != ASM_RDBCHANNEL_TRANSFER ||
        c->conn != task->rdb_channel_conn)
    {
        /* Unexpected SNAPSHOT-EOF command */
        serverLog(LL_WARNING, "Unexpected CLUSTER SYNCSLOTS SNAPSHOT-EOF command: "
                              "rdb_channel_state=%s",
                              asmTaskStateToString(task ? task->rdb_channel_state : ASM_NONE));
        freeClientAsync(c);
        return;
    }

    /* RDB channel state: ASM_RDBCHANNEL_TRANSFER */
    if (unlikely(asmDebugIsFailPointActive(ASM_IMPORT_RDB_CHANNEL, task->rdb_channel_state))) {
        freeClientAsync(c); /* Simulate a failure */
        return;
    }

    /* Clear the RDB channel connection */
    task->rdb_channel_conn = NULL;
    task->rdb_channel_state = ASM_DONE;
    serverLog(LL_NOTICE, "RDB channel snapshot transfer completed for the import task.");

    /* Free the RDB channel connection. */
    c->task = NULL;
    c->flags &= ~CLIENT_MASTER;
    freeClientAsync(c);

    /* Will start streaming the buffer to DB, don't start here since now
     * we are in the context of executing command, otherwise, redis will
     * generate a big MULTI-EXEC including all the commands in the buffer.
     * just update the state here, and do it in beforeSleep(). */
    task->state = ASM_STREAMING_BUF;
    connSetReadHandler(task->main_channel_conn, NULL);
}

/* CLUSTER SYNCSLOTS STREAM-EOF
 *
 * This command is sent by the source node to the destination node to indicate
 * that the slot sync stream has ended and the slots can be handed off. */
void clusterSyncSlotsStreamEOF(client *c) {
    asmTask *task = c->task;

    if (!task || task->operation != ASM_IMPORT) {
        serverLog(LL_WARNING, "Unexpected CLUSTER SYNCSLOTS STREAM-EOF command");
        freeClientAsync(c);
        return;
    }

    if (task->state == ASM_STREAMING_BUF) {
        /* We are still streaming the buffer to DB, mark the EOF received, and we
         * can takeover after streaming is done. Since we may release the context
         * in asmImportTakeover, this breaks the context for streaming buffer. */
        task->stream_eof_during_streaming = 1;
        serverLog(LL_NOTICE, "CLUSTER SYNCSLOTS STREAM-EOF received during streaming buffer");
        return;
    }

    if (task->state != ASM_WAIT_STREAM_EOF) {
        serverLog(LL_WARNING, "Unexpected CLUSTER SYNCSLOTS STREAM-EOF state: %s",
                               asmTaskStateToString(task->state));
        freeClientAsync(c);
        return;
    }
    serverLog(LL_NOTICE, "CLUSTER SYNCSLOTS STREAM-EOF received when waiting for STREAM-EOF");

    /* STREAM-EOF received, the source is ready to handoff, takeover now. */
    asmImportTakeover(task);
}

/* Start the import task. */
void asmStartImportTask(asmTask *task) {
    if (task->operation != ASM_IMPORT || task->state != ASM_NONE) return;

    sds slot_ranges_str = slotRangeArrayToString(task->slot_ranges);
    serverLog(LL_NOTICE, "Import task starting: src=%.40s, dest=%.40s, slots=%s",
              task->source, task->dest, slot_ranges_str);
    sdsfree(slot_ranges_str);

    clusterAsmOnEvent(task->id, ASM_EVENT_IMPORT_STARTED, task->slot_ranges);
    /* TODO: async clean up slots data, and propagate to replica */
    clusterDelKeysInSlotRangeArray(task->slot_ranges, CLUSTER_DELKEYS_ASYNC);

    task->start_time = server.mstime;

    /* TODO: tls support tests */
    task->main_channel_conn = connCreate(server.el, connTypeOfReplication());
    char *ip = clusterNodeIp(task->source_node);
    int port = server.tls_replication ? clusterNodeTlsPort(task->source_node) :
                                        clusterNodeTcpPort(task->source_node);
    if (connConnect(task->main_channel_conn, ip, port, server.bind_source_addr,
                    asmSyncWithSource) == C_ERR)
    {
        asmTaskSetFailed(task, "Main channel - Failed to connect to source node: %s",
                         connGetLastError(task->main_channel_conn));
        return;
    }
    connSetPrivateData(task->main_channel_conn, task);
    task->state = ASM_CONNECTING;
}

void clusterSyncSlotsCommand(client *c) {
    /* Only internal clients are allowed to execute this command to avoid
     * potential attack, since some state changes are not well protected,
     * external clients may damage the slot migration state. */
    if (!(c->flags & (CLIENT_INTERNAL | CLIENT_MASTER))) {
        addReplyError(c, "CLUSTER SYNCSLOTS subcommands are only allowed for internal clients");
        return;
    }

    /* Only allow CONF subcommand on replica. */
    if (server.masterhost && strcasecmp(c->argv[2]->ptr, "conf")) return;

    if (!strcasecmp(c->argv[2]->ptr, "ranges") && c->argc >= 6) {
        /* CLUSTER SYNCSLOTS RANGES <ID> <start-slot> <end-slot> [<start-slot> <end-slot>] */
        if (c->argc % 2 == 1) {
            addReplyErrorArity(c);
            return;
        }

        slotRangeArray *slot_ranges = parseSlotRangesOrReply(c, c->argc, 4);
        if (!slot_ranges) return;

        /* Validate that the slot ranges are valid and that migration can be
         * initiated for them. */
        sds err = NULL;
        clusterNode *source = validateImportSlotRanges(slot_ranges, &err);
        if (!source) {
            addReplyErrorSds(c, err);
            zfree(slot_ranges);
            return;
        }

        /* Check if the source node is the same as the current node. */
        if (source != getMyClusterNode()) {
            addReplyError(c, "This node is not the owner of the slots");
            zfree(slot_ranges);
            return;
        }

        sds task_id = c->argv[3]->ptr;
        asmTask *task = listLength(asmManager->tasks) == 0 ? NULL :
                            listNodeValue(listFirst(asmManager->tasks));
        if (task && !strcmp(task->id, task_id) &&
            task->operation == ASM_MIGRATE && task->state == ASM_FAILED &&
            slotRangeArrayIsEqual(slot_ranges, task->slot_ranges) &&
            memcmp(task->dest, c->node_id, CLUSTER_NAMELEN) == 0)
        {
            /* Reuse the failed task */
            asmTaskReset(task);
            zfree(task->slot_ranges); /* Will be set again later */
            task->retry_count++;
        } else if (task) {
            addReplyError(c, "Another migration task is already in progress");
            zfree(slot_ranges);
            return;
        }

        /* Create the migrate slots task and add it to the list,
         * otherwise reuse the existing one */
        if (task == NULL) {
            task = asmTaskCreate(task_id);
            task->start_time = server.mstime; /* Start immediately */
            listAddNodeTail(asmManager->tasks, task);
        }

        task->slot_ranges = slot_ranges;
        task->operation = ASM_MIGRATE;
        memcpy(task->source, getMyClusterNode()->name, CLUSTER_NAMELEN);
        if (c->node_id) memcpy(task->dest, c->node_id, CLUSTER_NAMELEN);

        clusterNode *dst = clusterLookupNode(task->dest, CLUSTER_NAMELEN);
        if (dst) {
            int port = server.tls_replication ? dst->tls_port : dst->tcp_port;
            c->slave_listening_port = port;
        }

        task->main_channel_client = c;
        c->task = task;

        /* We mark the main channel client as a replica, so this client is limited
         * by the client output buffer settings for replicas. The replstate has no
         * real significance, just to prevent it from going online. */
        c->flags |= (CLIENT_SLAVE | CLIENT_REPL_MIGRATION_DEST);
        c->replstate = SLAVE_STATE_WAIT_RDB_CHANNEL;
        if (server.repl_disable_tcp_nodelay)
            connDisableTcpNoDelay(c->conn);  /* Non critical if it fails. */
        listAddNodeTail(server.slaves, c);
        createReplicationBacklogIfNeeded();

        /* Wait for RDB channel to be ready */
        task->state = ASM_WAIT_RDBCHANNEL;

        sds slot_ranges_str = slotRangeArrayToString(slot_ranges);
        serverLog(LL_NOTICE, "Migrate task created: src=%.40s, dest=%.40s, slots=%s",
                              task->source, task->dest, slot_ranges_str);
        sdsfree(slot_ranges_str);

        clusterAsmOnEvent(task->id, ASM_EVENT_MIGRATE_STARTED, task->slot_ranges);

        /* addReply*() is not suitable for replica clients in this state. */
        if (connWrite(c->conn, "+RDBCHANNELSYNCSLOTS\r\n", 22) != 22)
            freeClientAsync(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "rdbchannel") && c->argc == 4) {
        /* CLUSTER SYNCSLOTS RDBCHANNEL <task-id> */
        sds task_id = c->argv[3]->ptr;
        if (sdslen(task_id) != CLUSTER_NAMELEN) {
            addReplyError(c, "Invalid task id");
            return;
        }

        if (listLength(asmManager->tasks) == 0) {
            addReplyError(c, "No slot migration task in progress");
            return;
        }

        asmTask *task = listNodeValue(listFirst(asmManager->tasks));
        if (task->operation != ASM_MIGRATE || task->state != ASM_WAIT_RDBCHANNEL ||
            strcmp(task->id, task_id) != 0)
        {
            addReplyError(c, "Another migration task is already in progress");
            return;
        }

        if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_MAIN_CHANNEL, task->state))) {
            /* Close the main channel client before rdb channel client connects */
            if (task->main_channel_client)
                freeClient(task->main_channel_client);
        }

        /* The main channel client must be present when setting RDB channel client */
        if (task->main_channel_client == NULL) {
            /* Maybe the main channel connection is closed. */
            addReplyError(c, "Main channel connection is not established");
            return;
        }

        /* Mark the client as a slave to generate slots snapshot */
        c->flags |= (CLIENT_SLAVE | CLIENT_REPL_RDB_CHANNEL | CLIENT_REPL_RDBONLY | CLIENT_REPL_MIGRATION_DEST);
        c->slave_capa |= SLAVE_CAPA_EOF;
        c->slave_req |= (SLAVE_REQ_SLOTS_SNAPSHOT | SLAVE_REQ_RDB_CHANNEL);
        c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
        c->repldbfd = -1;
        c->slave_listening_port = task->main_channel_client->slave_listening_port;
        if (server.repl_disable_tcp_nodelay)
            connDisableTcpNoDelay(c->conn); /* Non critical if it fails. */
        listAddNodeTail(server.slaves, c);

        /* Wait for bgsave to start for slots sync */
        task->state = ASM_WAIT_BGSAVE_START;
        task->rdb_channel_state = ASM_WAIT_BGSAVE_START;
        task->rdb_channel_client = c;
        c->task = task;

        if (!hasActiveChildProcess()) {
            startBgsaveForReplication(c->slave_capa, c->slave_req);
        } else {
            serverLog(LL_NOTICE, "BGSAVE for slots snapshot sync delayed");
        }
    } else if (!strcasecmp(c->argv[2]->ptr, "snapshot-eof") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS SNAPSHOT-EOF */
        clusterSyncSlotsSnapshotEOF(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "stream-eof") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS STREAM-EOF */
        clusterSyncSlotsStreamEOF(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "ack") && c->argc == 4) {
        /* CLUSTER SYNCSLOTS ACK <offset> */
        long long offset;
        if ((getLongLongFromObject(c->argv[3], &offset) != C_OK))
            return;

        if (c->task && c->task->operation == ASM_IMPORT) {
            /* This is a main channel connection, and we are streaming buffer. */
            asmTask *task = c->task;
            if (task->state == ASM_STREAMING_BUF) {
                /* Update the source offset*/
                if (task->source_offset > (unsigned long long) offset) {
                    serverLog(LL_WARNING, "CLUSTER SYNCSLOTS ACK received, but offset %lld is less than the current source offset %lld",
                              offset, task->source_offset);
                    return;
                }
                task->source_offset = offset;
                serverLog(LL_DEBUG, "CLUSTER SYNCSLOTS ACK received, updated source offset to %lld, destination offset: %lld",
                                     task->source_offset, task->dest_offset);
            }
        } else if (c->task && c->task->operation == ASM_MIGRATE) {
            /* Update the ACKed offset from destination. */
            asmTask *task = c->task;
            if (task->dest_offset > (unsigned long long) offset) {
                serverLog(LL_WARNING, "CLUSTER SYNCSLOTS ACK received, but offset %lld is less than the current destination offset %lld",
                        offset, task->dest_offset);
                return;
            }
            task->dest_offset = offset;
            serverLog(LL_DEBUG, "CLUSTER SYNCSLOTS ACK received, updated destination offset to %lld, source offset: %lld",
                                 task->dest_offset, task->source_offset);

            /* Pause write if needed */
            if (task->state == ASM_SEND_BULK_AND_STREAM) {
                /* Pause writes on the main channel connection if the gap is
                 * less than the desired threshold. */
                if (task->dest_offset + ASM_PAUSE_WRITE_MAX_GAP_BYTES >= task->source_offset) {
                    serverLog(LL_NOTICE, "The applied offset gap %lld is less than the threshold %d, "
                                         "pausing writes for slot handoff",
                                         task->source_offset - task->dest_offset,
                                         (int)ASM_PAUSE_WRITE_MAX_GAP_BYTES);
                    task->state = ASM_HANDOFF_PREP;
                    clusterAsmOnEvent(task->id, ASM_EVENT_HANDOFF_PREP, task->slot_ranges);
                }
            }
        }
    } else if (!strcasecmp(c->argv[2]->ptr, "fail") && c->argc == 4) {
        /* CLUSTER SYNCSLOTS FAIL <err> */
        return; /* This is a no-op, just to handle the command syntax. */
    } else if (!strcasecmp(c->argv[2]->ptr, "conf") && c->argc >= 5) {
        /* CLUSTER SYNCSLOTS CONF <option> <value> [<option> <value>] */
        for (int j = 3; j < c->argc; j += 2) {
            if (j + 1 >= c->argc) {
                addReplyErrorArity(c);
                return;
            }
            /* Handle each option here */
            if (!strcasecmp(c->argv[j]->ptr, "node-id")) {
                /* node-id <node-id> */
                sds node_id = c->argv[j + 1]->ptr;
                int node_id_len = (int) sdslen(node_id);
                if (node_id_len != CLUSTER_NAMELEN) {
                    addReplyErrorFormat(c, "Invalid node id length %d", node_id_len);
                    return;
                }

                /* Lookup the node in the cluster. */
                clusterNode *node = clusterLookupNode(node_id, node_id_len);
                if (node == NULL) {
                    addReplyErrorFormat(c, "Node %s not found in cluster", node_id);
                    return;
                }

                if (c->node_id) sdsfree(c->node_id);
                c->node_id = sdsdup(node_id);
                addReply(c, shared.ok);
            } else if (!strcasecmp(c->argv[j]->ptr, "slot-info")) {
                /* slot-info slot:key_size:expire_size */
                int count;
                long long slot, key_size, expire_size;
                sds slot_info = c->argv[j + 1]->ptr;
                sds *parts = sdssplitlen(slot_info, sdslen(slot_info), ":", 1, &count);

                /* Validate the slot info format, parse slot, key_size, expire_size */
                if (parts == NULL || count != 3 ||
                    (string2ll(parts[0], sdslen(parts[0]), &slot) == 0 || slot < 0 || slot >= CLUSTER_SLOTS) ||
                    (string2ll(parts[1], sdslen(parts[1]), &key_size) == 0 || key_size < 0) ||
                    (string2ll(parts[2], sdslen(parts[2]), &expire_size) == 0 || expire_size < 0))
                {
                    addReplyErrorFormat(c, "Invalid slot info: %s", slot_info);
                    sdsfreesplitres(parts, count);
                    return;
                }

                /* We resize individual slot specific dictionaries. */
                redisDb *db = c->db;
                serverAssert(db->id == 0); /* Only support DB 0 for cluster mode. */
                kvstoreDictExpand(db->keys, slot, key_size);
                kvstoreDictExpand(db->expires, slot, expire_size);

                sdsfreesplitres(parts, count);
                addReply(c, shared.ok);
            } else {
                addReplyErrorFormat(c, "Unknown option %s", (char *)c->argv[j]->ptr);
            }
        }
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
    }
}

/* Save the slot ranges snapshot to the file. It generates the DUMP encoded
 * representation of each key in the slot ranges and writes it to the file.
 *
 * Returns C_OK on success, or C_ERR on error. */
int slotRangesSnapshotSaveRio(int req, rio *rdb, int *error) {
    serverAssert(req & SLAVE_REQ_SLOTS_SNAPSHOT);

    dictEntry *de;
    kvstoreDictIterator *kvs_di = NULL;

    if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_RDB_CHANNEL, ASM_SEND_BULK_AND_STREAM)))
        rioAbort(rdb); /* Simulate a failure */

    for (int i = 0; i < server.dbnum; i++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db + i;
        if (kvstoreSize(db->keys) == 0) continue;

        /* SELECT the new DB */
        if (rioWrite(rdb,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(rdb, i) == 0) goto werr;

        /* Only support a single migrate task */
        serverAssert(listLength(asmManager->tasks) == 1);
        asmTask *task = listNodeValue(listFirst(asmManager->tasks));
        serverAssert(task->operation == ASM_MIGRATE);

        /* Iterate all slot ranges, and generate the DUMP encoded
         * representation of each key in the DB. */
        for (int j = 0; j < task->slot_ranges->num_ranges; j++) {
            slotRange *sr = &task->slot_ranges->ranges[j];
            /* Iterate all keys in the slot range */
            for (int k = sr->start; k <= sr->end; k++) {
                int send_slot_info = 0;
                kvs_di = kvstoreGetDictIterator(server.db->keys, k);

                while ((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
                    /* Send slot info before the first key in the slot */
                    if (!send_slot_info) {
                        /* Format slot info */
                        char buf[128];
                        int len = snprintf(buf, sizeof(buf), "%d:%lu:%lu",
                                    k, kvstoreDictSize(db->keys, k),
                                    kvstoreDictSize(db->expires, k));
                        serverAssert(len > 0 && len < (int)sizeof(buf));

                        /* Send slot info */
                        if (rioWriteBulkCount(rdb, '*', 5) == 0) goto werr;
                        if (rioWriteBulkString(rdb, "CLUSTER", 7) == 0) goto werr;
                        if (rioWriteBulkString(rdb, "SYNCSLOTS", 9) == 0) goto werr;
                        if (rioWriteBulkString(rdb, "CONF", 4) == 0) goto werr;
                        if (rioWriteBulkString(rdb, "SLOT-INFO", 9) == 0) goto werr;
                        if (rioWriteBulkString(rdb, buf, len) == 0) goto werr;
                        send_slot_info = 1;
                    }

                    /* Get the value object (of type kvobj) */
                    kvobj *o = dictGetKV(de);

                    /* Get the expire time */
                    long long expiretime = kvobjGetExpire(o);

                    /* Set on stack string object for key */
                    robj key;
                    initStaticStringObject(key, kvobjGetKey(o));

                    if (rioWriteBulkCount(rdb, '*', 5) == 0) goto werr;
                    if (rioWriteBulkString(rdb, "RESTORE", 7) == 0) goto werr;
                    if (rioWriteBulkObject(rdb, &key) == 0) goto werr;
                    if (rioWriteBulkLongLong(rdb, expiretime == -1 ? 0 : expiretime) == 0) goto werr;

                    /* Create the DUMP encoded representation. */
                    rio payload;
                    createDumpPayload(&payload, o, &key, i);
                    sds buf = payload.io.buffer.ptr;
                    if (rioWriteBulkString(rdb, buf, sdslen(buf)) == 0) {
                        sdsfree(payload.io.buffer.ptr);
                        goto werr;
                    }
                    sdsfree(payload.io.buffer.ptr);

                    /* Write ABSTTL */
                    if (rioWriteBulkString(rdb, "ABSTTL", 6) == 0) goto werr;

                    /* Delay return if required (for testing) */
                    if (unlikely(server.rdb_key_save_delay)) {
                        rioFlush(rdb); /* Send buffer to the destination ASAP. */
                        debugDelay(server.rdb_key_save_delay);
                    }
                }
                kvstoreReleaseDictIterator(kvs_di);
                kvs_di = NULL;
            }
        }
    }

    /* Write the end of the snapshot file command */
    if (rioWriteBulkCount(rdb, '*', 3) == 0) goto werr;
    if (rioWriteBulkString(rdb, "CLUSTER", 7) == 0) goto werr;
    if (rioWriteBulkString(rdb, "SYNCSLOTS", 9) == 0) goto werr;
    if (rioWriteBulkString(rdb, "SNAPSHOT-EOF", 12) == 0) goto werr;
    return C_OK;

werr:
    if (kvs_di) kvstoreReleaseDictIterator(kvs_di);
    if (error) *error = errno;
    return C_ERR;
}

/* ======================== ASM Sync Buffer Functions ======================== */

/* Read error handler for sync buffer */
static void asmReadSyncBufferErrorHandler(connection *conn) {
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    serverAssert(task->main_channel_conn == conn);

    if (task->state == ASM_STREAMING_BUF) {
        /* Since the client is protected, just mark the connection as closed.
         * Actual failure will be detected in the asmSyncBufferStreamToDb. */
        freeClientAsync(connGetPrivateData(conn));
    } else {
        asmTaskSetFailed(task, "Main channel - Read error: %s", connGetLastError(conn));
    }
}

/* Read data from connection into sync buffer. */
static void asmSyncBufferReadFromConn(connection *conn) {
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    serverAssert(task->main_channel_conn == conn);

    /* ASM_ACCUMULATE_BUF and ASM_STREAMING_BUF fail points are handled here */
    if (unlikely(asmDebugIsFailPointActive(ASM_IMPORT_MAIN_CHANNEL, task->state)))
        connShutdown(conn);

    replDataBuf *buf = &task->sync_buffer;
    if (task->state == ASM_STREAMING_BUF) {
        /* While streaming accumulated buffers, we continue reading from the
         * source to prevent accumulation on source side as much as possible.
         * However, we aim to drain buffer eventually. To ensure we consume more
         * than we read, we'll read at most one block after two blocks of
         * buffers are consumed. */
        if (listLength(buf->blocks) + 1 >= buf->last_num_blocks)
            return;
        buf->last_num_blocks = listLength(buf->blocks);
    }

    replDataBufReadFromConn(conn, buf, asmReadSyncBufferErrorHandler);
}

static void asmSyncBufferStreamYieldCallback(void *ctx) {
    replDataBufToDbCtx *context = ctx;
    client *c = context->client;

    char offset[64];
    ull2string(offset, sizeof(offset), context->applied_offset);

    char *err = sendCommand(c->conn, "CLUSTER", "SYNCSLOTS", "ACK", offset, NULL);
    if (err) {
        serverLog(LL_WARNING, "Error sending CLUSTER SYNCSLOTS ACK: %s", err);
        sdsfree(err);
        /* Since this client is protected, freeClient just masks it as closed */
        freeClientAsync(c);
    }
    serverLog(LL_DEBUG, "Yielding sending ACK during streaming buffer, applied offset: %zu",
                         context->applied_offset);
}

static int asmSyncBufferStreamShouldContinue(void *ctx) {
    replDataBufToDbCtx *context = ctx;
    client *c = context->client;

    /* Check if the client is still valid, maybe killed by `client kill`, or an
     * error happened during accumulating buffer. Since the client is protected,
     * won't be freed, here we check if the connection is marked as closed. */
    if (c->flags & CLIENT_CLOSE_ASAP) return 0;

    return 1;
}

/* Stream the sync buffer to the database. */
void asmSyncBufferStreamToDb(asmTask *task) {
    serverAssert(task->state == ASM_STREAMING_BUF);
    serverLog(LL_NOTICE, "Starting to stream accumulated buffer for the import task (%zu bytes)",
                         task->sync_buffer.used);

    /* The buffered stream from the main channel connection into
     * the database is processed by a fake client. */
    client *c = createClient(task->main_channel_conn);
    c->flags |= (CLIENT_MASTER | CLIENT_INTERNAL);
    c->querybuf = sdsempty();
    c->authenticated = 1;
    c->user = NULL;
    c->task = task;

    /* Mark the peek buffer block count. We'll use it to verify we consume
     * faster than we read from the source side. */
    task->sync_buffer.last_num_blocks = listLength(task->sync_buffer.blocks);

    replDataBufToDbCtx ctx = {
        .client = c,
        .applied_offset = 0,
        .should_continue = asmSyncBufferStreamShouldContinue,
        .yield_callback = asmSyncBufferStreamYieldCallback,
    };

    /* Protect the client from being killed by `client kill` */
    protectClient(c);
    /* protectClient will set read handler to NULL to prevent it from being freed,
     * but now we want to continue accumulating during streaming, so set it back
     * and guarantee the client won't be freed in the read handler. */
    connSetReadHandler(c->conn, asmSyncBufferReadFromConn);
    int ret = replDataBufStreamToDb(&task->sync_buffer, &ctx);
    unprotectClient(c);

    if (ret == C_OK) {
        if (task->stream_eof_during_streaming) {
            /* STREAM-EOF received during streaming, we can takeover now. */
            asmImportTakeover(task);
            return;
        }

        /* Update the dest offset according to applied bytes. */
        task->dest_offset = ctx.applied_offset;
        /* Wait STREAM-EOF from the source node. */
        task->state = ASM_WAIT_STREAM_EOF;
        connSetReadHandler(task->main_channel_conn, readQueryFromClient);
        serverLog(LL_NOTICE, "Successfully streamed accumulated buffer for the import task, applied offset: %lld",
                             task->dest_offset);

        if (unlikely(asmDebugIsFailPointActive(ASM_IMPORT_MAIN_CHANNEL, task->state)))
            connShutdown(task->main_channel_conn); /* Simulate a failure */

        /* ACK offset after streaming buffer is done. */
        asmImportSendACK(task);
    } else {
        /* If the streaming buffer failed, we need to clean up the task and
         * the main channel connection, to avoid client exposure when not in
         * ASM_WAIT_STREAM_EOF state */
        task->main_channel_conn = NULL;
        c->task = NULL;
        c->flags &= ~CLIENT_MASTER;
        freeClientAsync(c);
        asmTaskSetFailed(task, "Main channel - Failed to stream into the DB");
    }
}

void asmImportIncrAppliedBytes(struct asmTask *task, size_t bytes) {
    if (!task || task->state != ASM_WAIT_STREAM_EOF) return;
    serverAssert(task->operation == ASM_IMPORT);
    task->dest_offset += bytes;
}

void asmBeforeSleep(void) {
    if (listLength(asmManager->tasks) == 0) return;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));

    if (task->operation == ASM_IMPORT) {
        if (task->state == ASM_NONE)
            asmStartImportTask(task);
        else if (task->state == ASM_STREAMING_BUF)
            asmSyncBufferStreamToDb(task);
    }

    if (task->operation == ASM_MIGRATE) {
        if (task->state == ASM_HANDOFF) {
            client *c = task->main_channel_client;
            /* The command streams for slot ranges have been drained. */
            if (!clientHasPendingReplies(c)) {
                serverLog(LL_NOTICE, "Slot migration command stream drained, sending STREAM-EOF to the destination");

                if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_MAIN_CHANNEL, task->state)))
                    connShutdown(c->conn);

                /* Send STREAM-EOF to indicate the end of the stream. */
                char *err = sendCommand(c->conn, "CLUSTER", "SYNCSLOTS", "STREAM-EOF", NULL);
                if (err) {
                    asmTaskSetFailed(task, "Main channel - Failed to send STREAM-EOF: %s", err);
                    sdsfree(err);
                    return;
                }

               /* Even though the main channel client is no longer needed, we
                * can't close it directly because the destination may still be
                * sending ACKs over this connection. Instead, we leave it to the
                * destination to close it. We just clear the task and client
                * references */
                task->main_channel_client->task = NULL;
                task->main_channel_client = NULL;

                task->state = ASM_STREAM_DONE;
            }
        }
    }
}

void asmCron(void) {
    static long long asm_cron_runs = 0;
    asm_cron_runs++;

    if (listLength(asmManager->tasks) == 0) return;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));

    if (task->operation == ASM_IMPORT) {
        if (task->state == ASM_FAILED) {
            /* Retry every 1 second */
            if (asm_cron_runs % 10 == 0) {
                asmTaskReset(task);
                task->retry_count++;
                serverAssert(task->state == ASM_NONE);
                asmStartImportTask(task);
            }
        } else if (task->state == ASM_WAIT_STREAM_EOF) {
            asmImportSendACK(task);
        }
    }
}

/* Cancel a specific task if ID is provided, otherwise cancel all tasks. */
int clusterAsmCancel(const char *task_id) {
    if (task_id) {
        asmTask *task = lookupAsmTaskById(task_id);
        if (!task) return 0; /* Not found */

        asmTaskCancel(task);
        return 1;
    } else {
        int num_cancelled = 0;
        listIter li;
        listNode *ln;

        listRewind(asmManager->tasks, &li);
        while ((ln = listNext(&li)) != NULL) {
            asmTask *task = listNodeValue(ln);
            asmTaskCancel(task);
            num_cancelled++;
        }
        return num_cancelled;
    }
}

int clusterAsmHandoff(const char *task_id, sds *err) {
    serverAssert(task_id);

    asmTask *task = lookupAsmTaskById(task_id);
    if (!task || task->state != ASM_HANDOFF_PREP) {
        *err = sdscatprintf(sdsempty(), "No suitable ASM task found for id: %s, task_state: %s",
                            task_id, task ? asmTaskStateToString(task->state) : "null");
        return C_ERR;
    }

    task->state = ASM_HANDOFF;
    task->paused_time = server.mstime;

    return C_OK;
}

int asmNotifyConfigUpdated(slotRangeArray *slot_ranges, sds *err) {
    /* TODO: Validation, cancel asmTasks if required */

    asmTask *task = lookupAsmTaskBySlotRangeArray(slot_ranges);
    if (!task) {
        sds slot_ranges_str = slotRangeArrayToString(slot_ranges);
        *err = sdscatprintf(sdsempty(), "No ASM task found for slots: %s", slot_ranges_str);
        sdsfree(slot_ranges_str);
        return C_ERR;
    }

    if (task->operation == ASM_IMPORT && task->state == ASM_TAKEOVER) {
        task->state = ASM_DONE;
        clusterAsmOnEvent(task->id, ASM_EVENT_IMPORT_COMPLETED, NULL);
        asmTaskComplete(task);
        return C_OK;
    } else if (task->operation == ASM_MIGRATE && task->state == ASM_STREAM_DONE) {
        /* TODO: for plugin, we need to clean up the data of slot ranges
         * such as slotsflush */

        task->state = ASM_DONE;
        clusterAsmOnEvent(task->id, ASM_EVENT_MIGRATE_COMPLETED, NULL);
        asmTaskComplete(task);
        return C_OK;
    } else {
        *err = sdscatprintf(sdsempty(),
                            "ASM task is not in the correct state for config update: %s",
                            asmTaskStateToString(task->state));
        return C_ERR;
    }

    serverAssert(0); /* Unreachable */
}

/* Import/Migrate task is done, config is updated. */
int clusterAsmDone(const char *task_id, sds *err) {
    serverAssert(task_id);

    asmTask *task = lookupAsmTaskById(task_id);
    if (!task) {
        *err = sdscatprintf(sdsempty(), "No ASM task found for id: %s", task_id);
        return C_ERR;
    }
    return asmNotifyConfigUpdated(task->slot_ranges, err);
}

int clusterAsmProcess(const char *task_id, int event, void *arg, char **err) {
    int ret, num_cancelled;
    sds errsds = NULL;
    static char buf[256];

    if (err) *err = NULL;

    switch (event) {
        case ASM_EVENT_IMPORT_START:
            ret = asmCreateImportTask(task_id, arg, &errsds) ? C_OK : C_ERR;
            break;
        case ASM_EVENT_CANCEL:
            num_cancelled = clusterAsmCancel(task_id);
            if (arg) *((int *)arg) = num_cancelled;
            ret = C_OK;
            break;
        case ASM_EVENT_HANDOFF:
            ret = clusterAsmHandoff(task_id, &errsds);
            break;
        case ASM_EVENT_DONE:
            ret = clusterAsmDone(task_id, &errsds);
            break;
        default:
            ret = C_ERR;
            errsds = sdscatprintf(sdsempty(), "Unknown operation: %d", event);
            break;
    }

    if (ret != C_OK && errsds && err) {
        snprintf(buf, sizeof(buf), "%s", errsds);
        *err = buf;
    }
    sdsfree(errsds);

    return ret;
}
