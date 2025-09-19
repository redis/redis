/*
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "cluster.h"
#include "functions.h"
#include "cluster_legacy.h"
#include "cluster_asm.h"

#define ASM_IMPORT  (1 << 1)
#define ASM_MIGRATE (1 << 2)

#define ASM_DEBUG_TRIM_DEFAULT 0
#define ASM_DEBUG_TRIM_NONE 1
#define ASM_DEBUG_TRIM_BG 2
#define ASM_DEBUG_TRIM_ACTIVE 3

typedef struct asmTask {
    sds id;                             /* Task ID */
    int operation;                      /* Either ASM_IMPORT or ASM_MIGRATE */
    slotRangeArray *slot_ranges;        /* List of slot ranges for this migration task */
    int state;                          /* Current state of the task */
    int dest_state;                     /* Destination node's main state (approximate) */
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
    mstime_t dest_slots_snapshot_time;  /* The time when the destination starts applying the slot snapshot */
    mstime_t dest_accum_applied_time;   /* The time when the destination finishes applying the accumulated buffer */
    sds error;                          /* Error message for this task */
    redisOpArray *module_commands;      /* Module commands to be propagated at the beginning of slot migration */
} asmTask;

struct asmManager {
    list *tasks;                        /* List of asmTask to be processed */
    list *done_tasks;                   /* List of completed asmTask */
    list *pending_trim_jobs;            /* List of pending trim jobs (due to write pause) */
    list *active_trim_jobs;             /* List of active trim jobs */
    slotRangeArrayIter *active_trim_it; /* Iterator of the current active trim job */
    size_t sync_buffer_peak;            /* Peak size of sync buffer */
    long long total_done_tasks;         /* Total number of completed tasks */

    /* Fail point injection for debugging */
    int debug_failed_channel;     /* Channel where the task failed */
    int debug_failed_state;       /* State where the task failed */
    int debug_trim_method;        /* Method to trim the buffer */
    int debug_active_trim_delay;  /* Sleep before trimming each key */

    /* Active trim stats */
    unsigned long long active_trim_started;      /* Number of times active trim was started */
    unsigned long long active_trim_cancelled;    /* Number of times active trim was cancelled */
    unsigned long long active_trim_keys_total;   /* Total number of keys to trim in the current job */
    unsigned long long active_trim_keys_deleted; /* Number of keys trimmed in the current job */
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
    ASM_READY_TO_STREAM,
    ASM_STREAMING_BUF,
    ASM_WAIT_STREAM_EOF,
    ASM_TAKEOVER,

