/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#include "server.h"
#include "cluster.h"
#include "script.h"
#include "cluster_legacy.h"

/* General use-cases. */
void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);

/* cpu-usec metric. */
void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration);
void clusterSlotStatsInvalidateSlotIfApplicable(scriptRunCtx *ctx);

/* network-bytes-in metric. */
void clusterSlotStatsAddNetworkBytesInForUserClient(client *c);

/* network-bytes-out metric. */
void clusterSlotStatsAddNetworkBytesOutForUserClient(client *c);
void clusterSlotStatsIncrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsDecrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(client *c, int slot);
