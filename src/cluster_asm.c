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
    int operation;                      /* Either ASM_IMPORT or ASM_MIGRATE */
    slotRangeArray *slot_ranges;        /* List of slot ranges for this migration operation */
    int state;                          /* Current state of the task */
    char source[CLUSTER_NAMELEN];       /* Source node name */
    char dest[CLUSTER_NAMELEN];         /* Destination node name */
    clusterNode *source_node;           /* Source node */
    connection *main_channel_conn;      /* Main channel connection */
    long long main_channel_id;          /* Main channel ID for the task */
    connection *rdb_channel_conn;       /* RDB channel connection */
    int rdb_channel_state;              /* State of the RDB channel */
    long long dest_offset;              /* Destination offset */
    long long source_offset;            /* Source offset */
    replDataBuf sync_buffer;            /* Buffer for the stream */
    client *main_channel_client;        /* Client for the main channel on the source side */
    long long retry_count;              /* Number of retries for this task */
    mstime_t create_time;               /* The time of creating this task */
    mstime_t start_time;                /* The start time of this task */
    mstime_t done_time;                 /* The completed time of this task */
} asmTask;

struct asmManager {
    list *tasks;                       /* List of asmTask to be processed */
    list *done_tasks;             /* List of completed asmTask */
    size_t sync_buffer_peak;           /* Peak size of sync buffer */
    long long total_done_tasks;   /* Total number of completed tasks */
};

enum asmState {
    /* Common state */
    ASM_NONE,
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
    ASM_SLOTS_HANDOFF,

    /* Migrate state */
    ASM_WAIT_RDBCHANNEL,
    ASM_WAIT_BGSAVE_START,
    ASM_SEND_BULK_AND_STREAM,
    ASM_WAIT_PAUSE_WRITE,
    ASM_PAUSED_WRITE,
    ASM_STREAM_DONE,

    /* RDB channel state */
    ASM_SEND_RDBCHANNEL,
    ASM_RDBCHANNEL_REPLY,
    ASM_RDBCHANNEL_TRANSFER,
    ASM_RDBCHANNEL_DONE
};

/* Global ASM manager */
struct asmManager *asmManager = NULL;

void asmStartSyncSlots(asmTask *task);

char *sendCommand(connection *conn, ...);
char *sendCommandArgv(connection *conn, int argc, char **argv, size_t *argv_lens);
char *receiveSynchronousResponse(connection *conn);
ConnectionType *connTypeOfReplication(void);
void createDumpPayload(rio *payload, robj *o, robj *key, int dbid);
int startBgsaveForReplication(int mincapa, int req);
void createReplicationBacklogIfNeeded(void);
static void asmSyncBufferReadFromConn(connection *conn);

void clusterAsmInit(void) {
    asmManager = zcalloc(sizeof(*asmManager));
    asmManager->tasks = listCreate();
    asmManager->done_tasks = listCreate();
    asmManager->sync_buffer_peak = 0;
    asmManager->total_done_tasks = 0;
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
        case ASM_SLOTS_HANDOFF: return "slots-handoff";
    
        /* Migrate state */
        case ASM_WAIT_RDBCHANNEL: return "wait-rdbchannel";
        case ASM_WAIT_BGSAVE_START: return "wait-bgsave-start";
        case ASM_SEND_BULK_AND_STREAM: return "send-bulk-and-stream";
        case ASM_WAIT_PAUSE_WRITE: return "wait-pause-write";
        case ASM_PAUSED_WRITE: return "paused-write";
        case ASM_STREAM_DONE: return "stream-done";

        default: return "unknown";
    }
    return NULL; /* Unreachable */
}

void asmTaskReset(asmTask *task) {
    task->state = ASM_NONE;
    task->rdb_channel_state = ASM_NONE;
    task->main_channel_id = -1;
    task->main_channel_conn = NULL;
    task->rdb_channel_conn = NULL;
    task->dest_offset = -1;
    task->source_offset = -1;
    replDataBufInit(&task->sync_buffer);
}