    /* Migrate state */
    ASM_WAIT_RDBCHANNEL,
    ASM_WAIT_BGSAVE_START,
    ASM_SEND_BULK_AND_STREAM,
    ASM_SEND_STREAM,
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

/* replication.c */
char *sendCommand(connection *conn, ...);
char *sendCommandArgv(connection *conn, int argc, char **argv, size_t *argv_lens);
char *receiveSynchronousResponse(connection *conn);
ConnectionType *connTypeOfReplication(void);
int startBgsaveForReplication(int mincapa, int req);
void createReplicationBacklogIfNeeded(void);
/* cluster.c */
void createDumpPayload(rio *payload, robj *o, robj *key, int dbid, int skip_checksum);
/* cluster_asm.c */
static void asmStartImportTask(asmTask *task);
static void asmTaskCancel(asmTask *task, const char *reason);
static void asmSyncBufferReadFromConn(connection *conn);
static void propagateTrimSlots(slotRangeArray *slots);
void asmTrimJobSchedule(slotRangeArray *slots);
void asmTrimJobProcessPending(void);
int asmTrimJobIsPending(void);
void asmTriggerActiveTrim(slotRangeArray *slots);
void asmActiveTrimEnd(int start_next_job);
int asmIsAnyTrimJobOverlaps(slotRangeArray *slots);

void clusterAsmInit(void) {
    asmManager = zcalloc(sizeof(*asmManager));
    asmManager->tasks = listCreate();
    asmManager->done_tasks = listCreate();
    asmManager->pending_trim_jobs = listCreate();
    asmManager->sync_buffer_peak = 0;
    asmManager->total_done_tasks = 0;
    asmManager->debug_failed_channel = 0;
    asmManager->debug_failed_state = 0;
    asmManager->debug_trim_method = ASM_DEBUG_TRIM_DEFAULT;
    asmManager->debug_active_trim_delay = 0;
    asmManager->active_trim_jobs = listCreate();
    asmManager->active_trim_started = 0;
    asmManager->active_trim_cancelled = 0;
    listSetFreeMethod(asmManager->active_trim_jobs, (void (*)(void*))slotRangeArrayFree);
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
        case ASM_READY_TO_STREAM: return "ready-to-stream";
        case ASM_STREAMING_BUF: return "streaming-buffer";
        case ASM_WAIT_STREAM_EOF: return "wait-stream-eof";
        case ASM_TAKEOVER: return "takeover";
    
        /* Migrate state */
        case ASM_WAIT_RDBCHANNEL: return "wait-rdbchannel";
        case ASM_WAIT_BGSAVE_START: return "wait-bgsave-start";
        case ASM_SEND_BULK_AND_STREAM: return "send-bulk-and-stream";
        case ASM_SEND_STREAM: return "send-stream";
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
    else if (!strcasecmp(state, "send-stream")) asmManager->debug_failed_state = ASM_SEND_STREAM;
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

int asmDebugSetTrimMethod(const char *method, int active_trim_delay) {
    if (!asmManager) {
        serverLog(LL_WARNING, "ASM manager is not initialized");
        return C_ERR;
    }
    int prev = asmManager->debug_trim_method;
    if (!strcasecmp(method, "default")) asmManager->debug_trim_method = ASM_DEBUG_TRIM_DEFAULT;
    else if (!strcasecmp(method, "none")) asmManager->debug_trim_method = ASM_DEBUG_TRIM_NONE;
    else if (!strcasecmp(method, "bg")) asmManager->debug_trim_method = ASM_DEBUG_TRIM_BG;
    else if (!strcasecmp(method, "active")) asmManager->debug_trim_method = ASM_DEBUG_TRIM_ACTIVE;
    else return C_ERR;

    /* If we are switching from none to default, delete all the keys in the
     * slots we don't own */
    if (prev == ASM_DEBUG_TRIM_NONE && asmManager->debug_trim_method != ASM_DEBUG_TRIM_NONE) {
        for (int i = 0; i < CLUSTER_SLOTS; i++)
            if (!clusterIsMySlot(i))
                clusterDelKeysInSlot(i, 0);
    }
    asmManager->debug_active_trim_delay = active_trim_delay;
    serverLog(LL_NOTICE, "ASM trim method was set=%s, active_trim_delay=%d", method, active_trim_delay);
    return C_OK;
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

sds asmCatInfoString(sds info) {
    int active_tasks = 0;

    listIter li;
    listNode *ln;
    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (task->operation == ASM_IMPORT ||
            (task->operation == ASM_MIGRATE && task->state != ASM_FAILED))
        {
            active_tasks++;
        }
    }

    return sdscatprintf(info ? info : sdsempty(),
                        "cluster_slot_migration_task_count:%d\r\n"
                        "cluster_slot_migration_total_done_tasks:%lld\r\n"
                        "cluster_slot_migration_sync_buffer_peak:%zu\r\n"
                        "cluster_slot_migration_active_trim_jobs:%lu\r\n"
                        "cluster_slot_migration_active_trim_started:%llu\r\n"
                        "cluster_slot_migration_active_trim_cancelled:%llu\r\n"
                        "cluster_slot_migration_active_trim_keys_total:%llu\r\n"
                        "cluster_slot_migration_active_trim_keys_deleted:%llu\r\n",
                        active_tasks,
                        asmManager->total_done_tasks,
                        asmGetPeakSyncBufferSize(),
                        listLength(asmManager->active_trim_jobs),
                        asmManager->active_trim_started,
                        asmManager->active_trim_cancelled,
                        asmManager->active_trim_keys_total,
                        asmManager->active_trim_keys_deleted);
}

void asmTaskReset(asmTask *task) {
    task->state = ASM_NONE;
    task->dest_state = ASM_NONE;
    task->rdb_channel_state = ASM_NONE;
    task->main_channel_conn = NULL;
    task->rdb_channel_conn = NULL;
    task->dest_offset = 0;
    task->source_offset = 0;
    task->stream_eof_during_streaming = 0;
    replDataBufInit(&task->sync_buffer);
    task->main_channel_client = NULL;
    task->rdb_channel_client = NULL;
    task->paused_time = 0;
    task->dest_slots_snapshot_time = 0;
    task->dest_accum_applied_time = 0;
    task->module_commands = NULL;
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

size_t asmGetImportingBufferSize(void) {
    if (!asmManager || listLength(asmManager->tasks) == 0) return 0;

    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    if (task->operation == ASM_IMPORT)
        return task->sync_buffer.mem_used;

    return 0;
}

size_t asmGetMigratingBufferSize(void) {
    if (!asmManager || listLength(asmManager->tasks) == 0) return 0;

    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    if (task->operation == ASM_MIGRATE && task->main_channel_client)
        return getClientOutputBufferMemoryUsage(task->main_channel_client);

    return 0;
}

/* Returns the ASM task with the given ID, or NULL if no such task exists. */
static asmTask *asmLookupTaskAt(list *tasks, const char *id) {
    listIter li;
    listNode *ln;

    listRewind(tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (!strcmp(task->id, id)) return task;
    }
    return NULL;
}

/* Returns the ASM task with the given ID, or NULL if no such task exists. */
asmTask *asmLookupTaskById(const char *id) {
    return asmLookupTaskAt(asmManager->tasks, id);
}

/* Returns the ASM task that is identical to the given slot range array, or NULL
 * if no such task exists. */
asmTask *asmLookupTaskBySlotRangeArray(slotRangeArray *sra) {
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
    if (!task_id || (task = asmLookupTaskById(task_id)) == NULL) return NULL;

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

/* Returns 1 if the two slot range arrays overlap, 0 otherwise. */
static int slotRangeArraysOverlap(slotRangeArray *sra1, slotRangeArray *sra2) {
    for (int i = 0; i < sra1->num_ranges; i++) {
        slotRange *sr1 = &sra1->ranges[i];
        if (slotRangeArrayOverlaps(sra2, sr1)) return 1;
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
static clusterNode *validateImportSlotRanges(slotRangeArray *slot_ranges, sds *err, asmTask *current) {
    clusterNode *source = NULL;
    unsigned char *slots = zcalloc(CLUSTER_SLOTS);

    *err = NULL;

    /* Ensure this is a master node */
    if (!clusterNodeIsMaster(getMyClusterNode())) {
        *err = sdsnew("slot migration not allowed on replica.");
        goto out;
    }

    /* Ensure no manual migration is in progress. */
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (getImportingSlotSource(i) != NULL ||
            getMigratingSlotDest(i) != NULL)
        {
            *err = sdsnew("all slot states must be STABLE to start a slot migration task.");
            goto out;
        }
    }

    for (int i = 0; i < slot_ranges->num_ranges; i++) {
        slotRange *sr = &slot_ranges->ranges[i];

        /* Ensure no import task overlaps with this slot range.
         * Skip check current task that is running for this slot range. */
        asmTask *task = lookupAsmTaskBySlotRange(sr);
        if (task && task != current && task->operation == ASM_IMPORT) {
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

/* Returns 1 if a task with the specified operation is in progress, 0 otherwise. */
static int asmTaskInProgress(int operation) {
    listIter li;
    listNode *ln;

    if (!asmManager || listLength(asmManager->tasks) == 0) return 0;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (task->operation == operation) return 1;
    }
    return 0;
}

/* Returns 1 if a migrate task is in progress, 0 otherwise. */
int asmMigrateInProgress(void) {
    return asmTaskInProgress(ASM_MIGRATE);
}

/* Returns 1 if an import task is in progress, 0 otherwise. */
int asmImportInProgress(void) {
    return asmTaskInProgress(ASM_IMPORT);
}

/* Returns 1 if the task is in a state where it can receive replication stream
*  for the slot range, 0 otherwise. */
int asmCanFeedMigrationClient(asmTask *task) {
    return task->operation == ASM_MIGRATE &&
             (task->state == ASM_SEND_BULK_AND_STREAM ||
              task->state == ASM_SEND_STREAM ||
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

    /* Ensure all arguments are converted to string encoding if necessary,
     * since getSlotFromCommand expects them to be string-encoded.
     * Generally the arguments are string-encoded, but we may rewrite
     * the command arguments to integer encoding. */
    for (int i = 0; i < argc; i++) {
        if (!sdsEncodedObject(argv[i])) {
            serverAssert(argv[i]->encoding == OBJ_ENCODING_INT);
            robj *old = argv[i];
            argv[i] = createStringObjectFromLongLongWithSds((long)old->ptr);
            decrRefCount(old);
        }
    }
    int slot = getSlotFromCommand(cmd, argv, argc);

    /* If the command does not have keys, skip it now.
     * SELECT is not propagated, since we only support a single db in cluster mode.
     * MULTI/EXEC is not needed, since transaction semantics are unnecessary
     * before the slot handoff.
     * FUNCTION subcommands should be executed on all nodes, so here we skip it,
     * and even propagating them may cause an error when executing.
     *
     * NOTICE: if some keyless commands should be propagated to the destination,
     * we should identify them here and send. */
    if (slot == GETSLOT_NOKEYS) return;

    /* Generally we reject cross-slot commands before executing, but module may
     * replicate this kind of command, so we check again. To guarantee data
     * consistency, we cancel the task if we encounter a cross-slot command. */
    if (slot == GETSLOT_CROSSSLOT) {
        asmTaskCancel(task, "cross-slot command");
        return;
    }

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
    source = validateImportSlotRanges(slot_ranges, err, NULL);
    if (!source)
        return NULL;

    if (source == getMyClusterNode()) {
        *err = sdsnew("this node is already the owner of the slot range");
        return NULL;
    }

    /* Only support a single task at a time now. */
    if (listLength(asmManager->tasks) != 0) {
        asmTask *current = listNodeValue(listFirst(asmManager->tasks));
        if (current->state == ASM_FAILED) {
            /* We can create a new import task only if the current one is failed,
             * cancel the failed task to create a new one. */
            asmTaskCancel(current, "new import requested");
        } else {
            *err = sdsnew("another ASM task is already in progress");
            return NULL;
        }
    }
    /* There should be no task in progress. */
    serverAssert(listLength(asmManager->tasks) == 0);

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

/* CLUSTER MIGRATION CANCEL [ID <task-id> | ALL]
 *   - Reply: Number of cancelled tasks
 *
 * Cancels import tasks that overlap with the specified slot ranges.
 * Multiple tasks may be cancelled. */
static void clusterMigrationCommandCancel(client *c) {
    sds task_id = NULL;
    int num_cancelled = 0;

    /* Validate slot range arg count */
    if (c->argc != 4 && c->argc != 5) {
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
        addReplyError(c, "unknown argument");
        return;
    }

    num_cancelled = clusterAsmCancel(task_id, "user request");
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

/* CLUSTER MIGRATION STATUS [ID <task-id> | ALL]
 *  - Reply: Array of atomic slot migration tasks */
static void clusterMigrationCommandStatus(client *c) {
    listIter li;
    listNode *ln;

    if (c->argc != 4 && c->argc != 5) {
        addReplyErrorArity(c);
        return;
    }

    if (!strcasecmp(c->argv[3]->ptr, "id")) {
        if (c->argc != 5) {
            addReplyErrorArity(c);
            return;
        }
        sds id = c->argv[4]->ptr;
        asmTask *task = asmLookupTaskAt(asmManager->tasks, id);
        if (!task) task = asmLookupTaskAt(asmManager->done_tasks, id);
        if (!task) {
            addReplyArrayLen(c, 0);
            return;
        }

        addReplyArrayLen(c, 1);
        replyTaskStatus(c, task);
    } else if (!strcasecmp(c->argv[3]->ptr, "all")) {
        if (c->argc != 4) {
            addReplyErrorArity(c);
            return;
        }
        addReplyArrayLen(c, listLength(asmManager->tasks) +
                            listLength(asmManager->done_tasks));
        listRewind(asmManager->tasks, &li);
        while ((ln = listNext(&li)) != NULL)
            replyTaskStatus(c, listNodeValue(ln));

        listRewind(asmManager->done_tasks, &li);
        while ((ln = listNext(&li)) != NULL)
            replyTaskStatus(c, listNodeValue(ln));
    } else {
        addReplyError(c, "unknown argument");
        return;
    }
}

/* CLUSTER MIGRATION
 *      <IMPORT <start-slot end-slot [start-slot end-slot ...]> |
 *       STATUS [ID <task-id> | ALL] |
 *       CANCEL [ID <task-id> | ALL]>
*/
void clusterMigrationCommand(client *c) {
    if (c->argc < 4) {
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

/* Notify the state change to the module and the plugin. */
void asmNotifyStateChange(asmTask *task, int state) {
    RedisModuleClusterAsmMigrationInfo info = {
            REDISMODULE_CLUSTER_ASM_MIGRATIONINFO_VERSION,
            task->id,
            (RedisModuleSlotRangeArray *) task->slot_ranges
    };

    int module_event = -1;
    if (state == ASM_EVENT_IMPORT_STARTED) module_event = REDISMODULE_SUBEVENT_CLUSTER_ASM_IMPORT_STARTED;
    else if (state == ASM_EVENT_IMPORT_COMPLETED) module_event = REDISMODULE_SUBEVENT_CLUSTER_ASM_IMPORT_COMPLETED;
    else if (state == ASM_EVENT_IMPORT_FAILED) module_event = REDISMODULE_SUBEVENT_CLUSTER_ASM_IMPORT_FAILED;
    else if (state == ASM_EVENT_MIGRATE_STARTED) module_event = REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_STARTED;
    else if (state == ASM_EVENT_MIGRATE_COMPLETED) module_event = REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_COMPLETED;
    else if (state == ASM_EVENT_MIGRATE_FAILED) module_event = REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_FAILED;
    serverAssert(module_event != -1);

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_ASM, module_event, &info);
    clusterAsmOnEvent(task->id, state, task->slot_ranges);
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

    /* If in the wait stream EOF or streaming buffer state, we need to close the
     * client that was created for the main channel. */
    if (task->main_channel_conn &&
        (task->state == ASM_STREAMING_BUF || task->state == ASM_WAIT_STREAM_EOF))
    {
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
    asmTrimJobSchedule(task->slot_ranges);
    asmNotifyStateChange(task, ASM_EVENT_IMPORT_FAILED);
}

void asmMigrateSetFailed(asmTask *task) {
    serverAssert(task->operation == ASM_MIGRATE);
    if (task->state == ASM_FAILED) return;

    /* Close the RDB and main channel clients*/
    if (task->rdb_channel_client) {
        task->rdb_channel_client->task = NULL;
        freeClientAsync(task->rdb_channel_client);
        task->rdb_channel_client = NULL;
    }
    if (task->main_channel_client) {
        task->main_channel_client->task = NULL;
        freeClientAsync(task->main_channel_client);
        task->main_channel_client = NULL;
    }

    /* Actually it is not necessary to clear the sync buffer here,
     * to make asmTaskReset work properly after migrate task failed  */
    replDataBufClear(&task->sync_buffer);

    /* Mark the task as failed and notify the cluster */
    task->state = ASM_FAILED;
    asmNotifyStateChange(task, ASM_EVENT_MIGRATE_FAILED);
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
 * move it to the completed list. */
void asmTaskComplete(asmTask *task) {
    listNode *ln = listFirst(asmManager->tasks);
    serverAssert(ln->value == task);

    /* Should never access it */
    task->source_node = NULL;

    task->done_time = server.mstime;
    asmManager->total_done_tasks++;

    if (task->operation == ASM_IMPORT) {
        asmManager->sync_buffer_peak = max(asmManager->sync_buffer_peak,
                                           task->sync_buffer.peak);
        replDataBufClear(&task->sync_buffer); /* Not used, so save memory */
    }

    /* Move the task to the completed list */
    listUnlinkNode(asmManager->tasks, ln);
    listLinkNodeHead(asmManager->done_tasks, ln);
}

static void asmTaskCancel(asmTask *task, const char *reason) {
    if (task->state == ASM_CANCELED) return;

    asmTaskSetFailed(task, "Cancelled due to %s", reason);
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
        /* After or in the process of streaming buffer to DB, a client will be
         * created based on the main channel connection. */
        serverAssert(task->state == ASM_STREAMING_BUF ||
                     task->state == ASM_WAIT_STREAM_EOF);
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
         * so we can't determine whether the client has completed the slots
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
         * surface the error condition.
         * using shutdown() instead of connShutdown() because connTLSShutdown()
         * will free the connection directly, which is not what we want. */
        shutdown(conn->fd, SHUT_RDWR);
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
        /* The source node did not reply */
        if (err == NULL) goto no_response_error;

        /* Ignore ‘\n' sent from the source node to keep the connection alive. */
        if (sdslen(err) == 0) {
            serverLog(LL_DEBUG, "Received an empty line in RDBCHANNEL reply, slots snapshot delivery will start later");
            sdsfree(err);
            return;
        }

        /* Check `+SLOTSSNAPSHOT` reply */
        if (!strncmp(err, "+SLOTSSNAPSHOT", strlen("+SLOTSSNAPSHOT"))) {
            sdsfree(err);
            err = NULL;
            task->state = ASM_ACCUMULATE_BUF;
            /* The main channel buffers pending commands. */
            connSetReadHandler(task->main_channel_conn, asmSyncBufferReadFromConn);

            task->rdb_channel_state = ASM_RDBCHANNEL_TRANSFER;
            client *c = createClient(conn);
            c->flags |= (CLIENT_MASTER | CLIENT_INTERNAL | CLIENT_ASM_IMPORTING);
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
    /* Prepare CLUSTER SYNCSLOTS SYNC command */
    serverAssert(task->slot_ranges->num_ranges <= CLUSTER_SLOTS);
    int argc = task->slot_ranges->num_ranges*2 + 4;
    char **args = zcalloc(sizeof(char*) * argc);
    size_t *lens = zcalloc(sizeof(size_t) * argc);

    args[0] = "CLUSTER";
    args[1] = "SYNCSLOTS";
    args[2] = "SYNC";
    args[3] = task->id;
    lens[0] = strlen("CLUSTER");
    lens[1] = strlen("SYNCSLOTS");
    lens[2] = strlen("SYNC");
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
        shutdown(conn->fd, SHUT_RDWR);
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
        sds node_id = sdsnewlen(clusterNodeGetName(getMyClusterNode()), CLUSTER_NAMELEN);
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
                "Error reply to CLUSTER SYNCSLOTS SYNC from the source: %s", err);
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
            task->source);

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

int asmImportSendACK(asmTask *task) {
    serverAssert(task->operation == ASM_IMPORT && task->state == ASM_WAIT_STREAM_EOF);
    serverLog(LL_DEBUG, "Destination node applied offset is %lld", task->dest_offset);

    char offset[64];
    ull2string(offset, sizeof(offset), task->dest_offset);

    char *err = sendCommand(task->main_channel_conn, "CLUSTER", "SYNCSLOTS", "ACK",
                    asmTaskStateToString(task->state), offset, NULL);
    if (err) {
        asmTaskSetFailed(task, "Main channel - Failed to send ACK: %s", err);
        sdsfree(err);
        return C_ERR;
    }
    return C_OK;
}

/* Called when the RDB channel begins sending the snapshot.
 * From this point on, the main channel also starts sending incremental streams. */
void asmSlotSnapshotAndStreamStart(struct asmTask *task) {
    if (task == NULL || task->state != ASM_WAIT_BGSAVE_START) return;

    if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_RDB_CHANNEL, task->state))) {
        shutdown(task->rdb_channel_client->conn->fd, SHUT_RDWR);
        return;
    }
    task->main_channel_client->replstate = SLAVE_STATE_SEND_BULK_AND_STREAM;

    task->state = ASM_SEND_BULK_AND_STREAM;
    task->rdb_channel_state = ASM_RDBCHANNEL_TRANSFER;

    /* From the source node's perspective, the destination node begins to accumulate
     * the buffer while the RDB channel starts applying the slot snapshot data. */
    task->dest_state = ASM_ACCUMULATE_BUF;
    task->dest_slots_snapshot_time = server.mstime;
}

/* Called when the RDB channel has succeeded in sending the snapshot. */
void asmSlotSnapshotSucceed(struct asmTask *task) {
    if (task == NULL || task->state != ASM_SEND_BULK_AND_STREAM) return;

    /* The destination starts sending ACKs to keep the main channel alive after
     * receiving the snapshot, so here we need to update the last interaction
     * time to avoid false timeout. */
    task->main_channel_client->lastinteraction = server.unixtime;

    task->state = ASM_SEND_STREAM;
    task->rdb_channel_state = ASM_DONE;
}

/* Called when the RDB channel fails to send the snapshot. */
void asmSlotSnapshotFailed(struct asmTask *task) {
    if (task == NULL || task->state != ASM_SEND_BULK_AND_STREAM) return;

    asmTaskSetFailed(task, "RDB channel - Failed to send slots snapshot");
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
    task->state = ASM_READY_TO_STREAM;
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
static void asmStartImportTask(asmTask *task) {
    if (task->operation != ASM_IMPORT || task->state != ASM_NONE) return;
    sds slot_ranges_str = slotRangeArrayToString(task->slot_ranges);

    /* Check if there is any trim job in progress for the slot ranges.
     * We can't start the import task since the trim job will modify the data.*/
    int trim_in_progress = asmIsAnyTrimJobOverlaps(task->slot_ranges);

    /* Cannot start import task since pause action is performed. Otherwise, we will
     * break the promise that no writes are performed during the pause. */
    if (isPausedActions(PAUSE_ACTION_CLIENT_ALL) ||
        isPausedActions(PAUSE_ACTION_CLIENT_WRITE) ||
        trim_in_progress)
    {
        static time_t last_log = 0;
        const char *reason = trim_in_progress ? "trim in progress for some of the slots" :
                                                "server paused";
        if (server.unixtime - last_log >= 5) { /* Log every 5 seconds to avoid spam */
            serverLog(LL_NOTICE, "Can not start import task for slots: %s since %s",
                                 slot_ranges_str, reason);
            last_log = server.unixtime;
        }
        sdsfree(slot_ranges_str);
        return;
    }

    /* Detect if the cluster topology is change. We should cancel the task if we can
     * not schedule it, and update the source node if needed. */
    sds err = NULL;
    clusterNode *source = validateImportSlotRanges(task->slot_ranges, &err, task);
    if (!source) {
        serverLog(LL_WARNING, "Cancel the import task for slots: %s, occur error: %s",
                              slot_ranges_str, err);
        asmTaskCancel(task, "slots configuration updated");
        sdsfree(slot_ranges_str);
        sdsfree(err);
        return;
    }
    /* Now I'm the owner of the slot range, cancel the import task. */
    if (source == getMyClusterNode()) {
        serverLog(LL_NOTICE, "Cancel the import task for slots: %s, since this node is"
                             " already the owner of the slot range", slot_ranges_str);
        asmTaskCancel(task, "slots owned by myself");
        sdsfree(slot_ranges_str);
        return;
    }
    /* Change the source node if needed. */
    if (source != task->source_node) {
        task->source_node = source;
        memcpy(task->source, source->name, CLUSTER_NAMELEN);
        serverLog(LL_NOTICE, "Import slots %s task source node changed to %.40s",
                             slot_ranges_str, source->name);
    }

    serverLog(LL_NOTICE, "Import task starting: src=%.40s, dest=%.40s, slots=%s",
              task->source, task->dest, slot_ranges_str);
    sdsfree(slot_ranges_str);

    asmNotifyStateChange(task, ASM_EVENT_IMPORT_STARTED);
    task->start_time = server.mstime;

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
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        return;
    }

    /* On replica, only allow master client to execute CONF subcommand. */
    if (server.masterhost) {
        if (!(c->flags & CLIENT_MASTER)) {
            /* Not master client, reject all subcommands and close the connection. */
            addReplyError(c, "CLUSTER SYNCSLOTS subcommands are only allowed for master");
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
            return;
        } else {
            /* Only allow CONF subcommand on replica. */
            if (strcasecmp(c->argv[2]->ptr, "conf")) return;
        }
    }

    if (!strcasecmp(c->argv[2]->ptr, "sync") && c->argc >= 6) {
        /* CLUSTER SYNCSLOTS SYNC <ID> <start-slot> <end-slot> [<start-slot> <end-slot>] */
        if (c->argc % 2 == 1) {
            addReplyErrorArity(c);
            return;
        }

        slotRangeArray *slot_ranges = parseSlotRangesOrReply(c, c->argc, 4);
        if (!slot_ranges) return;

        /* Validate that the slot ranges are valid and that migration can be
         * initiated for them. */
        sds err = NULL;
        clusterNode *source = validateImportSlotRanges(slot_ranges, &err, NULL);
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
            if (task->state == ASM_FAILED) {
                /* We can create a new migrate task only if the current one is
                 * failed, cancel the failed task to create a new one. */
                asmTaskCancel(task, "new migration requested");
                task = NULL;
            } else {
                addReplyError(c, "Another ASM task is already in progress");
                zfree(slot_ranges);
                return;
            }
        }

        /* Create the migrate slots task and add it to the list,
         * otherwise reuse the existing one */
        if (task == NULL) {
            task = asmTaskCreate(task_id);
            task->start_time = server.mstime; /* Start immediately */
            serverAssert(listLength(asmManager->tasks) == 0);
            listAddNodeTail(asmManager->tasks, task);
        }

        task->slot_ranges = slot_ranges;
        task->operation = ASM_MIGRATE;
        memcpy(task->source, clusterNodeGetName(getMyClusterNode()), CLUSTER_NAMELEN);
        if (c->node_id) memcpy(task->dest, c->node_id, CLUSTER_NAMELEN);

        task->main_channel_client = c;
        c->task = task;

        /* We mark the main channel client as a replica, so this client is limited
         * by the client output buffer settings for replicas. The replstate has no
         * real significance, just to prevent it from going online. */
        c->flags |= (CLIENT_SLAVE | CLIENT_ASM_MIGRATING);
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

        asmNotifyStateChange(task, ASM_EVENT_MIGRATE_STARTED);

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
        c->flags |= (CLIENT_SLAVE | CLIENT_REPL_RDB_CHANNEL | CLIENT_REPL_RDBONLY | CLIENT_ASM_MIGRATING);
        c->slave_capa |= SLAVE_CAPA_EOF;
        c->slave_req |= (SLAVE_REQ_SLOTS_SNAPSHOT | SLAVE_REQ_RDB_CHANNEL);
        c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
        c->repldbfd = -1;
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
    } else if (!strcasecmp(c->argv[2]->ptr, "ack") && c->argc == 5) {
        /* CLUSTER SYNCSLOTS ACK <state> <offset> */
        long long offset;
        int dest_state;

        if (!strcasecmp(c->argv[3]->ptr, asmTaskStateToString(ASM_STREAMING_BUF))) {
            dest_state = ASM_STREAMING_BUF;
        } else if (!strcasecmp(c->argv[3]->ptr, asmTaskStateToString(ASM_WAIT_STREAM_EOF))) {
            dest_state = ASM_WAIT_STREAM_EOF;
        } else {
            return; /* Not support now. */
        }

        if ((getLongLongFromObject(c->argv[4], &offset) != C_OK))
            return;

        if (c->task && c->task->operation == ASM_MIGRATE) {
            /* Update the state and ACKed offset from destination. */
            asmTask *task = c->task;
            task->dest_state = dest_state;
            if (task->dest_offset > (unsigned long long) offset) {
                serverLog(LL_WARNING, "CLUSTER SYNCSLOTS ACK received, dest state: %s, "
                                      "but offset %lld is less than the current dest offset %lld",
                        asmTaskStateToString(dest_state), offset, task->dest_offset);
                return;
            }
            task->dest_offset = offset;
            serverLog(LL_DEBUG, "CLUSTER SYNCSLOTS ACK received, dest state: %s, "
                                "updated dest offset to %lld, source offset: %lld",
                asmTaskStateToString(dest_state), task->dest_offset, task->source_offset);

            /* Pause write if needed */
            if (task->state == ASM_SEND_BULK_AND_STREAM || task->state == ASM_SEND_STREAM) {
                /* Pause writes on the main channel connection if the gap is
                 * less than the desired threshold. */
                if (task->dest_offset + server.asm_pause_write_max_gap_size >= task->source_offset) {
                    serverLog(LL_NOTICE, "The applied offset gap %lld is less than the threshold %lld, "
                                         "pausing writes for slot handoff",
                                         task->source_offset - task->dest_offset,
                                         server.asm_pause_write_max_gap_size);
                    task->state = ASM_HANDOFF_PREP;
                    clusterAsmOnEvent(task->id, ASM_EVENT_HANDOFF_PREP, task->slot_ranges);
                }
            }

            /* Record the time when the destination finishes applying the accumulated buffer */
            if (task->dest_state == ASM_WAIT_STREAM_EOF && task->dest_accum_applied_time == 0)
                task->dest_accum_applied_time = server.mstime;
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

/* Save a key-value pair to stream I/O using either RESTORE or AOF format. */
static int slotSnapshotSaveKeyValuePair(rio *rdb, kvobj *o, int dbid) {
    /* Get the expire time */
    long long expiretime = kvobjGetExpire(o);

    /* Set on stack string object for key */
    robj key;
    initStaticStringObject(key, kvobjGetKey(o));

    /* If non-string/module object that is not too big, or module object
     * that does not support aof_rewrite, use RESTORE to import data.
     * Generally RDB binary format is more efficient, but it may cause
     * block in the destination if the object is too large, so fall back
     * to AOF format if necessary. */
    if ((o->type != OBJ_STRING && o->type != OBJ_MODULE && getObjectLength(o) <= AOF_REWRITE_ITEMS_PER_CMD) ||
        (o->type == OBJ_MODULE && ((moduleValue*)o->ptr)->type->aof_rewrite == NULL))
    {
        if (rioWriteBulkCount(rdb, '*', 5) == 0) return C_ERR;
        if (rioWriteBulkString(rdb, "RESTORE", 7) == 0) return C_ERR;
        if (rioWriteBulkObject(rdb, &key) == 0) return C_ERR;
        if (rioWriteBulkLongLong(rdb, expiretime == -1 ? 0 : expiretime) == 0) return C_ERR;

        /* Create the DUMP encoded representation. */
        rio payload;
        createDumpPayload(&payload, o, &key, dbid, 1);
        sds buf = payload.io.buffer.ptr;
        if (rioWriteBulkString(rdb, buf, sdslen(buf)) == 0) {
            sdsfree(payload.io.buffer.ptr);
            return C_ERR;
        }
        sdsfree(payload.io.buffer.ptr);

        /* Write ABSTTL */
        if (rioWriteBulkString(rdb, "ABSTTL", 6) == 0) return C_ERR;
    } else {
        /* Use AOF format to import data */
        if (rewriteObject(rdb, &key, o, dbid, expiretime) == C_ERR) return C_ERR;
    }

    return C_OK;
}

/* Modules can use RM_ClusterPropagateForSlotMigration() during the
 * CLUSTER_ASM_MIGRATE_MODULE_PROPAGATE event to propagate commands that should be
 * delivered just before the slot snapshot delivery starts. This function
 * triggers the event, collects the commands and writes them to the rio. */
static int propagateModuleCommands(asmTask *task, rio *rdb) {
    RedisModuleClusterAsmMigrationInfo info = {
            REDISMODULE_CLUSTER_ASM_MIGRATIONINFO_VERSION,
            task->id,
            (RedisModuleSlotRangeArray *) task->slot_ranges
    };

    task->module_commands = zcalloc(sizeof(*task->module_commands));
    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_ASM,
                          REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_MODULE_PROPAGATE,
                          &info
    );

    int ret = C_OK;
    /* Write the module commands to the rio */
    for (int i = 0; i < task->module_commands->numops; i++) {
        redisOp *op = &task->module_commands->ops[i];
        if (rioWriteBulkCount(rdb, '*', op->argc) == 0) {
            ret = C_ERR;
            break;
        }
        for (int j = 0; j < op->argc; j++)
            if (rioWriteBulkObject(rdb, op->argv[j]) == 0) {
                ret = C_ERR;
                break;
            }
    }
    redisOpArrayFree(task->module_commands);
    zfree(task->module_commands);
    task->module_commands = NULL;
    return ret;
}

/* Save the slot ranges snapshot to the file. It generates the DUMP encoded
 * representation of each key in the slot ranges and writes it to the file.
 *
 * Returns C_OK on success, or C_ERR on error. */
int slotSnapshotSaveRio(int req, rio *rdb, int *error) {
    serverAssert(req & SLAVE_REQ_SLOTS_SNAPSHOT);

    dictEntry *de;
    kvstoreDictIterator *kvs_di = NULL;

    if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_RDB_CHANNEL, ASM_SEND_BULK_AND_STREAM)))
        rioAbort(rdb); /* Simulate a failure */

    /* Disable RDB compression for slots snapshot since compression is too
     * expensive both in source and destination. */
    server.rdb_compression = 0;

    /* Only support a single migrate task */
    serverAssert(listLength(asmManager->tasks) == 1);
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    serverAssert(task->operation == ASM_MIGRATE);

    if (propagateModuleCommands(task, rdb) == C_ERR) goto werr;

    /* Dump functions and send to destination side. */
    rio payload;
    createFunctionDumpPayload(&payload);
    sds functions = payload.io.buffer.ptr;
    if (rioWriteBulkCount(rdb, '*', 4) == 0) goto werr;
    if (rioWriteBulkString(rdb, "FUNCTION", 8) == 0) goto werr;
    if (rioWriteBulkString(rdb, "RESTORE", 7) == 0) goto werr;
    if (rioWriteBulkString(rdb, functions, sdslen(functions)) == 0) {
        sdsfree(payload.io.buffer.ptr);
        goto werr;
    }
    sdsfree(payload.io.buffer.ptr);
    /* Add the REPLACE option to the RESTORE command, to avoid error
     * when migrating to a node with existing libraries. */
    if (rioWriteBulkString(rdb, "REPLACE", 7) == 0) goto werr;

    for (int i = 0; i < server.dbnum; i++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db + i;
        if (kvstoreSize(db->keys) == 0) continue;

        /* SELECT the new DB */
        if (rioWrite(rdb,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(rdb, i) == 0) goto werr;

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

                    /* Save a key-value pair */
                    kvobj *o = dictGetKV(de);
                    if (slotSnapshotSaveKeyValuePair(rdb, o, db->id) == C_ERR) goto werr;

                    /* Delay return if required (for testing) */
                    if (unlikely(server.rdb_key_save_delay)) {
                        /* Send buffer to the destination ASAP. */
                        if (rioFlush(rdb) == 0) goto werr;
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

/* Read error handler for sync buffer */
static void asmReadSyncBufferErrorHandler(connection *conn) {
    if (listLength(asmManager->tasks) == 0) return;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    if (task->state != ASM_ACCUMULATE_BUF && task->state != ASM_STREAMING_BUF) return;

    if (task->state == ASM_STREAMING_BUF) {
        freeClient(connGetPrivateData(conn));
    } else {
        asmTaskSetFailed(task, "Main channel - Read error: %s", connGetLastError(conn));
    }
}

/* Read data from connection into sync buffer. */
static void asmSyncBufferReadFromConn(connection *conn) {
    /* The task may be canceled (move to done list) or failed during streaming buffer. */
    if (listLength(asmManager->tasks) == 0) return;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    if (task->state != ASM_ACCUMULATE_BUF && task->state != ASM_STREAMING_BUF) return;

    /* ASM_ACCUMULATE_BUF and ASM_STREAMING_BUF fail points are handled here */
    if (unlikely(asmDebugIsFailPointActive(ASM_IMPORT_MAIN_CHANNEL, task->state)))
        shutdown(conn->fd, SHUT_RDWR);

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
    asmTask *task = context->privdata;
    client *c = context->client;

    char offset[64];
    ull2string(offset, sizeof(offset), context->applied_offset);

    char *err = sendCommand(c->conn, "CLUSTER", "SYNCSLOTS", "ACK",
                    asmTaskStateToString(task->state), offset, NULL);
    if (err) {
        serverLog(LL_WARNING, "Error sending CLUSTER SYNCSLOTS ACK: %s", err);
        sdsfree(err);
        freeClient(c);
    }
    serverLog(LL_DEBUG, "Yielding sending ACK during streaming buffer, applied offset: %zu",
                         context->applied_offset);
}

static int asmSyncBufferStreamShouldContinue(void *ctx) {
    replDataBufToDbCtx *context = ctx;

    /* If the task is failed or canceled, we should stop streaming immediately. */
    asmTask *task = context->privdata;
    if (task->state == ASM_FAILED || task->state == ASM_CANCELED) return 0;

    /* Check the client-close flag only if the task has not failed or been canceled,
     * otherwise the client may have already been freed. */
    if (context->client->flags & CLIENT_CLOSE_ASAP) return 0;

    return 1;
}

/* Stream the sync buffer to the database. */
void asmSyncBufferStreamToDb(asmTask *task) {
    task->state = ASM_STREAMING_BUF;
    serverLog(LL_NOTICE, "Starting to stream accumulated buffer for the import task (%zu bytes)",
                         task->sync_buffer.used);

    /* The buffered stream from the main channel connection into
     * the database is processed by a fake client. */
    client *c = createClient(task->main_channel_conn);
    c->flags |= (CLIENT_MASTER | CLIENT_INTERNAL | CLIENT_ASM_IMPORTING);
    c->querybuf = sdsempty();
    c->authenticated = 1;
    c->user = NULL;
    c->task = task;

    /* Mark the peek buffer block count. We'll use it to verify we consume
     * faster than we read from the source side. */
    task->sync_buffer.last_num_blocks = listLength(task->sync_buffer.blocks);

    /* Continue accumulating during streaming to prevent accumulation on source side. */
    connSetReadHandler(c->conn, asmSyncBufferReadFromConn);

    replDataBufToDbCtx ctx = {
        .privdata = task,
        .client = c,
        .applied_offset = 0,
        .should_continue = asmSyncBufferStreamShouldContinue,
        .yield_callback = asmSyncBufferStreamYieldCallback,
    };

    /* Start streaming the buffer to the DB. This task may fail due to network
     * errors or cancellations. We never release the task immediately; instead,
     * it may be moved to the 'done' list. The actual free happens in serverCron,
     * which ensures there is no use-after-free issue. */
    int ret = replDataBufStreamToDb(&task->sync_buffer, &ctx);

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
            shutdown(task->main_channel_conn->fd, SHUT_RDWR); /* Simulate a failure */

        /* ACK offset after streaming buffer is done. */
        asmImportSendACK(task);
    } else {
        /* If the task is already canceled or failed, we don't need to do anything here. */
        if (task->state == ASM_FAILED || task->state == ASM_CANCELED) return;

        asmTaskSetFailed(task, "Main channel - Failed to stream into the DB");
    }
}

void asmImportIncrAppliedBytes(struct asmTask *task, size_t bytes) {
    serverAssert(task->operation == ASM_IMPORT);
    if (!task || task->state != ASM_WAIT_STREAM_EOF) return;
    task->dest_offset += bytes;
}

/* Send STREAM-EOF if the sync buffer stream is drained. */
void asmSendStreamEofIfDrained(asmTask *task) {
    client *c = task->main_channel_client;

    /* The command streams for slot ranges have been drained. */
    if (!clientHasPendingReplies(c)) {
        serverLog(LL_NOTICE, "Slot migration command stream drained, sending STREAM-EOF to the destination");

        if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_MAIN_CHANNEL, task->state)))
            shutdown(c->conn->fd, SHUT_RDWR);

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

        /* There may be a delay to handle the disconnection of RDB channel,
         * so we clear the task and client references here. */
        if (task->rdb_channel_client != NULL) {
            task->rdb_channel_state = ASM_DONE;
            task->rdb_channel_client->task = NULL;
            freeClientAsync(task->rdb_channel_client);
            task->rdb_channel_client = NULL;
        }

        task->state = ASM_STREAM_DONE;
    }
}

void asmBeforeSleep(void) {
    asmTrimJobProcessPending();

    if (listLength(asmManager->tasks) == 0) return;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));

    if (task->operation == ASM_IMPORT) {
        if (task->state == ASM_NONE)
            asmStartImportTask(task);
        else if (task->state == ASM_READY_TO_STREAM)
            asmSyncBufferStreamToDb(task);
    }

    if (task->operation == ASM_MIGRATE) {
        if (task->state == ASM_HANDOFF) {
            /* To avoid long pause, we fail the task if the pause takes too long. */
            if (server.mstime - task->paused_time >= server.asm_pause_write_timeout) {
                asmTaskSetFailed(task, "Server paused timeout");
                return;
            }
            asmSendStreamEofIfDrained(task);
        } else if (task->state == ASM_STREAM_DONE) {
            /* In state ASM_STREAM_DONE (server is still paused), we are waiting
             * for the destination node to broadcast the slot ownership change.
             * But maybe the destination node is failed or network is not available,
             * the source node may be paused forever. So we fail the task if it
             * takes too long.
             *
             * NOTE: There is a tricky case where the destination node may advertise
             * ownership of the slot, causing a temporary configuration conflict.
             * However, the configuration will eventually converge. In most cases,
             * the destination node becomes the winner, since it bumps its config
             * epoch before taking over slot ownership. */
            if (server.mstime - task->paused_time >= server.asm_pause_write_timeout)
                asmTaskSetFailed(task, "Server paused timeout");
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
            if (asmImportSendACK(task) == C_ERR) return;

            /* Check if the main channel is timed out */
            client *c = connGetPrivateData(task->main_channel_conn);
            serverAssert(c->task == task);
            if (server.unixtime - c->lastinteraction > server.repl_timeout)
                asmTaskSetFailed(task, "Main channel - Connection timeout");
        } else if (task->state == ASM_ACCUMULATE_BUF &&
                   task->rdb_channel_state == ASM_RDBCHANNEL_TRANSFER)
        {
            /* Check if the RDB channel is timed out */
            client *c = connGetPrivateData(task->rdb_channel_conn);
            serverAssert(c->task == task);
            if (server.unixtime - c->lastinteraction > server.repl_timeout)
                asmTaskSetFailed(task, "RDB channel - Connection timeout");
        }
    } else if (task->operation == ASM_MIGRATE) {
        if (task->state == ASM_SEND_STREAM) {
            /* Currently, we only need to check the main channel timeout when sending streams.
             * For RDB channel connections, the timeout is handled by the socket itself
             * during writes in slotSnapshotSaveRio. */
            if (server.unixtime - task->main_channel_client->lastinteraction > server.repl_timeout)
                asmTaskSetFailed(task, "Main channel - Connection timeout");

            /* After the destination applies the accumulated buffer, the source continues
             * sending commands for migrating slots. The destination keeps applying them,
             * but the gap remains above the acceptable limit, which may cause endless
             * synchronization. A timeout check is required to handle this case.
             *
             * The timeout is calculated as the maximum of two values:
             * - A configurable timeout (slot-migration-sync-buffer-drain-timeout) to
             *   avoid false positives.
             * - A dynamic timeout based on the time that the destination took to apply the
             *   slot snapshot and the accumulated buffer during slot snapshot delivery.
             *   The destination should be able to drain the remaining sync buffer in less
             *   time than this. We multiply it by 2 to be more conservative. */
            if (task->dest_state == ASM_WAIT_STREAM_EOF && task->dest_accum_applied_time &&
                server.mstime - task->dest_accum_applied_time >
                    max(server.asm_sync_buffer_drain_timeout,
                        (task->dest_accum_applied_time - task->dest_slots_snapshot_time) * 2))
            {
                asmTaskSetFailed(task, "Sync buffer drain timeout");
            }
        }
    }

    /* Trim the done tasks list if it grows too large */
    while (listLength(asmManager->done_tasks) > (unsigned long)server.asm_max_done_tasks) {
        asmTask *oldest = listNodeValue(listLast(asmManager->done_tasks));
        asmTaskFree(oldest);
        listDelNode(asmManager->done_tasks, listLast(asmManager->done_tasks));
    }
}

/* Cancel a specific task if ID is provided, otherwise cancel all tasks. */
int clusterAsmCancel(const char *task_id, const char *reason) {
    if (asmManager == NULL) return 0;

    if (task_id) {
        asmTask *task = asmLookupTaskById(task_id);
        if (!task) return 0; /* Not found */

        asmTaskCancel(task, reason);
        return 1;
    } else {
        int num_cancelled = 0;
        listIter li;
        listNode *ln;

        listRewind(asmManager->tasks, &li);
        while ((ln = listNext(&li)) != NULL) {
            asmTask *task = listNodeValue(ln);
            asmTaskCancel(task, reason);
            num_cancelled++;
        }
        return num_cancelled;
    }
}

/* Cancel all tasks that overlap with the given slot ranges.
 * If slot_ranges is NULL, cancel all tasks. */
int clusterAsmCancelBySlotRangeArray(struct slotRangeArray *slot_ranges, const char *reason) {
    if (asmManager == NULL) return 0;

    int num_cancelled = 0;
    listIter li;
    listNode *ln;
    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (!slot_ranges || slotRangeArraysOverlap(task->slot_ranges, slot_ranges)) {
            asmTaskCancel(task, reason);
            num_cancelled++;
        }
    }
    return num_cancelled;
}

/* Cancel the task that overlap with the given slot. */
int clusterAsmCancelBySlot(int slot, const char *reason) {
    slotRange req = {slot, slot};
    if (asmManager == NULL) return 0;

    /* Cancel it if found. */
    asmTask *task = lookupAsmTaskBySlotRange(&req);
    if (task) asmTaskCancel(task, reason);

    return task ? 1 : 0;
}

/* Cancel all tasks that involve the given node. */
int clusterAsmCancelByNode(void *node, const char *reason) {
    if (asmManager == NULL || node == NULL) return 0;

    /* If the node to be deleted is myself, cancel all tasks. */
    clusterNode *n = node;
    if (n == getMyClusterNode()) return clusterAsmCancel(NULL, reason);

    int num_cancelled = 0;
    listIter li;
    listNode *ln;
    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        /* Cancel the task if the source node is the one to be deleted, or
         * the dest node is the one to be deleted. */
        if (task->source_node == n ||
            !memcmp(task->dest, clusterNodeGetName(n), CLUSTER_NAMELEN) ||
            !memcmp(task->source, clusterNodeGetName(n), CLUSTER_NAMELEN))
        {
            asmTaskCancel(task, reason);
            num_cancelled++;
        }
    }
    return num_cancelled;
}

/* Check if the slot is in an active ASM task. */
int isSlotInAsmTask(int slot) {
    slotRange req = {slot, slot};
    if (!asmManager) return 0;

    listIter li;
    listNode *ln;
    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (slotRangeArrayOverlaps(task->slot_ranges, &req))
            return 1;
    }
    return 0;
}

/* Check if the slot is in a pending trim job. It may happen if we can't trim
 * the slots immediately due to a write pause or when active trim is in progress. */
int isSlotInTrimJob(int slot) {
    slotRange req = {slot, slot};

    if (!asmManager || !asmIsTrimInProgress()) return 0;

    /* Check if the slot is in any pending trim job. */
    listIter li;
    listNode *ln;
    listRewind(asmManager->pending_trim_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRangeArray *slots = listNodeValue(ln);
        if (slotRangeArrayOverlaps(slots, &req))
            return 1;
    }

    /* Check if the slot is in any active trim job. */
    listRewind(asmManager->active_trim_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRangeArray *slots = listNodeValue(ln);
        if (slotRangeArrayOverlaps(slots, &req))
            return 1;
    }
    return 0;
}

int clusterAsmHandoff(const char *task_id, sds *err) {
    serverAssert(task_id);

    asmTask *task = asmLookupTaskById(task_id);
    if (!task || task->state != ASM_HANDOFF_PREP) {
        *err = sdscatprintf(sdsempty(), "No suitable ASM task found for id: %s, task_state: %s",
                            task_id, task ? asmTaskStateToString(task->state) : "null");
        return C_ERR;
    }

    task->state = ASM_HANDOFF;
    task->paused_time = server.mstime;

    return C_OK;
}

/* Notify Redis that the config is updated for the task. */
int asmNotifyConfigUpdated(asmTask *task, sds *err) {
    int event = -1;

    if (task->operation == ASM_IMPORT && task->state == ASM_TAKEOVER) {
        event = ASM_EVENT_IMPORT_COMPLETED;
    } else if (task->operation == ASM_MIGRATE && task->state == ASM_STREAM_DONE) {
        event = ASM_EVENT_MIGRATE_COMPLETED;
    } else {
        *err = sdscatprintf(sdsempty(),
                            "ASM task is not in the correct state for config update: %s",
                            asmTaskStateToString(task->state));
        asmTaskCancel(task, "slots configuration updated");
        return C_ERR;
    }

    /* Clear error message if successful. */
    sdsfree(task->error);
    task->error = sdsempty();
    task->state = ASM_DONE;

    asmNotifyStateChange(task, event);
    asmTaskComplete(task);

    /* Trim the slots after the migrate task is done. */
    if (event == ASM_EVENT_MIGRATE_COMPLETED)
        asmTrimJobSchedule(task->slot_ranges);

    return C_OK;
}

/* Import/Migrate task is done, config is updated. */
int clusterAsmDone(const char *task_id, sds *err) {
    serverAssert(task_id);

    asmTask *task = asmLookupTaskById(task_id);
    if (!task) {
        *err = sdscatprintf(sdsempty(), "No ASM task found for id: %s", task_id);
        return C_ERR;
    }
    return asmNotifyConfigUpdated(task, err);
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
            num_cancelled = clusterAsmCancel(task_id, "user request");
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

/* Check if we can propagate TRIMSLOTS command to AOF and replicas. */
static int canPropagateTrimSlots(void) {
    return !isPausedActions(PAUSE_ACTION_CLIENT_WRITE) &&
           !isPausedActions(PAUSE_ACTION_CLIENT_ALL) &&
           !isPausedActions(PAUSE_ACTION_REPLICA);
}

/* Propagate TRIMSLOTS command to AOF and replicas. */
static void propagateTrimSlots(slotRangeArray *slots) {
    int argc = slots->num_ranges * 2 + 3;
    robj **argv = zmalloc(sizeof(robj*) * argc);
    argv[0] = createStringObject("TRIMSLOTS", 9);
    argv[1] = createStringObject("RANGES", 6);
    argv[2] = createStringObjectFromLongLong(slots->num_ranges);
    for (int i = 0; i < slots->num_ranges; i++) {
        argv[i*2+3] = createStringObjectFromLongLong(slots->ranges[i].start);
        argv[i*2+4] = createStringObjectFromLongLong(slots->ranges[i].end);
    }

    enterExecutionUnit(1, 0);

    int prev_replication_allowed = server.replication_allowed;
    server.replication_allowed = 1;
    alsoPropagate(-1, argv, argc, PROPAGATE_AOF | PROPAGATE_REPL);
    server.replication_allowed = prev_replication_allowed;

    exitExecutionUnit();
    postExecutionUnitOperations();

    for (int i = 0; i < argc; i++)
        decrRefCount(argv[i]);
    zfree(argv);
}

/* Trim the slots asynchronously in the BIO thread. */
void asmTriggerBackgroundTrim(slotRangeArray *slots) {
    RedisModuleClusterAsmTrimInfoV1 fsi = {
            REDISMODULE_CLUSTER_ASM_TRIMINFO_VERSION,
            (RedisModuleSlotRangeArray *) slots
    };

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_ASM_TRIM,
                          REDISMODULE_SUBEVENT_CLUSTER_ASM_TRIM_BACKGROUND,
                          &fsi);
    /* TODO: This is going to send invalidation message for all the keys for
     * the tracking clients. We need to consider how we can do that for only
     * the keys in the slots we are trimming. */
    signalFlushedDb(0, 1, slots);

    /* Create temp kvstores and estore, move relevant slot dicts/ebuckets into them,
     * and delete them in BIO thread asynchronously. */
    kvstore *keys = kvstoreCreate(&dbDictType,
                                  CLUSTER_SLOT_MASK_BITS,
                                  KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    kvstore *expires = kvstoreCreate(&dbExpiresDictType,
                                     CLUSTER_SLOT_MASK_BITS,
                                     KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    estore *subexpires = estoreCreate(&subexpiresBucketsType, CLUSTER_SLOT_MASK_BITS);

    for (int i = 0; i < slots->num_ranges; i++) {
        for (int slot = slots->ranges[i].start; slot <= slots->ranges[i].end; slot++) {
            kvstoreMoveDict(server.db[0].keys, keys, slot);
            kvstoreMoveDict(server.db[0].expires, expires, slot);
            estoreMoveEbuckets(server.db[0].subexpires, subexpires, slot);
        }
    }

    emptyDbDataAsync(keys, expires, subexpires);

    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Background trim started for slots: %s", str);
    sdsfree(str);
}

/* Trim the slots. */
void asmTrimSlots(slotRangeArray *slots) {
    if (asmManager->debug_trim_method == ASM_DEBUG_TRIM_NONE)
        return;

    /* TODO: Is that two event enough or shall we check for zero subscribers? */
    int activetrim = (asmManager->debug_trim_method == ASM_DEBUG_TRIM_ACTIVE) ||
                     (asmManager->debug_trim_method == ASM_DEBUG_TRIM_DEFAULT &&
                      moduleHasSubscribersForKeyspaceEvent(NOTIFY_GENERIC | NOTIFY_TRIMMED));
    if (activetrim)
        asmTriggerActiveTrim(slots);
    else
        asmTriggerBackgroundTrim(slots);
}

int asmTrimJobIsPending(void) {
    return listLength(asmManager->pending_trim_jobs);
}

/* Schedule a trim job for the specified slot ranges. The job will be
 * deferred and handled later in asmBeforeSleep(). We delay the trim jobs to
 * asmBeforeSleep() to ensure it only runs when there is no write pause.
 * Attempting to process it during a write pause could trigger an assertion
 * in propagateNow(), as propagation is not allowed during a write pause. */
void asmTrimJobSchedule(slotRangeArray *slots) {
    listAddNodeTail(asmManager->pending_trim_jobs, slotRangeArrayDup(slots));
}

/* Process any pending trim jobs. */
void asmTrimJobProcessPending(void) {
    /* Check if there is any pending trim job and we can propagate it. */
    if (!asmTrimJobIsPending() || !canPropagateTrimSlots())
        return;

    listIter li;
    listNode *ln;
    listRewind(asmManager->pending_trim_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRangeArray *sra = listNodeValue(ln);
        asmTrimSlots(sra);
        propagateTrimSlots(sra);
        listDelNode(asmManager->pending_trim_jobs, ln);
        slotRangeArrayFree(sra);
    }
}

/* Trim keys in slots not owned by this node (if any). */
void asmTrimSlotsIfNotOwned(void) {
    if (!server.cluster_enabled || server.masterhost != NULL) return;

    slotRangeArray *sra = NULL;
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (clusterIsMySlot(i)) continue;
        if (kvstoreDictSize(server.db[0].keys, i) == 0) continue;
        sra = slotRangeArrayAppend(sra, i);
    }
    if (!sra) return;

    sds str = slotRangeArrayToString(sra);
    serverLog(LL_NOTICE,
              "Detected keys in slots that does not belong to this node. "
              "Scheduling trim for slots: %s", str);
    sdsfree(str);

    asmTrimJobSchedule(sra);
    slotRangeArrayFree(sra);
}

/* If this node is a replica and there is an active trim job, we cannot
 * process commands from the master for the slot being trimmed. Otherwise,
 * the trim cycle could mistakenly delete newly added keys. In this case,
 * the master will be blocked until the trim job finishes. */
void asmUnblockMasterAfterTrim(void) {
    if (server.master &&
        server.master->flags & CLIENT_BLOCKED &&
        server.master->bstate.btype == BLOCKED_POSTPONE_TRIM)
    {
        unblockClient(server.master, 1);
        serverLog(LL_NOTICE, "Unblocking master client after active trim is done");
    }
}

/* Cancel all pending and active trim jobs. */
void asmCancelTrimJobs(void) {
    if (!asmManager) return;

    /* Unblock master if blocked */
    asmUnblockMasterAfterTrim();

    /* Cancel pending trim jobs */
    listIter li;
    listNode *ln;
    listRewind(asmManager->pending_trim_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRangeArray *sra = listNodeValue(ln);
        listDelNode(asmManager->pending_trim_jobs, ln);
        slotRangeArrayFree(sra);
    }

    /* Cancel active trim jobs */
    if (listLength(asmManager->active_trim_jobs) == 0)
        return;

    serverLog(LL_NOTICE, "Cancelling all active trim jobs");
    asmManager->active_trim_cancelled += listLength(asmManager->active_trim_jobs);
    asmActiveTrimEnd(0);
    listEmpty(asmManager->active_trim_jobs);
}

/* It's used to trim slots after the migration is done or import is failed.
 * TRIMSLOTS RANGES <numranges> <start-slot> <end-slot> ... */
void trimslotsCommand(client *c) {
    long numranges = 0;

    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (c->argc < 5) {
        addReplyErrorArity(c);
        return;
    }

    /* Validate the ranges argument */
    if (strcasecmp(c->argv[1]->ptr, "ranges") != 0) {
        addReplyError(c, "missing ranges argument");
        return;
    }

    /* Get the number of ranges */
    if (getLongFromObjectOrReply(c, c->argv[2], &numranges, NULL) != C_OK)
        return;

    /* Validate the number of ranges and argument count */
    if (numranges < 1 || numranges > CLUSTER_SLOTS || c->argc != 3 + numranges * 2) {
        addReplyError(c, "invalid number of ranges");
        return;
    }

    /* Parse the slot ranges and start trimming */
    slotRangeArray *slots = parseSlotRangesOrReply(c, c->argc, 3);
    if (!slots) return;

    if (c->id == CLIENT_ID_AOF) {
        serverAssert(server.loading);
        /* If we are loading the AOF, we can't trigger active trim because next
         * command may have an update for the same key that is supposed to be
         * trimmed. We have to trim the keys synchronously. */
        clusterDelKeysInSlotRangeArray(slots, 1);
    } else {
        asmTrimSlots(slots);
    }

    /* Command will not be propagated automatically since it does not modify
     * the dataset. */
    forceCommandPropagation(c, PROPAGATE_REPL | PROPAGATE_AOF);

    slotRangeArrayFree(slots);
    addReply(c, shared.ok);
}

/* Start the active trim job. */
void asmActiveTrimStart(void) {
    if (listLength(asmManager->active_trim_jobs) != 1) return;

    slotRangeArray *slots = listNodeValue(listFirst(asmManager->active_trim_jobs));

    serverAssert(asmManager->active_trim_it == NULL);
    asmManager->active_trim_it = slotRangeArrayGetIterator(slots);
    asmManager->active_trim_started++;
    asmManager->active_trim_keys_deleted = 0;

    /* Count the number of keys to trim */
    for (int i = 0; i < slots->num_ranges; i++)  {
        for (int slot = slots->ranges[i].start; slot <= slots->ranges[i].end; slot++)
            asmManager->active_trim_keys_total += kvstoreDictSize(server.db[0].keys, slot);
    }        

    RedisModuleClusterAsmTrimInfoV1 fsi = {
            REDISMODULE_CLUSTER_ASM_TRIMINFO_VERSION,
            (RedisModuleSlotRangeArray *) slots
    };

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_ASM_TRIM,
                          REDISMODULE_SUBEVENT_CLUSTER_ASM_TRIM_STARTED,
                          &fsi);

    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Active trim initiated for slots: %s", str);
    sdsfree(str);
}

/* Schedule an active trim job. */
void asmTriggerActiveTrim(slotRangeArray *slots) {
    listAddNodeTail(asmManager->active_trim_jobs, slotRangeArrayDup(slots));
    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Active trim scheduled for slots: %s", str);
    sdsfree(str);
    asmActiveTrimStart();
}

/* End the active trim job. */
void asmActiveTrimEnd(int start_next_job) {
    slotRangeArray *slots = listNodeValue(listFirst(asmManager->active_trim_jobs));

    if (asmManager->active_trim_it) {
        slotRangeArrayIteratorFree(asmManager->active_trim_it);
        asmManager->active_trim_it = NULL;
    }

    /* Unblock the master if it is blocked */
    asmUnblockMasterAfterTrim();

    RedisModuleClusterAsmTrimInfoV1 fsi = {
            REDISMODULE_CLUSTER_ASM_TRIMINFO_VERSION,
            (RedisModuleSlotRangeArray *) slots
    };

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_ASM_TRIM,
                          REDISMODULE_SUBEVENT_CLUSTER_ASM_TRIM_COMPLETED,
                          &fsi);

    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Active trim completed for slots: %s, %llu keys trimmed.",
              str, asmManager->active_trim_keys_deleted);
    sdsfree(str);
    listDelNode(asmManager->active_trim_jobs, listFirst(asmManager->active_trim_jobs));

    if (start_next_job) asmActiveTrimStart();
}

/* Check if the slot range array overlaps with any trim job. */
int asmIsAnyTrimJobOverlaps(slotRangeArray *slots) {
    if (!asmIsTrimInProgress()) return 0;
    for (int i = 0; i < slots->num_ranges; i++) {
        for (int j = slots->ranges[i].start; j <= slots->ranges[i].end; j++) {
            if (isSlotInTrimJob(j)) return 1;
        }
    }
    return 0;
}

/* Check if there is any trim job in progress. */
int asmIsTrimInProgress(void) {
    if (!server.cluster_enabled) return 0;
    return (listLength(asmManager->active_trim_jobs) != 0 ||
            listLength(asmManager->pending_trim_jobs) != 0);
}

/* Delete the key and notify the modules. */
void asmActiveTrimDeleteKey(redisDb *db, robj *keyobj) {
    if (asmManager->debug_active_trim_delay > 0)
        debugDelay(asmManager->debug_active_trim_delay);

    /* The key needs to be converted from static to heap before deleted */
    int static_key = keyobj->refcount == OBJ_STATIC_REFCOUNT;
    if (static_key) keyobj = createStringObject(keyobj->ptr, sdslen(keyobj->ptr));

    dbDelete(db, keyobj);
    notifyKeyspaceEvent(NOTIFY_TRIMMED, "trimmed", keyobj, db->id);
    asmManager->active_trim_keys_deleted++;

    if (static_key) decrRefCount(keyobj);
}

/* Trim keys in the active trim job. */
void asmActiveTrimCycle(int type) {
    if (asmManager->debug_active_trim_delay < 0 ||
        listLength(asmManager->active_trim_jobs) == 0 ||
        isPausedActions(PAUSE_ACTIONS_CLIENT_WRITE_SET) ||
        isPausedActions(PAUSE_ACTION_CLIENT_WRITE))
    {
        return;
    }

    /* This works in a similar way to activeExpireCycle, in the sense that
     * we do incremental work across calls. */
    static long long last_fast_cycle = 0; /* When last fast cycle ran. */
    long long start = ustime(), timelimit;

    /* See activeExpireCycle for how timelimit is handled. */
    timelimit = 1000000 * server.asm_trim_slow_cycle_time_perc / server.hz / 100;
    if (timelimit <= 0) timelimit = 1;
    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        if (start < last_fast_cycle + server.asm_trim_fast_cycle_duration * 2 ||
            !server.asm_trim_fast_cycle_duration)
            return;
        last_fast_cycle = start;
        timelimit = server.asm_trim_fast_cycle_duration; /* in microseconds. */
    }

