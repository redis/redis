#include "redismodule.h"

#include <stdlib.h>
#include <memory.h>
#include <errno.h>

#define MAX_EVENTS 1024

/* Log of cluster events. */
const char *clusterEventLog[MAX_EVENTS];
int numClusterEvents = 0;

/* Log of cluster trim events. */
const char *clusterTrimEventLog[MAX_EVENTS];
int numClusterTrimEvents = 0;

int replicateModuleCommand = 0;   /* Enable or disable module command replication. */
RedisModuleString *moduleCommandKeyName = NULL; /* Key name to replicate. */
RedisModuleString *moduleCommandKeyVal = NULL;  /* Key value to replicate. */

/* Enable or disable module command replication. */
int replicate_module_command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        RedisModule_ReplyWithError(ctx, "ERR wrong number of arguments");
        return REDISMODULE_OK;
    }

    long long enable = 0;
    if (RedisModule_StringToLongLong(argv[1], &enable) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR enable value");
        return REDISMODULE_OK;
    }
    replicateModuleCommand = (enable != 0);

    /* Set the key name and value to replicate. */
    if (moduleCommandKeyName) RedisModule_FreeString(ctx, moduleCommandKeyName);
    if (moduleCommandKeyVal) RedisModule_FreeString(ctx, moduleCommandKeyVal);
    moduleCommandKeyName = RedisModule_CreateStringFromString(ctx, argv[2]);
    moduleCommandKeyVal = RedisModule_CreateStringFromString(ctx, argv[3]);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int lpush_and_replicate_crossslot_command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);

    /* LPUSH */
    RedisModuleCallReply *rep = RedisModule_Call(ctx, "LPUSH", "!ss", argv[1], argv[2]);
    RedisModule_Assert(RedisModule_CallReplyType(rep) != REDISMODULE_REPLY_ERROR);
    RedisModule_FreeCallReply(rep);

    /* Replicate cross slot command */
    int ret = RedisModule_Replicate(ctx, "MSET", "cccccc", "key1", "val1", "key2", "val2", "key3", "val3");
    RedisModule_Assert(ret == REDISMODULE_OK);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int testClusterGetLocalSlotRanges(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    static int use_auto_memory = 0;
    use_auto_memory = !use_auto_memory;

    RedisModuleSlotRangeArray *slots;
    if (use_auto_memory) {
        RedisModule_AutoMemory(ctx);
        slots = RedisModule_ClusterGetLocalSlotRanges(ctx);
    } else {
        slots = RedisModule_ClusterGetLocalSlotRanges(NULL);
    }

    RedisModule_ReplyWithArray(ctx, slots->num_ranges);
    for (int i = 0; i < slots->num_ranges; i++) {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithLongLong(ctx, slots->ranges[i].start);
        RedisModule_ReplyWithLongLong(ctx, slots->ranges[i].end);
    }
    if (!use_auto_memory)
        RedisModule_ClusterFreeSlotRanges(NULL, slots);
    return REDISMODULE_OK;
}

/* Helper function to check if a slot range array contains a given slot. */
int slotRangeArrayContains(RedisModuleSlotRangeArray *sra, unsigned int slot) {
    for (int i = 0; i < sra->num_ranges; i++)
        if (sra->ranges[i].start <= slot && sra->ranges[i].end >= slot)
            return 1;
    return 0;
}

/* Sanity check. */
int sanity(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_Assert(RedisModule_ClusterCanAccessKeysInSlot(-1) == 0);
    RedisModule_Assert(RedisModule_ClusterCanAccessKeysInSlot(16384) == 0);
    RedisModule_Assert(RedisModule_ClusterCanAccessKeysInSlot(100000) == 0);

    /* Call with invalid args. */
    errno = 0;
    RedisModule_Assert(RedisModule_ClusterPropagateForSlotMigration(NULL, NULL, NULL) == REDISMODULE_ERR);
    RedisModule_Assert(errno == EINVAL);

    /* Call with invalid args. */
    errno = 0;
    RedisModule_Assert(RedisModule_ClusterPropagateForSlotMigration(ctx, NULL, NULL) == REDISMODULE_ERR);
    RedisModule_Assert(errno == EINVAL);

    /* Call with invalid args. */
    errno = 0;
    RedisModule_Assert(RedisModule_ClusterPropagateForSlotMigration(NULL, "asm.keyless_cmd", "") == REDISMODULE_ERR);
    RedisModule_Assert(errno == EINVAL);

    /* Call outside of slot migration. */
    errno = 0;
    RedisModule_Assert(RedisModule_ClusterPropagateForSlotMigration(ctx, "asm.keyless_cmd", "") == REDISMODULE_ERR);
    RedisModule_Assert(errno == EBADF);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Command to test RM_ClusterCanAccessKeysInSlot(). */
int testClusterCanAccessKeysInSlot(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argc);
    long long slot = 0;

    if (RedisModule_StringToLongLong(argv[1],&slot) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid slot");
    }
    RedisModule_ReplyWithLongLong(ctx, RedisModule_ClusterCanAccessKeysInSlot(slot));
    return REDISMODULE_OK;
}

