#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOTS 16384
#define CLUSTER_NAMELEN 40      /* sha1 hex length */

/* Redirection errors returned by getNodeByQuery(). */
#define CLUSTER_REDIR_NONE 0          /* Node can serve the request. */
#define CLUSTER_REDIR_CROSS_SLOT 1    /* -CROSSSLOT request. */
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required */
#define CLUSTER_REDIR_ASK 3           /* -ASK redirection required. */
#define CLUSTER_REDIR_MOVED 4         /* -MOVED redirection required. */
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. */
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, unbound slot. */
#define CLUSTER_REDIR_DOWN_RO_STATE 7 /* -CLUSTERDOWN, allow reads. */

/* Flags that a module can set in order to prevent certain Redis Cluster
 * features to be enabled. Useful when implementing a different distributed
 * system on top of Redis Cluster message bus, using modules. */
#define CLUSTER_MODULE_FLAG_NONE 0
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1<<1)
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1<<2)

/*-----------------------------------------------------------------------------
 * Slot to Keys mapping structures
 *----------------------------------------------------------------------------*/
/* Slot to keys for a single slot. The keys in the same slot are linked together
 * using dictEntry metadata. */
typedef struct slotToKeys {
    uint64_t count;             /* Number of keys in the slot. */
    dictEntry *head;            /* The first key-value entry in the slot. */
} slotToKeys;

/* Slot to keys mapping for all slots, opaque outside this file. */
struct clusterSlotToKeyMapping {
    slotToKeys by_slot[CLUSTER_SLOTS];
};

/* Dict entry metadata for cluster mode, used for the Slot to Key API to form a
 * linked list of the entries belonging to the same slot. */
typedef struct clusterDictEntryMetadata {
    dictEntry *prev;            /* Prev entry with key in the same slot */
    dictEntry *next;            /* Next entry with key in the same slot */
} clusterDictEntryMetadata;

typedef struct {
    redisDb *db;                /* A link back to the db this dict belongs to */
} clusterDictMetadata;

/*-----------------------------------------------------------------------------
 * Cluster public structs
 *----------------------------------------------------------------------------*/

typedef struct clusterNode {
    char name[CLUSTER_NAMELEN]; /* Node name, hex string, sha1-size */
    char shard_id[CLUSTER_NAMELEN]; /* shard id, hex string, sha1-size */
    void* data;
} clusterNode;

typedef struct clusterState {
    clusterNode *myself;  /* This node */
    dict *nodes;          /* Hash table of name -> clusterNode structures */
    /* Manual failover state in common. */
    mstime_t mf_end;            /* Manual failover time limit (ms unixtime).
                                   It is zero if there is no MF in progress. */
    void *internal;
} clusterState;

/* ---------------------- API exported outside cluster.c -------------------- */
void clusterInit(void);
void clusterInitListeners(void);
void clusterCron(void);
void clusterBeforeSleep(void);
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
int verifyClusterNodeId(const char *name, int length);
clusterNode *clusterLookupNode(const char *name, int length);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);
void migrateCloseTimedoutSockets(void);
int verifyClusterConfigWithData(void);
unsigned long getClusterConnectionsCount(void);
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len);
void clusterPropagatePublish(robj *channel, robj *message, int sharded);
unsigned int keyHashSlot(char *key, int keylen);
void slotToKeyAddEntry(dictEntry *entry, redisDb *db);
void slotToKeyDelEntry(dictEntry *entry, redisDb *db);
void slotToKeyReplaceEntry(dict *d, dictEntry *entry);
void slotToKeyInit(redisDb *db);
void slotToKeyFlush(redisDb *db);
void slotToKeyDestroy(redisDb *db);
void clusterUpdateMyselfFlags(void);
void clusterUpdateMyselfIp(void);
void slotToChannelAdd(sds channel);
void slotToChannelDel(sds channel);
void clusterUpdateMyselfHostname(void);
void clusterUpdateMyselfAnnouncedPorts(void);
sds clusterGenNodesDescription(client *c, int filter, int use_pport);
sds genClusterInfoString(void);
char* clusterNodeLastKnownIp(clusterNode *node);
int clusterNodePort(clusterNode *node);
void freeThisNodesLink(clusterNode *node);
void freeNodeInboundLink(clusterNode *node);
clusterNode* clusterNodeGetSlaveof(clusterNode *node);
int clusterNodeConfirmedReachable(clusterNode* node);
int clusterNodeIsMaster(clusterNode* node);
int clusterNodeIsSlave(clusterNode* node);
int clusterNodeIsFailing(clusterNode* node);
int clusterNodeTimedOut(clusterNode* node);
int clusterNodeIsMyself(clusterNode* node);
int clusterNodeIsNoFailover(clusterNode* node);
#endif /* __CLUSTER_H */
