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
#include "cluster_plugin.h"

ClusterPlugin *clusterPlugin = NULL;
void *clusterState = NULL;

void clusterPluginInit(ClusterPlugin *plugin) {
    clusterPlugin = plugin;
}

void clusterPluginSetState(void *state) {
    clusterState = state;
}

void *clusterPluginGetState(void) {
    return clusterState;
}

int clusterAllowFailoverCmd(client *c) {
    if (!server.cluster_enabled) {
        return 1;
    }
    return clusterPlugin->clusterAllowFailoverCmd(c);
}

sds clusterGenNodesDescription(client *c, int filter, int tls_primary) {
    return clusterPlugin->clusterGenNodesDescription(c, filter, tls_primary);
}

sds clusterGenNodeDescription(client *c, clusterNode *node, int tls_primary) {
    return clusterPlugin->clusterGenNodeDescription(c, node, tls_primary);
}

clusterNode *clusterLookupNode(const char *name, int length) {
    return clusterPlugin->clusterLookupNode(name, length);
}

int clusterNodeClientPort(clusterNode *n, int use_tls) {
    return clusterPlugin->clusterNodeClientPort(n, use_tls);
}

int clusterNodeCoversSlot(clusterNode *n, int slot) {
    return clusterPlugin->clusterNodeCoversSlot(n, slot);
}

char *clusterNodeGetName(clusterNode *node) {
    return clusterPlugin->clusterNodeGetName(node);
}

char *clusterNodeGetShardId(clusterNode *node) {
    return clusterPlugin->clusterNodeGetShardId(node);
}

clusterNode *clusterNodeGetSlave(clusterNode *node, int slave_idx) {
    return clusterPlugin->clusterNodeGetSlave(node, slave_idx);
}

clusterNode *clusterNodeGetSlaveof(clusterNode *node) {
    return clusterPlugin->clusterNodeGetSlaveof(node);
}

char *clusterNodeHostname(clusterNode *node) {
    return clusterPlugin->clusterNodeHostname(node);
}

char *clusterNodeIp(clusterNode *node) {
    return clusterPlugin->clusterNodeIp(node);
}

int clusterNodeIsMaster(clusterNode *n) {
    return clusterPlugin->clusterNodeIsMaster(n);
}

int clusterNodeIsMyself(clusterNode *n) {
    return clusterPlugin->clusterNodeIsMyself(n);
}

int clusterNodeNumSlaves(clusterNode *node) {
    return clusterPlugin->clusterNodeNumSlaves(node);
}

const char *clusterNodePreferredEndpoint(clusterNode *n) {
    return clusterPlugin->clusterNodePreferredEndpoint(n);
}

long long clusterNodeReplOffset(clusterNode *node) {
    return clusterPlugin->clusterNodeReplOffset(node);
}

void clusterPromoteSelfToMaster(void) {
    clusterPlugin->clusterPromoteSelfToMaster();
}

sds genClusterInfoString(void) {
    return clusterPlugin->genClusterInfoString();
}

int getClusterSize(void) {
    return clusterPlugin->getClusterSize();
}
clusterNode *getMigratingSlotDest(int slot) {
    return clusterPlugin->getMigratingSlotDest(slot);
}

clusterNode *getImportingSlotSource(int slot) {
    return clusterPlugin->getImportingSlotSource(slot);
}

clusterNode *getMyClusterNode(void) {
    if (clusterPlugin == NULL) {
        return NULL;
    }
    return clusterPlugin->getMyClusterNode();
}

int getMyShardSlotCount(void) {
    return clusterPlugin->getMyShardSlotCount();
}

clusterNode *getNodeBySlot(int slot) {
    return clusterPlugin->getNodeBySlot(slot);
}

void clusterInit(void) {
    clusterCommonInit();
}

void clusterCron(void) {
    clusterPlugin->clusterCron();
}

int clusterCommandSpecial(client *c) {
    UNUSED(c);
    return 0;
}

const char** clusterCommandExtendedHelp(void) {
    static const char *help[] = {NULL};
    return help;
}

const char** clusterDebugCommandExtendedHelp(void) {
    static const char *help[] = {NULL};
    return help;
}

int handleDebugClusterCommand(client *c) {
    UNUSED(c);
    return 0;
}

void clusterInitLast(void) {
}

void clusterBeforeSleep(void) {
}

int verifyClusterConfigWithData(void) {
    return 0;
}

void clusterUpdateMyselfFlags(void) {
}

unsigned long getClusterConnectionsCount(void) {
    return 0;
}

void clusterUpdateMyselfHostname(void) {
}

void clusterUpdateMyselfAnnouncedPorts(void) {
}

