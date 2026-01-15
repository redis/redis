/* cluster_asm.c -- Atomic slot migration implementation for cluster
 *
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
#include "cluster_asm.h"
#include "cluster_slot_stats.h"

#define ASM_IMPORT  (1 << 1)
#define ASM_MIGRATE (1 << 2)

#define ASM_DEBUG_TRIM_DEFAULT 0
#define ASM_DEBUG_TRIM_NONE 1
#define ASM_DEBUG_TRIM_BG 2
#define ASM_DEBUG_TRIM_ACTIVE 3

#define ASM_AOF_MIN_ITEMS_PER_KEY 512 /* Minimum number of items per key to use AOF format encoding */

typedef struct asmTask {
    sds id;                                 /* Task ID */
    int operation;                          /* Either ASM_IMPORT or ASM_MIGRATE */
    slotRangeArray *slots;                  /* List of slot ranges for this migration task */
    int state;                              /* Current state of the task */
    int dest_state;                         /* Destination node's main state (approximate) */
    char source[CLUSTER_NAMELEN];           /* Source node name */
    char dest[CLUSTER_NAMELEN];             /* Destination node name */
    clusterNode *source_node;               /* Source node */
    connection *main_channel_conn;          /* Main channel connection */
    connection *rdb_channel_conn;           /* RDB channel connection */
    int rdb_channel_state;                  /* State of the RDB channel */
    unsigned long long dest_offset;         /* Destination offset */
    unsigned long long source_offset;       /* Source offset */
    int cross_slot_during_propagating;      /* If cross-slot commands are encountered during propagating */
    int stream_eof_during_streaming;        /* If STREAM-EOF is received during streaming buffer */
    replDataBuf sync_buffer;                /* Buffer for the stream */
    client *main_channel_client;            /* Client for the main channel on the source side */
    client *rdb_channel_client;             /* Client for the RDB channel on the source side */
    long long retry_count;                  /* Number of retries for this task */
    mstime_t create_time;                   /* Task creation time */
    mstime_t start_time;                    /* Task start time */
    mstime_t end_time;                      /* Task end time */
    mstime_t paused_time;                   /* The time when the slot writes were paused */
    mstime_t dest_slots_snapshot_time;      /* The time when the destination starts applying the slot snapshot */
    mstime_t dest_accum_applied_time;       /* The time when the destination finishes applying the accumulated buffer */
    sds error;                              /* Error message for this task */
    redisOpArray *pre_snapshot_module_cmds; /* Module commands to be propagated at the beginning of slot migration */
} asmTask;

struct asmManager {
    list *tasks;                        /* List of asmTask to be processed */
    list *archived_tasks;               /* List of archived asmTask */
    list *pending_trim_jobs;            /* List of pending trim jobs (due to write pause) */
    list *active_trim_jobs;             /* List of active trim jobs */
    slotRangeArrayIter *active_trim_it; /* Iterator of the current active trim job */
    size_t sync_buffer_peak;            /* Peak size of sync buffer */
    asmTask *master_task;               /* The task that is currently active on the master */

    /* Fail point injection for debugging */
    int debug_fail_channel;       /* Channel where the task will fail */
    int debug_fail_state;         /* State where the task will fail */
    int debug_trim_method;        /* Method to trim the buffer */
    int debug_active_trim_delay;  /* Sleep before trimming each key */

    /* Active trim stats */
    unsigned long long active_trim_started;             /* Number of times active trim was started */
    unsigned long long active_trim_completed;           /* Number of times active trim was completed */
    unsigned long long active_trim_cancelled;           /* Number of times active trim was cancelled */
    unsigned long long active_trim_current_job_keys;    /* Total number of keys to trim in the current job */
    unsigned long long active_trim_current_job_trimmed; /* Number of keys trimmed in the current job */
};