asmTask *asmTaskCreate(void) {
    asmTask *task = zcalloc(sizeof(*task));
    asmTaskReset(task);
    task->slot_ranges = NULL;
    task->source_node = NULL;
    task->main_channel_client = NULL;
    task->retry_count = 0;
    task->create_time = server.mstime;
    task->start_time = 0;
    task->done_time = 0;

    return task;
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

/* Returns C_OK if there is no overlapping import operation in progress for the
 * given slot range. Otherwise, returns C_ERR */
static int checkOverlappingImport(slotRange *req) {
    listIter li;
    listNode *ln;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);

        /* Only import operations on the destination side can be cancelled. */
        if (task->operation != ASM_IMPORT || task->state == ASM_DONE) {
            continue;
        }

        for (int i = 0; i < task->slot_ranges->num_ranges; i++) {
            slotRange *sr = &task->slot_ranges->ranges[i];
            /* Cancel if any slot range overlaps with the requested range. */
            if (sr->start <= req->end && sr->end >= req->start)
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
            *err = sdsnew("Slot states must be STABLE to start a slot migration operation.");
            goto out;
        }
    }

    for (int i = 0; i < slot_ranges->num_ranges; i++) {
        slotRange *sr = &slot_ranges->ranges[i];

        /* Ensure no import operation overlaps with this slot range. */
        if (checkOverlappingImport(sr) != C_OK) {
            *err = sdscatprintf(sdsempty(),
                                "overlapping import exists for slot range: %d-%d",
                                sr->start, sr->end);
            goto out;
        }

        /* Validate if we can start migration operation for this slot range. */
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

int asmStartImportTask(slotRangeArray *slot_ranges, sds *err) {
    clusterNode *source;

    *err = NULL;
    /* Validate that the slot ranges are valid and that migration can be
     * initiated for them. */
    source = validateImportSlotRanges(slot_ranges, err);
    if (!source) {
        zfree(slot_ranges);
        return C_ERR;
    }

    if (source == getMyClusterNode()) {
        *err = sdsnew("this node is already the owner of the slot range");
        zfree(slot_ranges);
        return C_ERR;
    }

    /* Create a slot migration task */
    asmTask *task = asmTaskCreate();
    task->slot_ranges = slot_ranges;
    task->state = ASM_NONE;
    task->operation = ASM_IMPORT;
    task->source_node = source;
    memcpy(task->source, source->name, CLUSTER_NAMELEN);
    memcpy(task->dest, getMyClusterId(), CLUSTER_NAMELEN);

    listAddNodeTail(asmManager->tasks, task);
    sds slot_ranges_str = createSlotRangesStr(slot_ranges);
    serverLog(LL_NOTICE, "Import slots task created: source: %s, dest: %s, sync slot ranges: %s",
                          task->source, task->dest, slot_ranges_str);
    sdsfree(slot_ranges_str);

    /* Start the import task */
    clusterAsmOnEvent(task->slot_ranges, ASM_EVENT_IMPORT_STARTED, NULL);
    asmStartSyncSlots(task);

    return C_OK;
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

    slotRangeArray *slot_ranges = parseSlotRangesOrReply(c, c->argc, 3);
    if (!slot_ranges) return;

    sds err = NULL;
    if (asmStartImportTask(slot_ranges, &err) != C_OK) {
        addReplyErrorSds(c, err);
        return;
    }

    addReply(c, shared.ok);
}

/* Cancel atomic slot migration operations that overlap with the given slot
 * range. Returns the number of cancelled operations. */
static int cancelLinksForSlotRange(slotRange *req_range) {
    int num_cancelled = 0;
    listIter li;
    listNode *ln;

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);

        /* Only import operations on the destination side can be cancelled. */
        if (task->operation != ASM_IMPORT || task->state == ASM_DONE) {
            continue;
        }

        for (int i = 0; i < task->slot_ranges->num_ranges; i++) {
            slotRange *sr = &task->slot_ranges->ranges[i];
            /* Cancel if any slot range overlaps with the requested range. */
            if (sr->start <= req_range->end && sr->end >= req_range->start) {
                task->state = ASM_CANCELED;
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
    int remaining, num_cancelled = 0;
    slotRangeArray *slot_ranges;

    /* Validate slot range arg count */
    remaining = c->argc - 3;
    if (remaining == 0 || remaining % 2 != 0) {
        addReplyErrorArity(c);
        return;
    }

    slot_ranges = parseSlotRangesOrReply(c, c->argc, 3);
    if (!slot_ranges) return;

    /* Cancel asm operations that overlaps with the slot ranges. */
    for (int i = 0; i < slot_ranges->num_ranges; i++)
        num_cancelled += cancelLinksForSlotRange(&slot_ranges->ranges[i]);

    addReplyLongLong(c, num_cancelled);
    zfree(slot_ranges);
}

/* CLUSTER MIGRATION STATUS
 *  - Reply: Array of atomic slot migration links */
static void clusterCommandMigrationStatus(client *c) {
    listIter li;
    listNode *ln;

    addReplyArrayLen(c, listLength(asmManager->tasks));

    listRewind(asmManager->tasks, &li);
    while ((ln = listNext(&li)) != NULL) {
        asmTask *task = listNodeValue(ln);

        addReplyMapLen(c, 5);
        addReplyBulkCString(c, "slots_range");
        addReplyBulkSds(c, createSlotRangesStr(task->slot_ranges));
        addReplyBulkCString(c, "source");
        addReplyBulkCBuffer(c, task->source, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "dest");
        addReplyBulkCBuffer(c, task->dest, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "operation");
        addReplyBulkCString(c, task->operation == ASM_IMPORT ? "importing" : "migrating");
        addReplyBulkCString(c, "state");
        addReplyBulkCString(c, asmTaskStateToString(task->state));
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

void asmImportFailed(asmTask *task) {
    serverAssert(task->operation == ASM_IMPORT);

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

    /* Close the connections */
    if (task->rdb_channel_conn) connClose(task->rdb_channel_conn);
    if (task->main_channel_conn) connClose(task->main_channel_conn);

    /* Clear the replication data buffer */
    asmManager->sync_buffer_peak = max(asmManager->sync_buffer_peak, task->sync_buffer.peak);
    replDataBufClear(&task->sync_buffer);

    sds slot_ranges_str = createSlotRangesStr(task->slot_ranges);
    serverLog(LL_WARNING, "Import operation failed for source: %s, dest: %s, slots: %s",
              task->source, task->dest, slot_ranges_str);
    sdsfree(slot_ranges_str);

    /* Mark the task as failed and notify the cluster */
    task->state = ASM_FAILED;
    clusterAsmOnEvent(task->slot_ranges, ASM_EVENT_IMPORT_FAILED, NULL);
}

void asmCallbackOnFreeClient(client *c) {
    asmTask *task = c->task;
    if (!task) return;

    /* If the RDB channel connection is closed, mark the task as failed. */
    if (c->conn && task->rdb_channel_conn == c->conn) {
        /* We create the client only when transferring data on the RDB channel */
        serverAssert(task->rdb_channel_state == ASM_RDBCHANNEL_TRANSFER);
        task->rdb_channel_conn = NULL; /* Will be freed by freeClient */
        asmImportFailed(task);
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

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING, "Error condition on socket for establishing rdb channel: %s",
                connGetLastError(conn));
        goto error;
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
            task->rdb_channel_state = ASM_SEND_RDBCHANNEL;
            serverLog(LL_NOTICE, "Source node replied to AUTH command, slots rdb channel sync can continue...");
        } else {
            serverLog(LL_WARNING, "Error reply to AUTH from source: '%s'", err);
            sdsfree(err);
            goto error;
        }
    }

    if (task->rdb_channel_state == ASM_SEND_RDBCHANNEL) {
        char cid[LONG_STR_SIZE];
        ull2string(cid, sizeof(cid), task->main_channel_id);

        err = sendCommand(conn, "CLUSTER", "SYNCSLOTS", "RDBCHANNEL", cid, NULL);
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
            task->state = ASM_ACCUMULATE_BUF;
            /* The main channel buffers pending commands. */
            connSetReadHandler(task->main_channel_conn, asmSyncBufferReadFromConn);

            task->rdb_channel_state = ASM_RDBCHANNEL_TRANSFER;
            client *c = createClient(conn);
            c->flags |= CLIENT_MASTER;
            c->querybuf = sdsempty();
            c->authenticated = 1;
            c->user = NULL;
            c->task = task;
            serverLog(LL_NOTICE,
                "Source replied to SLOTSSNAPSHOT, sync slots snapshot can continue...");
            sdsfree(err);
        } else {
            serverLog(LL_WARNING,"Error reply to CLUSTER SYNCSLOTS RDBCHANNEL from the source: '%s'",err);
            sdsfree(err);
            goto error;
        }
        return;
    }
    return;

no_response_error:
    serverLog(LL_WARNING, "Source node did not respond to command during RDBCHANNELSYNCSLOTS handshake");
    /* Fall through to regular error handling */

error:
    asmImportFailed(task);
    return;

write_error: /* Handle sendCommand() errors. */
    serverLog(LL_WARNING, "Failed sending command to the source: %s", err);
    sdsfree(err);
    goto error;
}

char *asmSendSlotRangesSync(connection *conn, asmTask *task) {
    /* Prepare CLUSTER SYNCSLOTS RANGES command */
    size_t argc = task->slot_ranges->num_ranges*2 + 3;
    char **args = zcalloc(sizeof(char*) * argc);
    size_t *lens = zcalloc(sizeof(size_t) * argc);

    args[0] = "CLUSTER";
    args[1] = "SYNCSLOTS";
    args[2] = "RANGES";
    lens[0] = strlen("CLUSTER");
    lens[1] = strlen("SYNCSLOTS");
    lens[2] = strlen("RANGES");

    size_t i = 3;
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
    for (size_t j = 3; j < argc; j++) {
        sdsfree(args[j]);
    }
    zfree(args);
    zfree(lens);

    return err;
}

void asmSyncWithSource(connection *conn) {
    asmTask *task = connGetPrivateData(conn);
    char *err = NULL;

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING, "Error condition on socket for sync slots: %s",
                connGetLastError(conn));
        goto error;
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
            serverLog(LL_NOTICE, "Source node replied to AUTH command, sync slots can continue...");
        } else {
            serverLog(LL_WARNING, "Error reply to AUTH from the source: '%s'", err);
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
            serverLog(LL_NOTICE, "Source node replied to SYNCSLOTS CONF command, sync slots can continue...");
        } else {
            serverLog(LL_WARNING, "Error reply to SYNCSLOTS CONF from the source: '%s'", err);
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

        /* Check `+RDBCHANNELSYNCSLOTS client-id` reply */
        if (!strncmp(err, "+RDBCHANNELSYNCSLOTS", strlen("+RDBCHANNELSYNCSLOTS"))) {
            /* Parse main channel id */
            char *client_id = strchr(err,' ');
            if (client_id) client_id++;
            if (!client_id) {
                serverLog(LL_WARNING,
                            "Source replied with wrong +RDBCHANNELSYNC syntax: %s", err);
                sdsfree(err);
                goto error;
            }
            task->main_channel_id = strtoll(client_id, NULL, 10);
            serverLog(LL_NOTICE,
                "Source replied to RDBCHANNELSYNCSLOTS, sync slots can continue...");
            sdsfree(err);
            err = NULL;
            task->state = ASM_INIT_RDBCHANNEL;
        } else {
            serverLog(LL_WARNING, "Error reply to SYNCSLOTS RANGES from the source: '%s'", err);
            sdsfree(err);
            goto error;
        }
    }

    if (task->state == ASM_INIT_RDBCHANNEL) {
        /* Create RDB channel connection */
        task->rdb_channel_conn = connCreate(server.el, connTypeOfReplication());
        if (connConnect(task->rdb_channel_conn, task->source_node->ip,
                server.tls_replication ? task->source_node->tls_port : task->source_node->tcp_port,
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
            task->source_node->name);

        /* Main channel waits for the new event */
        connSetReadHandler(conn, NULL);
        return;
    }
    return;

no_response_error:
    serverLog(LL_WARNING, "Source node did not respond to command during SYNC handshake");
    /* Fall through to regular error handling */

error:
    asmImportFailed(task);
    return;

write_error: /* Handle sendCommand() errors. */
    serverLog(LL_WARNING, "Failed sending command to source: %s", err);
    sdsfree(err);
    goto error;
}

