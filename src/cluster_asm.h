/* cluster_asm.h -- Atomic slot migration implementation for cluster
 *
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef CLUSTER_ASM_H
#define CLUSTER_ASM_H

struct asmTask;
struct slotRangeArray;
struct slotRange;

void asmInit(void);
void asmBeforeSleep(void);
void asmCron(void);
void asmSlotSnapshotAndStreamStart(struct asmTask *task);
void asmSlotSnapshotSucceed(struct asmTask *task);
void asmSlotSnapshotFailed(struct asmTask *task);
void asmCallbackOnFreeClient(client *c);
int asmMigrateInProgress(void);
int asmImportInProgress(void);
void asmFeedMigrationClient(robj **argv, int argc);
int asmDebugSetFailPoint(char * channel, char *state);
int asmDebugSetTrimMethod(const char *method, int active_trim_delay);

void asmImportIncrAppliedBytes(struct asmTask *task, size_t bytes);
struct slotRangeArray *asmTaskGetSlotRanges(const char *task_id);
int asmNotifyConfigUpdated(struct asmTask *task, sds *err);
size_t asmGetPeakSyncBufferSize(void);
size_t asmGetImportInputBufferSize(void);
size_t asmGetMigrateOutputBufferSize(void);
int clusterAsmCancel(const char *task_id, const char *reason);
int clusterAsmCancelBySlot(int slot, const char *reason);
int clusterAsmCancelBySlotRangeArray(struct slotRangeArray *slots, const char *reason);
int clusterAsmCancelByNode(void *node, const char *reason);
int isSlotInAsmTask(int slot);
int isSlotInTrimJob(int slot);
sds asmCatInfoString(sds info);
void clusterMigrationCommand(client *c);
void clusterSyncSlotsCommand(client *c);
struct asmTask *asmLookupTaskBySlotRangeArray(struct slotRangeArray *slots);
void asmCancelTrimJobs(void);
sds asmDumpActiveImportTask(void);
int asmReplicaHandleMasterTask(sds task_info);
void asmFinalizeMasterTask(void);
int asmIsTrimInProgress(void);
int asmGetTrimmingSlotForCommand(struct redisCommand *cmd, robj **argv, int argc);
void asmActiveTrimCycle(void);
int asmActiveTrimDelIfNeeded(redisDb *db, robj *key, kvobj *kv);
int asmModulePropagateBeforeSlotSnapshot(struct redisCommand *cmd, robj **argv, int argc);
#endif