enum asmState {
    /* Common state */
    ASM_NONE = 0,
    ASM_CONNECTING,
    ASM_AUTH_REPLY,
    ASM_CANCELED,
    ASM_FAILED,
    ASM_COMPLETED,

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
    ASM_STREAM_EOF,

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
void asmCancelPendingTrimJobs(void);
void asmTriggerActiveTrim(slotRangeArray *slots);
void asmActiveTrimEnd(void);
int asmIsAnyTrimJobOverlaps(slotRangeArray *slots);
void asmTrimSlotsIfNotOwned(slotRangeArray *slots);
void asmNotifyStateChange(asmTask *task, int event);

void asmInit(void) {
    asmManager = zcalloc(sizeof(*asmManager));
    asmManager->tasks = listCreate();
    asmManager->archived_tasks = listCreate();
    asmManager->pending_trim_jobs = listCreate();
    asmManager->sync_buffer_peak = 0;
    asmManager->master_task = NULL;
    asmManager->debug_fail_channel = -1;
    asmManager->debug_fail_state = -1;
    asmManager->debug_trim_method = ASM_DEBUG_TRIM_DEFAULT;
    asmManager->debug_active_trim_delay = 0;
    asmManager->active_trim_jobs = listCreate();
    asmManager->active_trim_started = 0;
    asmManager->active_trim_completed = 0;
    asmManager->active_trim_cancelled = 0;
    listSetFreeMethod(asmManager->active_trim_jobs, slotRangeArrayFreeGeneric);
}

char *asmTaskStateToString(int state) {
    switch (state) {
        case ASM_NONE: return "none";
        case ASM_CONNECTING: return "connecting";
        case ASM_AUTH_REPLY: return "auth-reply";
        case ASM_CANCELED: return "canceled";
        case ASM_FAILED: return "failed";
        case ASM_COMPLETED: return "completed";

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
        case ASM_STREAM_EOF: return "stream-eof";

        /* RDB channel state */
        case ASM_RDBCHANNEL_REQUEST: return "rdbchannel-request";
        case ASM_RDBCHANNEL_REPLY: return "rdbchannel-reply";
        case ASM_RDBCHANNEL_TRANSFER: return "rdbchannel-transfer";

        default: return "unknown";
    }
    serverAssert(0); /* Unreachable */
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

int asmDebugSetFailPoint(char *channel, char *state) {
    if (!asmManager) {
        serverLog(LL_WARNING, "ASM manager is not initialized");
        return C_ERR;
    }
    asmManager->debug_fail_channel = -1;
    asmManager->debug_fail_state = -1;
    if (!channel && !state) return C_ERR;
    if (sdslen(channel) == 0 && sdslen(state) == 0) {
        serverLog(LL_WARNING, "ASM fail point is cleared");
        return C_OK;
    }

    for (int i = ASM_IMPORT_MAIN_CHANNEL; i <= ASM_MIGRATE_RDB_CHANNEL; i++) {
        if (!strcasecmp(channel, asmChannelToString(i))) {
            asmManager->debug_fail_channel = i;
            break;
        }
    }
    if (asmManager->debug_fail_channel == -1) return C_ERR;

    for (int i = ASM_NONE; i <= ASM_RDBCHANNEL_TRANSFER; i++) {
        if (!strcasecmp(state, asmTaskStateToString(i))) {
            asmManager->debug_fail_state = i;
            break;
        }
    }
    if (asmManager->debug_fail_state == -1) return C_ERR;

    serverLog(LL_NOTICE, "ASM fail point set: channel=%s, state=%s", channel, state);
    return C_OK;
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
    if (asmManager->debug_fail_channel == channel && asmManager->debug_fail_state == state) {
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
                        "cluster_slot_migration_active_tasks:%d\r\n"
                        "cluster_slot_migration_active_trim_running:%lu\r\n"
                        "cluster_slot_migration_active_trim_current_job_keys:%llu\r\n"
                        "cluster_slot_migration_active_trim_current_job_trimmed:%llu\r\n"
                        "cluster_slot_migration_stats_active_trim_started:%llu\r\n"
                        "cluster_slot_migration_stats_active_trim_completed:%llu\r\n"
                        "cluster_slot_migration_stats_active_trim_cancelled:%llu\r\n",
                        active_tasks,
                        listLength(asmManager->active_trim_jobs),
                        asmManager->active_trim_current_job_keys,
                        asmManager->active_trim_current_job_trimmed,
                        asmManager->active_trim_started,
                        asmManager->active_trim_completed,
                        asmManager->active_trim_cancelled);
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
    task->cross_slot_during_propagating = 0;
    replDataBufInit(&task->sync_buffer);
    task->main_channel_client = NULL;
    task->rdb_channel_client = NULL;
    task->paused_time = 0;
    task->dest_slots_snapshot_time = 0;
    task->dest_accum_applied_time = 0;
    task->pre_snapshot_module_cmds = NULL;
}

asmTask *asmTaskCreate(const char *task_id) {
    asmTask *task = zcalloc(sizeof(*task));
    task->error = sdsempty();
    asmTaskReset(task);
    task->slots = NULL;
    task->source_node = NULL;
    task->retry_count = 0;
    task->create_time = server.mstime;
    task->start_time = -1;
    task->end_time = -1;
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
    slotRangeArrayFree(task->slots);
    sdsfree(task->error);
    zfree(task);
}

/* Convert the task state to the corresponding event. */
int asmTaskStateToEvent(asmTask *task) {
    if (task->operation == ASM_IMPORT) {
        if (task->state == ASM_COMPLETED) return ASM_EVENT_IMPORT_COMPLETED;
        else if (task->state == ASM_FAILED) return ASM_EVENT_IMPORT_FAILED;
        else return ASM_EVENT_IMPORT_STARTED;
    } else {
        if (task->state == ASM_COMPLETED) return ASM_EVENT_MIGRATE_COMPLETED;
        else if (task->state == ASM_FAILED) return ASM_EVENT_MIGRATE_FAILED;
        else return ASM_EVENT_MIGRATE_STARTED;
    }
}

/* Serialize ASM task information into a string for transmission to replicas.
 * Format: "task_id:source_node:dest_node:operation:state:slot_ranges"
 * Where slot_ranges is in the format "1000-2000 3000-4000 ..." */
sds asmTaskSerialize(asmTask *task) {
    sds serialized = sdsempty();

    /* Add task ID */
    serialized = sdscatprintf(serialized, "%s:", task->id);

    /* Add source node ID (40 chars) */
    serialized = sdscatlen(serialized, task->source, CLUSTER_NAMELEN);
    serialized = sdscat(serialized, ":");

    /* Add destination node ID (40 chars) */
    serialized = sdscatlen(serialized, task->dest, CLUSTER_NAMELEN);
    serialized = sdscat(serialized, ":");

    /* Add operation type */
    serialized = sdscatprintf(serialized, "%s:", task->operation == ASM_IMPORT ?
                                                 "import" : "migrate");

    /* Add current state */
    serialized = sdscatprintf(serialized, "%s:", asmTaskStateToString(task->state));

    /* Add slot ranges sds */
    sds slots_str = slotRangeArrayToString(task->slots);
    serialized = sdscatprintf(serialized, "%s", slots_str);
    sdsfree(slots_str);

    return serialized;
}

/* Deserialize ASM task information from a string and create a complete asmTask.
 * Format: "task_id:source_node:dest_node:operation:state:slot_ranges"
 * Returns a new asmTask on success, NULL on failure. */
asmTask *asmTaskDeserialize(sds data) {
    int count, idx = 0;
    asmTask *task = NULL;
    if (!data || sdslen(data) == 0) return NULL;

    sds *parts = sdssplitlen(data, sdslen(data), ":", 1, &count);
    if (count < 6) goto err;

    /* Parse task ID */
    if (sdslen(parts[idx]) == 0) goto err;
    task = asmTaskCreate(parts[idx]);
    if (!task) goto err;
    idx++;

    /* Parse source node ID */
    if (sdslen(parts[idx]) != CLUSTER_NAMELEN) goto err;
    memcpy(task->source, parts[idx], CLUSTER_NAMELEN);
    idx++;

    /* Parse destination node ID */
    if (sdslen(parts[idx]) != CLUSTER_NAMELEN) goto err;
    memcpy(task->dest, parts[idx], CLUSTER_NAMELEN);
    idx++;

    /* Parse operation type */
    if (!strcasecmp(parts[idx], "import")) {
        task->operation = ASM_IMPORT;
    } else if (!strcasecmp(parts[idx], "migrate")) {
        task->operation = ASM_MIGRATE;
    } else {
        goto err;
    }
    idx++;

    /* Parse state */
    task->state = ASM_NONE; /* Default state */
    for (int state = ASM_NONE; state <= ASM_RDBCHANNEL_TRANSFER; state++) {
        if (!strcasecmp(parts[idx], asmTaskStateToString(state))) {
            task->state = state;
            break;
        }
    }
    idx++;

    /* Parse slot ranges */
    task->slots = slotRangeArrayFromString(parts[idx]);
    if (!task->slots) goto err;
    idx++;

    /* Ignore any extra fields for future compatibility */

    sdsfreesplitres(parts, count);
    return task;

err:
    if (task) asmTaskFree(task);
    sdsfreesplitres(parts, count);
    return NULL;
}

/* Notify replicas about ASM task information to maintain consistency during
 * slot migration. This function sends a CLUSTER SYNCSLOTS CONF ASM-TASK command
 * to all connected replicas with the serialized task information. */
void asmNotifyReplicasStateChange(struct asmTask *task) {
    if (!server.cluster_enabled || !clusterNodeIsMaster(getMyClusterNode())) return;

    /* Create command arguments for CLUSTER SYNCSLOTS CONF ASM-TASK */
    robj *argv[5];
    argv[0] = createStringObject("CLUSTER", 7);
    argv[1] = createStringObject("SYNCSLOTS", 9);
    argv[2] = createStringObject("CONF", 4);
    argv[3] = createStringObject("ASM-TASK", 8);
    argv[4] = createObject(OBJ_STRING, asmTaskSerialize(task));

    /* Send the command to all replicas */
    replicationFeedSlaves(server.slaves, -1, argv, 5);

    /* Clean up command objects */
    for (int i = 0; i < 5; i++) {
        decrRefCount(argv[i]);
    }
}

/* Dump the active import ASM task information. */
sds asmDumpActiveImportTask(void) {
    if (!server.cluster_enabled) return NULL;

    /* For replica, dump the master active task. */
    if (clusterNodeIsSlave(getMyClusterNode()) &&
        asmManager->master_task &&
        asmManager->master_task->state != ASM_FAILED &&
        asmManager->master_task->state != ASM_COMPLETED)
    {
        return asmTaskSerialize(asmManager->master_task);
    }

    /* For master, dump the first active task. */
    if (!asmManager || listLength(asmManager->tasks) == 0) return NULL;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    if (task->state == ASM_NONE || task->state == ASM_FAILED ||
        task->state == ASM_COMPLETED) return NULL;

    return asmTaskSerialize(task);
}

size_t asmGetPeakSyncBufferSize(void) {
    if (!asmManager) return 0;
    /* Compute peak sync buffer usage. The current task's peak may not
     * reflect in asmManager->sync_buffer_peak immediately. */
    size_t peak = asmManager->sync_buffer_peak;
    asmTask *task = listFirst(asmManager->tasks) ?
                    listNodeValue(listFirst(asmManager->tasks)) : NULL;
    if (task && task->operation == ASM_IMPORT)
        peak = max(task->sync_buffer.peak, asmManager->sync_buffer_peak);
    
    return peak;
}

size_t asmGetImportInputBufferSize(void) {
    if (!asmManager || listLength(asmManager->tasks) == 0) return 0;

    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    if (task->operation == ASM_IMPORT)
        return task->sync_buffer.mem_used;

    return 0;
}

size_t asmGetMigrateOutputBufferSize(void) {
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
asmTask *asmLookupTaskBySlotRangeArray(slotRangeArray *slots) {
    listIter li;
    listNode *ln;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (slotRangeArrayIsEqual(task->slots, slots))
            return task;
    }
    return NULL;
}

/* Returns the slot range array for the given task ID */
slotRangeArray *asmTaskGetSlotRanges(const char *task_id) {
    asmTask *task = NULL;
    if (!task_id || (task = asmLookupTaskById(task_id)) == NULL) return NULL;

    return task->slots;
}

/* Returns 1 if the slot range array overlaps with the given slot range. */
static int slotRangeArrayOverlaps(slotRangeArray *slots, slotRange *req) {
    for (int i = 0; i < slots->num_ranges; i++) {
        slotRange *sr = &slots->ranges[i];
        if (sr->start <= req->end && sr->end >= req->start)
            return 1;
    }
    return 0;
}

/* Returns 1 if the two slot range arrays overlap, 0 otherwise. */
static int slotRangeArraysOverlap(slotRangeArray *slots1, slotRangeArray *slots2) {
    for (int i = 0; i < slots1->num_ranges; i++) {
        slotRange *sr1 = &slots1->ranges[i];
        if (slotRangeArrayOverlaps(slots2, sr1)) return 1;
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
        if (slotRangeArrayOverlaps(task->slots, req))
            return task;
    }
    return NULL;
}

/* Validates the given slot ranges for a migration task:
 * - Ensures the current node is a master.
 * - Verifies all slots are in a STABLE state.
 * - Confirms all slots belong to a single source node.
 * - Confirms no ongoing import task that overlaps with the slot ranges.
 *
 * Returns the source node if validation succeeds.
 * Otherwise, returns NULL and sets 'err' variable. */
static clusterNode *validateImportSlotRanges(slotRangeArray *slots, sds *err, asmTask *current) {
    clusterNode *source = NULL;

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

    for (int i = 0; i < slots->num_ranges; i++) {
        slotRange *sr = &slots->ranges[i];

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
inline static int asmCanFeedMigrationClient(asmTask *task) {
    return task->operation == ASM_MIGRATE &&
           !task->cross_slot_during_propagating &&
             (task->state == ASM_SEND_BULK_AND_STREAM ||
              task->state == ASM_SEND_STREAM ||
              task->state == ASM_HANDOFF_PREP);
}

/* Feed the migration client with the replication stream for the slot range. */
void asmFeedMigrationClient(robj **argv, int argc) {
    asmTask *task = NULL;

    if (server.cluster_enabled == 0 || listLength(asmManager->tasks) == 0)
        return;

    /* Check if there is a migrate task that can receive replication stream. */
    task = listNodeValue(listFirst(asmManager->tasks));
    if (!asmCanFeedMigrationClient(task)) return;

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

    /* Check if the command belongs to the slot range. */
    struct redisCommand *cmd = lookupCommand(argv, argc);
    serverAssert(cmd);

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
    if (slot == INVALID_CLUSTER_SLOT) return;

    /* Generally we reject cross-slot commands before executing, but module may
     * replicate this kind of command, so we check again. To guarantee data
     * consistency, we cancel the task if we encounter a cross-slot command. */
    if (slot == CLUSTER_CROSSSLOT) {
        /* We cannot cancel the task directly here, since it may lead to a recursive
         * call: asmTaskCancel() --> moduleFireServerEvent() --> moduleFreeContext()
         * --> postExecutionUnitOperations() --> propagateNow(). Even worse, this
         * could result in propagating pending commands to the replication stream twice.
         * To avoid this, we simply set a flag here, cancel the task in beforeSleep. */
        task->cross_slot_during_propagating = 1;
        return;
    }

    /* Check if the slot belongs to the task's slot range. */
    slotRange sr = {slot, slot};
    if (!slotRangeArrayOverlaps(task->slots, &sr)) return;

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

asmTask *asmCreateImportTask(const char *task_id, slotRangeArray *slots, sds *err) {
    clusterNode *source;

    *err = NULL;
    /* Validate that the slot ranges are valid and that migration can be
     * initiated for them. */
    source = validateImportSlotRanges(slots, err, NULL);
    if (!source)
        goto err;

    if (source == getMyClusterNode()) {
        *err = sdsnew("this node is already the owner of the slot range");
        goto err;
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
            goto err;
        }
    }
    /* There should be no task in progress. */
    serverAssert(listLength(asmManager->tasks) == 0);

    /* Create a slot migration task */
    asmTask *task = asmTaskCreate(task_id);
    task->slots = slots;
    task->state = ASM_NONE;
    task->operation = ASM_IMPORT;
    task->source_node = source;
    memcpy(task->source, clusterNodeGetName(source), CLUSTER_NAMELEN);
    memcpy(task->dest, getMyClusterId(), CLUSTER_NAMELEN);

    listAddNodeTail(asmManager->tasks, task);
    sds slots_str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Import task %s created: src=%.40s, dest=%.40s, slots=%s",
                         task->id, task->source, task->dest, slots_str);
    sdsfree(slots_str);

    return task;

err:
    slotRangeArrayFree(slots);
    return NULL;
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

    slotRangeArray *slots = parseSlotRangesOrReply(c, c->argc, 3);
    if (!slots) return;

    sds err = NULL;
    asmTask *task = asmCreateImportTask(NULL, slots, &err);
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
    addReplyBulkCString(c, "slots");
    addReplyBulkSds(c, slotRangeArrayToString(task->slots));
    addReplyBulkCString(c, "source");
    addReplyBulkCBuffer(c, task->source, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "dest");
    addReplyBulkCBuffer(c, task->dest, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "operation");
    addReplyBulkCString(c, task->operation == ASM_IMPORT ? "import" : "migrate");
    addReplyBulkCString(c, "state");
    addReplyBulkCString(c, asmTaskStateToString(task->state));
    addReplyBulkCString(c, "last_error");
    addReplyBulkCBuffer(c, task->error, sdslen(task->error));
    addReplyBulkCString(c, "retries");
    addReplyLongLong(c, task->retry_count);
    addReplyBulkCString(c, "create_time");
    addReplyLongLong(c, task->create_time);
    addReplyBulkCString(c, "start_time");
    addReplyLongLong(c, task->start_time);
    addReplyBulkCString(c, "end_time");
    addReplyLongLong(c, task->end_time);

    if (task->operation == ASM_MIGRATE && task->state == ASM_COMPLETED)
        p = task->end_time - task->paused_time;
    addReplyBulkCString(c, "write_pause_ms");
    addReplyLongLong(c, p);
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
        if (!task) task = asmLookupTaskAt(asmManager->archived_tasks, id);
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
                            listLength(asmManager->archived_tasks));
        listRewind(asmManager->tasks, &li);
        while ((ln = listNext(&li)) != NULL)
            replyTaskStatus(c, listNodeValue(ln));

        listRewind(asmManager->archived_tasks, &li);
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

/* Return the number of keys in the specified slot ranges. */
unsigned long long asmCountKeysInSlots(slotRangeArray *slots) {
    if (!slots) return 0;

    unsigned long long key_count = 0;
    for (int i = 0; i < slots->num_ranges; i++) {
        for (int j = slots->ranges[i].start; j <= slots->ranges[i].end; j++) {
            key_count += kvstoreDictSize(server.db[0].keys, j);
        }
    }
    return key_count;
}

/* Log a human-readable message for ASM task lifecycle events. */
void asmLogTaskEvent(asmTask *task, int event) {
    sds str = slotRangeArrayToString(task->slots);

    switch (event) {
        case ASM_EVENT_IMPORT_STARTED:
            serverLog(LL_NOTICE, "Import task %s started for slots: %s", task->id, str);
            break;
        case ASM_EVENT_IMPORT_FAILED:
            serverLog(LL_NOTICE, "Import task %s failed for slots: %s", task->id, str);
            break;
        case ASM_EVENT_TAKEOVER:
            serverLog(LL_NOTICE, "Import task %s is ready to takeover slots: %s", task->id, str);
            break;
        case ASM_EVENT_IMPORT_COMPLETED:
            serverLog(LL_NOTICE, "Import task %s completed for slots: %s (imported %llu keys)",
                      task->id, str, asmCountKeysInSlots(task->slots));
            break;
        case ASM_EVENT_MIGRATE_STARTED:
            serverLog(LL_NOTICE, "Migrate task %s started for slots: %s (keys at start: %llu)",
                      task->id, str, asmCountKeysInSlots(task->slots));
            break;
        case ASM_EVENT_MIGRATE_FAILED:
            serverLog(LL_NOTICE, "Migrate task %s failed for slots: %s", task->id, str);
            break;
        case ASM_EVENT_HANDOFF_PREP:
            serverLog(LL_NOTICE, "Migrate task %s preparing to handoff for slots: %s", task->id, str);
            break;
        case ASM_EVENT_MIGRATE_COMPLETED:
            serverLog(LL_NOTICE, "Migrate task %s completed for slots: %s (migrated %llu keys)",
                      task->id, str, asmCountKeysInSlots(task->slots));
            break;
        default:
            break;
    }

    sdsfree(str);
}

/* Notify the state change to the module and the cluster implementation. */
void asmNotifyStateChange(asmTask *task, int event) {
    RedisModuleClusterSlotMigrationInfo info = {
            .version = REDISMODULE_CLUSTER_SLOT_MIGRATION_INFO_VERSION,
            .task_id = task->id,
            .slots = (RedisModuleSlotRangeArray *) task->slots
    };
    memcpy(info.source_node_id, task->source, CLUSTER_NAMELEN);
    memcpy(info.destination_node_id, task->dest, CLUSTER_NAMELEN);

    int module_event = -1;
    if (event == ASM_EVENT_IMPORT_STARTED) module_event = REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_IMPORT_STARTED;
    else if (event == ASM_EVENT_IMPORT_COMPLETED) module_event = REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_IMPORT_COMPLETED;
    else if (event == ASM_EVENT_IMPORT_FAILED) module_event = REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_IMPORT_FAILED;
    else if (event == ASM_EVENT_MIGRATE_STARTED) module_event = REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_MIGRATE_STARTED;
    else if (event == ASM_EVENT_MIGRATE_COMPLETED) module_event = REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_MIGRATE_COMPLETED;
    else if (event == ASM_EVENT_MIGRATE_FAILED) module_event = REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_MIGRATE_FAILED;
    serverAssert(module_event != -1);

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_SLOT_MIGRATION, module_event, &info);
    serverLog(LL_DEBUG, "Fire cluster asm module event, task %s: state=%s",
                        task->id, asmTaskStateToString(task->state));

    if (clusterNodeIsMaster(getMyClusterNode())) {
        /* Notify the cluster impl only if it is a real active import task. */
        if (task != asmManager->master_task) {
            asmLogTaskEvent(task, event);
            clusterAsmOnEvent(task->id, event, task->slots);
        }
        asmNotifyReplicasStateChange(task); /* Propagate state change to replicas */
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
    asmNotifyStateChange(task, ASM_EVENT_IMPORT_FAILED);
    /* This node may become replica, only master can setup new slot trimming jobs. */
    if (clusterNodeIsMaster(getMyClusterNode()))
        asmTrimJobSchedule(task->slots);
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
    sds slots_str = slotRangeArrayToString(task->slots);
    serverLog(LL_WARNING, "%s task %s failed: slots=%s, err=%s",
              task->operation == ASM_IMPORT ? "Import" : "Migrate",
              task->id, slots_str, task->error);
    sdsfree(slots_str);

    if (task->operation == ASM_IMPORT)
        asmImportSetFailed(task);
    else
        asmMigrateSetFailed(task);
}

/* The task is completed or canceled. Update stats and move it to
 * the archived list. */
void asmTaskFinalize(asmTask *task) {
    listNode *ln = listFirst(asmManager->tasks);
    serverAssert(ln->value == task);

    task->source_node = NULL; /* Should never access it */
    task->end_time = server.mstime;

    if (task->operation == ASM_IMPORT) {
        asmManager->sync_buffer_peak = max(asmManager->sync_buffer_peak,
                                           task->sync_buffer.peak);
        replDataBufClear(&task->sync_buffer); /* Not used, so save memory */
    }

    /* Move the task to the archived list */
    listUnlinkNode(asmManager->tasks, ln);
    listLinkNodeHead(asmManager->archived_tasks, ln);
}

static void asmTaskCancel(asmTask *task, const char *reason) {
    if (task->state == ASM_CANCELED) return;

    asmTaskSetFailed(task, "Cancelled due to %s", reason);
    task->state = ASM_CANCELED;
    asmTaskFinalize(task);
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
    asmLogTaskEvent(task, ASM_EVENT_TAKEOVER);
    clusterAsmOnEvent(task->id, ASM_EVENT_TAKEOVER, task->slots);
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
        task->rdb_channel_state = ASM_COMPLETED;
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
         * systems (e.g. Linux), the socket's receive buffer is not flushed
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
                "Source node replied to SLOTSSNAPSHOT, syncing slots snapshot can continue...");
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
    serverAssert(task->slots->num_ranges <= CLUSTER_SLOTS);
    int argc = task->slots->num_ranges * 2 + 4;
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
    for (int j = 0; j < task->slots->num_ranges; j++) {
        slotRange *sr = &task->slots->ranges[j];
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
                "Source node replied to SYNCSLOTS SYNC, syncslots can continue...");
        } else if (!strncmp(err, "-NOTREADY", strlen("-NOTREADY"))) {
            /* The source-side cluster is temporarily not ready to start a
             * migration and replied -NOTREADY. We could fail this attempt and
             * let the import task start another attempt later but that could
             * trigger unnecessary cleanup in the cluster implementation.
             * Instead, we'll retry sending SYNCSLOTS later in asmCron(). */
            sdsfree(err);
            task->state = ASM_SEND_SYNCSLOTS;
            serverLog(LL_NOTICE,
                "Source node replied to SYNCSLOTS SYNC with -NOTREADY, will retry later...");
            return;
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
    task->rdb_channel_state = ASM_COMPLETED;
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
    task->rdb_channel_state = ASM_COMPLETED;
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
         * can take over after streaming is EOF. Since we may release the context
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
    sds slots_str = slotRangeArrayToString(task->slots);

    /* Sanity check: Clean up any keys that exist in slots not owned by this node.
     * This handles cases where users previously migrated slots using legacy method
     * but left behind orphaned keys, or maybe cluster missed cleaning up during
     * previous operations, which could interfere with the ASM import process. */
    asmTrimSlotsIfNotOwned(task->slots);

    /* Check if there is any trim job in progress for the slot ranges.
     * We can't start the import task since the trim job will modify the data.*/
    int trim_in_progress = asmIsAnyTrimJobOverlaps(task->slots);

    /* Notify the cluster implementation to prepare for the import task. */
    int impl_ret = clusterAsmOnEvent(task->id, ASM_EVENT_IMPORT_PREP, task->slots);

    /* We do not start the import task if trim is disabled by module. */
    int disabled_by_module = server.cluster_module_trim_disablers > 0;

    static int start_blocked_logged = 0;
    /* Cannot start import task since pause action is performed. Otherwise, we
     * will break the promise that no writes are performed during the pause. */
    if (isPausedActions(PAUSE_ACTION_CLIENT_ALL) ||
        isPausedActions(PAUSE_ACTION_CLIENT_WRITE) ||
        trim_in_progress ||
        impl_ret != C_OK ||
        disabled_by_module)
    {
        const char *reason = disabled_by_module ? "trim is disabled by module" :
                             impl_ret != C_OK ? "cluster is not ready" :
                             trim_in_progress ? "trim in progress for some of the slots" :
                                                "server paused";
        if (start_blocked_logged == 0) {
            serverLog(LL_WARNING, "Can not start import task %s for slots: %s due to %s",
                                  task->id, slots_str, reason);
            start_blocked_logged = 1;
        }
        sdsfree(slots_str);
        return;
    }
    start_blocked_logged = 0; /* Reset the log flag */

    /* Detect if the cluster topology is changed. We should cancel the task if
     * we can not schedule it, and update the source node if needed. */
    sds err = NULL;
    clusterNode *source = validateImportSlotRanges(task->slots, &err, task);
    if (!source) {
        asmTaskCancel(task, err);
        sdsfree(slots_str);
        sdsfree(err);
        return;
    }
    /* Now I'm the owner of the slot range, cancel the import task. */
    if (source == getMyClusterNode()) {
        asmTaskCancel(task, "slots owned by myself now");
        sdsfree(slots_str);
        return;
    }
    /* Change the source node if needed. */
    if (source != task->source_node) {
        task->source_node = source;
        memcpy(task->source, clusterNodeGetName(source), CLUSTER_NAMELEN);
        serverLog(LL_NOTICE, "Import task %s source node changed: slots=%s, "
                             "new_source=%.40s", task->id, slots_str, clusterNodeGetName(source));
    }
    sdsfree(slots_str);

    task->state = ASM_CONNECTING;
    task->start_time = server.mstime;
    asmNotifyStateChange(task, ASM_EVENT_IMPORT_STARTED);

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
    if (!clusterNodeIsMaster(getMyClusterNode())) {
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

        slotRangeArray *slots = parseSlotRangesOrReply(c, c->argc, 4);
        if (!slots) return;

        /* Validate that the slot ranges are valid and that migration can be
         * initiated for them. */
        sds err = NULL;
        clusterNode *source = validateImportSlotRanges(slots, &err, NULL);
        if (!source) {
            addReplyErrorSds(c, err);
            slotRangeArrayFree(slots);
            return;
        }

        /* Check if the source node is the same as the current node. */
        if (source != getMyClusterNode()) {
            addReplyError(c, "This node is not the owner of the slots");
            slotRangeArrayFree(slots);
            return;
        }

        /* Verify the destination node is known and is a master. */
        if (c->node_id) {
            clusterNode *dest = clusterLookupNode(c->node_id, CLUSTER_NAMELEN);
            if (dest == NULL || !clusterNodeIsMaster(dest)) {
                addReplyErrorFormat(c, "Destination node %.40s is not a master", c->node_id);
                slotRangeArrayFree(slots);
                return;
            }
        }

        sds task_id = c->argv[3]->ptr;
        /* Notify the cluster implementation to prepare for the migrate task. */
        if (clusterAsmOnEvent(task_id, ASM_EVENT_MIGRATE_PREP, slots) != C_OK ||
            asmDebugIsFailPointActive(ASM_MIGRATE_MAIN_CHANNEL, ASM_NONE))
        {
            addReplyError(c, "-NOTREADY Cluster is not ready to migrate slots");
            slotRangeArrayFree(slots);
            return;
        }

        /* We do not start the migrate task if trim is disabled by module. */
        int disabled_by_module = server.cluster_module_trim_disablers > 0;
        if (disabled_by_module) {
            addReplyError(c, "Trim is disabled by module");
            slotRangeArrayFree(slots);
            return;
        }

        asmTask *task = listLength(asmManager->tasks) == 0 ? NULL :
                            listNodeValue(listFirst(asmManager->tasks));
        if (task && !strcmp(task->id, task_id) &&
            task->operation == ASM_MIGRATE && task->state == ASM_FAILED &&
            slotRangeArrayIsEqual(slots, task->slots) &&
            memcmp(task->dest, c->node_id, CLUSTER_NAMELEN) == 0)
        {
            /* Reuse the failed task */
            asmTaskReset(task);
            slotRangeArrayFree(task->slots); /* Will be set again later */
            task->retry_count++;
        } else if (task) {
            if (task->state == ASM_FAILED) {
                /* We can create a new migrate task only if the current one is
                 * failed, cancel the failed task to create a new one. */
                asmTaskCancel(task, "new migration requested");
                task = NULL;
            } else {
                addReplyError(c, "Another ASM task is already in progress");
                slotRangeArrayFree(slots);
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

        task->slots = slots;
        task->operation = ASM_MIGRATE;
        memcpy(task->source, clusterNodeGetName(getMyClusterNode()), CLUSTER_NAMELEN);
        if (c->node_id) memcpy(task->dest, c->node_id, CLUSTER_NAMELEN);

        task->main_channel_client = c;
        c->task = task;

        /* We mark the main channel client as a replica, so this client is limited
         * by the client output buffer settings for replicas. The replstate has
         * no real significance, just to prevent it from going online. */
        c->flags |= (CLIENT_SLAVE | CLIENT_ASM_MIGRATING);
        c->replstate = SLAVE_STATE_WAIT_RDB_CHANNEL;
        if (server.repl_disable_tcp_nodelay)
            connDisableTcpNoDelay(c->conn);  /* Non-critical if it fails. */
        listAddNodeTail(server.slaves, c);
        createReplicationBacklogIfNeeded();

        /* Wait for RDB channel to be ready */
        task->state = ASM_WAIT_RDBCHANNEL;

        sds slots_str = slotRangeArrayToString(slots);
        serverLog(LL_NOTICE, "Migrate task %s created: src=%.40s, dest=%.40s, slots=%s",
                              task->id, task->source, task->dest, slots_str);
        sdsfree(slots_str);

        asmNotifyStateChange(task, ASM_EVENT_MIGRATE_STARTED);

        /* Keep the client in the main thread to avoid data races between the
         * connWrite call below and the client's event handler in IO threads. */
        if (c->tid != IOTHREAD_MAIN_THREAD_ID) keepClientInMainThread(c);

        /* addReply*() is not suitable for clients in SLAVE_STATE_WAIT_RDB_CHANNEL state. */
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
            connDisableTcpNoDelay(c->conn); /* Non-critical if it fails. */
        listAddNodeTail(server.slaves, c);

        /* Wait for bgsave to start for slots sync */
        task->state = ASM_WAIT_BGSAVE_START;
        task->rdb_channel_state = ASM_WAIT_BGSAVE_START;
        task->rdb_channel_client = c;
        c->task = task;

        /* Keep the client in the main thread to avoid data races between the
         * connWrite call in startBgsaveForReplication and the client's event
         * handler in IO threads. */
        if (c->tid != IOTHREAD_MAIN_THREAD_ID) keepClientInMainThread(c);

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

            /* Record the time when the destination finishes applying the accumulated buffer */
            if (task->dest_state == ASM_WAIT_STREAM_EOF && task->dest_accum_applied_time == 0)
                task->dest_accum_applied_time = server.mstime;

            /* Pause write if needed */
            if (task->state == ASM_SEND_BULK_AND_STREAM || task->state == ASM_SEND_STREAM) {
                /* Pause writes on the main channel if the lag is less than the threshold. */
                if (task->dest_offset + server.asm_handoff_max_lag_bytes >= task->source_offset) {
                    if (unlikely(asmDebugIsFailPointActive(ASM_MIGRATE_MAIN_CHANNEL, ASM_HANDOFF_PREP)))
                        return; /* Do not enter handoff prep state for testing buffer drain timeout. */

                    serverLog(LL_NOTICE, "The applied offset lag %lld is less than the threshold %lld, "
                                         "pausing writes for slot handoff",
                                         task->source_offset - task->dest_offset,
                                         server.asm_handoff_max_lag_bytes);
                    task->state = ASM_HANDOFF_PREP;
                    asmLogTaskEvent(task, ASM_EVENT_HANDOFF_PREP);
                    clusterAsmOnEvent(task->id, ASM_EVENT_HANDOFF_PREP, task->slots);
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
            } else if (!strcasecmp(c->argv[j]->ptr, "asm-task")) {
                /* asm-task task_id:source_node:dest_node:operation:state:slot_ranges */
                if (clusterNodeIsMaster(getMyClusterNode())) {
                    addReplyError(c, "CLUSTER SYNCSLOTS CONF ASM-TASK only allowed on replica");
                    return;
                }
                if (asmReplicaHandleMasterTask(c->argv[j + 1]->ptr) != C_OK) {
                    addReplyErrorFormat(c, "Failed to handle master task: %s",
                                        (char *)c->argv[j + 1]->ptr);
                }
            } else if (!strcasecmp(c->argv[j]->ptr, "capa")) {
                /* Ignore unrecognized capabilities. This is for future extensions. */
            } else {
                addReplyErrorFormat(c, "Unknown option %s", (char *)c->argv[j]->ptr);
            }
        }
        addReply(c, shared.ok);
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

    /* If module object or non-string object that is not too big,
     * use RESTORE command (RDB format) to migrate data.
     * Generally RDB binary format is more efficient, but it may cause
     * block in the destination if the object is too large, so fall back
     * to AOF format if necessary. */
    if ((o->type == OBJ_MODULE) ||
        (o->type != OBJ_STRING && getObjectLength(o) <= ASM_AOF_MIN_ITEMS_PER_KEY))
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
        /* Use AOF format to migrate data */
        if (rewriteObject(rdb, &key, o, dbid, expiretime) == C_ERR) return C_ERR;
    }

    return C_OK;
}

/* Modules can use RM_ClusterPropagateForSlotMigration() during the
 * CLUSTER_SLOT_MIGRATION_MIGRATE_MODULE_PROPAGATE event to propagate commands
 * that should be delivered just before the slot snapshot delivery starts. This
 * function triggers the event, collects the commands and writes them to the rio. */
static int propagateModuleCommands(asmTask *task, rio *rdb) {
    RedisModuleClusterSlotMigrationInfo info = {
            .version = REDISMODULE_CLUSTER_SLOT_MIGRATION_INFO_VERSION,
            .task_id = task->id,
            .slots = (RedisModuleSlotRangeArray *) task->slots
    };
    memcpy(info.source_node_id, task->source, CLUSTER_NAMELEN);
    memcpy(info.destination_node_id, task->dest, CLUSTER_NAMELEN);

    task->pre_snapshot_module_cmds = zcalloc(sizeof(*task->pre_snapshot_module_cmds));
    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_SLOT_MIGRATION,
                          REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_MIGRATE_MODULE_PROPAGATE,
                          &info
    );

    int ret = C_OK;
    /* Write the module commands to the rio */
    for (int i = 0; i < task->pre_snapshot_module_cmds->numops; i++) {
        redisOp *op = &task->pre_snapshot_module_cmds->ops[i];
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
    redisOpArrayFree(task->pre_snapshot_module_cmds);
    zfree(task->pre_snapshot_module_cmds);
    task->pre_snapshot_module_cmds = NULL;
    return ret;
}

/* Save the slot ranges snapshot to the file. It generates the DUMP encoded
 * representation of each key in the slot ranges and writes it to the file.
 *
 * Returns C_OK on success, or C_ERR on error. */
int slotSnapshotSaveRio(int req, rio *rdb, int *error) {
    serverAssert(req & SLAVE_REQ_SLOTS_SNAPSHOT);

    dictEntry *de;
    kvstoreDictIterator kvs_di;

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
        for (int j = 0; j < task->slots->num_ranges; j++) {
            slotRange *sr = &task->slots->ranges[j];
            /* Iterate all keys in the slot range */
            for (int k = sr->start; k <= sr->end; k++) {
                int send_slot_info = 0;

                kvstoreInitDictIterator(&kvs_di, server.db->keys, k);
                while ((de = kvstoreDictIteratorNext(&kvs_di)) != NULL) {
                    /* Send slot info before the first key in the slot */
                    if (!send_slot_info) {
                        /* Format slot info */
                        char buf[128];
                        int len = snprintf(buf, sizeof(buf), "%d:%lu:%lu",
                                    k, kvstoreDictSize(db->keys, k),
                                    kvstoreDictSize(db->expires, k));
                        serverAssert(len > 0 && len < (int)sizeof(buf));

                        /* Send slot info */
                        if (rioWriteBulkCount(rdb, '*', 5) == 0) goto werr2;
                        if (rioWriteBulkString(rdb, "CLUSTER", 7) == 0) goto werr2;
                        if (rioWriteBulkString(rdb, "SYNCSLOTS", 9) == 0) goto werr2;
                        if (rioWriteBulkString(rdb, "CONF", 4) == 0) goto werr2;
                        if (rioWriteBulkString(rdb, "SLOT-INFO", 9) == 0) goto werr2;
                        if (rioWriteBulkString(rdb, buf, len) == 0) goto werr2;
                        send_slot_info = 1;
                    }

                    /* Save a key-value pair */
                    kvobj *o = dictGetKV(de);
                    if (slotSnapshotSaveKeyValuePair(rdb, o, db->id) == C_ERR) goto werr2;

                    /* Delay return if required (for testing) */
                    if (unlikely(server.rdb_key_save_delay)) {
                        /* Send buffer to the destination ASAP. */
                        if (rioFlush(rdb) == 0) goto werr2;
                        debugDelay(server.rdb_key_save_delay);
                    }
                }
                kvstoreResetDictIterator(&kvs_di);
            }
        }
    }

    /* Write the end of the snapshot file command */
    if (rioWriteBulkCount(rdb, '*', 3) == 0) goto werr;
    if (rioWriteBulkString(rdb, "CLUSTER", 7) == 0) goto werr;
    if (rioWriteBulkString(rdb, "SYNCSLOTS", 9) == 0) goto werr;
    if (rioWriteBulkString(rdb, "SNAPSHOT-EOF", 12) == 0) goto werr;
    return C_OK;

werr2:
    kvstoreResetDictIterator(&kvs_di);
werr:
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
    /* The task may be canceled (move to finished list) or failed during streaming buffer. */
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
     * it may be moved to the finished list. The actual free happens in serverCron,
     * which ensures there is no use-after-free issue. */
    int ret = replDataBufStreamToDb(&task->sync_buffer, &ctx);

    if (ret == C_OK) {
        if (task->stream_eof_during_streaming) {
            /* STREAM-EOF received during streaming, we can take over now. */
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
            task->rdb_channel_state = ASM_COMPLETED;
            task->rdb_channel_client->task = NULL;
            freeClientAsync(task->rdb_channel_client);
            task->rdb_channel_client = NULL;
        }

        task->state = ASM_STREAM_EOF;
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
        if (task->cross_slot_during_propagating) {
            asmTaskCancel(task, "propagating cross slot command");
            return;
        }

        if (task->state == ASM_HANDOFF) {
            /* To avoid long pause, we fail the task if the pause takes too long. */
            if (server.mstime - task->paused_time >= server.asm_write_pause_timeout) {
                asmTaskSetFailed(task, "Server paused timeout");
                return;
            }
            asmSendStreamEofIfDrained(task);
        } else if (task->state == ASM_STREAM_EOF) {
            /* In state ASM_STREAM_EOF (server is still paused), we are waiting
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
            if (server.mstime - task->paused_time >= server.asm_write_pause_timeout)
                asmTaskSetFailed(task, "Server paused timeout");
        }
    }
}

void asmCron(void) {
    static unsigned long long asm_cron_runs = 0;
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
        } else if (task->state == ASM_SEND_SYNCSLOTS) {
            /* Rare case: the source node replied to SYNCSLOTS with -NOTREADY
             * because it wasn't ready to start a migration. We'll retry
             * SYNCSLOTS every second instead of failing the attempt which could
             * trigger unnecessary cleanup in the cluster implementation. */
            if (asm_cron_runs % 10 == 0)
                asmSyncWithSource(task->main_channel_conn);
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
             * - A configurable timeout (cluster-slot-migration-sync-buffer-drain-timeout) to
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

    /* Trim the archived tasks list if it grows too large */
    while (listLength(asmManager->archived_tasks) > (unsigned long)server.asm_max_archived_tasks) {
        asmTask *oldest = listNodeValue(listLast(asmManager->archived_tasks));
        asmTaskFree(oldest);
        listDelNode(asmManager->archived_tasks, listLast(asmManager->archived_tasks));
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
 * If slots is NULL, cancel all tasks. */
int clusterAsmCancelBySlotRangeArray(struct slotRangeArray *slots, const char *reason) {
    if (asmManager == NULL) return 0;

    int num_cancelled = 0;
    listIter li;
    listNode *ln;
    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);
        if (!slots || slotRangeArraysOverlap(task->slots, slots)) {
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
        if (slotRangeArrayOverlaps(task->slots, &req))
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
    } else if (task->operation == ASM_MIGRATE && task->state == ASM_STREAM_EOF) {
        event = ASM_EVENT_MIGRATE_COMPLETED;
    } else {
        *err = sdscatprintf(sdsempty(),
                            "ASM task is not in the correct state for config update: %s",
                            asmTaskStateToString(task->state));
        asmTaskCancel(task, "slots configuration updated");
        return C_ERR;
    }

    /* Reset per-slot statistics for the migrated/imported ranges.
     * Note: cluster_legacy.c also cleans up, so this may run twice, but
     * required if an alternative cluster impl is in use. */
    for (int i = 0; i < task->slots->num_ranges; i++) {
        slotRange *sr = &task->slots->ranges[i];
        for (int j = sr->start; j <= sr->end; j++)
            clusterSlotStatReset(j);
    }

    /* Clear error message if successful. */
    sdsfree(task->error);
    task->error = sdsempty();
    task->state = ASM_COMPLETED;

    asmNotifyStateChange(task, event);
    asmTaskFinalize(task);

    /* Trim the slots after the migrate task is completed. */
    if (event == ASM_EVENT_MIGRATE_COMPLETED)
        asmTrimJobSchedule(task->slots);

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
        case ASM_EVENT_IMPORT_START: {
            /* Validate the slot ranges. */
            slotRangeArray *slots = slotRangeArrayDup(arg);
            if (slotRangeArrayNormalizeAndValidate(slots, &errsds) != C_OK) {
                slotRangeArrayFree(slots);
                ret = C_ERR;
                break;
            }
            ret = asmCreateImportTask(task_id, slots, &errsds) ? C_OK : C_ERR;
            break;
        }
        case ASM_EVENT_CANCEL: {
            num_cancelled = clusterAsmCancel(task_id, "user request");
            if (arg) *((int *)arg) = num_cancelled;
            ret = C_OK;
            break;
        }
        case ASM_EVENT_HANDOFF: {
            ret = clusterAsmHandoff(task_id, &errsds);
            break;
        }
        case ASM_EVENT_DONE: {
            ret = clusterAsmDone(task_id, &errsds);
            break;
        }
        default: {
            ret = C_ERR;
            errsds = sdscatprintf(sdsempty(), "Unknown operation: %d", event);
            break;
        }
    }

    if (ret != C_OK && errsds && err) {
        snprintf(buf, sizeof(buf), "%s", errsds);
        *err = buf;
    }
    sdsfree(errsds);

    return ret;
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

/* If this node is a replica and there is an active trim or a pending trim
 * job (due to write pause), we cannot process commands from the master for the
 * slots that are waiting to be trimmed. Otherwise, the trim cycle could
 * mistakenly delete newly added keys. In this case, the master will be blocked
 * until the trim job finishes. This is supposed to be a rare event as it needs
 * to migrate slots and import them back before the trim job is done. */
void asmUnblockMasterAfterTrim(void) {
    if (server.master &&
        server.master->flags & CLIENT_BLOCKED &&
        server.master->bstate.btype == BLOCKED_POSTPONE_TRIM)
    {
        unblockClient(server.master, 1);
        serverLog(LL_NOTICE, "Unblocking master client after active trim is completed");
    }
}

/* Trim the slots asynchronously in the BIO thread. */
void asmTriggerBackgroundTrim(slotRangeArray *slots) {
    RedisModuleClusterSlotMigrationTrimInfoV1 fsi = {
            REDISMODULE_CLUSTER_SLOT_MIGRATION_TRIMINFO_VERSION,
            (RedisModuleSlotRangeArray *) slots
    };

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_SLOT_MIGRATION_TRIM,
                          REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_TRIM_BACKGROUND,
                          &fsi);

    signalFlushedDb(0, 1, slots);

    /* Create temp kvstores and estore, move relevant slot dicts/ebuckets into them,
     * and delete them in BIO thread asynchronously. */
    kvstore *keys = kvstoreCreate(&kvstoreBaseType, &dbDictType,
                                  CLUSTER_SLOT_MASK_BITS,
                                  KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    kvstore *expires = kvstoreCreate(&kvstoreBaseType, &dbExpiresDictType,
                                     CLUSTER_SLOT_MASK_BITS,
                                     KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    estore *subexpires = estoreCreate(&subexpiresBucketsType, CLUSTER_SLOT_MASK_BITS);

    size_t total_keys = 0;

    for (int i = 0; i < slots->num_ranges; i++) {
        for (int slot = slots->ranges[i].start; slot <= slots->ranges[i].end; slot++) {
            total_keys += kvstoreDictSize(server.db[0].keys, slot);
            kvstoreMoveDict(server.db[0].keys, keys, slot);
            kvstoreMoveDict(server.db[0].expires, expires, slot);
            estoreMoveEbuckets(server.db[0].subexpires, subexpires, slot);
        }
    }

    emptyDbDataAsync(keys, expires, subexpires);

    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Background trim started for slots: %s to trim %zu keys.", str, total_keys);
    sdsfree(str);

    /* Unblock master if blocked. This can only happen in a very unlikely case,
     * trim job will be in pending list due to write pause and master will send
     * commands for the slots that are waiting to be trimmed. Just keeping this
     * call here for being defensive as it is harmless. */
    asmUnblockMasterAfterTrim();
}

/* Trim the slots. */
void asmTrimSlots(slotRangeArray *slots) {
    if (asmManager->debug_trim_method == ASM_DEBUG_TRIM_NONE)
        return;

    /* Trigger active trim for the following cases:
     * 1. Debug override: trim method is set to 'active'.
     * 2. There are clients using client side caching (client tracking is enabled):
     *   There is no way to invalidate specific slots in the client tracking
     *   protocol. For now, we just use active trim to trim the slots.
     * 3. Module subscribers: If any module is subscribed to TRIMMED event, we
     *   assume module needs per key notification and cannot use background trim.
     */
    int activetrim = server.tracking_clients != 0 ||
                     (asmManager->debug_trim_method == ASM_DEBUG_TRIM_ACTIVE) ||
                     (asmManager->debug_trim_method == ASM_DEBUG_TRIM_DEFAULT &&
                      moduleHasSubscribersForKeyspaceEvent(NOTIFY_KEY_TRIMMED));
    if (activetrim)
        asmTriggerActiveTrim(slots);
    else
        asmTriggerBackgroundTrim(slots);
}

/* Schedule a trim job for the specified slot ranges. The job will be
 * deferred and handled later in asmBeforeSleep(). We delay the trim jobs to
 * asmBeforeSleep() to ensure it only runs when there is no write pause. */
void asmTrimJobSchedule(slotRangeArray *slots) {
    listAddNodeTail(asmManager->pending_trim_jobs, slotRangeArrayDup(slots));
}

/* Process any pending trim jobs. */
void asmTrimJobProcessPending(void) {
    /* Check if there is any pending trim job and we can propagate it. */
    if (listLength(asmManager->pending_trim_jobs) == 0 ||
        asmManager->debug_trim_method == ASM_DEBUG_TRIM_NONE)
    {
        return;
    }

    /* If this node is a replica, it should not initiate slot trimming actively.
     * Cancel the trim job and unblock the master if it is blocked. */
    if (clusterNodeIsSlave(getMyClusterNode())) {
        asmCancelPendingTrimJobs();
        asmUnblockMasterAfterTrim();
        return;
    }

    /* Determine if we can start the trim job:
     * - require client writes not paused (so key deletions are allowed)
     * - require replicas not paused (so TRIMSLOTS can be propagated).
     * - require trim is not disabled via RedisModule_ClusterDisableTrim().
     */
    static int logged = 0;
    int disabled_by_module = server.cluster_module_trim_disablers > 0;

    if (isPausedActions(PAUSE_ACTION_CLIENT_WRITE) ||
        isPausedActions(PAUSE_ACTION_CLIENT_ALL) ||
        isPausedActions(PAUSE_ACTION_REPLICA) ||
        disabled_by_module)
    {
        if (logged == 0) {
            logged = 1;
            const char *reason = disabled_by_module ? "trim is disabled by module" :
                                                      "pause action is in effect";
            serverLog(LL_NOTICE, "Trim job is deferred since %s.", reason);
        }
        return;
    }
    logged = 0;

    listIter li;
    listNode *ln;
    listRewind(asmManager->pending_trim_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRangeArray *slots = listNodeValue(ln);
        asmTrimSlots(slots);
        propagateTrimSlots(slots);
        listDelNode(asmManager->pending_trim_jobs, ln);
        slotRangeArrayFree(slots);
    }
}

/* Trim keys in slots not owned by this node (if any). */
void asmTrimSlotsIfNotOwned(slotRangeArray *slots) {
    if (!server.cluster_enabled || !clusterNodeIsMaster(getMyClusterNode())) return;

    size_t num_keys = 0;
    slotRangeArray *trim_slots = NULL;
    for (int i = 0; i < slots->num_ranges; i++) {
        for (int j = slots->ranges[i].start; j <= slots->ranges[i].end; j++) {
            if (clusterIsMySlot(j) ||
                kvstoreDictSize(server.db[0].keys, j) == 0 ||
                isSlotInTrimJob(j))
            {
                continue;
            }

            trim_slots = slotRangeArrayAppend(trim_slots, j);
            num_keys += kvstoreDictSize(server.db[0].keys, j);
        }
    }
    if (!trim_slots) return;

    sds str = slotRangeArrayToString(trim_slots);
    serverLog(LL_NOTICE,
              "Detected keys in slots that do not belong to this node. "
              "Scheduling trim for %zu keys in slots: %s", num_keys, str);
    sdsfree(str);

    asmTrimJobSchedule(trim_slots);
    slotRangeArrayFree(trim_slots);
}

/* Handle the master task when it is no longer used, trim unowned slots if necessary.
 * This function is called when the replica is just promoted to master. */
void asmFinalizeMasterTask(void) {
    if (!server.cluster_enabled) return;

    asmTask *task = asmManager->master_task;
    if (task == NULL) return;

    if (task->operation == ASM_IMPORT) {
        /* Check if there is an ASM task that master did not finish. */
        if (task->state != ASM_COMPLETED && task->state != ASM_FAILED) {
            sds slots_str = slotRangeArrayToString(task->slots);
            serverLog(LL_WARNING, "Import task %s from old master failed: slots=%s",
                                task->id, slots_str);
            sdsfree(slots_str);
            /* Mark the task as failed and notify the replicas. */
            task->state = ASM_FAILED;
            asmNotifyStateChange(task, ASM_EVENT_IMPORT_FAILED);
        }

        /* Trim the slots if the import task is failed. */
        if (clusterNodeIsMaster(getMyClusterNode()) && task->state == ASM_FAILED) {
            asmTrimSlotsIfNotOwned(task->slots);
        }
    } else if (task->operation == ASM_MIGRATE) {
        /* For migrate tasks, attempt to trim slots if necessary. After ASM completed,
         * the previous master may not have initiated slot trimming before the failover
         * occurred. In that case, we need to initiate slot trimming here.
         * However, if ASM failed, slot ownership did not change, so no slot trimming
         * is needed. */
        if (clusterNodeIsMaster(getMyClusterNode()) && task->state != ASM_FAILED) {
            asmTrimSlotsIfNotOwned(task->slots);
        }
    }

    /* Clear the master task since it is not a replica anymore. */
    asmTaskFree(asmManager->master_task);
    asmManager->master_task = NULL;
}

/* The replicas handle the master import ASM task information. */
int asmReplicaHandleMasterTask(sds task_info) {
    if (!server.cluster_enabled || !clusterNodeIsSlave(getMyClusterNode())) return C_ERR;

    /* If the master task is migrating, just clear it when receiving a new task info,
     * even the task info is empty since it means the master finished the task. */
    if (asmManager->master_task && asmManager->master_task->operation == ASM_MIGRATE) {
        asmTaskFree(asmManager->master_task);
        asmManager->master_task = NULL;
    }

    /* If the master task is empty, it means the master finished the task, the
     * replica should check the slot ownership to decide to raise completed or
     * failed event. */
    if (!task_info || sdslen(task_info) == 0) {
        asmTask *task = asmManager->master_task;
        if (task && task->state != ASM_COMPLETED && task->state != ASM_FAILED) {
            /* Check if the slots are owned by the master. */
            int owned_by_master = 1;
            for (int i = 0; i < task->slots->num_ranges; i++) {
                slotRange *sr = &task->slots->ranges[i];
                for (int j = sr->start; j <= sr->end; j++) {
                    clusterNode *master = clusterNodeGetMaster(getMyClusterNode());
                    if (!master || !clusterNodeCoversSlot(master, j)) {
                        owned_by_master = 0;
                        break;
                    }
                }
            }
            if (owned_by_master) {
                task->state = ASM_COMPLETED;
                asmNotifyStateChange(task, ASM_EVENT_IMPORT_COMPLETED);
            } else {
                task->state = ASM_FAILED;
                asmNotifyStateChange(task, ASM_EVENT_IMPORT_FAILED);
            }
        }
        return C_OK;
    }

    asmTask *task = asmTaskDeserialize(task_info);
    if (!task) return C_ERR;

    /* For migrate task, replica just keeps the task info, doesn't notify any event. */
    if (task->operation == ASM_MIGRATE) {
        if (asmManager->master_task) asmTaskFree(asmManager->master_task);
        asmManager->master_task = task;
        return C_OK;
    }

    int notify_event = 0;
    int event = asmTaskStateToEvent(task);
    if (asmManager->master_task) {
        /* Notify when the task or event is changed, to avoid duplicated notification. */
        if (strcmp(task->id, asmManager->master_task->id) != 0 ||
            event != asmTaskStateToEvent(asmManager->master_task))
        {
            notify_event = 1;
        }
        asmTaskFree(asmManager->master_task);
    } else {
        /* Ignore completed or failed task when there is no active master task. */
        if (task->state != ASM_FAILED && task->state != ASM_COMPLETED)
            notify_event = 1;
    }

    asmManager->master_task = task;
    if (notify_event) asmNotifyStateChange(task, event);
    return C_OK;
}

/* Cancel all pending trim jobs. */
void asmCancelPendingTrimJobs(void) {
    if (!asmManager) return;

    listIter li;
    listNode *ln;
    listRewind(asmManager->pending_trim_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRangeArray *slots = listNodeValue(ln);
        listDelNode(asmManager->pending_trim_jobs, ln);
        sds str = slotRangeArrayToString(slots);
        serverLog(LL_NOTICE, "Cancelling the pending trim job for slots: %s", str);
        sdsfree(str);
        slotRangeArrayFree(slots);
    }
}

/* Cancel all pending and active trim jobs. */
void asmCancelTrimJobs(void) {
    if (!asmManager) return;

    /* Unblock master if blocked */
    asmUnblockMasterAfterTrim();

    /* Cancel pending trim jobs */
    asmCancelPendingTrimJobs();

    /* Cancel active trim jobs */
    if (listLength(asmManager->active_trim_jobs) == 0)
        return;

    serverLog(LL_NOTICE, "Cancelling all active trim jobs");
    asmManager->active_trim_cancelled += listLength(asmManager->active_trim_jobs);
    asmActiveTrimEnd();
    listEmpty(asmManager->active_trim_jobs);
}

/* It's used to trim slots after the migration is completed or import is failed.
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
        /* We cannot trim any slot served by this node. */
        if (clusterNodeIsMaster(getMyClusterNode())) {
            for (int i = 0; i < slots->num_ranges; i++) {
                for (int j = slots->ranges[i].start; j <= slots->ranges[i].end; j++) {
                    if (clusterCanAccessKeysInSlot(j)) {
                        addReplyErrorFormat(c, "the slot %d is served by this node", j);
                        slotRangeArrayFree(slots);
                        return;
                    }
                }
            }
        }
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
    slotRangeArray *slots = listNodeValue(listFirst(asmManager->active_trim_jobs));

    serverAssert(asmManager->active_trim_it == NULL);
    asmManager->active_trim_it = slotRangeArrayGetIterator(slots);
    asmManager->active_trim_started++;
    asmManager->active_trim_current_job_keys = 0;
    asmManager->active_trim_current_job_trimmed = 0;

    /* Count the number of keys to trim */
    asmManager->active_trim_current_job_keys += asmCountKeysInSlots(slots);

    RedisModuleClusterSlotMigrationTrimInfoV1 fsi = {
            REDISMODULE_CLUSTER_SLOT_MIGRATION_TRIMINFO_VERSION,
            (RedisModuleSlotRangeArray *) slots
    };

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_SLOT_MIGRATION_TRIM,
                          REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_TRIM_STARTED,
                          &fsi);

    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Active trim initiated for slots: %s, to trim %llu keys.",
              str, asmManager->active_trim_current_job_keys);
    sdsfree(str);
}

/* Schedule an active trim job. */
void asmTriggerActiveTrim(slotRangeArray *slots) {
    listAddNodeTail(asmManager->active_trim_jobs, slotRangeArrayDup(slots));
    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Active trim scheduled for slots: %s", str);
    sdsfree(str);

    /* Start an active trim job if no active trim job is running. */
    if (asmManager->active_trim_it == NULL) {
        serverAssert(listLength(asmManager->active_trim_jobs) > 0);
        asmActiveTrimStart();
    }
}

/* End the active trim job. */
void asmActiveTrimEnd(void) {
    slotRangeArray *slots = listNodeValue(listFirst(asmManager->active_trim_jobs));

    if (asmManager->active_trim_it) {
        slotRangeArrayIteratorFree(asmManager->active_trim_it);
        asmManager->active_trim_it = NULL;
    }

    /* Unblock the master if it is blocked */
    asmUnblockMasterAfterTrim();

    RedisModuleClusterSlotMigrationTrimInfoV1 fsi = {
            REDISMODULE_CLUSTER_SLOT_MIGRATION_TRIMINFO_VERSION,
            (RedisModuleSlotRangeArray *) slots
    };

    moduleFireServerEvent(REDISMODULE_EVENT_CLUSTER_SLOT_MIGRATION_TRIM,
                          REDISMODULE_SUBEVENT_CLUSTER_SLOT_MIGRATION_TRIM_COMPLETED,
                          &fsi);

    sds str = slotRangeArrayToString(slots);
    serverLog(LL_NOTICE, "Active trim completed for slots: %s, %llu keys trimmed.",
              str, asmManager->active_trim_current_job_trimmed);
    sdsfree(str);
    listDelNode(asmManager->active_trim_jobs, listFirst(asmManager->active_trim_jobs));
    asmManager->active_trim_completed++;
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


/* Check if the command is accessing keys in a slot being trimmed.
 * Return the slot if found, otherwise return -1. */
int asmGetTrimmingSlotForCommand(struct redisCommand *cmd, robj **argv, int argc) {
    if (!asmIsTrimInProgress()) return -1;

    /* Get the keys from the command */
    getKeysResult result = GETKEYS_RESULT_INIT;
    int numkeys = getKeysFromCommand(cmd, argv, argc, &result);

    int last_checked_slot = -1;
    for (int j = 0; j < numkeys; j++) {
        robj *key = argv[result.keys[j].pos];
        int slot = keyHashSlot((char*) key->ptr, sdslen(key->ptr));
        if (slot == last_checked_slot) continue;
        if (isSlotInTrimJob(slot)) {
            getKeysFreeResult(&result);
            return slot;
        }
        last_checked_slot = slot;
    }
    getKeysFreeResult(&result);
    return -1;
}

/* Delete the key and notify the modules. */
void asmActiveTrimDeleteKey(redisDb *db, robj *keyobj) {
    if (asmManager->debug_active_trim_delay > 0)
        debugDelay(asmManager->debug_active_trim_delay);

    /* The key needs to be converted from static to heap before deletion. */
    int static_key = keyobj->refcount == OBJ_STATIC_REFCOUNT;
    if (static_key) keyobj = createStringObject(keyobj->ptr, sdslen(keyobj->ptr));

    dbDelete(db, keyobj);
    keyModified(NULL, db, keyobj, NULL, 1);
    /* The keys are not actually logically deleted from the database, just moved
     * to another node. The modules need to know that these keys are no longer
     * available locally, so just send the keyspace notification to the modules,
     * but not to clients. */
    moduleNotifyKeyspaceEvent(NOTIFY_KEY_TRIMMED, "key_trimmed", keyobj, db->id);
    asmManager->active_trim_current_job_trimmed++;

    if (static_key) decrRefCount(keyobj);
}

/* Trim keys in the active trim job. */
void asmActiveTrimCycle(void) {
    if (asmManager->debug_active_trim_delay < 0 ||
        listLength(asmManager->active_trim_jobs) == 0)
    {
        return;
    }

    /* Verify client pause is not in effect and trim is not disabled by module,
     * so we can delete keys. */
    static int blocked = 0;
    int disabled_by_module = server.cluster_module_trim_disablers > 0;
    if (isPausedActions(PAUSE_ACTION_CLIENT_ALL) ||
        isPausedActions(PAUSE_ACTION_CLIENT_WRITE) ||
        disabled_by_module)
    {
        if (blocked == 0)  {
            blocked = 1;
            const char *reason = disabled_by_module ? "trim is disabled by module" :
                                                       "pause action is in effect";
            serverLog(LL_NOTICE, "Active trim cycle is blocked since %s.", reason);
        }
        return;
    }
    if (blocked) serverLog(LL_NOTICE, "Active trim cycle is unblocked.");
    blocked = 0;

    /* This works in a similar way to activeExpireCycle, in the sense that
     * we do incremental work across calls. */
    const int trim_cycle_time_perc = 25;
    int time_exceeded = 0;
    long long start = ustime(), timelimit;
    unsigned long long num_deleted = 0;

    /* Calculate the time limit in microseconds for this cycle. */
    timelimit = 1000000 * trim_cycle_time_perc / server.hz / 100;
    if (timelimit <= 0) timelimit = 1;

    serverAssert(asmManager->active_trim_it);
    int slot = slotRangeArrayGetCurrentSlot(asmManager->active_trim_it);

    while (!time_exceeded && slot != -1) {
        dictEntry *de;
        kvstoreDictIterator kvs_di;
        kvstoreInitDictSafeIterator(&kvs_di, server.db[0].keys, slot);
        while ((de = kvstoreDictIteratorNext(&kvs_di)) != NULL) {
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
        kvstoreResetDictIterator(&kvs_di);
        if (!time_exceeded) slot = slotRangeArrayNext(asmManager->active_trim_it);
    }

    if (slot == -1) {
#if defined(USE_JEMALLOC)
        jemalloc_purge();
#endif
        asmActiveTrimEnd();

        /* Immediately start the next trim job upon completion of the current
         * one. Eliminates gaps in notifications so modules are informed about
         * trimming unowned keys, which is important for modules that
         * continuously filter unowned keys from their replies. */
        if (listLength(asmManager->active_trim_jobs) != 0)
            asmActiveTrimStart();
    }
}

/* Check if the key in a trim job. */
int asmIsKeyInTrimJob(sds keyname) {
    if (!asmIsTrimInProgress() || !isSlotInTrimJob(getKeySlot(keyname)))
        return 0;
    return 1;
}

/* Modules can use RM_ClusterPropagateForSlotMigration() during the
 * CLUSTER_SLOT_MIGRATION_MIGRATE_MODULE_PROPAGATE event to propagate commands
 * that should be delivered just before the slot snapshot delivery starts. */
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
        task->pre_snapshot_module_cmds == NULL)
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
    if (slot == CLUSTER_CROSSSLOT) {
        errno = ENOTSUP;
        return C_ERR;
    }

    /* Allow no-keys commands or if keys are in the slot range. */
    slotRange sr = {slot, slot};
    if (slot != INVALID_CLUSTER_SLOT && !slotRangeArrayOverlaps(task->slots, &sr)) {
        errno = ERANGE;
        return C_ERR;
    }

    robj **argvcopy = zmalloc(sizeof(robj*) * argc);
    for (int i = 0; i < argc; i++) {
        argvcopy[i] = argv[i];
        incrRefCount(argv[i]);
    }

    redisOpArrayAppend(task->pre_snapshot_module_cmds, 0, argvcopy, argc, 0);
    return C_OK;
}