void clusterUpdateMyselfIp(void) {
}

int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len) {
    UNUSED(target);
    UNUSED(module_id);
    UNUSED(type);
    UNUSED(payload);
    UNUSED(len);
    return 0;
}

void clusterPropagatePublish(robj *channel, robj *message, int sharded) {
    UNUSED(channel);
    UNUSED(message);
    UNUSED(sharded);
}

void slotToChannelAdd(sds channel) {
    UNUSED(channel);
}

void slotToChannelDel(sds channel) {
    UNUSED(channel);
}

int clusterNodePending(clusterNode *node) {
    UNUSED(node);
    return 0;
}

int clusterNodeIsSlave(clusterNode *node) {
    return !clusterNodeIsMaster(node);
}

int clusterNodeIsFailing(clusterNode *node) {
    UNUSED(node);
    return 0;
}

int clusterNodeTimedOut(clusterNode *node) {
    UNUSED(node);
    return 0;
}

int clusterNodeIsNoFailover(clusterNode *node) {
    UNUSED(node);
    return 0;
}

int isClusterHealthy(void) {
    return 1;
}

int clusterManualFailoverTimeLimit(void) {
    return 0;
}

int clusterEnabled(void) {
    return server.cluster_enabled;
}

void clusterUpdateMyselfHumanNodename(void) {
}

int getNodeDefaultClientPort(clusterNode *n) {
    UNUSED(n);
    return 0;
}

char** getClusterNodesList(size_t *numnodes) {
    size_t count = clusterPlugin->getClusterSize();
    char **ids = zmalloc((count+1)*CLUSTER_NAMELEN);
    for(size_t i = 0; i < count; i++) {
        clusterNode *node = clusterPlugin->getNodeAtIdx(i);
        ids[i] = zmalloc(CLUSTER_NAMELEN);
        memcpy(ids[i], clusterNodeGetName(node),CLUSTER_NAMELEN);
    }
    *numnodes = count;
    ids[count] = NULL; /* Null term so that FreeClusterNodesList does not need
                    * to also get the count argument. */
    return ids;
}

clusterNode *clusterNodeGetMaster(clusterNode *node) {
    return clusterPlugin->clusterNodeGetMaster(node);
}

void clusterGenNodesSlotsInfo(int filter) {
    clusterPlugin->clusterGenNodesSlotsInfo(filter);
}

void clusterFreeNodesSlotsInfo(clusterNode *node) {
    clusterPlugin->clusterFreeNodesSlotsInfo(node);
}

int clusterNodeSlotInfoCount(clusterNode *n) {
    return clusterPlugin->clusterNodeSlotInfoCount(n);
}

uint16_t clusterNodeSlotInfoEntry(clusterNode *n, int idx) {
    return clusterPlugin->clusterNodeSlotInfoEntry(n, idx);
}

int clusterNodeHasSlotInfo(clusterNode *n) {
    return clusterPlugin->clusterNodeHasSlotInfo(n);
}

int clusterGetShardCount(void) {
    return clusterPlugin->clusterGetShardCount();
}

void *clusterGetShardIterator(void) {
    return clusterPlugin->clusterGetShardIterator();
}

void *clusterNextShardHandle(void *shard_iterator) {
    return clusterPlugin->clusterNextShardHandle(shard_iterator);
}

void clusterFreeShardIterator(void *shard_iterator) {
    clusterPlugin->clusterFreeShardIterator(shard_iterator);
}

int clusterGetShardNodeCount(void *shard) {
    return clusterPlugin->clusterGetShardNodeCount(shard);
}

void *clusterShardHandleGetNodeIterator(void *shard) {
    return clusterPlugin->clusterShardHandleGetNodeIterator(shard);
}

clusterNode *clusterShardNodeIteratorNext(void *node_iterator) {
    return clusterPlugin->clusterShardNodeIteratorNext(node_iterator);
}

void clusterShardNodeIteratorFree(void *node_iterator) {
    clusterPlugin->clusterShardNodeIteratorFree(node_iterator);
}

clusterNode *clusterShardNodeFirst(void *shard) {
    return clusterPlugin->clusterShardNodeFirst(shard);
}

int clusterNodeTcpPort(clusterNode *node) {
    return clusterPlugin->clusterNodeTcpPort(node);
}

int clusterNodeTlsPort(clusterNode *node) {
    return clusterPlugin->clusterNodeTlsPort(node);
}

const char *clusterGetSecret(size_t *len) {
    return clusterPlugin->clusterGetSecret(len);
}

int clusterAsmOnEvent(slotRangeArray *slot_ranges, int event, void *arg) {
    return clusterPlugin->clusterAsmOnEvent(slot_ranges, event, arg);
}

