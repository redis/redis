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

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However, if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
static inline unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (likely(s == keylen)) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing between {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* functions requiring mechanism specific implementations */
void clusterInit(void);
void clusterInitLast(void);
void clusterCommonInit(void);
void clusterCron(void);
void clusterBeforeSleep(void);
int verifyClusterConfigWithData(void);

int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len);

void clusterUpdateMyselfFlags(void);
void clusterUpdateMyselfIp(void);
void clusterUpdateMyselfHostname(void);
void clusterUpdateMyselfAnnouncedPorts(void);
void clusterUpdateMyselfHumanNodename(void);

void clusterPropagatePublish(robj *channel, robj *message, int sharded);

unsigned long getClusterConnectionsCount(void);
int isClusterHealthy(void);

sds clusterGenNodesDescription(client *c, int filter, int tls_primary);
sds genClusterInfoString(void);
/* handle implementation specific debug cluster commands. Return 1 if handled, 0 otherwise. */
int handleDebugClusterCommand(client *c);
const char **clusterDebugCommandExtendedHelp(void);
/* handle implementation specific cluster commands. Return 1 if handled, 0 otherwise. */
int clusterCommandSpecial(client *c);
const char** clusterCommandExtendedHelp(void);

int clusterAllowFailoverCmd(client *c);
void clusterPromoteSelfToMaster(void);
int clusterManualFailoverTimeLimit(void);

void clusterCommandSlots(client * c);
void clusterCommandMyId(client *c);
void clusterCommandMyShardId(client *c);

sds clusterGenNodeDescription(client *c, clusterNode *node, int tls_primary);

int clusterNodeCoversSlot(clusterNode *n, int slot);
int getNodeDefaultClientPort(clusterNode *n);
int clusterNodeIsMyself(clusterNode *n);
clusterNode *getMyClusterNode(void);
char *getMyClusterId(void);
int getClusterSize(void);
int getMyShardSlotCount(void);
int clusterNodePending(clusterNode  *node);
char **getClusterNodesList(size_t *numnodes);
int clusterNodeIsMaster(clusterNode *n);
char *clusterNodeIp(clusterNode *node);
int clusterNodeIsSlave(clusterNode *node);
clusterNode *clusterNodeGetSlaveof(clusterNode *node);
clusterNode *clusterNodeGetMaster(clusterNode *node);
char *clusterNodeGetName(clusterNode *node);
int clusterNodeTimedOut(clusterNode *node);
int clusterNodeIsFailing(clusterNode *node);
int clusterNodeIsNoFailover(clusterNode *node);
char *clusterNodeGetShardId(clusterNode *node);
int clusterNodeNumSlaves(clusterNode *node);
clusterNode *clusterNodeGetSlave(clusterNode *node, int slave_idx);
clusterNode *getMigratingSlotDest(int slot);
clusterNode *getImportingSlotSource(int slot);
clusterNode *getNodeBySlot(int slot);
int clusterNodeClientPort(clusterNode *n, int use_tls);
char *clusterNodeHostname(clusterNode *node);
const char *clusterNodePreferredEndpoint(clusterNode *n);
long long clusterNodeReplOffset(clusterNode *node);
clusterNode *clusterLookupNode(const char *name, int length);
const char *clusterGetSecret(size_t *len);
int clusterIsMySlot(int slot);

/* functions with shared implementations */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, uint64_t cmd_flags, int *error_code);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);
void migrateCloseTimedoutSockets(void);
int patternHashSlot(char *pattern, int length);
int getSlotOrReply(client *c, robj *o);
int isValidAuxString(char *s, unsigned int length);
void migrateCommand(client *c);
void clusterCommand(client *c);
ConnectionType *connTypeOfCluster(void);

typedef struct slotRange {
    unsigned short start, end;
} slotRange;
typedef struct slotRangeArray {
    int num_ranges;
    slotRange ranges[];
} slotRangeArray;
slotRangeArray *slotRangeArrayCreate(int num_ranges);
slotRangeArray *slotRangeArrayDup(slotRangeArray *sra);
void slotRangeArraySet(slotRangeArray *sra, int idx, int start, int end);
sds slotRangeArrayToString(slotRangeArray *sra);
int slotRangeArrayIsEqual(slotRangeArray *sra1, slotRangeArray *sra2);
slotRangeArray *slotRangeArrayAppend(slotRangeArray *sra, int slot);
int slotRangeArrayContains(slotRangeArray *sra, unsigned int slot);
void slotRangeArrayFree(slotRangeArray *sra);
int validateSlotRanges(slotRangeArray *sra, sds *err);
slotRangeArray *parseSlotRangesOrReply(client *c, int argc, int pos);

