/* This file is used by redis-cli in place of server.h when including commands.c
 * It contains alternative structs which omit the parts of the commands table
 * that are not suitable for redis-cli, e.g. the command proc. */

#ifndef __REDIS_CLI_COMMANDS_H
#define __REDIS_CLI_COMMANDS_H

#include <stddef.h>
#include "commands.h"

/* Syntax specifications for a command argument. */
typedef struct cliCommandArg {
    char *name;
    redisCommandArgType type;
    char *token;
    char *since;
    int flags;
    int numsubargs;
    struct cliCommandArg *subargs;
    const char *display_text;

    /*
     * For use at runtime.
     * Fields used to keep track of input word matches for command-line hinting.
     */
    int matched;  /* How many input words have been matched by this argument? */
    int matched_token;  /* Has the token been matched? */
    int matched_name;  /* Has the name been matched? */
    int matched_all;  /* Has the whole argument been consumed (no hint needed)? */
} cliCommandArg;

/* Command documentation info used for help output */
struct commandDocs {
    char *name;
    char *summary;
    char *group;
    char *since;
    int numargs;
    cliCommandArg *args; /* An array of the command arguments. */
    struct commandDocs *subcommands;
    char *params; /* A string describing the syntax of the command arguments. */
};

extern struct commandDocs redisCommandTable[];

#endif
