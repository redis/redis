
// Auto-generated, do not edit.

#include <stdio.h>
#include <string.h>

/*
 * List command groups.
 */

#define GROUPS \
  G(UNKNOWN, "unknown") \
  G(SET, "set") \
  G(LIST, "list") \
  G(HASH, "hash") \
  G(GENERIC, "generic") \
  G(PUBSUB, "pubsub") \
  G(STRING, "string") \
  G(SERVER, "server") \
  G(CONNECTION, "connection") \
  G(TRANSACTIONS, "transactions") \
  G(SORTED_SET, "sorted_set")

/*
 * Command group types.
 */

typedef enum {
  #define G(GROUP, _) COMMAND_GROUP_##GROUP,
  GROUPS
  #undef G
  COMMAND_GROUP_LENGTH
} command_group_type_t;

/*
 * Command group type names.
 */

static char *command_group_type_names[] = {
  #define G(_, STR) STR,
  GROUPS
  #undef G
};

/*
 * Command help struct.
 */

struct command_help {
  char *name;
  char *params;
  char *summary;
  command_group_type_t group;
  char *since;
} command_help[] = {
  { "APPEND",
    "key value",
    "Append a value to a key",
    COMMAND_GROUP_STRING,
    "1.3.3" }
,  { "AUTH",
    "password",
    "Authenticate to the server",
    COMMAND_GROUP_CONNECTION,
    "0.08" }
,  { "BGREWRITEAOF",
    "-",
    "Asynchronously rewrite the append-only file",
    COMMAND_GROUP_SERVER,
    "1.07" }
,  { "BGSAVE",
    "-",
    "Asynchronously save the dataset to disk",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "BLPOP",
    "key [key ...] timeout",
    "Remove and get the first element in a list, or block until one is available",
    COMMAND_GROUP_LIST,
    "1.3.1" }
,  { "BRPOP",
    "key [key ...] timeout",
    "Remove and get the last element in a list, or block until one is available",
    COMMAND_GROUP_LIST,
    "1.3.1" }
,  { "CONFIG GET",
    "parameter",
    "Get the value of a configuration parameter",
    COMMAND_GROUP_SERVER,
    "2.0" }
,  { "CONFIG SET",
    "parameter value",
    "Set a configuration parameter to the given value",
    COMMAND_GROUP_SERVER,
    "2.0" }
,  { "DBSIZE",
    "-",
    "Return the number of keys in the selected database",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "DEBUG OBJECT",
    "key",
    "Get debugging information about a key",
    COMMAND_GROUP_SERVER,
    "0.101" }
,  { "DEBUG SEGFAULT",
    "-",
    "Make the server crash",
    COMMAND_GROUP_SERVER,
    "0.101" }
,  { "DECR",
    "key decrement",
    "Decrement the integer value of a key by one",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "DECRBY",
    "key decrement",
    "Decrement the integer value of a key by the given number",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "DEL",
    "key [key ...]",
    "Delete a key",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "DISCARD",
    "-",
    "Discard all commands issued after MULTI",
    COMMAND_GROUP_TRANSACTIONS,
    "1.3.3" }
,  { "ECHO",
    "message",
    "Echo the given string",
    COMMAND_GROUP_CONNECTION,
    "0.07" }
,  { "EXEC",
    "-",
    "Execute all commands issued after MULTI",
    COMMAND_GROUP_TRANSACTIONS,
    "1.1.95" }
,  { "EXISTS",
    "key",
    "Determine if a key exists",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "EXPIRE",
    "key seconds",
    "Set a key's time to live in seconds",
    COMMAND_GROUP_GENERIC,
    "0.09" }
,  { "EXPIREAT",
    "key timestamp",
    "Set the expiration for a key as a UNIX timestamp",
    COMMAND_GROUP_GENERIC,
    "1.1" }
,  { "FLUSHALL",
    "-",
    "Remove all keys from all databases",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "FLUSHDB",
    "-",
    "Remove all keys from the current database",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "GET",
    "key",
    "Get the value of a key",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "GETSET",
    "key value",
    "Set the string value of a key and return its old value",
    COMMAND_GROUP_STRING,
    "0.091" }
,  { "HDEL",
    "key field",
    "Delete a hash field",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HEXISTS",
    "key field",
    "Determine if a hash field exists",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HGET",
    "key field",
    "Get the value of a hash field",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HGETALL",
    "key",
    "Get all the fields and values in a hash",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HINCRBY",
    "key field increment",
    "Increment the integer value of a hash field by the given number",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HKEYS",
    "key",
    "Get all the fields in a hash",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HLEN",
    "key",
    "Get the number of fields in a hash",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HMGET",
    "key field [field ...]",
    "Get the values of all the given hash fields",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HMSET",
    "key field value [field value ...]",
    "Set multiple hash fields to multiple values",
    COMMAND_GROUP_HASH,
    "1.3.8" }
,  { "HSET",
    "key field value",
    "Set the string value of a hash field",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "HSETNX",
    "key field value",
    "Set the value of a hash field, only if the field does not exist",
    COMMAND_GROUP_HASH,
    "1.3.8" }
,  { "HVALS",
    "key",
    "Get all the values in a hash",
    COMMAND_GROUP_HASH,
    "1.3.10" }
,  { "INCR",
    "key",
    "Increment the integer value of a key by one",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "INCRBY",
    "key increment",
    "Increment the integer value of a key by the given number",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "INFO",
    "-",
    "Get information and statistics about the server",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "KEYS",
    "pattern",
    "Find all keys matching the given pattern",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "LASTSAVE",
    "-",
    "Get the UNIX time stamp of the last successful save to disk",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "LINDEX",
    "key index",
    "Get an element from a list by its index",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "LINSERT",
    "key BEFORE|AFTER pivot value",
    "Insert an element before or after another element in a list",
    COMMAND_GROUP_LIST,
    "2.1.1" }
,  { "LLEN",
    "key",
    "Get the length of a list",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "LPOP",
    "key",
    "Remove and get the first element in a list",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "LPUSH",
    "key value",
    "Prepend a value to a list",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "LPUSHX",
    "key value",
    "Prepend a value to a list, only if the list exists",
    COMMAND_GROUP_LIST,
    "2.1.1" }
,  { "LRANGE",
    "key start stop",
    "Get a range of elements from a list",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "LREM",
    "key count value",
    "Remove elements from a list",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "LSET",
    "key index value",
    "Set the value of an element in a list by its index",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "LTRIM",
    "key start stop",
    "Trim a list to the specified range",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "MGET",
    "key [key ...]",
    "Get the values of all the given keys",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "MONITOR",
    "-",
    "Listen for all requests received by the server in real time",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "MOVE",
    "key db",
    "Move a key to another database",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "MSET",
    "key value [key value ...]",
    "Set multiple keys to multiple values",
    COMMAND_GROUP_STRING,
    "1.001" }
,  { "MSETNX",
    "key value [key value ...]",
    "Set multiple keys to multiple values, only if none of the keys exist",
    COMMAND_GROUP_STRING,
    "1.001" }
,  { "MULTI",
    "-",
    "Mark the start of a transaction block",
    COMMAND_GROUP_TRANSACTIONS,
    "1.1.95" }
,  { "PERSIST",
    "key",
    "Remove the expiration from a key",
    COMMAND_GROUP_GENERIC,
    "2.1.2" }
,  { "PING",
    "-",
    "Ping the server",
    COMMAND_GROUP_CONNECTION,
    "0.07" }
,  { "PSUBSCRIBE",
    "pattern",
    "Listen for messages published to channels matching the given patterns",
    COMMAND_GROUP_PUBSUB,
    "1.3.8" }
,  { "PUBLISH",
    "channel message",
    "Post a message to a channel",
    COMMAND_GROUP_PUBSUB,
    "1.3.8" }
,  { "PUNSUBSCRIBE",
    "[pattern [pattern ...]]",
    "Stop listening for messages posted to channels matching the given patterns",
    COMMAND_GROUP_PUBSUB,
    "1.3.8" }
,  { "QUIT",
    "-",
    "Close the connection",
    COMMAND_GROUP_CONNECTION,
    "0.07" }
,  { "RANDOMKEY",
    "-",
    "Return a random key from the keyspace",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "RENAME",
    "old new",
    "Rename a key",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "RENAMENX",
    "old new",
    "Rename a key, only if the new key does not exist",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "RPOP",
    "key",
    "Remove and get the last element in a list",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "RPOPLPUSH",
    "source destination",
    "Remove the last element in a list, append it to another list and return it",
    COMMAND_GROUP_LIST,
    "1.1" }
,  { "RPUSH",
    "key value",
    "Append a value to a list",
    COMMAND_GROUP_LIST,
    "0.07" }
,  { "RPUSHX",
    "key value",
    "Append a value to a list, only if the list exists",
    COMMAND_GROUP_LIST,
    "2.1.1" }
,  { "SADD",
    "key member",
    "Add a member to a set",
    COMMAND_GROUP_SET,
    "0.07" }
,  { "SAVE",
    "-",
    "Synchronously save the dataset to disk",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "SCARD",
    "key",
    "Get the number of members in a set",
    COMMAND_GROUP_SET,
    "0.07" }
,  { "SDIFF",
    "key [key ...]",
    "Subtract multiple sets",
    COMMAND_GROUP_SET,
    "0.100" }
,  { "SDIFFSTORE",
    "destination key [key ...]",
    "Subtract multiple sets and store the resulting set in a key",
    COMMAND_GROUP_SET,
    "0.100" }
,  { "SELECT",
    "index",
    "Change the selected database for the current connection",
    COMMAND_GROUP_CONNECTION,
    "0.07" }
,  { "SET",
    "key value",
    "Set the string value of a key",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "SETEX",
    "key timestamp value",
    "Set the value and expiration of a key",
    COMMAND_GROUP_STRING,
    "1.3.10" }
,  { "SETNX",
    "key value",
    "Set the value of a key, only if the key does not exist",
    COMMAND_GROUP_STRING,
    "0.07" }
,  { "SHUTDOWN",
    "-",
    "Synchronously save the dataset to disk and then shut down the server",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "SINTER",
    "key [key ...]",
    "Intersect multiple sets",
    COMMAND_GROUP_SET,
    "0.07" }
,  { "SINTERSTORE",
    "destination key [key ...]",
    "Intersect multiple sets and store the resulting set in a key",
    COMMAND_GROUP_SET,
    "0.07" }
,  { "SISMEMBER",
    "key member",
    "Determine if a given value is a member of a set",
    COMMAND_GROUP_SET,
    "0.07" }
,  { "SLAVEOF",
    "host port",
    "Make the server a slave of another instance, or promote it as master",
    COMMAND_GROUP_SERVER,
    "0.100" }
,  { "SMEMBERS",
    "key",
    "Get all the members in a set",
    COMMAND_GROUP_SET,
    "0.07" }
,  { "SMOVE",
    "source destination member",
    "Move a member from one set to another",
    COMMAND_GROUP_SET,
    "0.091" }
,  { "SORT",
    "key [BY pattern] [LIMIT start count] [GET pattern [GET pattern ...]] [ASC|DESC] [ALPHA] [STORE destination]",
    "Sort the elements in a list, set or sorted set",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "SPOP",
    "key",
    "Remove and return a random member from a set",
    COMMAND_GROUP_SET,
    "0.101" }
,  { "SRANDMEMBER",
    "key",
    "Get a random member from a set",
    COMMAND_GROUP_SET,
    "1.001" }
,  { "SREM",
    "key member",
    "Remove a member from a set",
    COMMAND_GROUP_SET,
    "0.07" }
,  { "STRLEN",
    "key",
    "Get the length of the value stored in a key",
    COMMAND_GROUP_STRING,
    "2.1.2" }
,  { "SUBSCRIBE",
    "channel",
    "Listen for messages published to the given channels",
    COMMAND_GROUP_PUBSUB,
    "1.3.8" }
,  { "SUBSTR",
    "key start stop",
    "Get a substring of the string stored at a key",
    COMMAND_GROUP_STRING,
    "1.3.4" }
,  { "SUNION",
    "key [key ...]",
    "Add multiple sets",
    COMMAND_GROUP_SET,
    "0.091" }
,  { "SUNIONSTORE",
    "destination key [key ...]",
    "Add multiple sets and store the resulting set in a key",
    COMMAND_GROUP_SET,
    "0.091" }
,  { "SYNC",
    "-",
    "Internal command used for replication",
    COMMAND_GROUP_SERVER,
    "0.07" }
,  { "TTL",
    "key",
    "Get the time to live for a key",
    COMMAND_GROUP_GENERIC,
    "0.100" }
,  { "TYPE",
    "key",
    "Determine the type stored at key",
    COMMAND_GROUP_GENERIC,
    "0.07" }
,  { "UNSUBSCRIBE",
    "[channel [channel ...]]",
    "Stop listening for messages posted to the given channels",
    COMMAND_GROUP_PUBSUB,
    "1.3.8" }
,  { "UNWATCH",
    "-",
    "Forget about all watched keys",
    COMMAND_GROUP_TRANSACTIONS,
    "2.1.0" }
,  { "WATCH",
    "key [key ...]",
    "Watch the given keys to determine execution of the MULTI/EXEC block",
    COMMAND_GROUP_TRANSACTIONS,
    "2.1.0" }
,  { "ZADD",
    "key score member",
    "Add a member to a sorted set, or update its score if it already exists",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZCARD",
    "key",
    "Get the number of members in a sorted set",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZCOUNT",
    "key min max",
    "Count the members in a sorted set with scores within the given values",
    COMMAND_GROUP_SORTED_SET,
    "1.3.3" }
,  { "ZINCRBY",
    "key increment member",
    "Increment the score of a member in a sorted set",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZINTERSTORE",
    "destination key [key ...] [WEIGHTS weight] [AGGREGATE SUM|MIN|MAX]",
    "Intersect multiple sorted sets and store the resulting sorted set in a new key",
    COMMAND_GROUP_SORTED_SET,
    "1.3.10" }
,  { "ZRANGE",
    "key start stop",
    "Return a range of members in a sorted set, by index",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZRANGEBYSCORE",
    "key min max",
    "Return a range of members in a sorted set, by score",
    COMMAND_GROUP_SORTED_SET,
    "1.050" }
,  { "ZRANK",
    "key member",
    "Determine the index of a member in a sorted set",
    COMMAND_GROUP_SORTED_SET,
    "1.3.4" }
,  { "ZREM",
    "key member",
    "Remove a member from a sorted set",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZREMRANGEBYRANK",
    "key start stop",
    "Remove all members in a sorted set within the given indexes",
    COMMAND_GROUP_SORTED_SET,
    "1.3.4" }
,  { "ZREMRANGEBYSCORE",
    "key min max",
    "Remove all members in a sorted set within the given scores",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZREVRANGE",
    "key start stop",
    "Return a range of members in a sorted set, by index, with scores ordered from high to low",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZREVRANK",
    "key member",
    "Determine the index of a member in a sorted set, with scores ordered from high to low",
    COMMAND_GROUP_SORTED_SET,
    "1.3.4" }
,  { "ZSCORE",
    "key member",
    "Get the score associated with the given member in a sorted set",
    COMMAND_GROUP_SORTED_SET,
    "1.1" }
,  { "ZUNIONSTORE",
    "destination key [key ...] [WEIGHTS weight] [AGGREGATE SUM|MIN|MAX]",
    "Add multiple sorted sets and store the resulting sorted set in a new key",
    COMMAND_GROUP_SORTED_SET,
    "1.3.10" }

};