#define CLUSTER_DELKEYS_NONE        (0)
#define CLUSTER_DELKEYS_ASYNC       (1 << 0)
#define CLUSTER_DELKEYS_BY_COMMAND  (1 << 1)
unsigned int clusterDelKeysInSlot(unsigned int hashslot, int flags);
unsigned int clusterDelKeysInSlotRangeArray(slotRangeArray *sra, int flags);

void clusterGenNodesSlotsInfo(int filter);
void clusterFreeNodesSlotsInfo(clusterNode *n);
int clusterNodeSlotInfoCount(clusterNode *n);
uint16_t clusterNodeSlotInfoEntry(clusterNode *n, int idx);
int clusterNodeHasSlotInfo(clusterNode *n);

int clusterGetShardCount(void);
void *clusterGetShardIterator(void);
void *clusterNextShardHandle(void *shard_iterator);
void clusterFreeShardIterator(void *shard_iterator);
int clusterGetShardNodeCount(void *shard);
void *clusterShardHandleGetNodeIterator(void *shard);
clusterNode *clusterShardNodeIteratorNext(void *node_iterator);
void clusterShardNodeIteratorFree(void *node_iterator);
clusterNode *clusterShardNodeFirst(void *shard);

int clusterNodeTcpPort(clusterNode *node);
int clusterNodeTlsPort(clusterNode *node);

/* API for implementation/plugin
 *
 * - On destination side, implementation calls clusterAsmProcess(ASM_EVENT_IMPORT_START)
 *   to start the import operation.
 * - Redis calls clusterAsmOnEvent() when an event occurs.
 * - On the source side, Redis will call clusterAsmOnEvent(ASM_EVENT_HANDOFF_PREP)
 *   when slots are ready to be handed off  and the write pause is needed.
 * - Implementation stops the traffic to the slots and calls clusterAsmProcess(ASM_EVENT_HANDOFF)
 * - On the destination side, Redis calls clusterAsmOnEvent(ASM_EVENT_TAKEOVER)
 *   when destination node is ready to take over the slot, waiting for config change.
 * - Plugin updates the config and calls clusterAsmProcess(ASM_EVENT_DONE)
 *   to notify Redis that the config is updated.
 *
 * Sequence diagram for import:
 *   - Note: shows only the events that plugin needs to react.
 *
 * ┌───────────────┐              ┌───────────────┐         ┌───────────────┐             ┌───────────────┐
 * │ Destination   │              │ Destination   │         │    Source     │             │ Source        │
 * │ Cluster plugin│              │ Master        │         │    Master     │             │ Cluster plugin│
 * └───────┬───────┘              └───────┬───────┘         └───────┬───────┘             └───────┬───────┘
 *         │                              │                         │                             │
 *         │     ASM_EVENT_IMPORT_START   │                         │                             │
 *         ├─────────────────────────────►│                         │                             │
 *         │                              │ CLUSTER SYNCSLOTS <arg> │                             │
 *         │                              ├────────────────────────►│                             │
 *         │                              │                         │                             │
 *         │                              │  SNAPSHOT(restore cmds) │                             │
 *         │                              │◄────────────────────────┤                             │
 *         │                              │  Repl stream            │                             │
 *         │                              │◄────────────────────────┤                             │
 *         │                              │                         │   ASM_EVENT_HANDOFF_PREP    │
 *         │                              │                         ├────────────────────────────►│
 *         │                              │                         │     ASM_EVENT_HANDOFF       │
 *         │                              │                         │◄────────────────────────────┤
 *         │                              │ Drain repl stream       │                             │
 *         │                              │◄────────────────────────┤                             │
 *         │     ASM_EVENT_TAKEOVER       │                         │                             │
 *         │◄─────────────────────────────┤                         │                             │
 *         │                              │                         │                             │
 *         │       ASM_EVENT_DONE         │                         │                             │
 *         ├─────────────────────────────►│                         │       ASM_EVENT_DONE        │
 *         │                              │                         │◄────────────────────────────┤
 *         │                              │                         │                             │
 */

