#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster exported API.
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOT_MASK_BITS 14 /* Number of bits used for slot id. */
#define CLUSTER_SLOTS (1<<CLUSTER_SLOT_MASK_BITS) /* Total number of slots in cluster mode, which is 16384. */
#define CLUSTER_SLOT_MASK ((unsigned long long)(CLUSTER_SLOTS - 1)) /* Bit mask for slot id stored in LSB. */
#define CLUSTER_OK 0            /* Everything looks ok */
#define CLUSTER_FAIL 1          /* The cluster can't work */
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

typedef struct _clusterNode clusterNode;
struct clusterState;

/* Flags that a module can set in order to prevent certain Redis Cluster
 * features to be enabled. Useful when implementing a different distributed
 * system on top of Redis Cluster message bus, using modules. */
#define CLUSTER_MODULE_FLAG_NONE 0
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1<<1)
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1<<2)

/* ---------------------- API exported outside cluster.c -------------------- */
void clusterInit(void);
void clusterInitListeners(void);
void clusterCron(void);
void clusterBeforeSleep(void);
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
clusterNode *clusterLookupNode(const char *name, int length);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);
void migrateCloseTimedoutSockets(void);
int verifyClusterConfigWithData(void);
unsigned long getClusterConnectionsCount(void);
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len);
void clusterPropagatePublish(robj *channel, robj *message, int sharded);
unsigned int keyHashSlot(char *key, int keylen);
int patternHashSlot(char *pattern, int length);
void clusterUpdateMyselfFlags(void);
void clusterUpdateMyselfIp(void);
void slotToChannelAdd(sds channel);
void slotToChannelDel(sds channel);
void clusterUpdateMyselfHostname(void);
void clusterUpdateMyselfAnnouncedPorts(void);
sds clusterGenNodesDescription(client *c, int filter, int tls_primary);
sds genClusterInfoString(void);
int clusterNodeCoversSlot(clusterNode *n, int slot);
void clusterUpdateMyselfHumanNodename(void);
int isValidAuxString(char *s, unsigned int length);
int getNodeDefaultClientPort(clusterNode *n);
int clusterNodeIsMyself(clusterNode *n);
clusterNode* getMyClusterNode(void);
int clusterManualFailoverTimeLimit(void);
char* getMyClusterId(void);
int getClusterSize(void);
char** getClusterNodesList(size_t *numnodes);
int nodeIsMaster(clusterNode *n);
int handleDebugClusterCommand(client *c);
int clusterNodeConfirmedReachable(clusterNode  *node);
char* clusterNodeIp(clusterNode *node);
int clusterNodeIsSlave(clusterNode *node);
clusterNode *clusterNodeGetSlaveof(clusterNode *node);
char* clusterNodeGetName(clusterNode *node);
int clusterNodeTimedOut(clusterNode *node);
int clusterNodeIsFailing(clusterNode *node);
int clusterNodeIsNoFailover(clusterNode *node);

char **clusterDebugCommandHelp(void);
ConnectionType *connTypeOfCluster(void);
#endif /* __CLUSTER_H */
