/*
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __CLUSTER_PLUGIN_H
#define __CLUSTER_PLUGIN_H

#include "server.h"
#include "cluster.h"

typedef int (*clusterAllowFailoverCmdFunc)(client *c);
typedef sds (*clusterGenNodesDescriptionFunc)(client *c, int filter, int tls_primary);
typedef sds (*clusterGenNodeDescriptionFunc)(client *c, clusterNode *node, int tls_primary);
typedef clusterNode* (*clusterLookupNodeFunc)(const char *name, int length);
typedef int (*clusterNodeClientPortFunc)(clusterNode *n, int use_tls);
typedef int (*clusterNodeCoversSlotFunc)(clusterNode *n, int slot);
typedef char* (*clusterNodeGetNameFunc)(clusterNode *node);
typedef char* (*clusterNodeGetShardIdFunc)(clusterNode *node);
typedef clusterNode* (*clusterNodeGetSlaveFunc)(clusterNode *node, int slave_idx);
typedef clusterNode* (*clusterNodeGetSlaveofFunc)(clusterNode *node);
typedef char* (*clusterNodeHostnameFunc)(clusterNode *node);
typedef char* (*clusterNodeIpFunc)(clusterNode *node);
typedef int (*clusterNodeIsMasterFunc)(clusterNode *n);
typedef int (*clusterNodeIsMyselfFunc)(clusterNode *n);
typedef int (*clusterNodeNumSlavesFunc)(clusterNode *node);
typedef const char* (*clusterNodePreferredEndpointFunc)(clusterNode *n);
typedef long long (*clusterNodeReplOffsetFunc)(clusterNode *node);
typedef void (*clusterPromoteSelfToMasterFunc)(void);
typedef sds (*genClusterInfoStringFunc)(void);
typedef int (*getClusterSizeFunc)(void);
typedef clusterNode* (*getMigratingSlotDestFunc)(int slot);
typedef clusterNode* (*getImportingSlotSourceFunc)(int slot);
typedef clusterNode* (*getMyClusterNodeFunc)(void);
typedef int (*getMyShardSlotCountFunc)(void);
typedef clusterNode* (*getNodeBySlotFunc)(int slot);
typedef void (*clusterCronFunc)(void);
typedef clusterNode* (*getNodeAtIdxFunc)(int idx);

typedef clusterNode* (*clusterNodeGetMasterFunc)(clusterNode *node);
typedef void (*clusterGenNodesSlotsInfoFunc)(int filter);
typedef void (*clusterFreeNodesSlotsInfoFunc)(clusterNode *n);
typedef int (*clusterNodeSlotInfoCountFunc)(clusterNode *n);
typedef uint16_t (*clusterNodeSlotInfoEntryFunc)(clusterNode *n, int idx);
typedef int (*clusterNodeHasSlotInfoFunc)(clusterNode *n);

typedef int (*clusterGetShardCountFunc)(void);
typedef void *(*clusterGetShardIteratorFunc)(void);
typedef void *(*clusterNextShardHandleFunc)(void *shard_iterator);
typedef void (*clusterFreeShardIteratorFunc)(void *shard_iterator);
typedef int (*clusterGetShardNodeCountFunc)(void *shard);

typedef void *(*clusterShardHandleGetNodeIteratorFunc)(void *shard);
typedef clusterNode *(*clusterShardNodeIteratorNextFunc)(void *node_iterator);
typedef void (*clusterShardNodeIteratorFreeFunc)(void *node_iterator);
typedef clusterNode *(*clusterShardNodeFirstFunc)(void *shard);

typedef int (*clusterNodeTcpPortFunc)(clusterNode *node);
typedef int (*clusterNodeTlsPortFunc)(clusterNode *node);

typedef const char *(*clusterGetSecretFunc)(size_t *len);

typedef int (*clusterAsmOnEventFunc)(sds task_id, int event, void *arg);

typedef struct {
    clusterAllowFailoverCmdFunc clusterAllowFailoverCmd;
    clusterGenNodesDescriptionFunc clusterGenNodesDescription;
    clusterGenNodeDescriptionFunc clusterGenNodeDescription;
    clusterLookupNodeFunc clusterLookupNode;
    clusterNodeClientPortFunc clusterNodeClientPort;
    clusterNodeCoversSlotFunc clusterNodeCoversSlot;
    clusterNodeGetNameFunc clusterNodeGetName;
    clusterNodeGetShardIdFunc clusterNodeGetShardId;
    clusterNodeGetSlaveFunc clusterNodeGetSlave;
    clusterNodeGetSlaveofFunc clusterNodeGetSlaveof;
    clusterNodeHostnameFunc clusterNodeHostname;
    clusterNodeIpFunc clusterNodeIp;
    clusterNodeIsMasterFunc clusterNodeIsMaster;
    clusterNodeIsMyselfFunc clusterNodeIsMyself;
    clusterNodeNumSlavesFunc clusterNodeNumSlaves;
    clusterNodePreferredEndpointFunc clusterNodePreferredEndpoint;
    clusterNodeReplOffsetFunc clusterNodeReplOffset;
    clusterPromoteSelfToMasterFunc clusterPromoteSelfToMaster;
    genClusterInfoStringFunc genClusterInfoString;
    getClusterSizeFunc getClusterSize;
    getMigratingSlotDestFunc getMigratingSlotDest;
    getImportingSlotSourceFunc getImportingSlotSource;
    getMyClusterNodeFunc getMyClusterNode;
    getMyShardSlotCountFunc getMyShardSlotCount;
    getNodeBySlotFunc getNodeBySlot;
    clusterCronFunc clusterCron;
    getNodeAtIdxFunc getNodeAtIdx;
    clusterNodeGetMasterFunc clusterNodeGetMaster;
    clusterGenNodesSlotsInfoFunc clusterGenNodesSlotsInfo;
    clusterFreeNodesSlotsInfoFunc clusterFreeNodesSlotsInfo;
    clusterNodeSlotInfoCountFunc clusterNodeSlotInfoCount;
    clusterNodeSlotInfoEntryFunc clusterNodeSlotInfoEntry;
    clusterNodeHasSlotInfoFunc clusterNodeHasSlotInfo;
    clusterGetShardCountFunc clusterGetShardCount;
    clusterGetShardIteratorFunc clusterGetShardIterator;
    clusterNextShardHandleFunc clusterNextShardHandle;
    clusterFreeShardIteratorFunc clusterFreeShardIterator;
    clusterGetShardNodeCountFunc clusterGetShardNodeCount;
    clusterShardHandleGetNodeIteratorFunc clusterShardHandleGetNodeIterator;
    clusterShardNodeIteratorNextFunc clusterShardNodeIteratorNext;
    clusterShardNodeIteratorFreeFunc clusterShardNodeIteratorFree;
    clusterShardNodeFirstFunc clusterShardNodeFirst;
    clusterNodeTcpPortFunc clusterNodeTcpPort;
    clusterNodeTlsPortFunc clusterNodeTlsPort;
    clusterGetSecretFunc clusterGetSecret;
    clusterAsmOnEventFunc clusterAsmOnEvent;


} ClusterPlugin;

void clusterPluginInit(ClusterPlugin *plugin);
void *clusterPluginGetState(void);
void clusterPluginSetState(void *state);

#endif //__CLUSTER_PLUGIN_H
