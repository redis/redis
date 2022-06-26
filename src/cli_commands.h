#ifndef __REDIS_CLI_COMMANDS_H
#define __REDIS_CLI_COMMANDS_H

#include <stddef.h>

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
} commandArgType;

typedef struct commandArg {
    char *name;
    commandArgType type;
    char *token;
    char *since;
    int optional;
    int multiple;
    int multiple_token;
    struct commandArg *subargs;
    int numsubargs;

    /* Fields used to keep track of input word matches for command-line hinting. */
    int matched;  /* How many input words have been matched by this argument? */
    int matched_token;  /* Has the token been matched? */
    int matched_name;  /* Has the name been matched? */
    int matched_all;  /* Has the whole argument been consumed (no hint needed)? */
} commandArg;

/* Command documentation info used for help output */
struct commandDocs {
    char *name;
    char *params; /* A string describing the syntax of the command arguments. */
    char *summary;
    char *group;
    char *since;
    commandArg *args; /* An array of the command arguments. Used since Redis 7.0. */
    int numargs;
    struct commandDocs *subcommands;
};

#endif
