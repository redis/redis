/*
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

void clusterAsmInit(void);
void asmBeforeSleep(void);
void asmCron(void);
void asmStartSendBulkAndStream(struct asmTask *task);
void asmCallbackOnFreeClient(client *c);
int asmMigrateInProgress(void);
void asmFeedMigrationClient(robj **argv, int argc);
int asmDebugSetFailPoint(char * channel, char *state);
void asmImportIncrAppliedBytes(struct asmTask *task, size_t bytes);
slotRangeArray *asmTaskGetSlotRanges(sds task_id);
int asmNotifyConfigUpdated(slotRangeArray *slot_ranges, sds *err);
size_t asmGetPeakSyncBufferSize(void);
int asmKeyBelongsToCurrentNode(kvobj *kv);

void clusterMigrationCommand(client *c);
void clusterSyncSlotsCommand(client *c);

#endif