#define ASM_EVENT_IMPORT_START      1  /* Start a new import operation (destination side) */
#define ASM_EVENT_CANCEL            2  /* Cancel an ongoing import/migrate operation (source and destination side) */
#define ASM_EVENT_HANDOFF_PREP      3  /* Slot is ready to be handed off to the destination shard (source side) */
#define ASM_EVENT_HANDOFF           4  /* Notify that the slot can be handed off (source side) */
#define ASM_EVENT_TAKEOVER          5  /* Ready to take over the slot, waiting for config change (destination side) */
#define ASM_EVENT_DONE              6  /* Notify that import/migrate is completed, config is updated (source and destination side) */

#define ASM_EVENT_IMPORT_STARTED    7  /* Import started */
#define ASM_EVENT_IMPORT_FAILED     8  /* Import failed */
#define ASM_EVENT_IMPORT_COMPLETED  9  /* Import completed (config updated) */
#define ASM_EVENT_MIGRATE_STARTED   10 /* Migration started */
#define ASM_EVENT_MIGRATE_FAILED    11 /* Migration failed */
#define ASM_EVENT_MIGRATE_COMPLETED 12 /* Migrate completed (config updated) */


/* Called by plugin/implementation to request an ASM operation. (plugin --> redis)
 * Valid values for 'event':
 *  ASM_EVENT_IMPORT_START
 *  ASM_EVENT_CANCEL
 *  ASM_EVENT_HANDOFF
 *  ASM_EVENT_DONE
 *
 * For ASM_EVENT_IMPORT_START, 'task_id' should be a unique string.
 * For other events (ASM_EVENT_CANCEL, ASM_EVENT_HANDOFF, ASM_EVENT_DONE),
 * 'task_id' should match the ID from the corresponding import operation.
 *    Usage:
 *      char *task_id = malloc(CLUSTER_NAMELEN + 1);
 *      getRandomHexChars(task_id, CLUSTER_NAMELEN);
 *      task_id[CLUSTER_NAMELEN] = '\0';
 *
 *      slotRangeArray *sra  = zmalloc(sizeof(*sra) + sizeof(slotRange));
 *      sra->num_ranges = 1;
 *      sra->ranges[0].start = 0;
 *      sra->ranges[0].end = 1000;
 *
 *      const char *err = NULL;
 *      int ret = clusterAsmProcess(task_id, ASM_EVENT_IMPORT_START, sra, &err);
 *      free(task_id);
 *      free(sra);
 *
 *      if (ret != C_OK) {
 *          perror(err);
 *          return;
 *      }
 *
 * For ASM_EVENT_CANCEL, if `task_id` is NULL, all tasks will be cancelled.
 * If `arg` parameter is provided, it should be a pointer to an int. It will be
 * set to the number of tasks cancelled.
 *
 * Return value:
 *  - Returns C_OK on success, C_ERR on failure and 'err' will be set to the
 *    error message.
 *
 * Memory management:
 *  - There is no ownership transfer of 'task_id', 'err' or `slotRangeArray`.
 *  - `task_id` and `slotRangeArray` should be allocated and be freed by the
 *     caller. Redis internally will make a copy of these.
 *  - `err` is allocated by Redis and should NOT be freed by the caller.
 **/
int clusterAsmProcess(const char *task_id, int event, void *arg, char **err);

/* Called when an ASM event occurs to notify implementation/plugin. (redis --> plugin)
 *
 * `arg` will point to a `slotRangeArray` for the following events:
 *  ASM_EVENT_IMPORT_STARTED
 *  ASM_EVENT_MIGRATE_STARTED
 *  ASM_EVENT_HANDOFF_PREP
 *
 *  Memory management:
 *  - Redis owns the `task_id` and `slotRangeArray`.
 *
 *  Returns C_OK on success.
 **/
int clusterAsmOnEvent(const char *task_id, int event, void *arg);

#endif /* __CLUSTER_H */