void asmStartSendBulkAndStream(struct asmTask *task) {
    serverAssert(task->state == ASM_WAIT_BGSAVE_START);
    task->state = ASM_SEND_BULK_AND_STREAM;
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
                              "rdb channel state: %s",
                              asmTaskStateToString(task ? task->rdb_channel_state : ASM_NONE));
        freeClientAsync(c);
        return;
    }

    /* Clear the RDB channel connection */
    task->rdb_channel_conn = NULL;
    task->rdb_channel_state = ASM_RDBCHANNEL_DONE;
    serverLog(LL_NOTICE, "RDB channel snapshot transfer done for task");

    /* Free the RDB channel connection. */
    c->task = NULL;
    c->flags &= ~CLIENT_MASTER;
    freeClientAsync(c);

    /* Will start streaming the buffer to DB, don't start here since now
     * we are in the context of executing command, otherwise, redis will
     * generate a big MULTI-EXEC including all the commands in the buffer.
     * just update the state here, and do it in beforeSleep(). */
    task->state = ASM_STREAMING_BUF;
}

/* CLUSTER SYNCSLOTS STREAM-EOF
 *
 * This command is sent by the source node to the destination node to indicate
 * that the slot sync stream has ended and the slots can be handed off. */
void clusterSyncSlotsStreamEOF(client *c) {
    asmTask *task = c->task;
    if (task->state != ASM_STREAMING_BUF) {
        serverLog(LL_WARNING, "Unexpected CLUSTER SYNCSLOTS STREAM-EOF state: %s",
                               asmTaskStateToString(task->state));
        return;
    }
    serverLog(LL_NOTICE, "CLUSTER SYNCSLOTS STREAM-EOF received");

    /* Free the main channel connection. */
    task->main_channel_conn = NULL;
    c->task = NULL;
    c->flags &= ~CLIENT_MASTER;
    freeClientAsync(c);

    task->state = ASM_SLOTS_HANDOFF;
    clusterAsmOnEvent(task->slot_ranges, ASM_EVENT_IMPORT_WAIT_FINALIZE, NULL);
}

