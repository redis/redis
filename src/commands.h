#ifndef __REDIS_COMMANDS_H
#define __REDIS_COMMANDS_H

/* Must be synced with ARG_TYPE_STR and generate-command-code.py */
typedef enum {
    ARG_TYPE_STRING,
    ARG_TYPE_INTEGER,
    ARG_TYPE_DOUBLE,
    ARG_TYPE_KEY, /* A string, but represents a keyname */
    ARG_TYPE_PATTERN,
    ARG_TYPE_UNIX_TIME,
    ARG_TYPE_PURE_TOKEN,
    ARG_TYPE_ONEOF, /* Has subargs */
    ARG_TYPE_BLOCK /* Has subargs */
} redisCommandArgType;

#define CMD_ARG_NONE            (0)
#define CMD_ARG_OPTIONAL        (1<<0)
#define CMD_ARG_MULTIPLE        (1<<1)
#define CMD_ARG_MULTIPLE_TOKEN  (1<<2)

typedef struct redisCommandArg {
    const char *name;
    redisCommandArgType type;
    int key_spec_index;
    const char *token;
    const char *summary;
    const char *since;
    int flags;
    const char *deprecated_since;
    struct redisCommandArg *subargs;
    /* runtime populated data */
    int num_args;
} redisCommandArg;

#endif
