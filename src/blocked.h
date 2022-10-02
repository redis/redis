#ifndef SRC_BLOCKED_H_
#define SRC_BLOCKED_H_

/* Client block type (btype field in client structure)
 * if CLIENT_BLOCKED flag is set. */
typedef enum blocking_type {
    BLOCKED_NONE,    /* Not blocked, no CLIENT_BLOCKED flag set. */
    BLOCKED_LIST,    /* BLPOP & co. */
    BLOCKED_WAIT,    /* WAIT for synchronous replication. */
    BLOCKED_MODULE,  /* Blocked by a loadable module. */
    BLOCKED_STREAM,  /* XREAD. */
    BLOCKED_ZSET,    /* BZPOP et al. */
    BLOCKED_POSTPONE, /* Blocked by processCommand, re-try processing later. */
    BLOCKED_SHUTDOWN, /* SHUTDOWN. */
    BLOCKED_NUM,      /* Number of blocked states. */
    BLOCKED_END       /* End of enumeration */
} blocking_type;

/* This structure holds the blocking operation state for a client.
 * The fields used depend on client->btype. */
typedef struct blockingState {
    /* Generic fields. */
    blocking_type btype;                  /* Type of blocking op if CLIENT_BLOCKED. */
    mstime_t timeout;           /* Blocking operation timeout. If UNIX current time
                                 * is > timeout then the operation timed out. */
    /* BLOCKED_LIST, BLOCKED_ZSET and BLOCKED_STREAM or any other Keys related blocking */
    dict *keys;                 /* The keys we are blocked on */

    /* BLOCKED_WAIT */
    int numreplicas;        /* Number of replicas we are waiting for ACK. */
    long long reploffset;   /* Replication offset to reach. */

    /* BLOCKED_MODULE */
    void *module_blocked_handle; /* RedisModuleBlockedClient structure.
                                    which is opaque for the Redis core, only
                                    handled in module.c. */
} blockingState;


#endif /* SRC_BLOCKED_H_ */