/*
 * Output command help to stdout.
 */

static void
output_command_help(struct command_help *help) {
  printf("\n  \x1b[1m%s\x1b[0m \x1b[90m%s\x1b[0m\n", help->name, help->params);
  printf("  \x1b[33msummary:\x1b[0m %s\n", help->summary);
  printf("  \x1b[33msince:\x1b[0m %s\n", help->since);
  printf("  \x1b[33mgroup:\x1b[0m %s\n", command_group_type_names[help->group]);
}

/*
 * Return command group type by name string.
 */

static command_group_type_t
command_group_type_by_name(const char *name) {
  for (int i = 0; i < COMMAND_GROUP_LENGTH; ++i) {
    const char *group = command_group_type_names[i];
    if (0 == strcasecmp(name, group)) return i;
  }
  return 0;
}

/*
 * Output group names.
 */

static void
output_group_help() {
	for (int i = 0; i < COMMAND_GROUP_LENGTH; ++i) {
		if (COMMAND_GROUP_UNKNOWN == i) continue;
		const char *group = command_group_type_names[i];
		printf("  \x1b[90m-\x1b[0m %s\n", group);
	}
}

/*
 * Output all command help, filtering by group or command name.
 */

static void
output_help(int argc, const char **argv) {
  int len = sizeof(command_help) / sizeof(struct command_help);

	if (argc && 0 == strcasecmp("groups", argv[0])) {
		output_group_help();
		return;
	}

	command_group_type_t group = argc
    ? command_group_type_by_name(argv[0])
    : COMMAND_GROUP_UNKNOWN;

  for (int i = 0; i < len; ++i) {
    struct command_help help = command_help[i];
    if (argc && !group && 0 != strcasecmp(help.name, argv[0])) continue;
    if (group && group != help.group) continue;
    output_command_help(&help);
  }
  puts("");
}