/* Generate a string representation of the info struct and subevent.
   e.g. 'sub: cluster-asm-import-started, task_id: aeBd..., slots: 0-100,200-300' */
const char *clusterMigrationInfoToString(RedisModuleClusterAsmMigrationInfo *info, uint64_t sub) {
    char buf[1024] = {0};

    if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_IMPORT_STARTED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-import-started, ");
    else  if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_IMPORT_FAILED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-import-failed, ");
    else if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_IMPORT_COMPLETED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-import-completed, ");
    else if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_STARTED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-migrate-started, ");
    else if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_FAILED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-migrate-failed, ");
    else if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_COMPLETED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-migrate-completed, ");
    else {
        RedisModule_Assert(0);
    }
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "source_node_id:%.40s, destination_node_id:%.40s, ",
             info->source_node_id, info->destination_node_id);
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "task_id:%s, slots:", info->task_id);
    for (int i = 0; i < info->slots->num_ranges; i++) {
        RedisModuleSlotRange *sr = &info->slots->ranges[i];
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d-%d", sr->start, sr->end);
        if (i != info->slots->num_ranges - 1)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",");
    }
    return RedisModule_Strdup(buf);
}

/* Generate a string representation of the info struct and subevent.
   e.g. 'sub: cluster-asm-trim-started, task_id: aeBd..., slots:0-100,200-300' */
const char *clusterTrimInfoToString(RedisModuleClusterAsmTrimInfo *info, uint64_t sub) {
    RedisModule_Assert(info);
    char buf[1024] = {0};

    if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_TRIM_BACKGROUND)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-trim-background, ");
    else if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_TRIM_STARTED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-trim-started, ");
    else if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_TRIM_COMPLETED)
        snprintf(buf, sizeof(buf), "sub: cluster-asm-trim-completed, ");
    else {
        RedisModule_Assert(0);
    }
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "slots:");
    for (int i = 0; i < info->slots->num_ranges; i++) {
        RedisModuleSlotRange *sr = &info->slots->ranges[i];
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d-%d", sr->start, sr->end);
        if (i != info->slots->num_ranges - 1)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",");
    }
    return RedisModule_Strdup(buf);
}

static void testReplicatingOutsideSlotRange(RedisModuleCtx *ctx, RedisModuleClusterAsmMigrationInfo *info) {
    int slot = 0;
    while (slot >= 0 && slot <= 16383) {
        if (!slotRangeArrayContains(info->slots, slot)) {
            break;
        }
        slot++;
    }
    char buf[128] = {0};
    const char *prefix = RedisModule_ClusterCanonicalKeyNameInSlot(slot);
    snprintf(buf, sizeof(buf), "{%s}%s", prefix, "modulekey");
    errno = 0;
    int ret = RedisModule_ClusterPropagateForSlotMigration(ctx, "SET", "cc", buf, "value");
    RedisModule_Assert(ret == REDISMODULE_ERR);
    RedisModule_Assert(errno == ERANGE);
}

static void testReplicatingCrossslotCommand(RedisModuleCtx *ctx) {
    errno = 0;
    int ret = RedisModule_ClusterPropagateForSlotMigration(ctx, "MSET", "cccccc", "key1", "val1", "key2", "val2", "key3", "val3");
    RedisModule_Assert(ret == REDISMODULE_ERR);
    RedisModule_Assert(errno == ENOTSUP);
}

static void testReplicatingUnknownCommand(RedisModuleCtx *ctx) {
    errno = 0;
    int ret = RedisModule_ClusterPropagateForSlotMigration(ctx, "unknowncommand", "");
    RedisModule_Assert(ret == REDISMODULE_ERR);
    RedisModule_Assert(errno == ENOENT);
}

static void testNonFatalScenarios(RedisModuleCtx *ctx, RedisModuleClusterAsmMigrationInfo *info) {
    testReplicatingOutsideSlotRange(ctx, info);
    testReplicatingCrossslotCommand(ctx);
    testReplicatingUnknownCommand(ctx);
}

void clusterEventCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data) {
    REDISMODULE_NOT_USED(ctx);
    int ret;

    if (e.id == REDISMODULE_EVENT_CLUSTER_ASM) {
        RedisModuleClusterAsmMigrationInfo *info = data;

        if (sub == REDISMODULE_SUBEVENT_CLUSTER_ASM_MIGRATE_MODULE_PROPAGATE) {
            /* Test some non-fatal scenarios. */
            testNonFatalScenarios(ctx, info);

            if (replicateModuleCommand == 0) return;

            /* Replicate a keyless command. */
            ret = RedisModule_ClusterPropagateForSlotMigration(ctx, "asm.keyless_cmd", "");
            RedisModule_Assert(ret == REDISMODULE_OK);

            /* Propagate configured key and value. */
            ret = RedisModule_ClusterPropagateForSlotMigration(ctx, "SET", "ss", moduleCommandKeyName, moduleCommandKeyVal);
            RedisModule_Assert(ret == REDISMODULE_OK);
        } else {
            /* Log the event. */
            if (numClusterEvents >= MAX_EVENTS) return;
            clusterEventLog[numClusterEvents++] = clusterMigrationInfoToString(info, sub);
        }
    }
}

void clusterTrimEventCallback(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data) {
    REDISMODULE_NOT_USED(ctx);
    if (e.id == REDISMODULE_EVENT_CLUSTER_ASM_TRIM) {
        /* Log the event. */
        if (numClusterEvents >= MAX_EVENTS) return;
        RedisModuleClusterAsmTrimInfo *info = data;
        clusterTrimEventLog[numClusterTrimEvents++] = clusterTrimInfoToString(info, sub);
    }
}

static int keyspaceNotificationTrimmedCallback(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(ctx);

    RedisModule_Assert(type == REDISMODULE_NOTIFY_TRIMMED);
    RedisModule_Assert(strcmp(event, "trimmed") == 0);

    if (numClusterTrimEvents >= MAX_EVENTS) return REDISMODULE_OK;

    /* Log the trimmed key event. */
    size_t len;
    const char *key_str = RedisModule_StringPtrLen(key, &len);

    char buf[1024] = {0};
    snprintf(buf, sizeof(buf), "keyspace: trimmed, key: %s", key_str);

    clusterTrimEventLog[numClusterTrimEvents++] = RedisModule_Strdup(buf);
    return REDISMODULE_OK;
}

/* Clear both the cluster and trim event logs. */
int clearEventLog(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    for (int i = 0; i < numClusterEvents; i++)
        RedisModule_Free((void *)clusterEventLog[i]);
    numClusterEvents = 0;

    for (int i = 0; i < numClusterTrimEvents; i++)
        RedisModule_Free((void *)clusterTrimEventLog[i]);
    numClusterTrimEvents = 0;

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Reply with the cluster event log. */
int getClusterEventLog(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_ReplyWithArray(ctx, numClusterEvents);
    for (int i = 0; i < numClusterEvents; i++)
        RedisModule_ReplyWithStringBuffer(ctx, clusterEventLog[i], strlen(clusterEventLog[i]));
    return REDISMODULE_OK;
}

/* Reply with the cluster trim event log. */
int getClusterTrimEventLog(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_ReplyWithArray(ctx, numClusterTrimEvents);
    for (int i = 0; i < numClusterTrimEvents; i++)
        RedisModule_ReplyWithStringBuffer(ctx, clusterTrimEventLog[i], strlen(clusterTrimEventLog[i]));
    return REDISMODULE_OK;
}

/* A keyless command to test module command replication. */
int moduledata = 0;
int keylessCmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    moduledata++;
    RedisModule_ReplyWithLongLong(ctx, moduledata);
    return REDISMODULE_OK;
}
int readkeylessCmdVal(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_ReplyWithLongLong(ctx, moduledata);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "asm", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.cluster_can_access_keys_in_slot", testClusterCanAccessKeysInSlot, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.clear_event_log", clearEventLog, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.get_cluster_event_log", getClusterEventLog, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.get_cluster_trim_event_log", getClusterTrimEventLog, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.keyless_cmd", keylessCmd, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.read_keyless_cmd_val", readkeylessCmdVal, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.sanity", sanity, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.replicate_module_command", replicate_module_command, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.lpush_replicate_crossslot_command", lpush_and_replicate_crossslot_command, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "asm.cluster_get_local_slot_ranges", testClusterGetLocalSlotRanges, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ClusterAsm, clusterEventCallback) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ClusterAsmTrim, clusterTrimEventCallback) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_TRIMMED, keyspaceNotificationTrimmedCallback) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