/* Start the sync slots task. */
void asmStartSyncSlots(asmTask *task) {
    if (task->operation != ASM_IMPORT || task->state != ASM_NONE) return;

    task->start_time = server.mstime;

    /* TODO: tls support tests */
    task->main_channel_conn = connCreate(server.el, connTypeOfReplication());
    if (connConnect(task->main_channel_conn, task->source_node->ip,
            server.tls_replication ? task->source_node->tls_port : task->source_node->tcp_port,
            server.bind_source_addr, asmSyncWithSource) == C_ERR)
    {
        serverLog(LL_WARNING,"Unable to connect to source node: %s",
                    connGetLastError(task->main_channel_conn));
        asmImportFailed(task);
        return;
    }
    connSetPrivateData(task->main_channel_conn, task);
    task->state = ASM_CONNECTING;
}

void clusterSyncSlotsCommand(client *c) {
    if (!strcasecmp(c->argv[2]->ptr, "ranges") && c->argc >= 5) {
        /* CLUSTER SYNCSLOTS RANGES <start-slot> <end-slot> [<start-slot> <end-slot>] */
        if (c->argc % 2 == 0) {
            addReplyErrorArity(c);
            return;
        }

        slotRangeArray *slot_ranges = parseSlotRangesOrReply(c, c->argc, 3);
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

        /* Only one slots sync on source node */
        if (listLength(asmManager->tasks) != 0) {
            addReplyError(c, "SYNCSLOTS RANGES already in progress");
            zfree(slot_ranges);
            return;
        }

        /* Create the migrate slots task */
        asmTask *task = asmTaskCreate();
        task->slot_ranges = slot_ranges;
        task->main_channel_id = c->id;
        task->operation = ASM_MIGRATE;
        memcpy(task->source, getMyClusterNode()->name, CLUSTER_NAMELEN);
        if (c->node_id) memcpy(task->dest, c->node_id, CLUSTER_NAMELEN);
        c->task = task;

        /* Add the task to the list of active tasks */
        listAddNodeTail(asmManager->tasks, task);

        /* Wait for RDB channel to be ready */
        task->main_channel_client = c;
        task->start_time = server.mstime;
        task->state = ASM_WAIT_RDBCHANNEL;

        clusterAsmOnEvent(task->slot_ranges, ASM_EVENT_MIGRATE_STARTED, NULL);

        sds slot_ranges_str = createSlotRangesStr(slot_ranges);
        serverLog(LL_NOTICE, "Migrate slots task created: source: %s, dest: %s, sync slot ranges: %s",
                              task->source, task->dest, slot_ranges_str);
        sdsfree(slot_ranges_str);

        addReplyStatusFormat(c, "RDBCHANNELSYNCSLOTS %llu",
                               (unsigned long long) c->id);
    } else if (!strcasecmp(c->argv[2]->ptr, "rdbchannel") && c->argc == 4) {
        /* CLUSTER SYNCSLOTS RDBCHANNEL <client-id> */
        long long client_id;

        if (getLongLongFromObjectOrReply(c, c->argv[3], &client_id, NULL) != C_OK) {
            return;
        }

        if (listLength(asmManager->tasks) == 0) {
            addReplyError(c, "No migrate slots task in progress");
            return;
        }

        asmTask *task = listNodeValue(listFirst(asmManager->tasks));
        serverAssert(task->operation == ASM_MIGRATE);
        if (task->main_channel_id != client_id) {
            addReplyErrorFormat(c, "Export slots task client id mismatch");
            return;
        }

        c->slave_capa |= SLAVE_CAPA_EOF;
        c->slave_req |= SLAVE_REQ_SLOTS_SNAPSHOT;
        c->slave_req |= SLAVE_REQ_RDB_CHANNEL;
        c->flags |= CLIENT_REPL_RDB_CHANNEL;
        c->flags |= CLIENT_REPL_RDBONLY;
        c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
        if (server.repl_disable_tcp_nodelay)
            connDisableTcpNoDelay(c->conn); /* Non critical if it fails. */
        c->repldbfd = -1;
        c->flags |= CLIENT_SLAVE;
        listAddNodeTail(server.slaves, c);
        /* Create the replication backlog if needed. */
        createReplicationBacklogIfNeeded();
        c->main_ch_client_id = task->main_channel_id;
        c->task = task;

        /* Add main channel client to the list of slaves */
        client *main_ch_client = task->main_channel_client;
        main_ch_client->flags |= CLIENT_SLAVE;
        main_ch_client->replstate = SLAVE_STATE_WAIT_RDB_CHANNEL;
        listAddNodeTail(server.slaves, main_ch_client);

        /* Wait for bgsave to start for slots sync */
        task->state = ASM_WAIT_BGSAVE_START;

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
                if (task->source_offset > offset) {
                    serverLog(LL_WARNING, "CLUSTER SYNCSLOTS ACK received, but offset %lld is less than the current source offset %lld",
                              offset, task->source_offset);
                    return;
                }
                task->source_offset = offset;
                serverLog(LL_NOTICE, "CLUSTER SYNCSLOTS ACK received, offset: %lld, updated source offset to %lld",
                          offset, task->source_offset);
            }
        } else if (c->task && c->task->operation == ASM_MIGRATE) {
            /* Update the ACKed offset from destination. */
            asmTask *task = c->task;
            if (task->dest_offset > offset) {
                serverLog(LL_WARNING, "CLUSTER SYNCSLOTS ACK received, but offset %lld is less than the current destination offset %lld",
                        offset, task->dest_offset);
                return;
            }
            task->dest_offset = offset;
            serverLog(LL_NOTICE, "CLUSTER SYNCSLOTS ACK received, offset: %lld, updated destination offset to %lld",
                    offset, task->dest_offset);

            /* Pause write if needed */
            task->state = ASM_WAIT_PAUSE_WRITE;
            task->main_channel_client = c;

            clusterAsmOnEvent(task->slot_ranges, ASM_EVENT_MIGRATE_WAIT_PAUSE, NULL);
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
                sds node_id = c->argv[j + 1]->ptr;
                if (sdslen(node_id) != CLUSTER_NAMELEN) {
                    addReplyErrorFormat(c, "Invalid node id length %d", (int)sdslen(node_id));
                    return;
                }

                /* Lookup the node in the cluster. */
                clusterNode *node = clusterLookupNode(node_id, sdslen(node_id));
                if (node == NULL) {
                    addReplyErrorFormat(c, "Node %s not found in cluster", node_id);
                    return;
                }

                if (c->node_id) sdsfree(c->node_id);
                c->node_id = sdsdup(node_id);
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

    for (int j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db + j;
        if (kvstoreSize(db->keys) == 0) continue;

        /* SELECT the new DB */
        if (rioWrite(rdb,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(rdb, j) == 0) goto werr;

        /* Only support a single migrate task */
        serverAssert(listLength(asmManager->tasks) == 1);
        asmTask *task = listNodeValue(listFirst(asmManager->tasks));
        serverAssert(task->operation == ASM_MIGRATE);

        /* Iterate all slot ranges, and generate the DUMP encoded
         * representation of each key in the DB. */
        for (int i = 0; i < task->slot_ranges->num_ranges; i++) {
            slotRange *sr = &task->slot_ranges->ranges[i];
            /* Iterate all keys in the slot range */
            for (int j = sr->start; j <= sr->end; j++) {
                kvs_di = kvstoreGetDictIterator(server.db->keys, j);
                while ((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
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
                    createDumpPayload(&payload, o, &key, j);
                    sds buf = payload.io.buffer.ptr;
                    if (rioWriteBulkString(rdb, buf, sdslen(buf)) == 0) {
                        sdsfree(payload.io.buffer.ptr);
                        goto werr;
                    }
                    sdsfree(payload.io.buffer.ptr);

                    /* Write ABSTTL */
                    if (rioWriteBulkString(rdb, "ABSTTL", 6) == 0) goto werr;

                    /* Delay return if required (for testing) */
                    if (unlikely(server.rdb_key_save_delay))
                        debugDelay(server.rdb_key_save_delay);
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
    serverLog(LL_WARNING, "ASM buffer stream: error while reading from source: %s",
              connGetLastError(conn));
    asmTask *task = connGetPrivateData(conn);

    asmImportFailed(task);
}

/* Read data from connection into sync buffer. */
static void asmSyncBufferReadFromConn(connection *conn) {
    asmTask *task = connGetPrivateData(conn);
    serverAssert(task->state == ASM_ACCUMULATE_BUF);

    replDataBufReadFromConn(conn, &task->sync_buffer, asmReadSyncBufferErrorHandler);
}

static void asmSyncBufferStreamYieldCallback(void *ctx) {
    replDataBufToDbCtx *context = ctx;
    client *c = context->client;

    sds offset = sdsfromlonglong(context->total_offset);
    char *err = sendCommand(c->conn, "CLUSTER", "SYNCSLOTS", "ACK", offset, NULL);
    sdsfree(offset);
    if (!err) return;

    serverLog(LL_WARNING, "Error sending CLUSTER SYNCSLOTS ACK: %s", err);
    sdsfree(err);

    /* Mask this client as closed, and then asmSyncBufferStreamShouldContinue
     * will this error, and then stop this task. */
    c->flags |= CLIENT_CLOSE_ASAP;
}

static int asmSyncBufferStreamShouldContinue(void *ctx) {
    replDataBufToDbCtx *context = ctx;
    client *c = context->client;
    asmTask *task = c->task;

    /* Check if the client is still valid, maybe killed by `client kill`,
     * or the task is no longer in the streaming state, maybe the task is
     * failed. */
    if (c->flags & CLIENT_CLOSE_ASAP || task->state != ASM_STREAMING_BUF)
        return 0;

    return 1;
}

/* Stream the sync buffer to the database. */
void asmSyncBufferStreamToDb(asmTask *task) {
    serverAssert(task->state == ASM_STREAMING_BUF);
    serverLog(LL_NOTICE, "Streaming buffer for task is started");

    /* The buffered stream from the main channel connection into
     * the database is processed by a fake client. */
    client *c = createClient(task->main_channel_conn);
    c->flags |= CLIENT_MASTER;
    c->querybuf = sdsempty();
    c->authenticated = 1;
    c->user = NULL;
    c->task = task;
    connSetReadHandler(c->conn, NULL);

    replDataBufToDbCtx ctx = {
        .client = c,
        .total_offset = 0,
        .should_continue = asmSyncBufferStreamShouldContinue,
        .yield_callback = asmSyncBufferStreamYieldCallback,
    };

    /* Protect the client from being killed by `client kill` */
    protectClient(c);
    int ret = replDataBufStreamToDb(&task->sync_buffer, &ctx);
    unprotectClient(c);

    if (ret == C_OK) {
        /* ACK offset during streaming buffer. */
        sds offset = sdsfromlonglong(ctx.total_offset);
        char *err = sendCommand(task->main_channel_conn, "CLUSTER", "SYNCSLOTS", "ACK", offset, NULL);
        sdsfree(offset);
        if (err == NULL) {
            /* Wait STREAM-EOF from the source node. */
            connSetReadHandler(task->main_channel_conn, readQueryFromClient);
            serverLog(LL_NOTICE, "Streaming buffer for task is done, size: %zu", ctx.total_offset);
            return;
        }
        serverLog(LL_WARNING, "Error sending CLUSTER SYNCSLOTS ACK: %s", err);
        sdsfree(err);
    } else {
        serverLog(LL_WARNING, "Streaming buffer for task failed");
    }

    /* Free the main channel connection. */
    task->main_channel_conn = NULL;
    c->task = NULL;
    c->flags &= ~CLIENT_MASTER;
    freeClientAsync(c);

    /* The task is failed. */
    asmImportFailed(task);
}

void asmBeforeSleep(void) {
    if (listLength(asmManager->tasks) == 0) return;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));

    if (task->operation == ASM_IMPORT) {
        if (task->state == ASM_NONE) {
            clusterAsmOnEvent(task->slot_ranges, ASM_EVENT_IMPORT_STARTED, NULL);
            asmStartSyncSlots(task);
        } else if (task->state == ASM_STREAMING_BUF) {
            asmSyncBufferStreamToDb(task);
        } else if (task->state == ASM_FAILED) {
            asmTaskReset(task);
            task->retry_count++;
            serverAssert(task->state == ASM_NONE);
            asmStartSyncSlots(task);
        }
    }
}

int clusterAsmImport(slotRangeArray *slot_ranges, sds *err) {
    if (validateSlotRanges(slot_ranges, err) != C_OK ||
        asmStartImportTask(slot_ranges, err) != C_OK)
    {
        return C_ERR;
    }

    return C_OK;
}

int clusterAsmCancel(slotRangeArray *slot_ranges, sds *err) {
    int num_cancelled = 0;

    if (validateSlotRanges(slot_ranges, err) != C_OK) {
        return -1;
    }

    /* Cancel asm operations that overlaps with the slot ranges. */
    for (int i = 0; i < slot_ranges->num_ranges; i++)
        num_cancelled += cancelLinksForSlotRange(&slot_ranges->ranges[i]);

    return num_cancelled;
}

int clusterAsmSlotWritesPaused(slotRangeArray *slot_ranges, sds *err) {
    UNUSED(slot_ranges);
    UNUSED(err);
    /* find task matching slot ranges */
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));
    client *c = task->main_channel_client;

    task->state = ASM_PAUSED_WRITE;

    /* Drain all slot ranges command stream */
    /* Send STREAM EOF */
    sendCommand(c->conn, "CLUSTER", "SYNCSLOTS", "STREAM-EOF", NULL);

    task->state = ASM_STREAM_DONE;

    /* Notify plugin import is completed */
    clusterAsmOnEvent(task->slot_ranges, ASM_EVENT_MIGRATE_WAIT_FINALIZE, NULL);

    return C_OK;
}

int clusterAsmNotifyConfigUpdated(slotRangeArray *slot_ranges, sds *err) {
    UNUSED(err);
    /* TODO: Validation, cancel asmTasks if required */

    sds slot_ranges_str = createSlotRangesStr(slot_ranges);
    serverLog(LL_NOTICE, "Slot ranges: %s handed off", slot_ranges_str);
    sdsfree(slot_ranges_str);

    if (listLength(asmManager->tasks) == 0) return C_ERR;
    asmTask *task = listNodeValue(listFirst(asmManager->tasks));

    /* The config updated slot_ranges validation, it must match the current ASM task */
    if (slotRangeArrayIsEqual(task->slot_ranges, slot_ranges) != 1) {
        serverLog(LL_WARNING, "Slot ranges mismatch: %s != %s",
                  createSlotRangesStr(task->slot_ranges), createSlotRangesStr(slot_ranges));
        return C_ERR;
    }

    if (task->operation == ASM_IMPORT && task->state == ASM_SLOTS_HANDOFF) {
        task->state = ASM_DONE;
        task->done_time = server.mstime;
        asmManager->sync_buffer_peak = max(asmManager->sync_buffer_peak, task->sync_buffer.peak);
        replDataBufClear(&task->sync_buffer); /* To save memory */

        /* Move the task to completed tasks */
        listNode *ln = listFirst(asmManager->tasks);
        listUnlinkNode(asmManager->tasks, ln);
        listLinkNodeHead(asmManager->done_tasks, ln);
        return C_OK;
    } else if (task->operation == ASM_MIGRATE && task->state == ASM_STREAM_DONE) {
        task->state = ASM_DONE;
        task->done_time = server.mstime;

        /* TODO: for plugin, we need to clean up the data of slot ranges
         * such as slotsflush */

        /* Migrate task is done, just remove it from the list */
        listNode *ln = listFirst(asmManager->tasks);
        listUnlinkNode(asmManager->tasks, ln);
        listLinkNodeHead(asmManager->done_tasks, ln);
        return C_OK;
    }

    return C_OK;
}

int clusterAsmRequest(slotRangeArray *slot_ranges, int request, void *arg, sds *err) {
    UNUSED(arg);

    switch (request) {
        case ASM_REQUEST_IMPORT_START:
            return clusterAsmImport(slot_ranges, err);
        case ASM_REQUEST_IMPORT_CANCEL:
            return clusterAsmCancel(slot_ranges, err);
        case ASM_REQUEST_MIGRATE_PAUSED:
            return clusterAsmSlotWritesPaused(slot_ranges, err);
        case ASM_REQUEST_CONFIG_UPDATED:
            return clusterAsmNotifyConfigUpdated(slot_ranges, err);
        default:
            *err = sdscatprintf(sdsempty(), "Unknown request: %d", request);
            return C_ERR;
    }
}
