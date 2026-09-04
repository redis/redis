/*
 * memcached.h -- memcached text protocol support.
 *
 * See memcached.c for the design notes and the list of intentional
 * deviations from both memcached and Redis semantics.
 */

#ifndef __MEMCACHED_H
#define __MEMCACHED_H

#include <stdint.h>
#include <stddef.h>

struct client;
struct redisDb;
struct redisObject;

/* Largest item we accept, matching memcached's default `-I 1m`. Larger
 * requests get "SERVER_ERROR object too large for cache". */
#define MEMCACHED_MAX_ITEM_SIZE (1024*1024)

/* memcached's key limit. Keys are also rejected if they contain a space or a
 * control character. */
#define MEMCACHED_MAX_KEY_LEN 250

/* Longest command line (everything up to and including the terminating "\n")
 * we are willing to buffer before declaring a protocol error. A multi-get of
 * 250 byte keys stays well below this. */
#define MEMCACHED_MAX_LINE_LEN (1024*64)

/* Anything at or below this many seconds is a relative expiry, anything above
 * it is an absolute unix timestamp. This is memcached's REALTIME_MAXDELTA. */
#define MEMCACHED_REALTIME_MAXDELTA 2592000 /* 30 days */

/* Per-connection state for a memcached client. Allocated only for clients
 * accepted on the memcached listener. */
typedef struct mcClient {
    long long swallow;  /* Bytes of a rejected data block still to discard. */
} mcClient;

/* Counters backing the `stats` command. These are memcached's own counters
 * and are deliberately kept separate from Redis' command stats, because the
 * two disagree about what a hit and a miss are. */
typedef struct mcStats {
    long long total_connections;
    long long curr_connections;
    long long cmd_get;
    long long cmd_set;
    long long cmd_flush;
    long long cmd_touch;
    long long get_hits;
    long long get_misses;
    long long delete_hits;
    long long delete_misses;
    long long incr_hits;
    long long incr_misses;
    long long decr_hits;
    long long decr_misses;
    long long touch_hits;
    long long touch_misses;
    long long total_items;
} mcStats;

/* Lifecycle. */
void memcachedInit(void);
void memcachedInitListener(void);
void memcachedValidateConfigOrExit(void);
void memcachedClientCreated(struct client *c);
void memcachedClientFreed(struct client *c);

/* Protocol entry point, called from processInputBuffer() for clients carrying
 * the CLIENT_MEMCACHED flag. Returns C_ERR if the client was freed. */
int memcachedProcessInputBuffer(struct client *c);

/* Item flags side table hooks, called from db.c. Keeping the 32 bit item
 * flags out of the value object is what lets a memcached item and a Redis
 * string be the same thing in the keyspace. */
void memcachedFlagsFree(struct redisDb *db);
void memcachedFlagsEmpty(struct redisDb *db);
void memcachedFlagsRemove(struct redisDb *db, struct redisObject *key);

#endif /* __MEMCACHED_H */