    unsigned long long num_deleted = 0;
    int time_exceeded = 0;
    int slot = slotRangeArrayGetCurrentSlot(asmManager->active_trim_it);

    while (!time_exceeded && slot != -1) {
        dictEntry *de;
        kvstoreDictIterator *kvs_di = kvstoreGetDictSafeIterator(server.db[0].keys, slot);
        while ((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
            kvobj *kv = dictGetKV(de);
            sds sdskey = kvobjGetKey(kv);

            enterExecutionUnit(1, 0);
            robj *keyobj = createStringObject(sdskey, sdslen(sdskey));
            asmActiveTrimDeleteKey(&server.db[0], keyobj);
            decrRefCount(keyobj);
            exitExecutionUnit();
            postExecutionUnitOperations();
            num_deleted++;

            /* Once in 32 deletions check if we reached the time limit. */
            if (num_deleted % 32 == 0 && (ustime() - start) > timelimit) {
                time_exceeded = 1;
                break;
            }
        }
        kvstoreReleaseDictIterator(kvs_di);
        if (!time_exceeded) slot = slotRangeArrayNext(asmManager->active_trim_it);
    }

    if (slot == -1) {
#if defined(USE_JEMALLOC)
        jemalloc_purge();
#endif
        asmActiveTrimEnd(1);
    }

    long long elapsed = ustime() - start;
    latencyAddSampleIfNeeded(type == ACTIVE_EXPIRE_CYCLE_FAST ?
                            "trim-cycle-fast": "trim-cycle-slow", elapsed / 1000);
}

/* Trim a specific key if trimming is pending or in progress for its slot.
 * Return 1 if the key was trimmed */
int asmActiveTrimDelIfNeeded(redisDb *db, robj *key, kvobj *kv) {
    sds keyname = key ? key->ptr : kvobjGetKey(kv);
    if (server.allow_access_trimmed ||
        !asmIsTrimInProgress() ||
        !isSlotInTrimJob(getKeySlot(keyname)))
    {
        return 0;
    }

    if (key) {
        asmActiveTrimDeleteKey(db, key);
    } else {
        robj *tmpkey = createStringObject(keyname, sdslen(keyname));
        asmActiveTrimDeleteKey(db, tmpkey);
        decrRefCount(tmpkey);
    }
    return 1;
}

/* Modules can use RM_ClusterPropagateForSlotMigration() during the
 * CLUSTER_ASM_MIGRATE_MODULE_PROPAGATE event to propagate commands that should be
 * delivered just before the slot snapshot delivery starts. */
int asmModulePropagateBeforeSlotSnapshot(struct redisCommand *cmd, robj **argv, int argc) {
    /* This API is only called in the fork child. */
    if (server.cluster_enabled == 0 ||
        server.in_fork_child != CHILD_TYPE_RDB ||
        listLength(asmManager->tasks) == 0)
    {
        errno = EBADF;
        return C_ERR;
    }

    /* Check if the task state is right. */
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    if (task->operation != ASM_MIGRATE ||
        task->state != ASM_SEND_BULK_AND_STREAM ||
        task->module_commands == NULL)
    {
        errno = EBADF;
        return C_ERR;
    }

    /* Ensure all arguments are converted to string encoding if necessary,
     * since getSlotFromCommand expects them to be string-encoded. */
    for (int i = 0; i < argc; i++) {
        if (!sdsEncodedObject(argv[i])) {
            serverAssert(argv[i]->encoding == OBJ_ENCODING_INT);
            robj *old = argv[i];
            argv[i] = createStringObjectFromLongLongWithSds((long)old->ptr);
            decrRefCount(old);
        }
    }

    /* Crossslot commands are not allowed */
    int slot = getSlotFromCommand(cmd, argv, argc);
    if (slot == GETSLOT_CROSSSLOT) {
        errno = ENOTSUP;
        return C_ERR;
    }

    /* Allow no-keys commands or if keys are in the slot range. */
    slotRange sr = {slot, slot};
    if (slot != GETSLOT_NOKEYS && !slotRangeArrayOverlaps(task->slot_ranges, &sr)) {
        errno = ERANGE;
        return C_ERR;
    }

    robj **argvcopy = zmalloc(sizeof(robj*) * argc);
    for (int i = 0; i < argc; i++) {
        argvcopy[i] = argv[i];
        incrRefCount(argv[i]);
    }

    redisOpArrayAppend(task->module_commands, 0, argvcopy, argc, 0);
    return C_OK;
}
