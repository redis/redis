/* Redis CLI (command line interface)
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#include "fmacros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <termios.h>

#include <hiredis.h>
#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <hiredis_ssl.h>
#endif
#include <sdscompat.h> /* Use hiredis' sds compat header that maps sds calls to their hi_ variants */
#include <sds.h> /* use sds.h from hiredis, so that only one set of sds functions will be present in the binary */
#include "dict.h"
#include "adlist.h"
#include "zmalloc.h"
#include "linenoise.h"
#include "anet.h"
#include "ae.h"
#include "connection.h"
#include "cli_common.h"
#include "mt19937-64.h"
#include "cli_commands.h"

#define UNUSED(V) ((void) V)

#define OUTPUT_STANDARD 0
#define OUTPUT_RAW 1
#define OUTPUT_CSV 2
#define OUTPUT_JSON 3
#define OUTPUT_QUOTED_JSON 4
#define REDIS_CLI_KEEPALIVE_INTERVAL 15 /* seconds */
#define REDIS_CLI_DEFAULT_PIPE_TIMEOUT 30 /* seconds */
#define REDIS_CLI_HISTFILE_ENV "REDISCLI_HISTFILE"
#define REDIS_CLI_HISTFILE_DEFAULT ".rediscli_history"
#define REDIS_CLI_RCFILE_ENV "REDISCLI_RCFILE"
#define REDIS_CLI_RCFILE_DEFAULT ".redisclirc"
#define REDIS_CLI_AUTH_ENV "REDISCLI_AUTH"
#define REDIS_CLI_CLUSTER_YES_ENV "REDISCLI_CLUSTER_YES"

#define CLUSTER_MANAGER_SLOTS               16384
#define CLUSTER_MANAGER_PORT_INCR           10000 /* same as CLUSTER_PORT_INCR */
#define CLUSTER_MANAGER_MIGRATE_TIMEOUT     60000
#define CLUSTER_MANAGER_MIGRATE_PIPELINE    10
#define CLUSTER_MANAGER_REBALANCE_THRESHOLD 2

#define CLUSTER_MANAGER_INVALID_HOST_ARG \
    "[ERR] Invalid arguments: you need to pass either a valid " \
    "address (ie. 120.0.0.1:7000) or space separated IP " \
    "and port (ie. 120.0.0.1 7000)\n"
#define CLUSTER_MANAGER_MODE() (config.cluster_manager_command.name != NULL)
#define CLUSTER_MANAGER_MASTERS_COUNT(nodes, replicas) ((nodes)/((replicas) + 1))
#define CLUSTER_MANAGER_COMMAND(n,...) \
        (redisCommand((n)->context, __VA_ARGS__))

#define CLUSTER_MANAGER_NODE_ARRAY_FREE(array) zfree((array)->alloc)

#define CLUSTER_MANAGER_PRINT_REPLY_ERROR(n, err) \
    clusterManagerLogErr("Node %s:%d replied with error:\n%s\n", \
                         (n)->ip, (n)->port, (err));

#define clusterManagerLogInfo(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_INFO,__VA_ARGS__)

#define clusterManagerLogErr(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_ERR,__VA_ARGS__)

#define clusterManagerLogWarn(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_WARN,__VA_ARGS__)

#define clusterManagerLogOk(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_SUCCESS,__VA_ARGS__)

#define CLUSTER_MANAGER_FLAG_MYSELF     1 << 0
#define CLUSTER_MANAGER_FLAG_SLAVE      1 << 1
#define CLUSTER_MANAGER_FLAG_FRIEND     1 << 2
#define CLUSTER_MANAGER_FLAG_NOADDR     1 << 3
#define CLUSTER_MANAGER_FLAG_DISCONNECT 1 << 4
#define CLUSTER_MANAGER_FLAG_FAIL       1 << 5

#define CLUSTER_MANAGER_CMD_FLAG_FIX            1 << 0
#define CLUSTER_MANAGER_CMD_FLAG_SLAVE          1 << 1
#define CLUSTER_MANAGER_CMD_FLAG_YES            1 << 2
#define CLUSTER_MANAGER_CMD_FLAG_AUTOWEIGHTS    1 << 3
#define CLUSTER_MANAGER_CMD_FLAG_EMPTYMASTER    1 << 4
#define CLUSTER_MANAGER_CMD_FLAG_SIMULATE       1 << 5
#define CLUSTER_MANAGER_CMD_FLAG_REPLACE        1 << 6
#define CLUSTER_MANAGER_CMD_FLAG_COPY           1 << 7
#define CLUSTER_MANAGER_CMD_FLAG_COLOR          1 << 8
#define CLUSTER_MANAGER_CMD_FLAG_CHECK_OWNERS   1 << 9
#define CLUSTER_MANAGER_CMD_FLAG_FIX_WITH_UNREACHABLE_MASTERS 1 << 10
#define CLUSTER_MANAGER_CMD_FLAG_MASTERS_ONLY   1 << 11
#define CLUSTER_MANAGER_CMD_FLAG_SLAVES_ONLY    1 << 12

#define CLUSTER_MANAGER_OPT_GETFRIENDS  1 << 0
#define CLUSTER_MANAGER_OPT_COLD        1 << 1
#define CLUSTER_MANAGER_OPT_UPDATE      1 << 2
#define CLUSTER_MANAGER_OPT_QUIET       1 << 6
#define CLUSTER_MANAGER_OPT_VERBOSE     1 << 7

#define CLUSTER_MANAGER_LOG_LVL_INFO    1
#define CLUSTER_MANAGER_LOG_LVL_WARN    2
#define CLUSTER_MANAGER_LOG_LVL_ERR     3
#define CLUSTER_MANAGER_LOG_LVL_SUCCESS 4

#define CLUSTER_JOIN_CHECK_AFTER        20

#define LOG_COLOR_BOLD      "29;1m"
#define LOG_COLOR_RED       "31;1m"
#define LOG_COLOR_GREEN     "32;1m"
#define LOG_COLOR_YELLOW    "33;1m"
#define LOG_COLOR_RESET     "0m"

/* cliConnect() flags. */
#define CC_FORCE (1<<0)         /* Re-connect if already connected. */
#define CC_QUIET (1<<1)         /* Don't log connecting errors. */

/* DNS lookup */
#define NET_IP_STR_LEN 46       /* INET6_ADDRSTRLEN is 46 */

/* --latency-dist palettes. */
int spectrum_palette_color_size = 19;
int spectrum_palette_color[] = {0,233,234,235,237,239,241,243,245,247,144,143,142,184,226,214,208,202,196};

int spectrum_palette_mono_size = 13;
int spectrum_palette_mono[] = {0,233,234,235,237,239,241,243,245,247,249,251,253};

/* The actual palette in use. */
int *spectrum_palette;
int spectrum_palette_size;

static int orig_termios_saved = 0;
static struct termios orig_termios; /* To restore terminal at exit.*/

/* Dict Helpers */
static uint64_t dictSdsHash(const void *key);
static int dictSdsKeyCompare(dict *d, const void *key1,
    const void *key2);
static void dictSdsDestructor(dict *d, void *val);
static void dictListDestructor(dict *d, void *val);

/* Cluster Manager Command Info */
typedef struct clusterManagerCommand {
    char *name;
    int argc;
    char **argv;
    sds stdin_arg; /* arg from stdin. (-X option) */
    int flags;
    int replicas;
    char *from;
    char *to;
    char **weight;
    int weight_argc;
    char *master_id;
    int slots;
    int timeout;
    int pipeline;
    float threshold;
    char *backup_dir;
    char *from_user;
    char *from_pass;
    int from_askpass;
} clusterManagerCommand;

static int createClusterManagerCommand(char *cmdname, int argc, char **argv);


static redisContext *context;
static struct config {
    cliConnInfo conn_info;
    struct timeval connect_timeout;
    char *hostsocket;
    int tls;
    cliSSLconfig sslconfig;
    long repeat;
    long interval;
    int dbnum; /* db num currently selected */
    int interactive;
    int shutdown;
    int monitor_mode;
    int pubsub_mode;
    int blocking_state_aborted; /* used to abort monitor_mode and pubsub_mode. */
    int latency_mode;
    int latency_dist_mode;
    int latency_history;
    int lru_test_mode;
    long long lru_test_sample_size;
    int cluster_mode;
    int cluster_reissue_command;
    int cluster_send_asking;
    int slave_mode;
    int pipe_mode;
    int pipe_timeout;
    int getrdb_mode;
    int get_functions_rdb_mode;
    int stat_mode;
    int scan_mode;
    int count;
    int intrinsic_latency_mode;
    int intrinsic_latency_duration;
    sds pattern;
    char *rdb_filename;
    int bigkeys;
    int memkeys;
    unsigned memkeys_samples;
    int hotkeys;
    int stdin_lastarg; /* get last arg from stdin. (-x option) */
    int stdin_tag_arg; /* get <tag> arg from stdin. (-X option) */
    char *stdin_tag_name; /* Placeholder(tag name) for user input. */
    int askpass;
    int quoted_input;   /* Force input args to be treated as quoted strings */
    int output; /* output mode, see OUTPUT_* defines */
    int push_output; /* Should we display spontaneous PUSH replies */
    sds mb_delim;
    sds cmd_delim;
    char prompt[128];
    char *eval;
    int eval_ldb;
    int eval_ldb_sync;  /* Ask for synchronous mode of the Lua debugger. */
    int eval_ldb_end;   /* Lua debugging session ended. */
    int enable_ldb_on_eval; /* Handle manual SCRIPT DEBUG + EVAL commands. */
    int last_cmd_type;
    redisReply *last_reply;
    int verbose;
    int set_errcode;
    clusterManagerCommand cluster_manager_command;
    int no_auth_warning;
    int resp2; /* value of 1: specified explicitly with option -2 */
    int resp3; /* value of 1: specified explicitly, value of 2: implicit like --json option */
    int current_resp3; /* 1 if we have RESP3 right now in the current connection. */
    int in_multi;
    int pre_multi_dbnum;
    char *server_version;
    char *test_hint;
    char *test_hint_file;
    int prefer_ipv4; /* Prefer IPv4 over IPv6 on DNS lookup. */
    int prefer_ipv6; /* Prefer IPv6 over IPv4 on DNS lookup. */
} config;

/* User preferences. */
static struct pref {
    int hints;
} pref;

static volatile sig_atomic_t force_cancel_loop = 0;
static void usage(int err);
static void slaveMode(int send_sync);
static int cliConnect(int flags);

static char *getInfoField(char *info, char *field);
static long getLongInfoField(char *info, char *field);

/*------------------------------------------------------------------------------
 * Utility functions
 *--------------------------------------------------------------------------- */
size_t redis_strlcpy(char *dst, const char *src, size_t dsize);

static void cliPushHandler(void *, void *);

uint16_t crc16(const char *buf, int len);

static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

static long long mstime(void) {
    return ustime()/1000;
}

static void cliRefreshPrompt(void) {
    if (config.eval_ldb) return;

    sds prompt = sdsempty();
    if (config.hostsocket != NULL) {
        prompt = sdscatfmt(prompt,"redis %s",config.hostsocket);
    } else {
        char addr[256];
        formatAddr(addr, sizeof(addr), config.conn_info.hostip, config.conn_info.hostport);
        prompt = sdscatlen(prompt,addr,strlen(addr));
    }

    /* Add [dbnum] if needed */
    if (config.dbnum != 0)
        prompt = sdscatfmt(prompt,"[%i]",config.dbnum);

    /* Add TX if in transaction state*/
    if (config.in_multi)  
        prompt = sdscatlen(prompt,"(TX)",4);

    if (config.pubsub_mode)
        prompt = sdscatfmt(prompt,"(subscribed mode)");

    /* Copy the prompt in the static buffer. */
    prompt = sdscatlen(prompt,"> ",2);
    snprintf(config.prompt,sizeof(config.prompt),"%s",prompt);
    sdsfree(prompt);
}

/* Return the name of the dotfile for the specified 'dotfilename'.
 * Normally it just concatenates user $HOME to the file specified
 * in 'dotfilename'. However if the environment variable 'envoverride'
 * is set, its value is taken as the path.
 *
 * The function returns NULL (if the file is /dev/null or cannot be
 * obtained for some error), or an SDS string that must be freed by
 * the user. */
static sds getDotfilePath(char *envoverride, char *dotfilename) {
    char *path = NULL;
    sds dotPath = NULL;

    /* Check the env for a dotfile override. */
    path = getenv(envoverride);
    if (path != NULL && *path != '\0') {
        if (!strcmp("/dev/null", path)) {
            return NULL;
        }

        /* If the env is set, return it. */
        dotPath = sdsnew(path);
    } else {
        char *home = getenv("HOME");
        if (home != NULL && *home != '\0') {
            /* If no override is set use $HOME/<dotfilename>. */
            dotPath = sdscatprintf(sdsempty(), "%s/%s", home, dotfilename);
        }
    }
    return dotPath;
}

static uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

static int dictSdsKeyCompare(dict *d, const void *key1, const void *key2)
{
    int l1,l2;
    UNUSED(d);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

static void dictSdsDestructor(dict *d, void *val)
{
    UNUSED(d);
    sdsfree(val);
}

void dictListDestructor(dict *d, void *val)
{
    UNUSED(d);
    listRelease((list*)val);
}

/*------------------------------------------------------------------------------
 * Help functions
 *--------------------------------------------------------------------------- */

#define CLI_HELP_COMMAND 1
#define CLI_HELP_GROUP 2

typedef struct {
    int type;
    int argc;
    sds *argv;
    sds full;

    /* Only used for help on commands */
    struct commandDocs docs;
} helpEntry;

static helpEntry *helpEntries = NULL;
static int helpEntriesLen = 0;

/* For backwards compatibility with pre-7.0 servers.
 * cliLegacyInitHelp() sets up the helpEntries array with the command and group
 * names from the commands.c file. However the Redis instance we are connecting
 * to may support more commands, so this function integrates the previous
 * entries with additional entries obtained using the COMMAND command
 * available in recent versions of Redis. */
static void cliLegacyIntegrateHelp(void) {
    if (cliConnect(CC_QUIET) == REDIS_ERR) return;

    redisReply *reply = redisCommand(context, "COMMAND");
    if(reply == NULL || reply->type != REDIS_REPLY_ARRAY) return;

    /* Scan the array reported by COMMAND and fill only the entries that
     * don't already match what we have. */
    for (size_t j = 0; j < reply->elements; j++) {
        redisReply *entry = reply->element[j];
        if (entry->type != REDIS_REPLY_ARRAY || entry->elements < 4 ||
            entry->element[0]->type != REDIS_REPLY_STRING ||
            entry->element[1]->type != REDIS_REPLY_INTEGER ||
            entry->element[3]->type != REDIS_REPLY_INTEGER) return;
        char *cmdname = entry->element[0]->str;
        int i;

        for (i = 0; i < helpEntriesLen; i++) {
            helpEntry *he = helpEntries+i;
            if (!strcasecmp(he->argv[0],cmdname))
                break;
        }
        if (i != helpEntriesLen) continue;

        helpEntriesLen++;
        helpEntries = zrealloc(helpEntries,sizeof(helpEntry)*helpEntriesLen);
        helpEntry *new = helpEntries+(helpEntriesLen-1);

        new->argc = 1;
        new->argv = zmalloc(sizeof(sds));
        new->argv[0] = sdsnew(cmdname);
        new->full = new->argv[0];
        new->type = CLI_HELP_COMMAND;
        sdstoupper(new->argv[0]);

        new->docs.name = new->argv[0];
        new->docs.args = NULL;
        new->docs.numargs = 0;
        new->docs.params = sdsempty();
        int args = llabs(entry->element[1]->integer);
        args--; /* Remove the command name itself. */
        if (entry->element[3]->integer == 1) {
            new->docs.params = sdscat(new->docs.params,"key ");
            args--;
        }
        while(args-- > 0) new->docs.params = sdscat(new->docs.params,"arg ");
        if (entry->element[1]->integer < 0)
            new->docs.params = sdscat(new->docs.params,"...options...");
        new->docs.summary = "Help not available";
        new->docs.since = "Not known";
        new->docs.group = "generic";
    }
    freeReplyObject(reply);
}

/* Concatenate a string to an sds string, but if it's empty substitute double quote marks. */
static sds sdscat_orempty(sds params, const char *value) {
    if (value[0] == '\0') {
        return sdscat(params, "\"\"");
    }
    return sdscat(params, value);
}

static sds makeHint(char **inputargv, int inputargc, int cmdlen, struct commandDocs docs);

static void cliAddCommandDocArg(cliCommandArg *cmdArg, redisReply *argMap);

static void cliMakeCommandDocArgs(redisReply *arguments, cliCommandArg *result) {
    for (size_t j = 0; j < arguments->elements; j++) {
        cliAddCommandDocArg(&result[j], arguments->element[j]);
    }
}

static void cliAddCommandDocArg(cliCommandArg *cmdArg, redisReply *argMap) {
    if (argMap->type != REDIS_REPLY_MAP && argMap->type != REDIS_REPLY_ARRAY) {
        return;
    }

    for (size_t i = 0; i < argMap->elements; i += 2) {
        assert(argMap->element[i]->type == REDIS_REPLY_STRING);
        char *key = argMap->element[i]->str;
        if (!strcmp(key, "name")) {
            assert(argMap->element[i + 1]->type == REDIS_REPLY_STRING);
            cmdArg->name = sdsnew(argMap->element[i + 1]->str);
        } else if (!strcmp(key, "display_text")) {
            assert(argMap->element[i + 1]->type == REDIS_REPLY_STRING);
            cmdArg->display_text = sdsnew(argMap->element[i + 1]->str);
        } else if (!strcmp(key, "token")) {
            assert(argMap->element[i + 1]->type == REDIS_REPLY_STRING);
            cmdArg->token = sdsnew(argMap->element[i + 1]->str);
        } else if (!strcmp(key, "type")) {
            assert(argMap->element[i + 1]->type == REDIS_REPLY_STRING);
            char *type = argMap->element[i + 1]->str;
            if (!strcmp(type, "string")) {
                cmdArg->type = ARG_TYPE_STRING;
            } else if (!strcmp(type, "integer")) {
                cmdArg->type = ARG_TYPE_INTEGER;
            } else if (!strcmp(type, "double")) {
                cmdArg->type = ARG_TYPE_DOUBLE;
            } else if (!strcmp(type, "key")) {
                cmdArg->type = ARG_TYPE_KEY;
            } else if (!strcmp(type, "pattern")) {
                cmdArg->type = ARG_TYPE_PATTERN;
            } else if (!strcmp(type, "unix-time")) {
                cmdArg->type = ARG_TYPE_UNIX_TIME;
            } else if (!strcmp(type, "pure-token")) {
                cmdArg->type = ARG_TYPE_PURE_TOKEN;
            } else if (!strcmp(type, "oneof")) {
                cmdArg->type = ARG_TYPE_ONEOF;
            } else if (!strcmp(type, "block")) {
                cmdArg->type = ARG_TYPE_BLOCK;
            }
        } else if (!strcmp(key, "arguments")) {
            redisReply *arguments = argMap->element[i + 1];
            cmdArg->subargs = zcalloc(arguments->elements * sizeof(cliCommandArg));
            cmdArg->numsubargs = arguments->elements;
            cliMakeCommandDocArgs(arguments, cmdArg->subargs);
        } else if (!strcmp(key, "flags")) {
            redisReply *flags = argMap->element[i + 1];
            assert(flags->type == REDIS_REPLY_SET || flags->type == REDIS_REPLY_ARRAY);
            for (size_t j = 0; j < flags->elements; j++) {
                assert(flags->element[j]->type == REDIS_REPLY_STATUS);
                char *flag = flags->element[j]->str;
                if (!strcmp(flag, "optional")) {
                    cmdArg->flags |= CMD_ARG_OPTIONAL;
                } else if (!strcmp(flag, "multiple")) {
                    cmdArg->flags |= CMD_ARG_MULTIPLE;
                } else if (!strcmp(flag, "multiple_token")) {
                    cmdArg->flags |= CMD_ARG_MULTIPLE_TOKEN;
                }
            }
        }
    }
}

/* Fill in the fields of a help entry for the command/subcommand name. */
static void cliFillInCommandHelpEntry(helpEntry *help, char *cmdname, char *subcommandname) {
    help->argc = subcommandname ? 2 : 1;
    help->argv = zmalloc(sizeof(sds) * help->argc);
    help->argv[0] = sdsnew(cmdname);
    sdstoupper(help->argv[0]);
    if (subcommandname) {
        /* Subcommand name may be two words separated by a pipe character. */
        char *pipe = strchr(subcommandname, '|');
        if (pipe != NULL) {
            help->argv[1] = sdsnew(pipe + 1);
        } else {
            help->argv[1] = sdsnew(subcommandname);
        }
        sdstoupper(help->argv[1]);
    }
    sds fullname = sdsnew(help->argv[0]);
    if (subcommandname) {
        fullname = sdscat(fullname, " ");
        fullname = sdscat(fullname, help->argv[1]);
    }
    help->full = fullname;
    help->type = CLI_HELP_COMMAND;

    help->docs.name = help->full;
    help->docs.params = NULL;
    help->docs.args = NULL;
    help->docs.numargs = 0;
    help->docs.since = NULL;
}

/* Initialize a command help entry for the command/subcommand described in 'specs'.
 * 'next' points to the next help entry to be filled in.
 * 'groups' is a set of command group names to be filled in.
 * Returns a pointer to the next available position in the help entries table.
 * If the command has subcommands, this is called recursively for the subcommands.
 */
static helpEntry *cliInitCommandHelpEntry(char *cmdname, char *subcommandname,
                                          helpEntry *next, redisReply *specs,
                                          dict *groups) {
    helpEntry *help = next++;
    cliFillInCommandHelpEntry(help, cmdname, subcommandname);

    assert(specs->type == REDIS_REPLY_MAP || specs->type == REDIS_REPLY_ARRAY);
    for (size_t j = 0; j < specs->elements; j += 2) {
        assert(specs->element[j]->type == REDIS_REPLY_STRING);
        char *key = specs->element[j]->str;
        if (!strcmp(key, "summary")) {
            redisReply *reply = specs->element[j + 1];
            assert(reply->type == REDIS_REPLY_STRING);
            help->docs.summary = sdsnew(reply->str);
        } else if (!strcmp(key, "since")) {
            redisReply *reply = specs->element[j + 1];
            assert(reply->type == REDIS_REPLY_STRING);
            help->docs.since = sdsnew(reply->str);
        } else if (!strcmp(key, "group")) {
            redisReply *reply = specs->element[j + 1];
            assert(reply->type == REDIS_REPLY_STRING);
            help->docs.group = sdsnew(reply->str);
            sds group = sdsdup(help->docs.group);
            if (dictAdd(groups, group, NULL) != DICT_OK) {
                sdsfree(group);
            }
        } else if (!strcmp(key, "arguments")) {
            redisReply *arguments = specs->element[j + 1];
            assert(arguments->type == REDIS_REPLY_ARRAY);
            help->docs.args = zcalloc(arguments->elements * sizeof(cliCommandArg));
            help->docs.numargs = arguments->elements;
            cliMakeCommandDocArgs(arguments, help->docs.args);
            help->docs.params = makeHint(NULL, 0, 0, help->docs);
        } else if (!strcmp(key, "subcommands")) {
            redisReply *subcommands = specs->element[j + 1];
            assert(subcommands->type == REDIS_REPLY_MAP || subcommands->type == REDIS_REPLY_ARRAY);
            for (size_t i = 0; i < subcommands->elements; i += 2) {
                assert(subcommands->element[i]->type == REDIS_REPLY_STRING);
                char *subcommandname = subcommands->element[i]->str;
                redisReply *subcommand = subcommands->element[i + 1];
                assert(subcommand->type == REDIS_REPLY_MAP || subcommand->type == REDIS_REPLY_ARRAY);
                next = cliInitCommandHelpEntry(cmdname, subcommandname, next, subcommand, groups);
            }
        }
    }
    return next;
}

/* Returns the total number of commands and subcommands in the command docs table. */
static size_t cliCountCommands(redisReply* commandTable) {
    size_t numCommands = commandTable->elements / 2;

    /* The command docs table maps command names to a map of their specs. */    
    for (size_t i = 0; i < commandTable->elements; i += 2) {
        assert(commandTable->element[i]->type == REDIS_REPLY_STRING);  /* Command name. */
        assert(commandTable->element[i + 1]->type == REDIS_REPLY_MAP ||
               commandTable->element[i + 1]->type == REDIS_REPLY_ARRAY);
        redisReply *map = commandTable->element[i + 1];
        for (size_t j = 0; j < map->elements; j += 2) {
            assert(map->element[j]->type == REDIS_REPLY_STRING);
            char *key = map->element[j]->str;
            if (!strcmp(key, "subcommands")) {
                redisReply *subcommands = map->element[j + 1];
                assert(subcommands->type == REDIS_REPLY_MAP || subcommands->type == REDIS_REPLY_ARRAY);
                numCommands += subcommands->elements / 2;
            }
        }
    }
    return numCommands;
}

/* Comparator for sorting help table entries. */
int helpEntryCompare(const void *entry1, const void *entry2) {
    helpEntry *i1 = (helpEntry *)entry1;
    helpEntry *i2 = (helpEntry *)entry2;
    return strcmp(i1->full, i2->full);
}

/* Initializes command help entries for command groups.
 * Called after the command help entries have already been filled in.
 * Extends the help table with new entries for the command groups.
 */
void cliInitGroupHelpEntries(dict *groups) {
    dictIterator *iter = dictGetIterator(groups);
    dictEntry *entry;
    helpEntry tmp;

    int numGroups = dictSize(groups);
    int pos = helpEntriesLen;
    helpEntriesLen += numGroups;
    helpEntries = zrealloc(helpEntries, sizeof(helpEntry)*helpEntriesLen);

    for (entry = dictNext(iter); entry != NULL; entry = dictNext(iter)) {
        tmp.argc = 1;
        tmp.argv = zmalloc(sizeof(sds));
        tmp.argv[0] = sdscatprintf(sdsempty(),"@%s",(char *)dictGetKey(entry));
        tmp.full = tmp.argv[0];
        tmp.type = CLI_HELP_GROUP;
        tmp.docs.name = NULL;
        tmp.docs.params = NULL;
        tmp.docs.args = NULL;
        tmp.docs.numargs = 0;
        tmp.docs.summary = NULL;
        tmp.docs.since = NULL;
        tmp.docs.group = NULL;
        helpEntries[pos++] = tmp;
    }
    dictReleaseIterator(iter);
}

/* Initializes help entries for all commands in the COMMAND DOCS reply. */
void cliInitCommandHelpEntries(redisReply *commandTable, dict *groups) {
    helpEntry *next = helpEntries;
    for (size_t i = 0; i < commandTable->elements; i += 2) {
        assert(commandTable->element[i]->type == REDIS_REPLY_STRING);
        char *cmdname = commandTable->element[i]->str;

        assert(commandTable->element[i + 1]->type == REDIS_REPLY_MAP ||
               commandTable->element[i + 1]->type == REDIS_REPLY_ARRAY);
        redisReply *cmdspecs = commandTable->element[i + 1];
        next = cliInitCommandHelpEntry(cmdname, NULL, next, cmdspecs, groups);
    }
}

/* Does the server version support a command/argument only available "since" some version?
 * Returns 1 when supported, or 0 when the "since" version is newer than "version". */
static int versionIsSupported(sds version, sds since) {
    int i;
    char *versionPos = version;
    char *sincePos = since;
    if (!since) {
        return 1;
    }

    for (i = 0; i != 3; i++) {
        int versionPart = atoi(versionPos);
        int sincePart = atoi(sincePos);
        if (versionPart > sincePart) {
            return 1;
        } else if (sincePart > versionPart) {
            return 0;
        }
        versionPos = strchr(versionPos, '.');
        sincePos = strchr(sincePos, '.');

        /* If we finished to parse both `version` and `since`, it means they are equal */
        if (!versionPos && !sincePos) return 1;

        /* Different number of digits considered as not supported */
        if (!versionPos || !sincePos) return 0;

        versionPos++;
        sincePos++;
    }
    return 0;
}

static void removeUnsupportedArgs(struct cliCommandArg *args, int *numargs, sds version) {
    int i = 0, j;
    while (i != *numargs) {
        if (versionIsSupported(version, args[i].since)) {
            if (args[i].subargs) {
                removeUnsupportedArgs(args[i].subargs, &args[i].numsubargs, version);
            }
            i++;
            continue;
        }
        for (j = i; j != *numargs - 1; j++) {
            args[j] = args[j + 1];
        }
        (*numargs)--;
    }
}

static helpEntry *cliLegacyInitCommandHelpEntry(char *cmdname, char *subcommandname,
                                                helpEntry *next, struct commandDocs *command,
                                                dict *groups, sds version) {
    helpEntry *help = next++;
    cliFillInCommandHelpEntry(help, cmdname, subcommandname);
    
    help->docs.summary = sdsnew(command->summary);
    help->docs.since = sdsnew(command->since);
    help->docs.group = sdsnew(command->group);
    sds group = sdsdup(help->docs.group);
    if (dictAdd(groups, group, NULL) != DICT_OK) {
        sdsfree(group);
    }

    if (command->args != NULL) {
        help->docs.args = command->args;
        help->docs.numargs = command->numargs;
        if (version)
            removeUnsupportedArgs(help->docs.args, &help->docs.numargs, version);
        help->docs.params = makeHint(NULL, 0, 0, help->docs);
    }

    if (command->subcommands != NULL) {
        for (size_t i = 0; command->subcommands[i].name != NULL; i++) {
            if (!version || versionIsSupported(version, command->subcommands[i].since)) {
                char *subcommandname = command->subcommands[i].name;
                next = cliLegacyInitCommandHelpEntry(
                    cmdname, subcommandname, next, &command->subcommands[i], groups, version);
            }
        }
    }
    return next;
}

int cliLegacyInitCommandHelpEntries(struct commandDocs *commands, dict *groups, sds version) {
    helpEntry *next = helpEntries;
    for (size_t i = 0; commands[i].name != NULL; i++) {
        if (!version || versionIsSupported(version, commands[i].since)) {
            next = cliLegacyInitCommandHelpEntry(commands[i].name, NULL, next, &commands[i], groups, version);
        }
    }
    return next - helpEntries;
}

/* Returns the total number of commands and subcommands in the command docs table,
 * filtered by server version (if provided).
 */
static size_t cliLegacyCountCommands(struct commandDocs *commands, sds version) {
    int numCommands = 0;
    for (size_t i = 0; commands[i].name != NULL; i++) {
        if (version && !versionIsSupported(version, commands[i].since)) {
            continue;
        }
        numCommands++;
        if (commands[i].subcommands != NULL) {
            numCommands += cliLegacyCountCommands(commands[i].subcommands, version);
        }
    }
    return numCommands;
}

/* Gets the server version string by calling INFO SERVER.
 * Stores the result in config.server_version.
 * When not connected, or not possible, returns NULL. */
static sds cliGetServerVersion(void) {
    static const char *key = "\nredis_version:";
    redisReply *serverInfo = NULL;
    char *pos;

    if (config.server_version != NULL) {
        return config.server_version;
    }

    if (!context) return NULL;
    serverInfo = redisCommand(context, "INFO SERVER");
    if (serverInfo == NULL || serverInfo->type == REDIS_REPLY_ERROR) {
        freeReplyObject(serverInfo);
        return sdsempty();
    }

    assert(serverInfo->type == REDIS_REPLY_STRING || serverInfo->type == REDIS_REPLY_VERB);
    sds info = serverInfo->str;

    /* Finds the first appearance of "redis_version" in the INFO SERVER reply. */
    pos = strstr(info, key);
    if (pos) {
        pos += strlen(key);
        char *end = strchr(pos, '\r');
        if (end) {
            sds version = sdsnewlen(pos, end - pos);
            freeReplyObject(serverInfo);
            config.server_version = version;
            return version;
        }
    }
    freeReplyObject(serverInfo);
    return NULL;
}

static void cliLegacyInitHelp(dict *groups) {
    sds serverVersion = cliGetServerVersion();
    
    /* Scan the commandDocs array and fill in the entries */
    helpEntriesLen = cliLegacyCountCommands(redisCommandTable, serverVersion);
    helpEntries = zmalloc(sizeof(helpEntry)*helpEntriesLen);

    helpEntriesLen = cliLegacyInitCommandHelpEntries(redisCommandTable, groups, serverVersion);
    cliInitGroupHelpEntries(groups);

    qsort(helpEntries, helpEntriesLen, sizeof(helpEntry), helpEntryCompare);
    dictRelease(groups);
}

/* cliInitHelp() sets up the helpEntries array with the command and group
 * names and command descriptions obtained using the COMMAND DOCS command.
 */
static void cliInitHelp(void) {
    /* Dict type for a set of strings, used to collect names of command groups. */
    dictType groupsdt = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL,                       /* val destructor */
        NULL                        /* allow to expand */
    };
    redisReply *commandTable;
    dict *groups;

    if (cliConnect(CC_QUIET) == REDIS_ERR) {
        /* Can not connect to the server, but we still want to provide
         * help, generate it only from the static cli_commands.c data instead. */
        groups = dictCreate(&groupsdt);
        cliLegacyInitHelp(groups);
        return;
    }
    commandTable = redisCommand(context, "COMMAND DOCS");
    if (commandTable == NULL || commandTable->type == REDIS_REPLY_ERROR) {
        /* New COMMAND DOCS subcommand not supported - generate help from
         * static cli_commands.c data instead. */
        freeReplyObject(commandTable);

        groups = dictCreate(&groupsdt);
        cliLegacyInitHelp(groups);
        cliLegacyIntegrateHelp();
        return;
    };
    if (commandTable->type != REDIS_REPLY_MAP && commandTable->type != REDIS_REPLY_ARRAY) return;

    /* Scan the array reported by COMMAND DOCS and fill in the entries */
    helpEntriesLen = cliCountCommands(commandTable);
    helpEntries = zmalloc(sizeof(helpEntry)*helpEntriesLen);

    groups = dictCreate(&groupsdt);
    cliInitCommandHelpEntries(commandTable, groups);
    cliInitGroupHelpEntries(groups);

    qsort(helpEntries, helpEntriesLen, sizeof(helpEntry), helpEntryCompare);
    freeReplyObject(commandTable);
    dictRelease(groups);
}

/* Output command help to stdout. */
static void cliOutputCommandHelp(struct commandDocs *help, int group) {
    printf("\r\n  \x1b[1m%s\x1b[0m \x1b[90m%s\x1b[0m\r\n", help->name, help->params);
    printf("  \x1b[33msummary:\x1b[0m %s\r\n", help->summary);
    if (help->since != NULL) {
        printf("  \x1b[33msince:\x1b[0m %s\r\n", help->since);
    }
    if (group) {
        printf("  \x1b[33mgroup:\x1b[0m %s\r\n", help->group);
    }
}

/* Print generic help. */
static void cliOutputGenericHelp(void) {
    sds version = cliVersion();
    printf(
        "redis-cli %s\n"
        "To get help about Redis commands type:\n"
        "      \"help @<group>\" to get a list of commands in <group>\n"
        "      \"help <command>\" for help on <command>\n"
        "      \"help <tab>\" to get a list of possible help topics\n"
        "      \"quit\" to exit\n"
        "\n"
        "To set redis-cli preferences:\n"
        "      \":set hints\" enable online hints\n"
        "      \":set nohints\" disable online hints\n"
        "Set your preferences in ~/.redisclirc\n",
        version
    );
    sdsfree(version);
}

/* Output all command help, filtering by group or command name. */
static void cliOutputHelp(int argc, char **argv) {
    int i, j;
    char *group = NULL;
    helpEntry *entry;
    struct commandDocs *help;

    if (argc == 0) {
        cliOutputGenericHelp();
        return;
    } else if (argc > 0 && argv[0][0] == '@') {
        group = argv[0]+1;
    }

    if (helpEntries == NULL) {
        /* Initialize the help using the results of the COMMAND command.
         * In case we are using redis-cli help XXX, we need to init it. */
        cliInitHelp();
    }

    assert(argc > 0);
    for (i = 0; i < helpEntriesLen; i++) {
        entry = &helpEntries[i];
        if (entry->type != CLI_HELP_COMMAND) continue;

        help = &entry->docs;
        if (group == NULL) {
            /* Compare all arguments */
            if (argc <= entry->argc) {
                for (j = 0; j < argc; j++) {
                    if (strcasecmp(argv[j],entry->argv[j]) != 0) break;
                }
                if (j == argc) {
                    cliOutputCommandHelp(help,1);
                }
            }
        } else if (strcasecmp(group, help->group) == 0) {
            cliOutputCommandHelp(help,0);
        }
    }
    printf("\r\n");
}

/* Linenoise completion callback. */
static void completionCallback(const char *buf, linenoiseCompletions *lc) {
    size_t startpos = 0;
    int mask;
    int i;
    size_t matchlen;
    sds tmp;

    if (strncasecmp(buf,"help ",5) == 0) {
        startpos = 5;
        while (isspace(buf[startpos])) startpos++;
        mask = CLI_HELP_COMMAND | CLI_HELP_GROUP;
    } else {
        mask = CLI_HELP_COMMAND;
    }

    for (i = 0; i < helpEntriesLen; i++) {
        if (!(helpEntries[i].type & mask)) continue;

        matchlen = strlen(buf+startpos);
        if (strncasecmp(buf+startpos,helpEntries[i].full,matchlen) == 0) {
            tmp = sdsnewlen(buf,startpos);
            tmp = sdscat(tmp,helpEntries[i].full);
            linenoiseAddCompletion(lc,tmp);
            sdsfree(tmp);
        }
    }
}

static sds addHintForArgument(sds hint, cliCommandArg *arg);

/* Adds a separator character between words of a string under construction.
 * A separator is added if the string length is greater than its previously-recorded
 * length (*len), which is then updated, and it's not the last word to be added.
 */
static sds addSeparator(sds str, size_t *len, char *separator, int is_last) {
    if (sdslen(str) > *len && !is_last) {
        str = sdscat(str, separator);
        *len = sdslen(str);
    }
    return str;
}

/* Recursively zeros the matched* fields of all arguments. */
static void clearMatchedArgs(cliCommandArg *args, int numargs) {
    for (int i = 0; i != numargs; ++i) {
        args[i].matched = 0;
        args[i].matched_token = 0;
        args[i].matched_name = 0;
        args[i].matched_all = 0;
        if (args[i].subargs) {
            clearMatchedArgs(args[i].subargs, args[i].numsubargs);
        }
    }
}

/* Builds a completion hint string describing the arguments, skipping parts already matched.
 * Hints for all arguments are added to the input 'hint' parameter, separated by 'separator'.
 */
static sds addHintForArguments(sds hint, cliCommandArg *args, int numargs, char *separator) {
    int i, j, incomplete;
    size_t len=sdslen(hint);
    for (i = 0; i < numargs; i++) {
        if (!(args[i].flags & CMD_ARG_OPTIONAL)) {
            hint = addHintForArgument(hint, &args[i]);
            hint = addSeparator(hint, &len, separator, i == numargs-1);
            continue;
        }

        /* The rule is that successive "optional" arguments can appear in any order.
         * But if they are followed by a required argument, no more of those optional arguments
         * can appear after that.
         * 
         * This code handles all successive optional args together. This lets us show the
         * completion of the currently-incomplete optional arg first, if there is one.
         */
        for (j = i, incomplete = -1; j < numargs; j++) {
            if (!(args[j].flags & CMD_ARG_OPTIONAL)) break;
            if (args[j].matched != 0 && args[j].matched_all == 0) {
                /* User has started typing this arg; show its completion first. */
                hint = addHintForArgument(hint, &args[j]);
                hint = addSeparator(hint, &len, separator, i == numargs-1);
                incomplete = j;
            }
        }

        /* If the following non-optional arg has not been matched, add hints for
         * any remaining optional args in this group. 
         */
        if (j == numargs || args[j].matched == 0) {
            for (; i < j; i++) {
                if (incomplete != i) {
                    hint = addHintForArgument(hint, &args[i]);
                    hint = addSeparator(hint, &len, separator, i == numargs-1);
                }
            }
        }

        i = j - 1;
    }
    return hint;
}

/* Adds the "repeating" section of the hint string for a multiple-typed argument: [ABC def ...]
 * The repeating part is a fixed unit; we don't filter matched elements from it.
 */
static sds addHintForRepeatedArgument(sds hint, cliCommandArg *arg) {
    if (!(arg->flags & CMD_ARG_MULTIPLE)) {
        return hint;
    }

    /* The repeating part is always shown at the end of the argument's hint,
     * so we can safely clear its matched flags before printing it.
     */
    clearMatchedArgs(arg, 1);
        
    if (hint[0] != '\0') {
        hint = sdscat(hint, " ");
    }
    hint = sdscat(hint, "[");

    if (arg->flags & CMD_ARG_MULTIPLE_TOKEN) {
        hint = sdscat_orempty(hint, arg->token);
        if (arg->type != ARG_TYPE_PURE_TOKEN) {
            hint = sdscat(hint, " ");
        }
    }

    switch (arg->type) {
     case ARG_TYPE_ONEOF:
        hint = addHintForArguments(hint, arg->subargs, arg->numsubargs, "|");
        break;

    case ARG_TYPE_BLOCK:
        hint = addHintForArguments(hint, arg->subargs, arg->numsubargs, " ");
        break;

    case ARG_TYPE_PURE_TOKEN:
        break;

    default:
        hint = sdscat_orempty(hint, arg->display_text ? arg->display_text : arg->name);
        break;
    }

    hint = sdscat(hint, " ...]");
    return hint;
}

/* Adds hint string for one argument, if not already matched. */
static sds addHintForArgument(sds hint, cliCommandArg *arg) {
    if (arg->matched_all) {
        return hint;
    }

    /* Surround an optional arg with brackets, unless it's partially matched. */
    if ((arg->flags & CMD_ARG_OPTIONAL) && !arg->matched) {
        hint = sdscat(hint, "[");
    }

    /* Start with the token, if present and not matched. */
    if (arg->token != NULL && !arg->matched_token) {
        hint = sdscat_orempty(hint, arg->token);
        if (arg->type != ARG_TYPE_PURE_TOKEN) {
            hint = sdscat(hint, " ");
        }
    }

    /* Add the body of the syntax string. */
    switch (arg->type) {
     case ARG_TYPE_ONEOF:
        if (arg->matched == 0) {
            hint = addHintForArguments(hint, arg->subargs, arg->numsubargs, "|");
        } else {
            int i;
            for (i = 0; i < arg->numsubargs; i++) {
                if (arg->subargs[i].matched != 0) {
                    hint = addHintForArgument(hint, &arg->subargs[i]);
                }
            }
        }
        break;

    case ARG_TYPE_BLOCK:
        hint = addHintForArguments(hint, arg->subargs, arg->numsubargs, " ");
        break;

    case ARG_TYPE_PURE_TOKEN:
        break;

    default:
        if (!arg->matched_name) {
            hint = sdscat_orempty(hint, arg->display_text ? arg->display_text : arg->name);
        }
        break;
    }

    hint = addHintForRepeatedArgument(hint, arg);

    if ((arg->flags & CMD_ARG_OPTIONAL) && !arg->matched) {
        hint = sdscat(hint, "]");
    }

    return hint;
}

static int matchArg(char **nextword, int numwords, cliCommandArg *arg);
static int matchArgs(char **words, int numwords, cliCommandArg *args, int numargs);

/* Tries to match the next words of the input against an argument. */
static int matchNoTokenArg(char **nextword, int numwords, cliCommandArg *arg) {
    int i;
    switch (arg->type) {
    case ARG_TYPE_BLOCK: {
        arg->matched += matchArgs(nextword, numwords, arg->subargs, arg->numsubargs);

        /* All the subargs must be matched for the block to match. */
        arg->matched_all = 1;
        for (i = 0; i < arg->numsubargs; i++) {
            if (arg->subargs[i].matched_all == 0) {
                arg->matched_all = 0;
            }
        }
        break;
    }
    case ARG_TYPE_ONEOF: {
        for (i = 0; i < arg->numsubargs; i++) {
            if (matchArg(nextword, numwords, &arg->subargs[i])) {
                arg->matched += arg->subargs[i].matched;
                arg->matched_all = arg->subargs[i].matched_all;
                break;
            }
        }
        break;
    }

    case ARG_TYPE_INTEGER:
    case ARG_TYPE_UNIX_TIME: {
        long long value;
        if (sscanf(*nextword, "%lld", &value) == 1) {
            arg->matched += 1;
            arg->matched_name = 1;
            arg->matched_all = 1;
        } else {
            /* Matching failed due to incorrect arg type. */
            arg->matched = 0;
            arg->matched_name = 0;
        }
        break;
    }

    case ARG_TYPE_DOUBLE: {
        double value;
        if (sscanf(*nextword, "%lf", &value) == 1) {
            arg->matched += 1;
            arg->matched_name = 1;
            arg->matched_all = 1;
        } else {
            /* Matching failed due to incorrect arg type. */
            arg->matched = 0;
            arg->matched_name = 0;
        }
        break;
    }

    default:
        arg->matched += 1;
        arg->matched_name = 1;
        arg->matched_all = 1;
        break;
    }
    return arg->matched;
}

/* Tries to match the next word of the input against a token literal. */
static int matchToken(char **nextword, cliCommandArg *arg) {
    if (strcasecmp(arg->token, nextword[0]) != 0) {
        return 0;
    }
    arg->matched_token = 1;
    arg->matched = 1;
    return 1;
}

/* Tries to match the next words of the input against the next argument.
 * If the arg is repeated ("multiple"), it will be matched only once.
 * If the next input word(s) can't be matched, returns 0 for failure.
 */
static int matchArgOnce(char **nextword, int numwords, cliCommandArg *arg) {
    /* First match the token, if present. */
    if (arg->token != NULL) {
        if (!matchToken(nextword, arg)) {
            return 0;
        }
        if (arg->type == ARG_TYPE_PURE_TOKEN) {
            arg->matched_all = 1;
            return 1;
        }
        if (numwords == 1) {
            return 1;
        }
        nextword++;
        numwords--;
    }

    /* Then match the rest of the argument. */
    if (!matchNoTokenArg(nextword, numwords, arg)) {
        return 0;
    }
    return arg->matched;
}

/* Tries to match the next words of the input against the next argument.
 * If the arg is repeated ("multiple"), it will be matched as many times as possible.
 */
static int matchArg(char **nextword, int numwords, cliCommandArg *arg) {
    int matchedWords = 0;
    int matchedOnce = matchArgOnce(nextword, numwords, arg);
    if (!(arg->flags & CMD_ARG_MULTIPLE)) {
        return matchedOnce;
    }

    /* Found one match; now match a "multiple" argument as many times as possible. */
    matchedWords += matchedOnce;
    while (arg->matched_all && matchedWords < numwords) {
        clearMatchedArgs(arg, 1);
        if (arg->token != NULL && !(arg->flags & CMD_ARG_MULTIPLE_TOKEN)) {
            /* The token only appears the first time; the rest of the times,
             * pretend we saw it so we don't hint it.
             */
            matchedOnce = matchNoTokenArg(nextword + matchedWords, numwords - matchedWords, arg);
            if (arg->matched) {
                arg->matched_token = 1;
            }
        } else {
            matchedOnce = matchArgOnce(nextword + matchedWords, numwords - matchedWords, arg);
        }
        matchedWords += matchedOnce;
    }
    arg->matched_all = 0;  /* Because more repetitions are still possible. */
    return matchedWords;
}

/* Tries to match the next words of the input against
 * any one of a consecutive set of optional arguments.
 */
static int matchOneOptionalArg(char **words, int numwords, cliCommandArg *args, int numargs, int *matchedarg) {
    for (int nextword = 0, nextarg = 0; nextword != numwords && nextarg != numargs; ++nextarg) {
        if (args[nextarg].matched) {
            /* Already matched this arg. */
            continue;
        }

        int matchedWords = matchArg(&words[nextword], numwords - nextword, &args[nextarg]);
        if (matchedWords != 0) {
            *matchedarg = nextarg;
            return matchedWords;
        }
    }
    return 0;
}

/* Matches as many input words as possible against a set of consecutive optional arguments. */
static int matchOptionalArgs(char **words, int numwords, cliCommandArg *args, int numargs) {
    int nextword = 0;
    int matchedarg = -1, lastmatchedarg = -1;
    while (nextword != numwords) {
        int matchedWords = matchOneOptionalArg(&words[nextword], numwords - nextword, args, numargs, &matchedarg);
        if (matchedWords == 0) {
            break;
        }
        /* Successfully matched an optional arg; mark any previous match as completed
         * so it won't be partially hinted.
         */
        if (lastmatchedarg != -1) {
            args[lastmatchedarg].matched_all = 1;
        }
        lastmatchedarg = matchedarg;
        nextword += matchedWords;
    }
    return nextword;
}

/* Matches as many input words as possible against command arguments. */
static int matchArgs(char **words, int numwords, cliCommandArg *args, int numargs) {
    int nextword, nextarg, matchedWords;
    for (nextword = 0, nextarg = 0; nextword != numwords && nextarg != numargs; ++nextarg) {
        /* Optional args can occur in any order. Collect a range of consecutive optional args
         * and try to match them as a group against the next input words.
         */
        if (args[nextarg].flags & CMD_ARG_OPTIONAL) {
            int lastoptional;
            for (lastoptional = nextarg; lastoptional < numargs; lastoptional++) {
                if (!(args[lastoptional].flags & CMD_ARG_OPTIONAL)) break;
            }
            matchedWords = matchOptionalArgs(&words[nextword], numwords - nextword, &args[nextarg], lastoptional - nextarg);
            nextarg = lastoptional - 1;
        } else {
            matchedWords = matchArg(&words[nextword], numwords - nextword, &args[nextarg]);
            if (matchedWords == 0) {
                /* Couldn't match a required word - matching fails! */
                return 0;
            }
        }

        nextword += matchedWords;
    }
    return nextword;
}

/* Compute the linenoise hint for the input prefix in inputargv/inputargc.
 * cmdlen is the number of words from the start of the input that make up the command.
 * If docs.args exists, dynamically creates a hint string by matching the arg specs
 * against the input words.
 */
static sds makeHint(char **inputargv, int inputargc, int cmdlen, struct commandDocs docs) {
    sds hint;

    if (docs.args) {
        /* Remove arguments from the returned hint to show only the
         * ones the user did not yet type. */
        clearMatchedArgs(docs.args, docs.numargs);
        hint = sdsempty();
        int matchedWords = 0;
        if (inputargv && inputargc)
            matchedWords = matchArgs(inputargv + cmdlen, inputargc - cmdlen, docs.args, docs.numargs);
        if (matchedWords == inputargc - cmdlen) {
            hint = addHintForArguments(hint, docs.args, docs.numargs, " ");
        }
        return hint;
    }

    /* If arg specs are not available, show the hint string until the user types something. */
    if (inputargc <= cmdlen) {
        hint = sdsnew(docs.params);
    } else {
        hint = sdsempty();
    }
    return hint;
}

/* Search for a command matching the longest possible prefix of input words. */
static helpEntry* findHelpEntry(int argc, char **argv) {
    helpEntry *entry = NULL;
    int i, rawargc, matchlen = 0;
    sds *rawargv;

    for (i = 0; i < helpEntriesLen; i++) {
        if (!(helpEntries[i].type & CLI_HELP_COMMAND)) continue;

        rawargv = helpEntries[i].argv;
        rawargc = helpEntries[i].argc;
        if (rawargc <= argc) {
            int j;
            for (j = 0; j < rawargc; j++) {
                if (strcasecmp(rawargv[j],argv[j])) {
                    break;
                }
            }
            if (j == rawargc && rawargc > matchlen) {
                matchlen = rawargc;
                entry = &helpEntries[i];
            }
        }
    }
    return entry;
}

/* Returns the command-line hint string for a given partial input. */
static sds getHintForInput(const char *charinput) {
    sds hint = NULL;
    int inputargc, inputlen = strlen(charinput);
    sds *inputargv = sdssplitargs(charinput, &inputargc);
    int endspace = inputlen && isspace(charinput[inputlen-1]);

    /* Don't match the last word until the user has typed a space after it. */
    int matchargc = endspace ? inputargc : inputargc - 1;

    helpEntry *entry = findHelpEntry(matchargc, inputargv);
    if (entry) {
       hint = makeHint(inputargv, matchargc, entry->argc, entry->docs);
    }
    sdsfreesplitres(inputargv, inputargc);
    return hint;
}

/* Linenoise hints callback. */
static char *hintsCallback(const char *buf, int *color, int *bold) {
    if (!pref.hints) return NULL;

    sds hint = getHintForInput(buf);
    if (hint == NULL) {
        return NULL;
    }

    *color = 90;
    *bold = 0;

    /* Add an initial space if needed. */
    int len = strlen(buf);
    int endspace = len && isspace(buf[len-1]);
    if (!endspace) {
        sds newhint = sdsnewlen(" ",1);
        newhint = sdscatsds(newhint,hint);
        sdsfree(hint);
        hint = newhint;
    }

    return hint;
}

static void freeHintsCallback(void *ptr) {
    sdsfree(ptr);
}

/*------------------------------------------------------------------------------
 * TTY manipulation
 *--------------------------------------------------------------------------- */

/* Restore terminal if we've changed it. */
void cliRestoreTTY(void) {
    if (orig_termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

/* Put the terminal in "press any key" mode */
static void cliPressAnyKeyTTY(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (!orig_termios_saved) {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return;
        atexit(cliRestoreTTY);
        orig_termios_saved = 1;
    }
    struct termios mode = orig_termios;
    mode.c_lflag &= ~(ECHO | ICANON); /* echoing off, canonical off */
    tcsetattr(STDIN_FILENO, TCSANOW, &mode);
}

/*------------------------------------------------------------------------------
 * Networking / parsing
 *--------------------------------------------------------------------------- */

/* Send AUTH command to the server */
static int cliAuth(redisContext *ctx, char *user, char *auth) {
    redisReply *reply;
    if (auth == NULL) return REDIS_OK;

    if (user == NULL)
        reply = redisCommand(ctx,"AUTH %s",auth);
    else
        reply = redisCommand(ctx,"AUTH %s %s",user,auth);

    if (reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        return REDIS_ERR;
    }

    int result = REDIS_OK;
    if (reply->type == REDIS_REPLY_ERROR) {
        result = REDIS_ERR;
        fprintf(stderr, "AUTH failed: %s\n", reply->str);
    }
    freeReplyObject(reply);
    return result;
}

/* Send SELECT input_dbnum to the server */
static int cliSelect(void) {
    redisReply *reply;
    if (config.conn_info.input_dbnum == config.dbnum) return REDIS_OK;

    reply = redisCommand(context,"SELECT %d",config.conn_info.input_dbnum);
    if (reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        return REDIS_ERR;
    }

    int result = REDIS_OK;
    if (reply->type == REDIS_REPLY_ERROR) {
        result = REDIS_ERR;
        fprintf(stderr,"SELECT %d failed: %s\n",config.conn_info.input_dbnum,reply->str);
    } else {
        config.dbnum = config.conn_info.input_dbnum;
        cliRefreshPrompt();
    }
    freeReplyObject(reply);
    return result;
}

/* Select RESP3 mode if redis-cli was started with the -3 option.  */
static int cliSwitchProto(void) {
    redisReply *reply;
    if (!config.resp3 || config.resp2) return REDIS_OK;

    reply = redisCommand(context,"HELLO 3");
    if (reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        return REDIS_ERR;
    }

    int result = REDIS_OK;
    if (reply->type == REDIS_REPLY_ERROR) {
        fprintf(stderr,"HELLO 3 failed: %s\n",reply->str);
        if (config.resp3 == 1) {
            result = REDIS_ERR;
        } else if (config.resp3 == 2) {
            result = REDIS_OK;
        }
    }

    /* Retrieve server version string for later use. */
    for (size_t i = 0; i < reply->elements; i += 2) {
        assert(reply->element[i]->type == REDIS_REPLY_STRING);
        char *key = reply->element[i]->str;
        if (!strcmp(key, "version")) {
            assert(reply->element[i + 1]->type == REDIS_REPLY_STRING);
            config.server_version = sdsnew(reply->element[i + 1]->str);
        }
    }
    freeReplyObject(reply);
    config.current_resp3 = 1;
    return result;
}

/* Connect to the server. It is possible to pass certain flags to the function:
 *      CC_FORCE: The connection is performed even if there is already
 *                a connected socket.
 *      CC_QUIET: Don't print errors if connection fails. */
static int cliConnect(int flags) {
    if (context == NULL || flags & CC_FORCE) {
        if (context != NULL) {
            redisFree(context);
            config.dbnum = 0;
            config.in_multi = 0;
            config.pubsub_mode = 0;
            cliRefreshPrompt();
        }

        /* Do not use hostsocket when we got redirected in cluster mode */
        if (config.hostsocket == NULL ||
            (config.cluster_mode && config.cluster_reissue_command)) {
            context = redisConnectWrapper(config.conn_info.hostip, config.conn_info.hostport,
                                          config.connect_timeout);
        } else {
            context = redisConnectUnixWrapper(config.hostsocket, config.connect_timeout);
        }

        if (!context->err && config.tls) {
            const char *err = NULL;
            if (cliSecureConnection(context, config.sslconfig, &err) == REDIS_ERR && err) {
                fprintf(stderr, "Could not negotiate a TLS connection: %s\n", err);
                redisFree(context);
                context = NULL;
                return REDIS_ERR;
            }
        }

        if (context->err) {
            if (!(flags & CC_QUIET)) {
                fprintf(stderr,"Could not connect to Redis at ");
                if (config.hostsocket == NULL ||
                    (config.cluster_mode && config.cluster_reissue_command))
                {
                    fprintf(stderr, "%s:%d: %s\n",
                        config.conn_info.hostip,config.conn_info.hostport,context->errstr);
                } else {
                    fprintf(stderr,"%s: %s\n",
                        config.hostsocket,context->errstr);
                }
            }
            redisFree(context);
            context = NULL;
            return REDIS_ERR;
        }


        /* Set aggressive KEEP_ALIVE socket option in the Redis context socket
         * in order to prevent timeouts caused by the execution of long
         * commands. At the same time this improves the detection of real
         * errors. */
        anetKeepAlive(NULL, context->fd, REDIS_CLI_KEEPALIVE_INTERVAL);

        /* State of the current connection. */
        config.current_resp3 = 0;

        /* Do AUTH, select the right DB, switch to RESP3 if needed. */
        if (cliAuth(context, config.conn_info.user, config.conn_info.auth) != REDIS_OK)
            return REDIS_ERR;
        if (cliSelect() != REDIS_OK)
            return REDIS_ERR;
        if (cliSwitchProto() != REDIS_OK)
            return REDIS_ERR;
    }

    /* Set a PUSH handler if configured to do so. */
    if (config.push_output) {
        redisSetPushCallback(context, cliPushHandler);
    }

    return REDIS_OK;
}

/* In cluster, if server replies ASK, we will redirect to a different node.
 * Before sending the real command, we need to send ASKING command first. */
static int cliSendAsking(void) {
    redisReply *reply;

    config.cluster_send_asking = 0;
    if (context == NULL) {
        return REDIS_ERR;
    }
    reply = redisCommand(context,"ASKING");
    if (reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        return REDIS_ERR;
    }
    int result = REDIS_OK;
    if (reply->type == REDIS_REPLY_ERROR) {
        result = REDIS_ERR;
        fprintf(stderr,"ASKING failed: %s\n",reply->str);
    }
    freeReplyObject(reply);
    return result;
}

static void cliPrintContextError(void) {
    if (context == NULL) return;
    fprintf(stderr,"Error: %s\n",context->errstr);
}

static int isInvalidateReply(redisReply *reply) {
    return reply->type == REDIS_REPLY_PUSH && reply->elements == 2 &&
        reply->element[0]->type == REDIS_REPLY_STRING &&
        !strncmp(reply->element[0]->str, "invalidate", 10) &&
        reply->element[1]->type == REDIS_REPLY_ARRAY;
}

/* Special display handler for RESP3 'invalidate' messages.
 * This function does not validate the reply, so it should
 * already be confirmed correct */
static sds cliFormatInvalidateTTY(redisReply *r) {
    sds out = sdsnew("-> invalidate: ");

    for (size_t i = 0; i < r->element[1]->elements; i++) {
        redisReply *key = r->element[1]->element[i];
        assert(key->type == REDIS_REPLY_STRING);

        out = sdscatfmt(out, "'%s'", key->str, key->len);
        if (i < r->element[1]->elements - 1)
            out = sdscatlen(out, ", ", 2);
    }

    return sdscatlen(out, "\n", 1);
}

/* Returns non-zero if cliFormatReplyTTY renders the reply in multiple lines. */
static int cliIsMultilineValueTTY(redisReply *r) {
    switch (r->type) {
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
        if (r->elements == 0) return 0;
        if (r->elements > 1) return 1;
        return cliIsMultilineValueTTY(r->element[0]);
    case REDIS_REPLY_MAP:
        if (r->elements == 0) return 0;
        if (r->elements > 2) return 1;
        return cliIsMultilineValueTTY(r->element[1]);
    default:
        return 0;
    }
}

static sds cliFormatReplyTTY(redisReply *r, char *prefix) {
    sds out = sdsempty();
    switch (r->type) {
    case REDIS_REPLY_ERROR:
        out = sdscatprintf(out,"(error) %s\n", r->str);
    break;
    case REDIS_REPLY_STATUS:
        out = sdscat(out,r->str);
        out = sdscat(out,"\n");
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"(integer) %lld\n",r->integer);
    break;
    case REDIS_REPLY_DOUBLE:
        out = sdscatprintf(out,"(double) %s\n",r->str);
    break;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        /* If you are producing output for the standard output we want
        * a more interesting output with quoted characters and so forth,
        * unless it's a verbatim string type. */
        if (r->type == REDIS_REPLY_STRING) {
            out = sdscatrepr(out,r->str,r->len);
            out = sdscat(out,"\n");
        } else {
            out = sdscatlen(out,r->str,r->len);
            out = sdscat(out,"\n");
        }
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"(nil)\n");
    break;
    case REDIS_REPLY_BOOL:
        out = sdscat(out,r->integer ? "(true)\n" : "(false)\n");
    break;
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_MAP:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
        if (r->elements == 0) {
            if (r->type == REDIS_REPLY_ARRAY)
                out = sdscat(out,"(empty array)\n");
            else if (r->type == REDIS_REPLY_MAP)
                out = sdscat(out,"(empty hash)\n");
            else if (r->type == REDIS_REPLY_SET)
                out = sdscat(out,"(empty set)\n");
            else if (r->type == REDIS_REPLY_PUSH)
                out = sdscat(out,"(empty push)\n");
            else
                out = sdscat(out,"(empty aggregate type)\n");
        } else {
            unsigned int i, idxlen = 0;
            char _prefixlen[16];
            char _prefixfmt[16];
            sds _prefix;
            sds tmp;

            /* Calculate chars needed to represent the largest index */
            i = r->elements;
            if (r->type == REDIS_REPLY_MAP) i /= 2;
            do {
                idxlen++;
                i /= 10;
            } while(i);

            /* Prefix for nested multi bulks should grow with idxlen+2 spaces */
            memset(_prefixlen,' ',idxlen+2);
            _prefixlen[idxlen+2] = '\0';
            _prefix = sdscat(sdsnew(prefix),_prefixlen);

            /* Setup prefix format for every entry */
            char numsep;
            if (r->type == REDIS_REPLY_SET) numsep = '~';
            else if (r->type == REDIS_REPLY_MAP) numsep = '#';
            /* TODO: this would be a breaking change for scripts, do that in a major version. */
            /* else if (r->type == REDIS_REPLY_PUSH) numsep = '>'; */
            else numsep = ')';
            snprintf(_prefixfmt,sizeof(_prefixfmt),"%%s%%%ud%c ",idxlen,numsep);

            for (i = 0; i < r->elements; i++) {
                unsigned int human_idx = (r->type == REDIS_REPLY_MAP) ?
                                         i/2 : i;
                human_idx++; /* Make it 1-based. */

                /* Don't use the prefix for the first element, as the parent
                 * caller already prepended the index number. */
                out = sdscatprintf(out,_prefixfmt,i == 0 ? "" : prefix,human_idx);

                /* Format the multi bulk entry */
                tmp = cliFormatReplyTTY(r->element[i],_prefix);
                out = sdscatlen(out,tmp,sdslen(tmp));
                sdsfree(tmp);

                /* For maps, format the value as well. */
                if (r->type == REDIS_REPLY_MAP) {
                    i++;
                    sdsrange(out,0,-2);
                    out = sdscat(out," => ");
                    if (cliIsMultilineValueTTY(r->element[i])) {
                        /* linebreak before multiline value to fix alignment */
                        out = sdscat(out, "\n");
                        out = sdscat(out, _prefix);
                    }
                    tmp = cliFormatReplyTTY(r->element[i],_prefix);
                    out = sdscatlen(out,tmp,sdslen(tmp));
                    sdsfree(tmp);
                }
            }
            sdsfree(_prefix);
        }
    break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}

/* Returns 1 if the reply is a pubsub pushed reply. */
int isPubsubPush(redisReply *r) {
    if (r == NULL ||
        r->type != (config.current_resp3 ? REDIS_REPLY_PUSH : REDIS_REPLY_ARRAY) ||
        r->elements < 3 ||
        r->element[0]->type != REDIS_REPLY_STRING)
    {
        return 0;
    }
    char *str = r->element[0]->str;
    size_t len = r->element[0]->len;
    /* Check if it is [p|s][un]subscribe or [p|s]message, but even simpler, we
     * just check that it ends with "message" or "subscribe". */
    return ((len >= strlen("message") &&
             !strcmp(str + len - strlen("message"), "message")) ||
            (len >= strlen("subscribe") &&
             !strcmp(str + len - strlen("subscribe"), "subscribe")));
}

int isColorTerm(void) {
    char *t = getenv("TERM");
    return t != NULL && strstr(t,"xterm") != NULL;
}

/* Helper function for sdsCatColorizedLdbReply() appending colorize strings
 * to an SDS string. */
sds sdscatcolor(sds o, char *s, size_t len, char *color) {
    if (!isColorTerm()) return sdscatlen(o,s,len);

    int bold = strstr(color,"bold") != NULL;
    int ccode = 37; /* Defaults to white. */
    if (strstr(color,"red")) ccode = 31;
    else if (strstr(color,"green")) ccode = 32;
    else if (strstr(color,"yellow")) ccode = 33;
    else if (strstr(color,"blue")) ccode = 34;
    else if (strstr(color,"magenta")) ccode = 35;
    else if (strstr(color,"cyan")) ccode = 36;
    else if (strstr(color,"white")) ccode = 37;

    o = sdscatfmt(o,"\033[%i;%i;49m",bold,ccode);
    o = sdscatlen(o,s,len);
    o = sdscat(o,"\033[0m");
    return o;
}

/* Colorize Lua debugger status replies according to the prefix they
 * have. */
sds sdsCatColorizedLdbReply(sds o, char *s, size_t len) {
    char *color = "white";

    if (strstr(s,"<debug>")) color = "bold";
    if (strstr(s,"<redis>")) color = "green";
    if (strstr(s,"<reply>")) color = "cyan";
    if (strstr(s,"<error>")) color = "red";
    if (strstr(s,"<hint>")) color = "bold";
    if (strstr(s,"<value>") || strstr(s,"<retval>")) color = "magenta";
    if (len > 4 && isdigit(s[3])) {
        if (s[1] == '>') color = "yellow"; /* Current line. */
        else if (s[2] == '#') color = "bold"; /* Break point. */
    }
    return sdscatcolor(o,s,len,color);
}

static sds cliFormatReplyRaw(redisReply *r) {
    sds out = sdsempty(), tmp;
    size_t i;

    switch (r->type) {
    case REDIS_REPLY_NIL:
        /* Nothing... */
        break;
    case REDIS_REPLY_ERROR:
        out = sdscatlen(out,r->str,r->len);
        out = sdscatlen(out,"\n",1);
        break;
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        if (r->type == REDIS_REPLY_STATUS && config.eval_ldb) {
            /* The Lua debugger replies with arrays of simple (status)
             * strings. We colorize the output for more fun if this
             * is a debugging session. */

            /* Detect the end of a debugging session. */
            if (strstr(r->str,"<endsession>") == r->str) {
                config.enable_ldb_on_eval = 0;
                config.eval_ldb = 0;
                config.eval_ldb_end = 1; /* Signal the caller session ended. */
                config.output = OUTPUT_STANDARD;
                cliRefreshPrompt();
            } else {
                out = sdsCatColorizedLdbReply(out,r->str,r->len);
            }
        } else {
            out = sdscatlen(out,r->str,r->len);
        }
        break;
    case REDIS_REPLY_BOOL:
        out = sdscat(out,r->integer ? "(true)" : "(false)");
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
        break;
    case REDIS_REPLY_DOUBLE:
        out = sdscatprintf(out,"%s",r->str);
        break;
    case REDIS_REPLY_SET:
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_PUSH:
        for (i = 0; i < r->elements; i++) {
            if (i > 0) out = sdscat(out,config.mb_delim);
            tmp = cliFormatReplyRaw(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            sdsfree(tmp);
        }
        break;
    case REDIS_REPLY_MAP:
        for (i = 0; i < r->elements; i += 2) {
            if (i > 0) out = sdscat(out,config.mb_delim);
            tmp = cliFormatReplyRaw(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            sdsfree(tmp);

            out = sdscatlen(out," ",1);
            tmp = cliFormatReplyRaw(r->element[i+1]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            sdsfree(tmp);
        }
        break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}

static sds cliFormatReplyCSV(redisReply *r) {
    unsigned int i;

    sds out = sdsempty();
    switch (r->type) {
    case REDIS_REPLY_ERROR:
        out = sdscat(out,"ERROR,");
        out = sdscatrepr(out,r->str,strlen(r->str));
    break;
    case REDIS_REPLY_STATUS:
        out = sdscatrepr(out,r->str,r->len);
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
    break;
    case REDIS_REPLY_DOUBLE:
        out = sdscatprintf(out,"%s",r->str);
        break;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        out = sdscatrepr(out,r->str,r->len);
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"NULL");
    break;
    case REDIS_REPLY_BOOL:
        out = sdscat(out,r->integer ? "true" : "false");
    break;
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
    case REDIS_REPLY_MAP: /* CSV has no map type, just output flat list. */
        for (i = 0; i < r->elements; i++) {
            sds tmp = cliFormatReplyCSV(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            if (i != r->elements-1) out = sdscat(out,",");
            sdsfree(tmp);
        }
    break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}

/* Append specified buffer to out and return it, using required JSON output
 * mode. */
static sds jsonStringOutput(sds out, const char *p, int len, int mode) {
    if (mode == OUTPUT_JSON) {
        return escapeJsonString(out, p, len);
    } else if (mode == OUTPUT_QUOTED_JSON) {
        /* Need to double-quote backslashes */
        sds tmp = sdscatrepr(sdsempty(), p, len);
        int tmplen = sdslen(tmp);
        char *n = tmp;
        while (tmplen--) {
            if (*n == '\\') out = sdscatlen(out, "\\\\", 2);
            else out = sdscatlen(out, n, 1);
            n++;
        }

        sdsfree(tmp);
        return out;
    } else {
        assert(0);
    }
}

static sds cliFormatReplyJson(sds out, redisReply *r, int mode) {
    unsigned int i;

    switch (r->type) {
    case REDIS_REPLY_ERROR:
        out = sdscat(out,"error:");
        out = jsonStringOutput(out,r->str,strlen(r->str),mode);
        break;
    case REDIS_REPLY_STATUS:
        out = jsonStringOutput(out,r->str,r->len,mode);
        break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
        break;
    case REDIS_REPLY_DOUBLE:
        out = sdscatprintf(out,"%s",r->str);
        break;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        out = jsonStringOutput(out,r->str,r->len,mode);
        break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"null");
        break;
    case REDIS_REPLY_BOOL:
        out = sdscat(out,r->integer ? "true" : "false");
        break;
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
        out = sdscat(out,"[");
        for (i = 0; i < r->elements; i++ ) {
            out = cliFormatReplyJson(out,r->element[i],mode);
            if (i != r->elements-1) out = sdscat(out,",");
        }
        out = sdscat(out,"]");
        break;
    case REDIS_REPLY_MAP:
        out = sdscat(out,"{");
        for (i = 0; i < r->elements; i += 2) {
            redisReply *key = r->element[i];
            if (key->type == REDIS_REPLY_ERROR ||
                key->type == REDIS_REPLY_STATUS ||
                key->type == REDIS_REPLY_STRING ||
                key->type == REDIS_REPLY_VERB)
            {
                out = cliFormatReplyJson(out,key,mode);
            } else {
                /* According to JSON spec, JSON map keys must be strings,
                 * and in RESP3, they can be other types. 
                 * The first one(cliFormatReplyJson) is to convert non string type to string
                 * The Second one(escapeJsonString) is to escape the converted string */
                sds keystr = cliFormatReplyJson(sdsempty(),key,mode);
                if (keystr[0] == '"') out = sdscatsds(out,keystr);
                else out = sdscatfmt(out,"\"%S\"",keystr);
                sdsfree(keystr);
            }
            out = sdscat(out,":");

            out = cliFormatReplyJson(out,r->element[i+1],mode);
            if (i != r->elements-2) out = sdscat(out,",");
        }
        out = sdscat(out,"}");
        break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}

/* Generate reply strings in various output modes */
static sds cliFormatReply(redisReply *reply, int mode, int verbatim) {
    sds out;

    if (verbatim) {
        out = cliFormatReplyRaw(reply);
    }  else if (mode == OUTPUT_STANDARD) {
        out = cliFormatReplyTTY(reply, "");
    } else if (mode == OUTPUT_RAW) {
        out = cliFormatReplyRaw(reply);
        out = sdscatsds(out, config.cmd_delim);
    } else if (mode == OUTPUT_CSV) {
        out = cliFormatReplyCSV(reply);
        out = sdscatlen(out, "\n", 1);
    } else if (mode == OUTPUT_JSON || mode == OUTPUT_QUOTED_JSON) {
        out = cliFormatReplyJson(sdsempty(), reply, mode);
        out = sdscatlen(out, "\n", 1);
    } else {
        fprintf(stderr, "Error:  Unknown output encoding %d\n", mode);
        exit(1);
    }

    return out;
}

/* Output any spontaneous PUSH reply we receive */
static void cliPushHandler(void *privdata, void *reply) {
    UNUSED(privdata);
    sds out;

    if (config.output == OUTPUT_STANDARD && isInvalidateReply(reply)) {
        out = cliFormatInvalidateTTY(reply);
    } else {
        out = cliFormatReply(reply, config.output, 0);
    }

    fwrite(out, sdslen(out), 1, stdout);

    freeReplyObject(reply);
    sdsfree(out);
}

static int cliReadReply(int output_raw_strings) {
    void *_reply;
    redisReply *reply;
    sds out = NULL;
    int output = 1;

    if (config.last_reply) {
        freeReplyObject(config.last_reply);
        config.last_reply = NULL;
    }

    if (redisGetReply(context,&_reply) != REDIS_OK) {
        if (config.blocking_state_aborted) {
            config.blocking_state_aborted = 0;
            config.monitor_mode = 0;
            config.pubsub_mode = 0;
            return cliConnect(CC_FORCE);
        }

        if (config.shutdown) {
            redisFree(context);
            context = NULL;
            return REDIS_OK;
        }
        if (config.interactive) {
            /* Filter cases where we should reconnect */
            if (context->err == REDIS_ERR_IO &&
                (errno == ECONNRESET || errno == EPIPE))
                return REDIS_ERR;
            if (context->err == REDIS_ERR_EOF)
                return REDIS_ERR;
        }
        cliPrintContextError();
        exit(1);
        return REDIS_ERR; /* avoid compiler warning */
    }

    config.last_reply = reply = (redisReply*)_reply;

    config.last_cmd_type = reply->type;

    /* Check if we need to connect to a different node and reissue the
     * request. */
    if (config.cluster_mode && reply->type == REDIS_REPLY_ERROR &&
        (!strncmp(reply->str,"MOVED ",6) || !strncmp(reply->str,"ASK ",4)))
    {
        char *p = reply->str, *s;
        int slot;

        output = 0;
        /* Comments show the position of the pointer as:
         *
         * [S] for pointer 's'
         * [P] for pointer 'p'
         */
        s = strchr(p,' ');      /* MOVED[S]3999 127.0.0.1:6381 */
        p = strchr(s+1,' ');    /* MOVED[S]3999[P]127.0.0.1:6381 */
        *p = '\0';
        slot = atoi(s+1);
        s = strrchr(p+1,':');    /* MOVED 3999[P]127.0.0.1[S]6381 */
        *s = '\0';
        if (p+1 != s) {
            /* Host might be empty, like 'MOVED 3999 :6381', if endpoint type is unknown. Only update the
             * host if it's non-empty. */
            sdsfree(config.conn_info.hostip);
            config.conn_info.hostip = sdsnew(p+1);
        }
        config.conn_info.hostport = atoi(s+1);
        if (config.interactive)
            printf("-> Redirected to slot [%d] located at %s:%d\n",
                slot, config.conn_info.hostip, config.conn_info.hostport);
        config.cluster_reissue_command = 1;
        if (!strncmp(reply->str,"ASK ",4)) {
            config.cluster_send_asking = 1;
        }
        cliRefreshPrompt();
    } else if (!config.interactive && config.set_errcode && 
        reply->type == REDIS_REPLY_ERROR) 
    {
        fprintf(stderr,"%s\n",reply->str);
        exit(1);
        return REDIS_ERR; /* avoid compiler warning */
    }

    if (output) {
        out = cliFormatReply(reply, config.output, output_raw_strings);
        fwrite(out,sdslen(out),1,stdout);
        fflush(stdout);
        sdsfree(out);
    }
    return REDIS_OK;
}

/* Simultaneously wait for pubsub messages from redis and input on stdin. */
static void cliWaitForMessagesOrStdin(void) {
    int show_info = config.output != OUTPUT_RAW && (isatty(STDOUT_FILENO) ||
                                                    getenv("FAKETTY"));
    int use_color = show_info && isColorTerm();
    cliPressAnyKeyTTY();
    while (config.pubsub_mode) {
        /* First check if there are any buffered replies. */
        redisReply *reply;
        do {
            if (redisGetReplyFromReader(context, (void **)&reply) != REDIS_OK) {
                cliPrintContextError();
                exit(1);
            }
            if (reply) {
                sds out = cliFormatReply(reply, config.output, 0);
                fwrite(out,sdslen(out),1,stdout);
                fflush(stdout);
                sdsfree(out);
            }
        } while(reply);

        /* Wait for input, either on the Redis socket or on stdin. */
        struct timeval tv;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(context->fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        if (show_info) {
            if (use_color) printf("\033[1;90m"); /* Bold, bright color. */
            printf("Reading messages... (press Ctrl-C to quit or any key to type command)\r");
            if (use_color) printf("\033[0m"); /* Reset color. */
            fflush(stdout);
        }
        select(context->fd + 1, &readfds, NULL, NULL, &tv);
        if (show_info) {
            printf("\033[K"); /* Erase current line */
            fflush(stdout);
        }
        if (config.blocking_state_aborted) {
            /* Ctrl-C pressed */
            config.blocking_state_aborted = 0;
            config.pubsub_mode = 0;
            if (cliConnect(CC_FORCE) != REDIS_OK) {
                cliPrintContextError();
                exit(1);
            }
            break;
        } else if (FD_ISSET(context->fd, &readfds)) {
            /* Message from Redis */
            if (cliReadReply(0) != REDIS_OK) {
                cliPrintContextError();
                exit(1);
            }
            fflush(stdout);
        } else if (FD_ISSET(STDIN_FILENO, &readfds)) {
            /* Any key pressed */
            break;
        }
    }
    cliRestoreTTY();
}

static int cliSendCommand(int argc, char **argv, long repeat) {
    char *command = argv[0];
    size_t *argvlen;
    int j, output_raw;

    if (context == NULL) return REDIS_ERR;

    output_raw = 0;
    if (!strcasecmp(command,"info") ||
        !strcasecmp(command,"lolwut") ||
        (argc >= 2 && !strcasecmp(command,"debug") &&
                       !strcasecmp(argv[1],"htstats")) ||
        (argc >= 2 && !strcasecmp(command,"debug") &&
                       !strcasecmp(argv[1],"htstats-key")) ||
        (argc >= 2 && !strcasecmp(command,"debug") &&
                       !strcasecmp(argv[1],"client-eviction")) ||
        (argc >= 2 && !strcasecmp(command,"memory") &&
                      (!strcasecmp(argv[1],"malloc-stats") ||
                       !strcasecmp(argv[1],"doctor"))) ||
        (argc == 2 && !strcasecmp(command,"cluster") &&
                      (!strcasecmp(argv[1],"nodes") ||
                       !strcasecmp(argv[1],"info"))) ||
        (argc >= 2 && !strcasecmp(command,"client") &&
                       (!strcasecmp(argv[1],"list") ||
                        !strcasecmp(argv[1],"info"))) ||
        (argc == 3 && !strcasecmp(command,"latency") &&
                       !strcasecmp(argv[1],"graph")) ||
        (argc == 2 && !strcasecmp(command,"latency") &&
                       !strcasecmp(argv[1],"doctor")) ||
        /* Format PROXY INFO command for Redis Cluster Proxy:
         * https://github.com/artix75/redis-cluster-proxy */
        (argc >= 2 && !strcasecmp(command,"proxy") &&
                       !strcasecmp(argv[1],"info")))
    {
        output_raw = 1;
    }

    if (!strcasecmp(command,"shutdown")) config.shutdown = 1;
    if (!strcasecmp(command,"monitor")) config.monitor_mode = 1;
    int is_subscribe = (!strcasecmp(command, "subscribe") ||
                        !strcasecmp(command, "psubscribe") ||
                        !strcasecmp(command, "ssubscribe"));
    int is_unsubscribe = (!strcasecmp(command, "unsubscribe") ||
                          !strcasecmp(command, "punsubscribe") ||
                          !strcasecmp(command, "sunsubscribe"));
    if (!strcasecmp(command,"sync") ||
        !strcasecmp(command,"psync")) config.slave_mode = 1;

    /* When the user manually calls SCRIPT DEBUG, setup the activation of
     * debugging mode on the next eval if needed. */
    if (argc == 3 && !strcasecmp(argv[0],"script") &&
                     !strcasecmp(argv[1],"debug"))
    {
        if (!strcasecmp(argv[2],"yes") || !strcasecmp(argv[2],"sync")) {
            config.enable_ldb_on_eval = 1;
        } else {
            config.enable_ldb_on_eval = 0;
        }
    }

    /* Actually activate LDB on EVAL if needed. */
    if (!strcasecmp(command,"eval") && config.enable_ldb_on_eval) {
        config.eval_ldb = 1;
        config.output = OUTPUT_RAW;
    }

    /* Setup argument length */
    argvlen = zmalloc(argc*sizeof(size_t));
    for (j = 0; j < argc; j++)
        argvlen[j] = sdslen(argv[j]);

    /* Negative repeat is allowed and causes infinite loop,
       works well with the interval option. */
    while(repeat < 0 || repeat-- > 0) {
        redisAppendCommandArgv(context,argc,(const char**)argv,argvlen);

        if (config.monitor_mode) {
            do {
                if (cliReadReply(output_raw) != REDIS_OK) {
                    cliPrintContextError();
                    exit(1);
                }
                fflush(stdout);

                /* This happens when the MONITOR command returns an error. */
                if (config.last_cmd_type == REDIS_REPLY_ERROR)
                    config.monitor_mode = 0;
            } while(config.monitor_mode);
            zfree(argvlen);
            return REDIS_OK;
        }

        int num_expected_pubsub_push = 0;
        if (is_subscribe || is_unsubscribe) {
            /* When a push callback is set, redisGetReply (hiredis) loops until
             * an in-band message is received, but these commands are confirmed
             * using push replies only. There is one push reply per channel if
             * channels are specified, otherwise at least one. */
            num_expected_pubsub_push = argc > 1 ? argc - 1 : 1;
            /* Unset our default PUSH handler so this works in RESP2/RESP3 */
            redisSetPushCallback(context, NULL);
        }

        if (config.slave_mode) {
            printf("Entering replica output mode...  (press Ctrl-C to quit)\n");
            slaveMode(0);
            config.slave_mode = 0;
            zfree(argvlen);
            return REDIS_ERR;  /* Error = slaveMode lost connection to master */
        }

        /* Read response, possibly skipping pubsub/push messages. */
        while (1) {
            if (cliReadReply(output_raw) != REDIS_OK) {
                zfree(argvlen);
                return REDIS_ERR;
            }
            fflush(stdout);
            if (config.pubsub_mode || num_expected_pubsub_push > 0) {
                if (isPubsubPush(config.last_reply)) {
                    if (num_expected_pubsub_push > 0 &&
                        !strcasecmp(config.last_reply->element[0]->str, command))
                    {
                        /* This pushed message confirms the
                         * [p|s][un]subscribe command. */
                        if (is_subscribe && !config.pubsub_mode) {
                            config.pubsub_mode = 1;
                            cliRefreshPrompt();
                        }
                        if (--num_expected_pubsub_push > 0) {
                            continue; /* We need more of these. */
                        }
                    } else {
                        continue; /* Skip this pubsub message. */
                    }
                } else if (config.last_reply->type == REDIS_REPLY_PUSH) {
                    continue; /* Skip other push message. */
                }
            }

            /* Store database number when SELECT was successfully executed. */
            if (!strcasecmp(command,"select") && argc == 2 && 
                config.last_cmd_type != REDIS_REPLY_ERROR) 
            {
                config.conn_info.input_dbnum = config.dbnum = atoi(argv[1]);
                cliRefreshPrompt();
            } else if (!strcasecmp(command,"auth") && (argc == 2 || argc == 3)) {
                cliSelect();
            } else if (!strcasecmp(command,"multi") && argc == 1 &&
                config.last_cmd_type != REDIS_REPLY_ERROR) 
            {
                config.in_multi = 1;
                config.pre_multi_dbnum = config.dbnum;
                cliRefreshPrompt();
            } else if (!strcasecmp(command,"exec") && argc == 1 && config.in_multi) {
                config.in_multi = 0;
                if (config.last_cmd_type == REDIS_REPLY_ERROR ||
                    config.last_cmd_type == REDIS_REPLY_NIL)
                {
                    config.conn_info.input_dbnum = config.dbnum = config.pre_multi_dbnum;
                }
                cliRefreshPrompt();
            } else if (!strcasecmp(command,"discard") && argc == 1 && 
                config.last_cmd_type != REDIS_REPLY_ERROR) 
            {
                config.in_multi = 0;
                config.conn_info.input_dbnum = config.dbnum = config.pre_multi_dbnum;
                cliRefreshPrompt();
            } else if (!strcasecmp(command,"reset") && argc == 1 &&
                                     config.last_cmd_type != REDIS_REPLY_ERROR) {
                config.in_multi = 0;
                config.dbnum = 0;
                config.conn_info.input_dbnum = 0;
                config.current_resp3 = 0;
                if (config.pubsub_mode && config.push_output) {
                    redisSetPushCallback(context, cliPushHandler);
                }
                config.pubsub_mode = 0;
                cliRefreshPrompt();
            } else if (!strcasecmp(command,"hello")) {
                if (config.last_cmd_type == REDIS_REPLY_MAP) {
                    config.current_resp3 = 1;
                } else if (config.last_cmd_type == REDIS_REPLY_ARRAY) {
                    config.current_resp3 = 0;
                }
            } else if ((is_subscribe || is_unsubscribe) && !config.pubsub_mode) {
                /* We didn't enter pubsub mode. Restore push callback. */
                if (config.push_output)
                    redisSetPushCallback(context, cliPushHandler);
            }

            break;
        }
        if (config.cluster_reissue_command){
            /* If we need to reissue the command, break to prevent a
               further 'repeat' number of dud interactions */
            break;
        }
        if (config.interval) usleep(config.interval);
        fflush(stdout); /* Make it grep friendly */
    }

    zfree(argvlen);
    return REDIS_OK;
}

/* Send a command reconnecting the link if needed. */
static redisReply *reconnectingRedisCommand(redisContext *c, const char *fmt, ...) {
    redisReply *reply = NULL;
    int tries = 0;
    va_list ap;

    assert(!c->err);
    while(reply == NULL) {
        while (c->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            printf("\r\x1b[0K"); /* Cursor to left edge + clear line. */
            printf("Reconnecting... %d\r", ++tries);
            fflush(stdout);

            redisFree(c);
            c = redisConnectWrapper(config.conn_info.hostip, config.conn_info.hostport,
                                    config.connect_timeout);
            if (!c->err && config.tls) {
                const char *err = NULL;
                if (cliSecureConnection(c, config.sslconfig, &err) == REDIS_ERR && err) {
                    fprintf(stderr, "TLS Error: %s\n", err);
                    exit(1);
                }
            }
            usleep(1000000);
        }

        va_start(ap,fmt);
        reply = redisvCommand(c,fmt,ap);
        va_end(ap);

        if (c->err && !(c->err & (REDIS_ERR_IO | REDIS_ERR_EOF))) {
            fprintf(stderr, "Error: %s\n", c->errstr);
            exit(1);
        } else if (tries > 0) {
            printf("\r\x1b[0K"); /* Cursor to left edge + clear line. */
        }
    }

    context = c;
    return reply;
}

/*------------------------------------------------------------------------------
 * User interface
 *--------------------------------------------------------------------------- */

static int parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;

        if (!strcmp(argv[i],"-h") && !lastarg) {
            sdsfree(config.conn_info.hostip);
            config.conn_info.hostip = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-h") && lastarg) {
            usage(0);
        } else if (!strcmp(argv[i],"--help")) {
            usage(0);
        } else if (!strcmp(argv[i],"-x")) {
            config.stdin_lastarg = 1;
        } else if (!strcmp(argv[i], "-X") && !lastarg) {
            config.stdin_tag_arg = 1;
            config.stdin_tag_name = argv[++i];
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.conn_info.hostport = atoi(argv[++i]);
            if (config.conn_info.hostport < 0 || config.conn_info.hostport > 65535) {
                fprintf(stderr, "Invalid server port.\n");
                exit(1);
            }
        } else if (!strcmp(argv[i],"-t") && !lastarg) {
            char *eptr;
            double seconds = strtod(argv[++i], &eptr);
            if (eptr[0] != '\0' || isnan(seconds) || seconds < 0.0) {
                fprintf(stderr, "Invalid connection timeout for -t.\n");
                exit(1);
            }
            config.connect_timeout.tv_sec = (long long)seconds;
            config.connect_timeout.tv_usec = ((long long)(seconds * 1000000)) % 1000000;
        } else if (!strcmp(argv[i],"-s") && !lastarg) {
            config.hostsocket = argv[++i];
        } else if (!strcmp(argv[i],"-r") && !lastarg) {
            config.repeat = strtoll(argv[++i],NULL,10);
        } else if (!strcmp(argv[i],"-i") && !lastarg) {
            double seconds = atof(argv[++i]);
            config.interval = seconds*1000000;
        } else if (!strcmp(argv[i],"-n") && !lastarg) {
            config.conn_info.input_dbnum = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-auth-warning")) {
            config.no_auth_warning = 1;
        } else if (!strcmp(argv[i], "--askpass")) {
            config.askpass = 1;
        } else if ((!strcmp(argv[i],"-a") || !strcmp(argv[i],"--pass"))
                   && !lastarg)
        {
            config.conn_info.auth = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"--user") && !lastarg) {
            config.conn_info.user = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-u") && !lastarg) {
            parseRedisUri(argv[++i],"redis-cli",&config.conn_info,&config.tls);
            if (config.conn_info.hostport < 0 || config.conn_info.hostport > 65535) {
                fprintf(stderr, "Invalid server port.\n");
                exit(1);
            }
        } else if (!strcmp(argv[i],"--raw")) {
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"--no-raw")) {
            config.output = OUTPUT_STANDARD;
        } else if (!strcmp(argv[i],"--quoted-input")) {
            config.quoted_input = 1;
        } else if (!strcmp(argv[i],"--csv")) {
            config.output = OUTPUT_CSV;
        } else if (!strcmp(argv[i],"--json")) {
            /* Not overwrite explicit value by -3 */
            if (config.resp3 == 0) {
                config.resp3 = 2;
            }
            config.output = OUTPUT_JSON;
        } else if (!strcmp(argv[i],"--quoted-json")) {
            /* Not overwrite explicit value by -3*/
            if (config.resp3 == 0) {
                config.resp3 = 2;
            }
            config.output = OUTPUT_QUOTED_JSON;
        } else if (!strcmp(argv[i],"--latency")) {
            config.latency_mode = 1;
        } else if (!strcmp(argv[i],"--latency-dist")) {
            config.latency_dist_mode = 1;
        } else if (!strcmp(argv[i],"--mono")) {
            spectrum_palette = spectrum_palette_mono;
            spectrum_palette_size = spectrum_palette_mono_size;
        } else if (!strcmp(argv[i],"--latency-history")) {
            config.latency_mode = 1;
            config.latency_history = 1;
        } else if (!strcmp(argv[i],"--lru-test") && !lastarg) {
            config.lru_test_mode = 1;
            config.lru_test_sample_size = strtoll(argv[++i],NULL,10);
        } else if (!strcmp(argv[i],"--slave")) {
            config.slave_mode = 1;
        } else if (!strcmp(argv[i],"--replica")) {
            config.slave_mode = 1;
        } else if (!strcmp(argv[i],"--stat")) {
            config.stat_mode = 1;
        } else if (!strcmp(argv[i],"--scan")) {
            config.scan_mode = 1;
        } else if (!strcmp(argv[i],"--pattern") && !lastarg) {
            sdsfree(config.pattern);
            config.pattern = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"--count") && !lastarg) {
            config.count = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--quoted-pattern") && !lastarg) {
            sdsfree(config.pattern);
            config.pattern = unquoteCString(argv[++i]);
            if (!config.pattern) {
                fprintf(stderr,"Invalid quoted string specified for --quoted-pattern.\n");
                exit(1);
            }
        } else if (!strcmp(argv[i],"--intrinsic-latency") && !lastarg) {
            config.intrinsic_latency_mode = 1;
            config.intrinsic_latency_duration = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--rdb") && !lastarg) {
            config.getrdb_mode = 1;
            config.rdb_filename = argv[++i];
        } else if (!strcmp(argv[i],"--functions-rdb") && !lastarg) {
            config.get_functions_rdb_mode = 1;
            config.rdb_filename = argv[++i];
        } else if (!strcmp(argv[i],"--pipe")) {
            config.pipe_mode = 1;
        } else if (!strcmp(argv[i],"--pipe-timeout") && !lastarg) {
            config.pipe_timeout = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--bigkeys")) {
            config.bigkeys = 1;
        } else if (!strcmp(argv[i],"--memkeys")) {
            config.memkeys = 1;
            config.memkeys_samples = 0; /* use redis default */
        } else if (!strcmp(argv[i],"--memkeys-samples") && !lastarg) {
            config.memkeys = 1;
            config.memkeys_samples = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--hotkeys")) {
            config.hotkeys = 1;
        } else if (!strcmp(argv[i],"--eval") && !lastarg) {
            config.eval = argv[++i];
        } else if (!strcmp(argv[i],"--ldb")) {
            config.eval_ldb = 1;
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"--ldb-sync-mode")) {
            config.eval_ldb = 1;
            config.eval_ldb_sync = 1;
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"-c")) {
            config.cluster_mode = 1;
        } else if (!strcmp(argv[i],"-d") && !lastarg) {
            sdsfree(config.mb_delim);
            config.mb_delim = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-D") && !lastarg) {
            sdsfree(config.cmd_delim);
            config.cmd_delim = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-e")) {
            config.set_errcode = 1;
        } else if (!strcmp(argv[i],"--verbose")) {
            config.verbose = 1;
        } else if (!strcmp(argv[i],"-4")) {
            config.prefer_ipv4 = 1;
        } else if (!strcmp(argv[i],"-6")) {
            config.prefer_ipv6 = 1;
        } else if (!strcmp(argv[i],"--cluster") && !lastarg) {
            if (CLUSTER_MANAGER_MODE()) usage(1);
            char *cmd = argv[++i];
            int j = i;
            while (j < argc && argv[j][0] != '-') j++;
            if (j > i) j--;
            int err = createClusterManagerCommand(cmd, j - i, argv + i + 1);
            if (err) exit(err);
            i = j;
        } else if (!strcmp(argv[i],"--cluster") && lastarg) {
            usage(1);
        } else if ((!strcmp(argv[i],"--cluster-only-masters"))) {
            config.cluster_manager_command.flags |=
                    CLUSTER_MANAGER_CMD_FLAG_MASTERS_ONLY;
        } else if ((!strcmp(argv[i],"--cluster-only-replicas"))) {
            config.cluster_manager_command.flags |=
                    CLUSTER_MANAGER_CMD_FLAG_SLAVES_ONLY;
        } else if (!strcmp(argv[i],"--cluster-replicas") && !lastarg) {
            config.cluster_manager_command.replicas = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-master-id") && !lastarg) {
            config.cluster_manager_command.master_id = argv[++i];
        } else if (!strcmp(argv[i],"--cluster-from") && !lastarg) {
            config.cluster_manager_command.from = argv[++i];
        } else if (!strcmp(argv[i],"--cluster-to") && !lastarg) {
            config.cluster_manager_command.to = argv[++i];
        } else if (!strcmp(argv[i],"--cluster-from-user") && !lastarg) {
            config.cluster_manager_command.from_user = argv[++i];
        } else if (!strcmp(argv[i],"--cluster-from-pass") && !lastarg) {
            config.cluster_manager_command.from_pass = argv[++i];
        } else if (!strcmp(argv[i], "--cluster-from-askpass")) {
            config.cluster_manager_command.from_askpass = 1;
        } else if (!strcmp(argv[i],"--cluster-weight") && !lastarg) {
            if (config.cluster_manager_command.weight != NULL) {
                fprintf(stderr, "WARNING: you cannot use --cluster-weight "
                                "more than once.\n"
                                "You can set more weights by adding them "
                                "as a space-separated list, ie:\n"
                                "--cluster-weight n1=w n2=w\n");
                exit(1);
            }
            int widx = i + 1;
            char **weight = argv + widx;
            int wargc = 0;
            for (; widx < argc; widx++) {
                if (strstr(argv[widx], "--") == argv[widx]) break;
                if (strchr(argv[widx], '=') == NULL) break;
                wargc++;
            }
            if (wargc > 0) {
                config.cluster_manager_command.weight = weight;
                config.cluster_manager_command.weight_argc = wargc;
                i += wargc;
            }
        } else if (!strcmp(argv[i],"--cluster-slots") && !lastarg) {
            config.cluster_manager_command.slots = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-timeout") && !lastarg) {
            config.cluster_manager_command.timeout = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-pipeline") && !lastarg) {
            config.cluster_manager_command.pipeline = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-threshold") && !lastarg) {
            config.cluster_manager_command.threshold = atof(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-yes")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_YES;
        } else if (!strcmp(argv[i],"--cluster-simulate")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_SIMULATE;
        } else if (!strcmp(argv[i],"--cluster-replace")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_REPLACE;
        } else if (!strcmp(argv[i],"--cluster-copy")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_COPY;
        } else if (!strcmp(argv[i],"--cluster-slave")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_SLAVE;
        } else if (!strcmp(argv[i],"--cluster-use-empty-masters")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_EMPTYMASTER;
        } else if (!strcmp(argv[i],"--cluster-search-multiple-owners")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_CHECK_OWNERS;
        } else if (!strcmp(argv[i],"--cluster-fix-with-unreachable-masters")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_FIX_WITH_UNREACHABLE_MASTERS;
        } else if (!strcmp(argv[i],"--test_hint") && !lastarg) {
            config.test_hint = argv[++i];
        } else if (!strcmp(argv[i],"--test_hint_file") && !lastarg) {
            config.test_hint_file = argv[++i];
#ifdef USE_OPENSSL
        } else if (!strcmp(argv[i],"--tls")) {
            config.tls = 1;
        } else if (!strcmp(argv[i],"--sni") && !lastarg) {
            config.sslconfig.sni = argv[++i];
        } else if (!strcmp(argv[i],"--cacertdir") && !lastarg) {
            config.sslconfig.cacertdir = argv[++i];
        } else if (!strcmp(argv[i],"--cacert") && !lastarg) {
            config.sslconfig.cacert = argv[++i];
        } else if (!strcmp(argv[i],"--cert") && !lastarg) {
            config.sslconfig.cert = argv[++i];
        } else if (!strcmp(argv[i],"--key") && !lastarg) {
            config.sslconfig.key = argv[++i];
        } else if (!strcmp(argv[i],"--tls-ciphers") && !lastarg) {
            config.sslconfig.ciphers = argv[++i];
        } else if (!strcmp(argv[i],"--insecure")) {
            config.sslconfig.skip_cert_verify = 1;
        #ifdef TLS1_3_VERSION
        } else if (!strcmp(argv[i],"--tls-ciphersuites") && !lastarg) {
            config.sslconfig.ciphersuites = argv[++i];
        #endif
#endif
        } else if (!strcmp(argv[i],"-v") || !strcmp(argv[i], "--version")) {
            sds version = cliVersion();
            printf("redis-cli %s\n", version);
            sdsfree(version);
            exit(0);
        } else if (!strcmp(argv[i],"-2")) {
            config.resp2 = 1;
        } else if (!strcmp(argv[i],"-3")) {
            config.resp3 = 1;
        } else if (!strcmp(argv[i],"--show-pushes") && !lastarg) {
            char *argval = argv[++i];
            if (!strncasecmp(argval, "n", 1)) {
                config.push_output = 0;
            } else if (!strncasecmp(argval, "y", 1)) {
                config.push_output = 1;
            } else {
                fprintf(stderr, "Unknown --show-pushes value '%s' "
                        "(valid: '[y]es', '[n]o')\n", argval);
            }
        } else if (CLUSTER_MANAGER_MODE() && argv[i][0] != '-') {
            if (config.cluster_manager_command.argc == 0) {
                int j = i + 1;
                while (j < argc && argv[j][0] != '-') j++;
                int cmd_argc = j - i;
                config.cluster_manager_command.argc = cmd_argc;
                config.cluster_manager_command.argv = argv + i;
                if (cmd_argc > 1) i = j - 1;
            }
        } else {
            if (argv[i][0] == '-') {
                fprintf(stderr,
                    "Unrecognized option or bad number of args for: '%s'\n",
                    argv[i]);
                exit(1);
            } else {
                /* Likely the command name, stop here. */
                break;
            }
        }
    }

    if (config.hostsocket && config.cluster_mode) {
        fprintf(stderr,"Options -c and -s are mutually exclusive.\n");
        exit(1);
    }

    if (config.resp2 && config.resp3 == 1) {
        fprintf(stderr,"Options -2 and -3 are mutually exclusive.\n");
        exit(1);
    }

    /* --ldb requires --eval. */
    if (config.eval_ldb && config.eval == NULL) {
        fprintf(stderr,"Options --ldb and --ldb-sync-mode require --eval.\n");
        fprintf(stderr,"Try %s --help for more information.\n", argv[0]);
        exit(1);
    }

    if (!config.no_auth_warning && config.conn_info.auth != NULL) {
        fputs("Warning: Using a password with '-a' or '-u' option on the command"
              " line interface may not be safe.\n", stderr);
    }

    if (config.get_functions_rdb_mode && config.getrdb_mode) {
        fprintf(stderr,"Option --functions-rdb and --rdb are mutually exclusive.\n");
        exit(1);
    }
 
    if (config.stdin_lastarg && config.stdin_tag_arg) {
        fprintf(stderr, "Options -x and -X are mutually exclusive.\n");
        exit(1);
    }

    if (config.prefer_ipv4 && config.prefer_ipv6) {
        fprintf(stderr, "Options -4 and -6 are mutually exclusive.\n");
        exit(1);
    }

    return i;
}

static void parseEnv(void) {
    /* Set auth from env, but do not overwrite CLI arguments if passed */
    char *auth = getenv(REDIS_CLI_AUTH_ENV);
    if (auth != NULL && config.conn_info.auth == NULL) {
        config.conn_info.auth = auth;
    }

    char *cluster_yes = getenv(REDIS_CLI_CLUSTER_YES_ENV);
    if (cluster_yes != NULL && !strcmp(cluster_yes, "1")) {
        config.cluster_manager_command.flags |= CLUSTER_MANAGER_CMD_FLAG_YES;
    }
}

static void usage(int err) {
    sds version = cliVersion();
    FILE *target = err ? stderr: stdout;
    const char *tls_usage =
#ifdef USE_OPENSSL
"  --tls              Establish a secure TLS connection.\n"
"  --sni <host>       Server name indication for TLS.\n"
"  --cacert <file>    CA Certificate file to verify with.\n"
"  --cacertdir <dir>  Directory where trusted CA certificates are stored.\n"
"                     If neither cacert nor cacertdir are specified, the default\n"
"                     system-wide trusted root certs configuration will apply.\n"
"  --insecure         Allow insecure TLS connection by skipping cert validation.\n"
"  --cert <file>      Client certificate to authenticate with.\n"
"  --key <file>       Private key file to authenticate with.\n"
"  --tls-ciphers <list> Sets the list of preferred ciphers (TLSv1.2 and below)\n"
"                     in order of preference from highest to lowest separated by colon (\":\").\n"
"                     See the ciphers(1ssl) manpage for more information about the syntax of this string.\n"
#ifdef TLS1_3_VERSION
"  --tls-ciphersuites <list> Sets the list of preferred ciphersuites (TLSv1.3)\n"
"                     in order of preference from highest to lowest separated by colon (\":\").\n"
"                     See the ciphers(1ssl) manpage for more information about the syntax of this string,\n"
"                     and specifically for TLSv1.3 ciphersuites.\n"
#endif
#endif
"";

    fprintf(target,
"redis-cli %s\n"
"\n"
"Usage: redis-cli [OPTIONS] [cmd [arg [arg ...]]]\n"
"  -h <hostname>      Server hostname (default: 127.0.0.1).\n"
"  -p <port>          Server port (default: 6379).\n"
"  -t <timeout>       Server connection timeout in seconds (decimals allowed).\n"
"                     Default timeout is 0, meaning no limit, depending on the OS.\n"
"  -s <socket>        Server socket (overrides hostname and port).\n"
"  -a <password>      Password to use when connecting to the server.\n"
"                     You can also use the " REDIS_CLI_AUTH_ENV " environment\n"
"                     variable to pass this password more safely\n"
"                     (if both are used, this argument takes precedence).\n"
"  --user <username>  Used to send ACL style 'AUTH username pass'. Needs -a.\n"
"  --pass <password>  Alias of -a for consistency with the new --user option.\n"
"  --askpass          Force user to input password with mask from STDIN.\n"
"                     If this argument is used, '-a' and " REDIS_CLI_AUTH_ENV "\n"
"                     environment variable will be ignored.\n"
"  -u <uri>           Server URI on format redis://user:password@host:port/dbnum\n"
"                     User, password and dbnum are optional. For authentication\n"
"                     without a username, use username 'default'. For TLS, use\n"
"                     the scheme 'rediss'.\n"
"  -r <repeat>        Execute specified command N times.\n"
"  -i <interval>      When -r is used, waits <interval> seconds per command.\n"
"                     It is possible to specify sub-second times like -i 0.1.\n"
"                     This interval is also used in --scan and --stat per cycle.\n"
"                     and in --bigkeys, --memkeys, and --hotkeys per 100 cycles.\n"
"  -n <db>            Database number.\n"
"  -2                 Start session in RESP2 protocol mode.\n"
"  -3                 Start session in RESP3 protocol mode.\n"
"  -x                 Read last argument from STDIN (see example below).\n"
"  -X                 Read <tag> argument from STDIN (see example below).\n"
"  -d <delimiter>     Delimiter between response bulks for raw formatting (default: \\n).\n"
"  -D <delimiter>     Delimiter between responses for raw formatting (default: \\n).\n"
"  -c                 Enable cluster mode (follow -ASK and -MOVED redirections).\n"
"  -e                 Return exit error code when command execution fails.\n"
"  -4                 Prefer IPv4 over IPv6 on DNS lookup.\n"
"  -6                 Prefer IPv6 over IPv4 on DNS lookup.\n"
"%s"
"  --raw              Use raw formatting for replies (default when STDOUT is\n"
"                     not a tty).\n"
"  --no-raw           Force formatted output even when STDOUT is not a tty.\n"
"  --quoted-input     Force input to be handled as quoted strings.\n"
"  --csv              Output in CSV format.\n"
"  --json             Output in JSON format (default RESP3, use -2 if you want to use with RESP2).\n"
"  --quoted-json      Same as --json, but produce ASCII-safe quoted strings, not Unicode.\n"
"  --show-pushes <yn> Whether to print RESP3 PUSH messages.  Enabled by default when\n"
"                     STDOUT is a tty but can be overridden with --show-pushes no.\n"
"  --stat             Print rolling stats about server: mem, clients, ...\n",
version,tls_usage);

    fprintf(target,
"  --latency          Enter a special mode continuously sampling latency.\n"
"                     If you use this mode in an interactive session it runs\n"
"                     forever displaying real-time stats. Otherwise if --raw or\n"
"                     --csv is specified, or if you redirect the output to a non\n"
"                     TTY, it samples the latency for 1 second (you can use\n"
"                     -i to change the interval), then produces a single output\n"
"                     and exits.\n"
"  --latency-history  Like --latency but tracking latency changes over time.\n"
"                     Default time interval is 15 sec. Change it using -i.\n"
"  --latency-dist     Shows latency as a spectrum, requires xterm 256 colors.\n"
"                     Default time interval is 1 sec. Change it using -i.\n"
"  --lru-test <keys>  Simulate a cache workload with an 80-20 distribution.\n"
"  --replica          Simulate a replica showing commands received from the master.\n"
"  --rdb <filename>   Transfer an RDB dump from remote server to local file.\n"
"                     Use filename of \"-\" to write to stdout.\n"
"  --functions-rdb <filename> Like --rdb but only get the functions (not the keys)\n"
"                     when getting the RDB dump file.\n"
"  --pipe             Transfer raw Redis protocol from stdin to server.\n"
"  --pipe-timeout <n> In --pipe mode, abort with error if after sending all data.\n"
"                     no reply is received within <n> seconds.\n"
"                     Default timeout: %d. Use 0 to wait forever.\n",
    REDIS_CLI_DEFAULT_PIPE_TIMEOUT);
    fprintf(target,
"  --bigkeys          Sample Redis keys looking for keys with many elements (complexity).\n"
"  --memkeys          Sample Redis keys looking for keys consuming a lot of memory.\n"
"  --memkeys-samples <n> Sample Redis keys looking for keys consuming a lot of memory.\n"
"                     And define number of key elements to sample\n"
"  --hotkeys          Sample Redis keys looking for hot keys.\n"
"                     only works when maxmemory-policy is *lfu.\n"
"  --scan             List all keys using the SCAN command.\n"
"  --pattern <pat>    Keys pattern when using the --scan, --bigkeys or --hotkeys\n"
"                     options (default: *).\n"
"  --count <count>    Count option when using the --scan, --bigkeys or --hotkeys (default: 10).\n"
"  --quoted-pattern <pat> Same as --pattern, but the specified string can be\n"
"                         quoted, in order to pass an otherwise non binary-safe string.\n"
"  --intrinsic-latency <sec> Run a test to measure intrinsic system latency.\n"
"                     The test will run for the specified amount of seconds.\n"
"  --eval <file>      Send an EVAL command using the Lua script at <file>.\n"
"  --ldb              Used with --eval enable the Redis Lua debugger.\n"
"  --ldb-sync-mode    Like --ldb but uses the synchronous Lua debugger, in\n"
"                     this mode the server is blocked and script changes are\n"
"                     not rolled back from the server memory.\n"
"  --cluster <command> [args...] [opts...]\n"
"                     Cluster Manager command and arguments (see below).\n"
"  --verbose          Verbose mode.\n"
"  --no-auth-warning  Don't show warning message when using password on command\n"
"                     line interface.\n"
"  --help             Output this help and exit.\n"
"  --version          Output version and exit.\n"
"\n");
    /* Using another fprintf call to avoid -Woverlength-strings compile warning */
    fprintf(target,
"Cluster Manager Commands:\n"
"  Use --cluster help to list all available cluster manager commands.\n"
"\n"
"Examples:\n"
"  redis-cli -u redis://default:PASSWORD@localhost:6379/0\n"
"  cat /etc/passwd | redis-cli -x set mypasswd\n"
"  redis-cli -D \"\" --raw dump key > key.dump && redis-cli -X dump_tag restore key2 0 dump_tag replace < key.dump\n"
"  redis-cli -r 100 lpush mylist x\n"
"  redis-cli -r 100 -i 1 info | grep used_memory_human:\n"
"  redis-cli --quoted-input set '\"null-\\x00-separated\"' value\n"
"  redis-cli --eval myscript.lua key1 key2 , arg1 arg2 arg3\n"
"  redis-cli --scan --pattern '*:12345*'\n"
"  redis-cli --scan --pattern '*:12345*' --count 100\n"
"\n"
"  (Note: when using --eval the comma separates KEYS[] from ARGV[] items)\n"
"\n"
"When no command is given, redis-cli starts in interactive mode.\n"
"Type \"help\" in interactive mode for information on available commands\n"
"and settings.\n"
"\n");
    sdsfree(version);
    exit(err);
}

static int confirmWithYes(char *msg, int ignore_force) {
    /* if --cluster-yes option is set and ignore_force is false,
     * do not prompt for an answer */
    if (!ignore_force &&
        (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_YES)) {
        return 1;
    }

    printf("%s (type 'yes' to accept): ", msg);
    fflush(stdout);
    char buf[4];
    int nread = read(fileno(stdin),buf,4);
    buf[3] = '\0';
    return (nread != 0 && !strcmp("yes", buf));
}

static int issueCommandRepeat(int argc, char **argv, long repeat) {
    /* In Lua debugging mode, we want to pass the "help" to Redis to get
     * it's own HELP message, rather than handle it by the CLI, see ldbRepl.
     *
     * For the normal Redis HELP, we can process it without a connection. */
    if (!config.eval_ldb &&
        (!strcasecmp(argv[0],"help") || !strcasecmp(argv[0],"?")))
    {
        cliOutputHelp(--argc, ++argv);
        return REDIS_OK;
    }

    while (1) {
        if (config.cluster_reissue_command || context == NULL ||
            context->err == REDIS_ERR_IO || context->err == REDIS_ERR_EOF)
        {
            if (cliConnect(CC_FORCE) != REDIS_OK) {
                cliPrintContextError();
                config.cluster_reissue_command = 0;
                return REDIS_ERR;
            }
        }
        config.cluster_reissue_command = 0;
        if (config.cluster_send_asking) {
            if (cliSendAsking() != REDIS_OK) {
                cliPrintContextError();
                return REDIS_ERR;
            }
        }
        if (cliSendCommand(argc,argv,repeat) != REDIS_OK) {
            cliPrintContextError();
            redisFree(context);
            context = NULL;
            return REDIS_ERR;
        }

        /* Issue the command again if we got redirected in cluster mode */
        if (config.cluster_mode && config.cluster_reissue_command) {
            continue;
        }
        break;
    }
    return REDIS_OK;
}

static int issueCommand(int argc, char **argv) {
    return issueCommandRepeat(argc, argv, config.repeat);
}

/* Split the user provided command into multiple SDS arguments.
 * This function normally uses sdssplitargs() from sds.c which is able
 * to understand "quoted strings", escapes and so forth. However when
 * we are in Lua debugging mode and the "eval" command is used, we want
 * the remaining Lua script (after "e " or "eval ") to be passed verbatim
 * as a single big argument. */
static sds *cliSplitArgs(char *line, int *argc) {
    if (config.eval_ldb && (strstr(line,"eval ") == line ||
                            strstr(line,"e ") == line))
    {
        sds *argv = sds_malloc(sizeof(sds)*2);
        *argc = 2;
        int len = strlen(line);
        int elen = line[1] == ' ' ? 2 : 5; /* "e " or "eval "? */
        argv[0] = sdsnewlen(line,elen-1);
        argv[1] = sdsnewlen(line+elen,len-elen);
        return argv;
    } else {
        return sdssplitargs(line,argc);
    }
}

/* Set the CLI preferences. This function is invoked when an interactive
 * ":command" is called, or when reading ~/.redisclirc file, in order to
 * set user preferences. */
void cliSetPreferences(char **argv, int argc, int interactive) {
    if (!strcasecmp(argv[0],":set") && argc >= 2) {
        if (!strcasecmp(argv[1],"hints")) pref.hints = 1;
        else if (!strcasecmp(argv[1],"nohints")) pref.hints = 0;
        else {
            printf("%sunknown redis-cli preference '%s'\n",
                interactive ? "" : ".redisclirc: ",
                argv[1]);
        }
    } else {
        printf("%sunknown redis-cli internal command '%s'\n",
            interactive ? "" : ".redisclirc: ",
            argv[0]);
    }
}

/* Load the ~/.redisclirc file if any. */
void cliLoadPreferences(void) {
    sds rcfile = getDotfilePath(REDIS_CLI_RCFILE_ENV,REDIS_CLI_RCFILE_DEFAULT);
    if (rcfile == NULL) return;
    FILE *fp = fopen(rcfile,"r");
    char buf[1024];

    if (fp) {
        while(fgets(buf,sizeof(buf),fp) != NULL) {
            sds *argv;
            int argc;

            argv = sdssplitargs(buf,&argc);
            if (argc > 0) cliSetPreferences(argv,argc,0);
            sdsfreesplitres(argv,argc);
        }
        fclose(fp);
    }
    sdsfree(rcfile);
}

/* Some commands can include sensitive information and shouldn't be put in the
 * history file. Currently these commands are include:
 * - AUTH
 * - ACL DELUSER, ACL SETUSER, ACL GETUSER
 * - CONFIG SET masterauth/masteruser/tls-key-file-pass/tls-client-key-file-pass/requirepass
 * - HELLO with [AUTH username password]
 * - MIGRATE with [AUTH password] or [AUTH2 username password] 
 * - SENTINEL CONFIG SET sentinel-pass password, SENTINEL CONFIG SET sentinel-user username 
 * - SENTINEL SET <mastername> auth-pass password, SENTINEL SET <mastername> auth-user username */
static int isSensitiveCommand(int argc, char **argv) {
    if (!strcasecmp(argv[0],"auth")) {
        return 1;
    } else if (argc > 1 &&
        !strcasecmp(argv[0],"acl") && (
            !strcasecmp(argv[1],"deluser") ||
            !strcasecmp(argv[1],"setuser") ||
            !strcasecmp(argv[1],"getuser")))
    {
        return 1;
    } else if (argc > 2 &&
        !strcasecmp(argv[0],"config") &&
        !strcasecmp(argv[1],"set")) {
            for (int j = 2; j < argc; j = j+2) {
                if (!strcasecmp(argv[j],"masterauth") ||
                    !strcasecmp(argv[j],"masteruser") ||
                    !strcasecmp(argv[j],"tls-key-file-pass") ||
                    !strcasecmp(argv[j],"tls-client-key-file-pass") ||
                    !strcasecmp(argv[j],"requirepass")) {
                    return 1;
                }
            }
            return 0;
    /* HELLO [protover [AUTH username password] [SETNAME clientname]] */
    } else if (argc > 4 && !strcasecmp(argv[0],"hello")) {
        for (int j = 2; j < argc; j++) {
            int moreargs = argc - 1 - j;
            if (!strcasecmp(argv[j],"AUTH") && moreargs >= 2) {
                return 1;
            } else if (!strcasecmp(argv[j],"SETNAME") && moreargs) {
                j++;
            } else {
                return 0;
            }
        }
    /* MIGRATE host port key|"" destination-db timeout [COPY] [REPLACE]
     * [AUTH password] [AUTH2 username password] [KEYS key [key ...]] */
    } else if (argc > 7 && !strcasecmp(argv[0], "migrate")) {
        for (int j = 6; j < argc; j++) {
            int moreargs = argc - 1 - j;
            if (!strcasecmp(argv[j],"auth") && moreargs) {
                return 1;
            } else if (!strcasecmp(argv[j],"auth2") && moreargs >= 2) {
                return 1;
            } else if (!strcasecmp(argv[j],"keys") && moreargs) {
                return 0;
            }
        }
    } else if (argc > 4 && !strcasecmp(argv[0], "sentinel")) {
        /* SENTINEL CONFIG SET sentinel-pass password
         * SENTINEL CONFIG SET sentinel-user username */
        if (!strcasecmp(argv[1], "config") && 
            !strcasecmp(argv[2], "set") &&
            (!strcasecmp(argv[3], "sentinel-pass") ||
             !strcasecmp(argv[3], "sentinel-user"))) 
        {
            return 1;
        }
        /* SENTINEL SET <mastername> auth-pass password 
         * SENTINEL SET <mastername> auth-user username */
        if (!strcasecmp(argv[1], "set") &&
            (!strcasecmp(argv[3], "auth-pass") || 
             !strcasecmp(argv[3], "auth-user"))) 
        {
            return 1;
        }
    }
    return 0;
}

static void repl(void) {
    sds historyfile = NULL;
    int history = 0;
    char *line;
    int argc;
    sds *argv;

    /* There is no need to initialize redis HELP when we are in lua debugger mode.
     * It has its own HELP and commands (COMMAND or COMMAND DOCS will fail and got nothing).
     * We will initialize the redis HELP after the Lua debugging session ended.*/
    if ((!config.eval_ldb) && isatty(fileno(stdin))) {
        /* Initialize the help using the results of the COMMAND command. */
        cliInitHelp();
    }

    config.interactive = 1;
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(completionCallback);
    linenoiseSetHintsCallback(hintsCallback);
    linenoiseSetFreeHintsCallback(freeHintsCallback);

    /* Only use history and load the rc file when stdin is a tty. */
    if (isatty(fileno(stdin))) {
        historyfile = getDotfilePath(REDIS_CLI_HISTFILE_ENV,REDIS_CLI_HISTFILE_DEFAULT);
        //keep in-memory history always regardless if history file can be determined
        history = 1;
        if (historyfile != NULL) {
            linenoiseHistoryLoad(historyfile);
        }
        cliLoadPreferences();
    }

    cliRefreshPrompt();
    while(1) {
        line = linenoise(context ? config.prompt : "not connected> ");
        if (line == NULL) {
            /* ^C, ^D or similar. */
            if (config.pubsub_mode) {
                config.pubsub_mode = 0;
                if (cliConnect(CC_FORCE) == REDIS_OK)
                    continue;
            }
            break;
        } else if (line[0] != '\0') {
            long repeat = 1;
            int skipargs = 0;
            char *endptr = NULL;

            argv = cliSplitArgs(line,&argc);
            if (argv == NULL) {
                printf("Invalid argument(s)\n");
                fflush(stdout);
                if (history) linenoiseHistoryAdd(line, 0);
                if (historyfile) linenoiseHistorySave(historyfile);
                linenoiseFree(line);
                continue;
            } else if (argc == 0) {
                sdsfreesplitres(argv,argc);
                linenoiseFree(line);
                continue;
            }

            /* check if we have a repeat command option and
             * need to skip the first arg */
            errno = 0;
            repeat = strtol(argv[0], &endptr, 10);
            if (argc > 1 && *endptr == '\0') {
                if (errno == ERANGE || errno == EINVAL || repeat <= 0) {
                    fputs("Invalid redis-cli repeat command option value.\n", stdout);
                    sdsfreesplitres(argv, argc);
                    linenoiseFree(line);
                    continue;
                }
                skipargs = 1;
            } else {
                repeat = 1;
            }

            /* Always keep in-memory history. But for commands with sensitive information,
             * avoid writing them to the history file. */
            int is_sensitive = isSensitiveCommand(argc - skipargs, argv + skipargs);
            if (history) linenoiseHistoryAdd(line, is_sensitive);
            if (!is_sensitive && historyfile) linenoiseHistorySave(historyfile);

            if (strcasecmp(argv[0],"quit") == 0 ||
                strcasecmp(argv[0],"exit") == 0)
            {
                exit(0);
            } else if (argv[0][0] == ':') {
                cliSetPreferences(argv,argc,1);
                sdsfreesplitres(argv,argc);
                linenoiseFree(line);
                continue;
            } else if (strcasecmp(argv[0],"restart") == 0) {
                if (config.eval) {
                    config.eval_ldb = 1;
                    config.output = OUTPUT_RAW;
                    sdsfreesplitres(argv,argc);
                    linenoiseFree(line);
                    return; /* Return to evalMode to restart the session. */
                } else {
                    printf("Use 'restart' only in Lua debugging mode.\n");
                    fflush(stdout);
                }
            } else if (argc == 3 && !strcasecmp(argv[0],"connect")) {
                sdsfree(config.conn_info.hostip);
                config.conn_info.hostip = sdsnew(argv[1]);
                config.conn_info.hostport = atoi(argv[2]);
                cliRefreshPrompt();
                cliConnect(CC_FORCE);
            } else if (argc == 1 && !strcasecmp(argv[0],"clear")) {
                linenoiseClearScreen();
            } else {
                long long start_time = mstime(), elapsed;

                issueCommandRepeat(argc-skipargs, argv+skipargs, repeat);

                /* If our debugging session ended, show the EVAL final
                    * reply. */
                if (config.eval_ldb_end) {
                    config.eval_ldb_end = 0;
                    cliReadReply(0);
                    printf("\n(Lua debugging session ended%s)\n\n",
                        config.eval_ldb_sync ? "" :
                        " -- dataset changes rolled back");
                    cliInitHelp();
                }

                elapsed = mstime()-start_time;
                if (elapsed >= 500 &&
                    config.output == OUTPUT_STANDARD)
                {
                    printf("(%.2fs)\n",(double)elapsed/1000);
                }
            }
            /* Free the argument vector */
            sdsfreesplitres(argv,argc);
        }

        if (config.pubsub_mode) {
            cliWaitForMessagesOrStdin();
        }

        /* linenoise() returns malloc-ed lines like readline() */
        linenoiseFree(line);
    }
    exit(0);
}

static int noninteractive(int argc, char **argv) {
    int retval = 0;
    sds *sds_args = getSdsArrayFromArgv(argc, argv, config.quoted_input);

    if (!sds_args) {
        printf("Invalid quoted string\n");
        return 1;
    }

    if (config.stdin_lastarg) {
        sds_args = sds_realloc(sds_args, (argc + 1) * sizeof(sds));
        sds_args[argc] = readArgFromStdin();
        argc++;
    } else if (config.stdin_tag_arg) {
        int i = 0, tag_match = 0;

        for (; i < argc; i++) {
            if (strcmp(config.stdin_tag_name, sds_args[i]) != 0) continue;

            tag_match = 1;
            sdsfree(sds_args[i]);
            sds_args[i] = readArgFromStdin();
            break;
        }

        if (!tag_match) {
            sdsfreesplitres(sds_args, argc);
            fprintf(stderr, "Using -X option but stdin tag not match.\n");
            return 1;
        }
    }

    retval = issueCommand(argc, sds_args);
    sdsfreesplitres(sds_args, argc);
    while (config.pubsub_mode) {
        if (cliReadReply(0) != REDIS_OK) {
            cliPrintContextError();
            exit(1);
        }
        fflush(stdout);
    }
    return retval == REDIS_OK ? 0 : 1;
}

/*------------------------------------------------------------------------------
 * Eval mode
 *--------------------------------------------------------------------------- */

static int evalMode(int argc, char **argv) {
    sds script = NULL;
    FILE *fp;
    char buf[1024];
    size_t nread;
    char **argv2;
    int j, got_comma, keys;
    int retval = REDIS_OK;

    while(1) {
        if (config.eval_ldb) {
            printf(
            "Lua debugging session started, please use:\n"
            "quit    -- End the session.\n"
            "restart -- Restart the script in debug mode again.\n"
            "help    -- Show Lua script debugging commands.\n\n"
            );
        }

        sdsfree(script);
        script = sdsempty();
        got_comma = 0;
        keys = 0;

        /* Load the script from the file, as an sds string. */
        fp = fopen(config.eval,"r");
        if (!fp) {
            fprintf(stderr,
                "Can't open file '%s': %s\n", config.eval, strerror(errno));
            exit(1);
        }
        while((nread = fread(buf,1,sizeof(buf),fp)) != 0) {
            script = sdscatlen(script,buf,nread);
        }
        fclose(fp);

        /* If we are debugging a script, enable the Lua debugger. */
        if (config.eval_ldb) {
            redisReply *reply = redisCommand(context,
                    config.eval_ldb_sync ?
                    "SCRIPT DEBUG sync": "SCRIPT DEBUG yes");
            if (reply) freeReplyObject(reply);
        }

        /* Create our argument vector */
        argv2 = zmalloc(sizeof(sds)*(argc+3));
        argv2[0] = sdsnew("EVAL");
        argv2[1] = script;
        for (j = 0; j < argc; j++) {
            if (!got_comma && argv[j][0] == ',' && argv[j][1] == 0) {
                got_comma = 1;
                continue;
            }
            argv2[j+3-got_comma] = sdsnew(argv[j]);
            if (!got_comma) keys++;
        }
        argv2[2] = sdscatprintf(sdsempty(),"%d",keys);

        /* Call it */
        int eval_ldb = config.eval_ldb; /* Save it, may be reverted. */
        retval = issueCommand(argc+3-got_comma, argv2);
        if (eval_ldb) {
            if (!config.eval_ldb) {
                /* If the debugging session ended immediately, there was an
                 * error compiling the script. Show it and they don't enter
                 * the REPL at all. */
                printf("Eval debugging session can't start:\n");
                cliReadReply(0);
                break; /* Return to the caller. */
            } else {
                strncpy(config.prompt,"lua debugger> ",sizeof(config.prompt));
                repl();
                /* Restart the session if repl() returned. */
                cliConnect(CC_FORCE);
                printf("\n");
            }
        } else {
            break; /* Return to the caller. */
        }
    }
    return retval == REDIS_OK ? 0 : 1;
}

/*------------------------------------------------------------------------------
 * Cluster Manager
 *--------------------------------------------------------------------------- */

/* The Cluster Manager global structure */
static struct clusterManager {
    list *nodes;    /* List of nodes in the configuration. */
    list *errors;
    int unreachable_masters;    /* Masters we are not able to reach. */
} cluster_manager;

/* Used by clusterManagerFixSlotsCoverage */
dict *clusterManagerUncoveredSlots = NULL;

typedef struct clusterManagerNode {
    redisContext *context;
    sds name;
    char *ip;
    int port;
    int bus_port; /* cluster-port */
    uint64_t current_epoch;
    time_t ping_sent;
    time_t ping_recv;
    int flags;
    list *flags_str; /* Flags string representations */
    sds replicate;  /* Master ID if node is a slave */
    int dirty;      /* Node has changes that can be flushed */
    uint8_t slots[CLUSTER_MANAGER_SLOTS];
    int slots_count;
    int replicas_count;
    list *friends;
    sds *migrating; /* An array of sds where even strings are slots and odd
                     * strings are the destination node IDs. */
    sds *importing; /* An array of sds where even strings are slots and odd
                     * strings are the source node IDs. */
    int migrating_count; /* Length of the migrating array (migrating slots*2) */
    int importing_count; /* Length of the importing array (importing slots*2) */
    float weight;   /* Weight used by rebalance */
    int balance;    /* Used by rebalance */
} clusterManagerNode;

/* Data structure used to represent a sequence of cluster nodes. */
typedef struct clusterManagerNodeArray {
    clusterManagerNode **nodes; /* Actual nodes array */
    clusterManagerNode **alloc; /* Pointer to the allocated memory */
    int len;                    /* Actual length of the array */
    int count;                  /* Non-NULL nodes count */
} clusterManagerNodeArray;

/* Used for the reshard table. */
typedef struct clusterManagerReshardTableItem {
    clusterManagerNode *source;
    int slot;
} clusterManagerReshardTableItem;

/* Info about a cluster internal link. */

typedef struct clusterManagerLink {
    sds node_name;
    sds node_addr;
    int connected;
    int handshaking;
} clusterManagerLink;

static dictType clusterManagerDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    dictSdsDestructor,         /* val destructor */
    NULL                       /* allow to expand */
};

static dictType clusterManagerLinkDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    dictSdsDestructor,         /* key destructor */
    dictListDestructor,        /* val destructor */
    NULL                       /* allow to expand */
};

typedef int clusterManagerCommandProc(int argc, char **argv);
typedef int (*clusterManagerOnReplyError)(redisReply *reply,
    clusterManagerNode *n, int bulk_idx);

/* Cluster Manager helper functions */

static clusterManagerNode *clusterManagerNewNode(char *ip, int port, int bus_port);
static clusterManagerNode *clusterManagerNodeByName(const char *name);
static clusterManagerNode *clusterManagerNodeByAbbreviatedName(const char *n);
static void clusterManagerNodeResetSlots(clusterManagerNode *node);
static int clusterManagerNodeIsCluster(clusterManagerNode *node, char **err);
static void clusterManagerPrintNotClusterNodeError(clusterManagerNode *node,
                                                   char *err);
static int clusterManagerNodeLoadInfo(clusterManagerNode *node, int opts,
                                      char **err);
static int clusterManagerLoadInfoFromNode(clusterManagerNode *node);
static int clusterManagerNodeIsEmpty(clusterManagerNode *node, char **err);
static int clusterManagerGetAntiAffinityScore(clusterManagerNodeArray *ipnodes,
    int ip_count, clusterManagerNode ***offending, int *offending_len);
static void clusterManagerOptimizeAntiAffinity(clusterManagerNodeArray *ipnodes,
    int ip_count);
static sds clusterManagerNodeInfo(clusterManagerNode *node, int indent);
static void clusterManagerShowNodes(void);
static void clusterManagerShowClusterInfo(void);
static int clusterManagerFlushNodeConfig(clusterManagerNode *node, char **err);
static void clusterManagerWaitForClusterJoin(void);
static int clusterManagerCheckCluster(int quiet);
static void clusterManagerLog(int level, const char* fmt, ...);
static int clusterManagerIsConfigConsistent(void);
static dict *clusterManagerGetLinkStatus(void);
static void clusterManagerOnError(sds err);
static void clusterManagerNodeArrayInit(clusterManagerNodeArray *array,
                                        int len);
static void clusterManagerNodeArrayReset(clusterManagerNodeArray *array);
static void clusterManagerNodeArrayShift(clusterManagerNodeArray *array,
                                         clusterManagerNode **nodeptr);
static void clusterManagerNodeArrayAdd(clusterManagerNodeArray *array,
                                       clusterManagerNode *node);

/* Cluster Manager commands. */

static int clusterManagerCommandCreate(int argc, char **argv);
static int clusterManagerCommandAddNode(int argc, char **argv);
static int clusterManagerCommandDeleteNode(int argc, char **argv);
static int clusterManagerCommandInfo(int argc, char **argv);
static int clusterManagerCommandCheck(int argc, char **argv);
static int clusterManagerCommandFix(int argc, char **argv);
static int clusterManagerCommandReshard(int argc, char **argv);
static int clusterManagerCommandRebalance(int argc, char **argv);
static int clusterManagerCommandSetTimeout(int argc, char **argv);
static int clusterManagerCommandImport(int argc, char **argv);
static int clusterManagerCommandCall(int argc, char **argv);
static int clusterManagerCommandHelp(int argc, char **argv);
static int clusterManagerCommandBackup(int argc, char **argv);

typedef struct clusterManagerCommandDef {
    char *name;
    clusterManagerCommandProc *proc;
    int arity;
    char *args;
    char *options;
} clusterManagerCommandDef;

clusterManagerCommandDef clusterManagerCommands[] = {
    {"create", clusterManagerCommandCreate, -1, "host1:port1 ... hostN:portN",
     "replicas <arg>"},
    {"check", clusterManagerCommandCheck, -1, "<host:port> or <host> <port> - separated by either colon or space",
     "search-multiple-owners"},
    {"info", clusterManagerCommandInfo, -1, "<host:port> or <host> <port> - separated by either colon or space", NULL},
    {"fix", clusterManagerCommandFix, -1, "<host:port> or <host> <port> - separated by either colon or space",
     "search-multiple-owners,fix-with-unreachable-masters"},
    {"reshard", clusterManagerCommandReshard, -1, "<host:port> or <host> <port> - separated by either colon or space",
     "from <arg>,to <arg>,slots <arg>,yes,timeout <arg>,pipeline <arg>,"
     "replace"},
    {"rebalance", clusterManagerCommandRebalance, -1, "<host:port> or <host> <port> - separated by either colon or space",
     "weight <node1=w1...nodeN=wN>,use-empty-masters,"
     "timeout <arg>,simulate,pipeline <arg>,threshold <arg>,replace"},
    {"add-node", clusterManagerCommandAddNode, 2,
     "new_host:new_port existing_host:existing_port", "slave,master-id <arg>"},
    {"del-node", clusterManagerCommandDeleteNode, 2, "host:port node_id",NULL},
    {"call", clusterManagerCommandCall, -2,
        "host:port command arg arg .. arg", "only-masters,only-replicas"},
    {"set-timeout", clusterManagerCommandSetTimeout, 2,
     "host:port milliseconds", NULL},
    {"import", clusterManagerCommandImport, 1, "host:port",
     "from <arg>,from-user <arg>,from-pass <arg>,from-askpass,copy,replace"},
    {"backup", clusterManagerCommandBackup, 2,  "host:port backup_directory",
     NULL},
    {"help", clusterManagerCommandHelp, 0, NULL, NULL}
};

typedef struct clusterManagerOptionDef {
    char *name;
    char *desc;
} clusterManagerOptionDef;

clusterManagerOptionDef clusterManagerOptions[] = {
    {"--cluster-yes", "Automatic yes to cluster commands prompts"}
};

static void getRDB(clusterManagerNode *node);

static int createClusterManagerCommand(char *cmdname, int argc, char **argv) {
    clusterManagerCommand *cmd = &config.cluster_manager_command;
    cmd->name = cmdname;
    cmd->argc = argc;
    cmd->argv = argc ? argv : NULL;
    if (isColorTerm()) cmd->flags |= CLUSTER_MANAGER_CMD_FLAG_COLOR;

    if (config.stdin_lastarg) {
        char **new_argv = zmalloc(sizeof(char*) * (cmd->argc+1));
        memcpy(new_argv, cmd->argv, sizeof(char*) * cmd->argc);

        cmd->stdin_arg = readArgFromStdin();
        new_argv[cmd->argc++] = cmd->stdin_arg;
        cmd->argv = new_argv;
    } else if (config.stdin_tag_arg) {
        int i = 0, tag_match = 0;
        cmd->stdin_arg = readArgFromStdin();

        for (; i < argc; i++) {
            if (strcmp(argv[i], config.stdin_tag_name) != 0) continue;

            tag_match = 1;
            cmd->argv[i] = (char *)cmd->stdin_arg;
            break;
        }

        if (!tag_match) {
            sdsfree(cmd->stdin_arg);
            fprintf(stderr, "Using -X option but stdin tag not match.\n");
            return 1;
        }
    }

    return 0;
}

static clusterManagerCommandProc *validateClusterManagerCommand(void) {
    int i, commands_count = sizeof(clusterManagerCommands) /
                            sizeof(clusterManagerCommandDef);
    clusterManagerCommandProc *proc = NULL;
    char *cmdname = config.cluster_manager_command.name;
    int argc = config.cluster_manager_command.argc;
    for (i = 0; i < commands_count; i++) {
        clusterManagerCommandDef cmddef = clusterManagerCommands[i];
        if (!strcmp(cmddef.name, cmdname)) {
            if ((cmddef.arity > 0 && argc != cmddef.arity) ||
                (cmddef.arity < 0 && argc < (cmddef.arity * -1))) {
                fprintf(stderr, "[ERR] Wrong number of arguments for "
                                "specified --cluster sub command\n");
                return NULL;
            }
            proc = cmddef.proc;
        }
    }
    if (!proc) fprintf(stderr, "Unknown --cluster subcommand\n");
    return proc;
}

static int parseClusterNodeAddress(char *addr, char **ip_ptr, int *port_ptr,
                                   int *bus_port_ptr)
{
    /* ip:port[@bus_port] */
    char *c = strrchr(addr, '@');
    if (c != NULL) {
        *c = '\0';
        if (bus_port_ptr != NULL)
            *bus_port_ptr = atoi(c + 1);
    }
    c = strrchr(addr, ':');
    if (c != NULL) {
        *c = '\0';
        *ip_ptr = addr;
        *port_ptr = atoi(++c);
    } else return 0;
    return 1;
}

/* Get host ip and port from command arguments. If only one argument has
 * been provided it must be in the form of 'ip:port', elsewhere
 * the first argument must be the ip and the second one the port.
 * If host and port can be detected, it returns 1 and it stores host and
 * port into variables referenced by 'ip_ptr' and 'port_ptr' pointers,
 * elsewhere it returns 0. */
static int getClusterHostFromCmdArgs(int argc, char **argv,
                                     char **ip_ptr, int *port_ptr) {
    int port = 0;
    char *ip = NULL;
    if (argc == 1) {
        char *addr = argv[0];
        if (!parseClusterNodeAddress(addr, &ip, &port, NULL)) return 0;
    } else {
        ip = argv[0];
        port = atoi(argv[1]);
    }
    if (!ip || !port) return 0;
    else {
        *ip_ptr = ip;
        *port_ptr = port;
    }
    return 1;
}

static void freeClusterManagerNodeFlags(list *flags) {
    listIter li;
    listNode *ln;
    listRewind(flags, &li);
    while ((ln = listNext(&li)) != NULL) {
        sds flag = ln->value;
        sdsfree(flag);
    }
    listRelease(flags);
}

static void freeClusterManagerNode(clusterManagerNode *node) {
    if (node->context != NULL) redisFree(node->context);
    if (node->friends != NULL) {
        listIter li;
        listNode *ln;
        listRewind(node->friends,&li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *fn = ln->value;
            freeClusterManagerNode(fn);
        }
        listRelease(node->friends);
        node->friends = NULL;
    }
    if (node->name != NULL) sdsfree(node->name);
    if (node->replicate != NULL) sdsfree(node->replicate);
    if ((node->flags & CLUSTER_MANAGER_FLAG_FRIEND) && node->ip)
        sdsfree(node->ip);
    int i;
    if (node->migrating != NULL) {
        for (i = 0; i < node->migrating_count; i++) sdsfree(node->migrating[i]);
        zfree(node->migrating);
    }
    if (node->importing != NULL) {
        for (i = 0; i < node->importing_count; i++) sdsfree(node->importing[i]);
        zfree(node->importing);
    }
    if (node->flags_str != NULL) {
        freeClusterManagerNodeFlags(node->flags_str);
        node->flags_str = NULL;
    }
    zfree(node);
}

static void freeClusterManager(void) {
    listIter li;
    listNode *ln;
    if (cluster_manager.nodes != NULL) {
        listRewind(cluster_manager.nodes,&li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            freeClusterManagerNode(n);
        }
        listRelease(cluster_manager.nodes);
        cluster_manager.nodes = NULL;
    }
    if (cluster_manager.errors != NULL) {
        listRewind(cluster_manager.errors,&li);
        while ((ln = listNext(&li)) != NULL) {
            sds err = ln->value;
            sdsfree(err);
        }
        listRelease(cluster_manager.errors);
        cluster_manager.errors = NULL;
    }
    if (clusterManagerUncoveredSlots != NULL)
        dictRelease(clusterManagerUncoveredSlots);
}

static clusterManagerNode *clusterManagerNewNode(char *ip, int port, int bus_port) {
    clusterManagerNode *node = zmalloc(sizeof(*node));
    node->context = NULL;
    node->name = NULL;
    node->ip = ip;
    node->port = port;
    /* We don't need to know the bus_port, at this point this value may be wrong.
     * If it is used, it will be corrected in clusterManagerLoadInfoFromNode. */
    node->bus_port = bus_port ? bus_port : port + CLUSTER_MANAGER_PORT_INCR;
    node->current_epoch = 0;
    node->ping_sent = 0;
    node->ping_recv = 0;
    node->flags = 0;
    node->flags_str = NULL;
    node->replicate = NULL;
    node->dirty = 0;
    node->friends = NULL;
    node->migrating = NULL;
    node->importing = NULL;
    node->migrating_count = 0;
    node->importing_count = 0;
    node->replicas_count = 0;
    node->weight = 1.0f;
    node->balance = 0;
    clusterManagerNodeResetSlots(node);
    return node;
}

static sds clusterManagerGetNodeRDBFilename(clusterManagerNode *node) {
    assert(config.cluster_manager_command.backup_dir);
    sds filename = sdsnew(config.cluster_manager_command.backup_dir);
    if (filename[sdslen(filename) - 1] != '/')
        filename = sdscat(filename, "/");
    filename = sdscatprintf(filename, "redis-node-%s-%d-%s.rdb", node->ip,
                            node->port, node->name);
    return filename;
}

/* Check whether reply is NULL or its type is REDIS_REPLY_ERROR. In the
 * latest case, if the 'err' arg is not NULL, it gets allocated with a copy
 * of reply error (it's up to the caller function to free it), elsewhere
 * the error is directly printed. */
static int clusterManagerCheckRedisReply(clusterManagerNode *n,
                                         redisReply *r, char **err)
{
    int is_err = 0;
    if (!r || (is_err = (r->type == REDIS_REPLY_ERROR))) {
        if (is_err) {
            if (err != NULL) {
                *err = zmalloc((r->len + 1) * sizeof(char));
                redis_strlcpy(*err, r->str,(r->len + 1));
            } else CLUSTER_MANAGER_PRINT_REPLY_ERROR(n, r->str);
        }
        return 0;
    }
    return 1;
}

/* Call MULTI command on a cluster node. */
static int clusterManagerStartTransaction(clusterManagerNode *node) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "MULTI");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}

/* Call EXEC command on a cluster node. */
static int clusterManagerExecTransaction(clusterManagerNode *node,
                                         clusterManagerOnReplyError onerror)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "EXEC");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (success) {
        if (reply->type != REDIS_REPLY_ARRAY) {
            success = 0;
            goto cleanup;
        }
        size_t i;
        for (i = 0; i < reply->elements; i++) {
            redisReply *r = reply->element[i];
            char *err = NULL;
            success = clusterManagerCheckRedisReply(node, r, &err);
            if (!success && onerror) success = onerror(r, node, i);
            if (err) {
                if (!success)
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                zfree(err);
            }
            if (!success) break;
        }
    }
cleanup:
    if (reply) freeReplyObject(reply);
    return success;
}

static int clusterManagerNodeConnect(clusterManagerNode *node) {
    if (node->context) redisFree(node->context);
    node->context = redisConnectWrapper(node->ip, node->port, config.connect_timeout);
    if (!node->context->err && config.tls) {
        const char *err = NULL;
        if (cliSecureConnection(node->context, config.sslconfig, &err) == REDIS_ERR && err) {
            fprintf(stderr,"TLS Error: %s\n", err);
            redisFree(node->context);
            node->context = NULL;
            return 0;
        }
    }
    if (node->context->err) {
        fprintf(stderr,"Could not connect to Redis at ");
        fprintf(stderr,"%s:%d: %s\n", node->ip, node->port,
                node->context->errstr);
        redisFree(node->context);
        node->context = NULL;
        return 0;
    }
    /* Set aggressive KEEP_ALIVE socket option in the Redis context socket
     * in order to prevent timeouts caused by the execution of long
     * commands. At the same time this improves the detection of real
     * errors. */
    anetKeepAlive(NULL, node->context->fd, REDIS_CLI_KEEPALIVE_INTERVAL);
    if (config.conn_info.auth) {
        redisReply *reply;
        if (config.conn_info.user == NULL)
            reply = redisCommand(node->context,"AUTH %s", config.conn_info.auth);
        else
            reply = redisCommand(node->context,"AUTH %s %s",
                                 config.conn_info.user,config.conn_info.auth);
        int ok = clusterManagerCheckRedisReply(node, reply, NULL);
        if (reply != NULL) freeReplyObject(reply);
        if (!ok) return 0;
    }
    return 1;
}

static void clusterManagerRemoveNodeFromList(list *nodelist,
                                             clusterManagerNode *node) {
    listIter li;
    listNode *ln;
    listRewind(nodelist, &li);
    while ((ln = listNext(&li)) != NULL) {
        if (node == ln->value) {
            listDelNode(nodelist, ln);
            break;
        }
    }
}

/* Return the node with the specified name (ID) or NULL. */
static clusterManagerNode *clusterManagerNodeByName(const char *name) {
    if (cluster_manager.nodes == NULL) return NULL;
    clusterManagerNode *found = NULL;
    sds lcname = sdsempty();
    lcname = sdscpy(lcname, name);
    sdstolower(lcname);
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->name && !sdscmp(n->name, lcname)) {
            found = n;
            break;
        }
    }
    sdsfree(lcname);
    return found;
}

/* Like clusterManagerNodeByName but the specified name can be just the first
 * part of the node ID as long as the prefix in unique across the
 * cluster.
 */
static clusterManagerNode *clusterManagerNodeByAbbreviatedName(const char*name)
{
    if (cluster_manager.nodes == NULL) return NULL;
    clusterManagerNode *found = NULL;
    sds lcname = sdsempty();
    lcname = sdscpy(lcname, name);
    sdstolower(lcname);
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->name &&
            strstr(n->name, lcname) == n->name) {
            found = n;
            break;
        }
    }
    sdsfree(lcname);
    return found;
}

static void clusterManagerNodeResetSlots(clusterManagerNode *node) {
    memset(node->slots, 0, sizeof(node->slots));
    node->slots_count = 0;
}

/* Call "INFO" redis command on the specified node and return the reply. */
static redisReply *clusterManagerGetNodeRedisInfo(clusterManagerNode *node,
                                                  char **err)
{
    redisReply *info = CLUSTER_MANAGER_COMMAND(node, "INFO");
    if (err != NULL) *err = NULL;
    if (info == NULL) return NULL;
    if (info->type == REDIS_REPLY_ERROR) {
        if (err != NULL) {
            *err = zmalloc((info->len + 1) * sizeof(char));
            redis_strlcpy(*err, info->str,(info->len + 1));
        }
        freeReplyObject(info);
        return  NULL;
    }
    return info;
}

static int clusterManagerNodeIsCluster(clusterManagerNode *node, char **err) {
    redisReply *info = clusterManagerGetNodeRedisInfo(node, err);
    if (info == NULL) return 0;
    int is_cluster = (int) getLongInfoField(info->str, "cluster_enabled");
    freeReplyObject(info);
    return is_cluster;
}

/* Checks whether the node is empty. Node is considered not-empty if it has
 * some key or if it already knows other nodes */
static int clusterManagerNodeIsEmpty(clusterManagerNode *node, char **err) {
    redisReply *info = clusterManagerGetNodeRedisInfo(node, err);
    int is_empty = 1;
    if (info == NULL) return 0;
    if (strstr(info->str, "db0:") != NULL) {
        is_empty = 0;
        goto result;
    }
    freeReplyObject(info);
    info = CLUSTER_MANAGER_COMMAND(node, "CLUSTER INFO");
    if (err != NULL) *err = NULL;
    if (!clusterManagerCheckRedisReply(node, info, err)) {
        is_empty = 0;
        goto result;
    }
    long known_nodes = getLongInfoField(info->str, "cluster_known_nodes");
    is_empty = (known_nodes == 1);
result:
    freeReplyObject(info);
    return is_empty;
}

/* Return the anti-affinity score, which is a measure of the amount of
 * violations of anti-affinity in the current cluster layout, that is, how
 * badly the masters and slaves are distributed in the different IP
 * addresses so that slaves of the same master are not in the master
 * host and are also in different hosts.
 *
 * The score is calculated as follows:
 *
 * SAME_AS_MASTER = 10000 * each slave in the same IP of its master.
 * SAME_AS_SLAVE  = 1 * each slave having the same IP as another slave
                      of the same master.
 * FINAL_SCORE = SAME_AS_MASTER + SAME_AS_SLAVE
 *
 * So a greater score means a worse anti-affinity level, while zero
 * means perfect anti-affinity.
 *
 * The anti affinity optimization will try to get a score as low as
 * possible. Since we do not want to sacrifice the fact that slaves should
 * not be in the same host as the master, we assign 10000 times the score
 * to this violation, so that we'll optimize for the second factor only
 * if it does not impact the first one.
 *
 * The ipnodes argument is an array of clusterManagerNodeArray, one for
 * each IP, while ip_count is the total number of IPs in the configuration.
 *
 * The function returns the above score, and the list of
 * offending slaves can be stored into the 'offending' argument,
 * so that the optimizer can try changing the configuration of the
 * slaves violating the anti-affinity goals. */
static int clusterManagerGetAntiAffinityScore(clusterManagerNodeArray *ipnodes,
    int ip_count, clusterManagerNode ***offending, int *offending_len)
{
    int score = 0, i, j;
    int node_len = cluster_manager.nodes->len;
    clusterManagerNode **offending_p = NULL;
    if (offending != NULL) {
        *offending = zcalloc(node_len * sizeof(clusterManagerNode*));
        offending_p = *offending;
    }
    /* For each set of nodes in the same host, split by
     * related nodes (masters and slaves which are involved in
     * replication of each other) */
    for (i = 0; i < ip_count; i++) {
        clusterManagerNodeArray *node_array = &(ipnodes[i]);
        dict *related = dictCreate(&clusterManagerDictType);
        char *ip = NULL;
        for (j = 0; j < node_array->len; j++) {
            clusterManagerNode *node = node_array->nodes[j];
            if (node == NULL) continue;
            if (!ip) ip = node->ip;
            sds types;
            /* We always use the Master ID as key. */
            sds key = (!node->replicate ? node->name : node->replicate);
            assert(key != NULL);
            dictEntry *entry = dictFind(related, key);
            if (entry) types = sdsdup((sds) dictGetVal(entry));
            else types = sdsempty();
            /* Master type 'm' is always set as the first character of the
             * types string. */
            if (node->replicate) types = sdscat(types, "s");
            else {
                sds s = sdscatsds(sdsnew("m"), types);
                sdsfree(types);
                types = s;
            }
            dictReplace(related, key, types);
        }
        /* Now it's trivial to check, for each related group having the
         * same host, what is their local score. */
        dictIterator *iter = dictGetIterator(related);
        dictEntry *entry;
        while ((entry = dictNext(iter)) != NULL) {
            sds types = (sds) dictGetVal(entry);
            sds name = (sds) dictGetKey(entry);
            int typeslen = sdslen(types);
            if (typeslen < 2) continue;
            if (types[0] == 'm') score += (10000 * (typeslen - 1));
            else score += (1 * typeslen);
            if (offending == NULL) continue;
            /* Populate the list of offending nodes. */
            listIter li;
            listNode *ln;
            listRewind(cluster_manager.nodes, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (n->replicate == NULL) continue;
                if (!strcmp(n->replicate, name) && !strcmp(n->ip, ip)) {
                    *(offending_p++) = n;
                    if (offending_len != NULL) (*offending_len)++;
                    break;
                }
            }
        }
        //if (offending_len != NULL) *offending_len = offending_p - *offending;
        dictReleaseIterator(iter);
        dictRelease(related);
    }
    return score;
}

static void clusterManagerOptimizeAntiAffinity(clusterManagerNodeArray *ipnodes,
    int ip_count)
{
    clusterManagerNode **offenders = NULL;
    int score = clusterManagerGetAntiAffinityScore(ipnodes, ip_count,
                                                   NULL, NULL);
    if (score == 0) goto cleanup;
    clusterManagerLogInfo(">>> Trying to optimize slaves allocation "
                          "for anti-affinity\n");
    int node_len = cluster_manager.nodes->len;
    int maxiter = 500 * node_len; // Effort is proportional to cluster size...
    srand(time(NULL));
    while (maxiter > 0) {
        int offending_len = 0;
        if (offenders != NULL) {
            zfree(offenders);
            offenders = NULL;
        }
        score = clusterManagerGetAntiAffinityScore(ipnodes,
                                                   ip_count,
                                                   &offenders,
                                                   &offending_len);
        if (score == 0 || offending_len == 0) break; // Optimal anti affinity reached
        /* We'll try to randomly swap a slave's assigned master causing
         * an affinity problem with another random slave, to see if we
         * can improve the affinity. */
        int rand_idx = rand() % offending_len;
        clusterManagerNode *first = offenders[rand_idx],
                           *second = NULL;
        clusterManagerNode **other_replicas = zcalloc((node_len - 1) *
                                                      sizeof(*other_replicas));
        int other_replicas_count = 0;
        listIter li;
        listNode *ln;
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n != first && n->replicate != NULL)
                other_replicas[other_replicas_count++] = n;
        }
        if (other_replicas_count == 0) {
            zfree(other_replicas);
            break;
        }
        rand_idx = rand() % other_replicas_count;
        second = other_replicas[rand_idx];
        char *first_master = first->replicate,
             *second_master = second->replicate;
        first->replicate = second_master, first->dirty = 1;
        second->replicate = first_master, second->dirty = 1;
        int new_score = clusterManagerGetAntiAffinityScore(ipnodes,
                                                           ip_count,
                                                           NULL, NULL);
        /* If the change actually makes thing worse, revert. Otherwise
         * leave as it is because the best solution may need a few
         * combined swaps. */
        if (new_score > score) {
            first->replicate = first_master;
            second->replicate = second_master;
        }
        zfree(other_replicas);
        maxiter--;
    }
    score = clusterManagerGetAntiAffinityScore(ipnodes, ip_count, NULL, NULL);
    char *msg;
    int perfect = (score == 0);
    int log_level = (perfect ? CLUSTER_MANAGER_LOG_LVL_SUCCESS :
                               CLUSTER_MANAGER_LOG_LVL_WARN);
    if (perfect) msg = "[OK] Perfect anti-affinity obtained!";
    else if (score >= 10000)
        msg = ("[WARNING] Some slaves are in the same host as their master");
    else
        msg=("[WARNING] Some slaves of the same master are in the same host");
    clusterManagerLog(log_level, "%s\n", msg);
cleanup:
    zfree(offenders);
}

/* Return a representable string of the node's flags */
static sds clusterManagerNodeFlagString(clusterManagerNode *node) {
    sds flags = sdsempty();
    if (!node->flags_str) return flags;
    int empty = 1;
    listIter li;
    listNode *ln;
    listRewind(node->flags_str, &li);
    while ((ln = listNext(&li)) != NULL) {
        sds flag = ln->value;
        if (strcmp(flag, "myself") == 0) continue;
        if (!empty) flags = sdscat(flags, ",");
        flags = sdscatfmt(flags, "%S", flag);
        empty = 0;
    }
    return flags;
}

/* Return a representable string of the node's slots */
static sds clusterManagerNodeSlotsString(clusterManagerNode *node) {
    sds slots = sdsempty();
    int first_range_idx = -1, last_slot_idx = -1, i;
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        int has_slot = node->slots[i];
        if (has_slot) {
            if (first_range_idx == -1) {
                if (sdslen(slots)) slots = sdscat(slots, ",");
                first_range_idx = i;
                slots = sdscatfmt(slots, "[%u", i);
            }
            last_slot_idx = i;
        } else {
            if (last_slot_idx >= 0) {
                if (first_range_idx == last_slot_idx)
                    slots = sdscat(slots, "]");
                else slots = sdscatfmt(slots, "-%u]", last_slot_idx);
            }
            last_slot_idx = -1;
            first_range_idx = -1;
        }
    }
    if (last_slot_idx >= 0) {
        if (first_range_idx == last_slot_idx) slots = sdscat(slots, "]");
        else slots = sdscatfmt(slots, "-%u]", last_slot_idx);
    }
    return slots;
}

static sds clusterManagerNodeGetJSON(clusterManagerNode *node,
                                     unsigned long error_count)
{
    sds json = sdsempty();
    sds replicate = sdsempty();
    if (node->replicate)
        replicate = sdscatprintf(replicate, "\"%s\"", node->replicate);
    else
        replicate = sdscat(replicate, "null");
    sds slots = clusterManagerNodeSlotsString(node);
    sds flags = clusterManagerNodeFlagString(node);
    char *p = slots;
    while ((p = strchr(p, '-')) != NULL)
        *(p++) = ',';
    json = sdscatprintf(json,
        "  {\n"
        "    \"name\": \"%s\",\n"
        "    \"host\": \"%s\",\n"
        "    \"port\": %d,\n"
        "    \"replicate\": %s,\n"
        "    \"slots\": [%s],\n"
        "    \"slots_count\": %d,\n"
        "    \"flags\": \"%s\",\n"
        "    \"current_epoch\": %llu",
        node->name,
        node->ip,
        node->port,
        replicate,
        slots,
        node->slots_count,
        flags,
        (unsigned long long)node->current_epoch
    );
    if (error_count > 0) {
        json = sdscatprintf(json, ",\n    \"cluster_errors\": %lu",
                            error_count);
    }
    if (node->migrating_count > 0 && node->migrating != NULL) {
        int i = 0;
        sds migrating = sdsempty();
        for (; i < node->migrating_count; i += 2) {
            sds slot = node->migrating[i];
            sds dest = node->migrating[i + 1];
            if (slot && dest) {
                if (sdslen(migrating) > 0) migrating = sdscat(migrating, ",");
                migrating = sdscatfmt(migrating, "\"%S\": \"%S\"", slot, dest);
            }
        }
        if (sdslen(migrating) > 0)
            json = sdscatfmt(json, ",\n    \"migrating\": {%S}", migrating);
        sdsfree(migrating);
    }
    if (node->importing_count > 0 && node->importing != NULL) {
        int i = 0;
        sds importing = sdsempty();
        for (; i < node->importing_count; i += 2) {
            sds slot = node->importing[i];
            sds from = node->importing[i + 1];
            if (slot && from) {
                if (sdslen(importing) > 0) importing = sdscat(importing, ",");
                importing = sdscatfmt(importing, "\"%S\": \"%S\"", slot, from);
            }
        }
        if (sdslen(importing) > 0)
            json = sdscatfmt(json, ",\n    \"importing\": {%S}", importing);
        sdsfree(importing);
    }
    json = sdscat(json, "\n  }");
    sdsfree(replicate);
    sdsfree(slots);
    sdsfree(flags);
    return json;
}


/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
static unsigned int clusterManagerKeyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing between {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* Return a string representation of the cluster node. */
static sds clusterManagerNodeInfo(clusterManagerNode *node, int indent) {
    sds info = sdsempty();
    sds spaces = sdsempty();
    int i;
    for (i = 0; i < indent; i++) spaces = sdscat(spaces, " ");
    if (indent) info = sdscat(info, spaces);
    int is_master = !(node->flags & CLUSTER_MANAGER_FLAG_SLAVE);
    char *role = (is_master ? "M" : "S");
    sds slots = NULL;
    if (node->dirty && node->replicate != NULL)
        info = sdscatfmt(info, "S: %S %s:%u", node->name, node->ip, node->port);
    else {
        slots = clusterManagerNodeSlotsString(node);
        sds flags = clusterManagerNodeFlagString(node);
        info = sdscatfmt(info, "%s: %S %s:%u\n"
                               "%s   slots:%S (%u slots) "
                               "%S",
                               role, node->name, node->ip, node->port, spaces,
                               slots, node->slots_count, flags);
        sdsfree(slots);
        sdsfree(flags);
    }
    if (node->replicate != NULL)
        info = sdscatfmt(info, "\n%s   replicates %S", spaces, node->replicate);
    else if (node->replicas_count)
        info = sdscatfmt(info, "\n%s   %U additional replica(s)",
                         spaces, node->replicas_count);
    sdsfree(spaces);
    return info;
}

static void clusterManagerShowNodes(void) {
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        sds info = clusterManagerNodeInfo(node, 0);
        printf("%s\n", (char *) info);
        sdsfree(info);
    }
}

static void clusterManagerShowClusterInfo(void) {
    int masters = 0;
    long long keys = 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        if (!(node->flags & CLUSTER_MANAGER_FLAG_SLAVE)) {
            if (!node->name) continue;
            int replicas = 0;
            long long dbsize = -1;
            char name[9];
            memcpy(name, node->name, 8);
            name[8] = '\0';
            listIter ri;
            listNode *rn;
            listRewind(cluster_manager.nodes, &ri);
            while ((rn = listNext(&ri)) != NULL) {
                clusterManagerNode *n = rn->value;
                if (n == node || !(n->flags & CLUSTER_MANAGER_FLAG_SLAVE))
                    continue;
                if (n->replicate && !strcmp(n->replicate, node->name))
                    replicas++;
            }
            redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "DBSIZE");
            if (reply != NULL && reply->type == REDIS_REPLY_INTEGER)
                dbsize = reply->integer;
            if (dbsize < 0) {
                char *err = "";
                if (reply != NULL && reply->type == REDIS_REPLY_ERROR)
                    err = reply->str;
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                if (reply != NULL) freeReplyObject(reply);
                return;
            };
            if (reply != NULL) freeReplyObject(reply);
            printf("%s:%d (%s...) -> %lld keys | %d slots | %d slaves.\n",
                   node->ip, node->port, name, dbsize,
                   node->slots_count, replicas);
            masters++;
            keys += dbsize;
        }
    }
    clusterManagerLogOk("[OK] %lld keys in %d masters.\n", keys, masters);
    float keys_per_slot = keys / (float) CLUSTER_MANAGER_SLOTS;
    printf("%.2f keys per slot on average.\n", keys_per_slot);
}

/* Flush dirty slots configuration of the node by calling CLUSTER ADDSLOTS */
static int clusterManagerAddSlots(clusterManagerNode *node, char**err)
{
    redisReply *reply = NULL;
    void *_reply = NULL;
    int success = 1;
    /* First two args are used for the command itself. */
    int argc = node->slots_count + 2;
    sds *argv = zmalloc(argc * sizeof(*argv));
    size_t *argvlen = zmalloc(argc * sizeof(*argvlen));
    argv[0] = "CLUSTER";
    argv[1] = "ADDSLOTS";
    argvlen[0] = 7;
    argvlen[1] = 8;
    *err = NULL;
    int i, argv_idx = 2;
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        if (argv_idx >= argc) break;
        if (node->slots[i]) {
            argv[argv_idx] = sdsfromlonglong((long long) i);
            argvlen[argv_idx] = sdslen(argv[argv_idx]);
            argv_idx++;
        }
    }
    if (argv_idx == 2) {
        success = 0;
        goto cleanup;
    }
    redisAppendCommandArgv(node->context,argc,(const char**)argv,argvlen);
    if (redisGetReply(node->context, &_reply) != REDIS_OK) {
        success = 0;
        goto cleanup;
    }
    reply = (redisReply*) _reply;
    success = clusterManagerCheckRedisReply(node, reply, err);
cleanup:
    zfree(argvlen);
    if (argv != NULL) {
        for (i = 2; i < argc; i++) sdsfree(argv[i]);
        zfree(argv);
    }
    if (reply != NULL) freeReplyObject(reply);
    return success;
}

/* Get the node the slot is assigned to from the point of view of node *n.
 * If the slot is unassigned or if the reply is an error, return NULL.
 * Use the **err argument in order to check whether the slot is unassigned
 * or the reply resulted in an error. */
static clusterManagerNode *clusterManagerGetSlotOwner(clusterManagerNode *n,
                                                      int slot, char **err)
{
    assert(slot >= 0 && slot < CLUSTER_MANAGER_SLOTS);
    clusterManagerNode *owner = NULL;
    redisReply *reply = CLUSTER_MANAGER_COMMAND(n, "CLUSTER SLOTS");
    if (clusterManagerCheckRedisReply(n, reply, err)) {
        assert(reply->type == REDIS_REPLY_ARRAY);
        size_t i;
        for (i = 0; i < reply->elements; i++) {
            redisReply *r = reply->element[i];
            assert(r->type == REDIS_REPLY_ARRAY && r->elements >= 3);
            int from, to;
            from = r->element[0]->integer;
            to = r->element[1]->integer;
            if (slot < from || slot > to) continue;
            redisReply *nr =  r->element[2];
            assert(nr->type == REDIS_REPLY_ARRAY && nr->elements >= 2);
            char *name = NULL;
            if (nr->elements >= 3)
                name =  nr->element[2]->str;
            if (name != NULL)
                owner = clusterManagerNodeByName(name);
            else {
                char *ip = nr->element[0]->str;
                assert(ip != NULL);
                int port = (int) nr->element[1]->integer;
                listIter li;
                listNode *ln;
                listRewind(cluster_manager.nodes, &li);
                while ((ln = listNext(&li)) != NULL) {
                    clusterManagerNode *nd = ln->value;
                    if (strcmp(nd->ip, ip) == 0 && port == nd->port) {
                        owner = nd;
                        break;
                    }
                }
            }
            if (owner) break;
        }
    }
    if (reply) freeReplyObject(reply);
    return owner;
}

/* Set slot status to "importing" or "migrating" */
static int clusterManagerSetSlot(clusterManagerNode *node1,
                                 clusterManagerNode *node2,
                                 int slot, const char *status, char **err) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node1, "CLUSTER "
                                                "SETSLOT %d %s %s",
                                                slot, status,
                                                (char *) node2->name);
    if (err != NULL) *err = NULL;
    if (!reply) {
        if (err) *err = zstrdup("CLUSTER SETSLOT failed to run");
        return 0;
    }
    int success = 1;
    if (reply->type == REDIS_REPLY_ERROR) {
        success = 0;
        if (err != NULL) {
            *err = zmalloc((reply->len + 1) * sizeof(char));
            redis_strlcpy(*err, reply->str,(reply->len + 1));
        } else CLUSTER_MANAGER_PRINT_REPLY_ERROR(node1, reply->str);
        goto cleanup;
    }
cleanup:
    freeReplyObject(reply);
    return success;
}

static int clusterManagerClearSlotStatus(clusterManagerNode *node, int slot) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER SETSLOT %d %s", slot, "STABLE");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}

static int clusterManagerDelSlot(clusterManagerNode *node, int slot,
                                 int ignore_unassigned_err)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER DELSLOTS %d", slot);
    char *err = NULL;
    int success = clusterManagerCheckRedisReply(node, reply, &err);
    if (!success && reply && reply->type == REDIS_REPLY_ERROR &&
        ignore_unassigned_err)
    {
        char *get_owner_err = NULL;
        clusterManagerNode *assigned_to =
            clusterManagerGetSlotOwner(node, slot, &get_owner_err);
        if (!assigned_to) {
            if (get_owner_err == NULL) success = 1;
            else {
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, get_owner_err);
                zfree(get_owner_err);
            }
        }
    }
    if (!success && err != NULL) {
        CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
        zfree(err);
    }
    if (reply) freeReplyObject(reply);
    return success;
}

static int clusterManagerAddSlot(clusterManagerNode *node, int slot) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER ADDSLOTS %d", slot);
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}

static signed int clusterManagerCountKeysInSlot(clusterManagerNode *node,
                                                int slot)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER COUNTKEYSINSLOT %d", slot);
    int count = -1;
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (success && reply->type == REDIS_REPLY_INTEGER) count = reply->integer;
    if (reply) freeReplyObject(reply);
    return count;
}

static int clusterManagerBumpEpoch(clusterManagerNode *node) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER BUMPEPOCH");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}

/* Callback used by clusterManagerSetSlotOwner transaction. It should ignore
 * errors except for ADDSLOTS errors.
 * Return 1 if the error should be ignored. */
static int clusterManagerOnSetOwnerErr(redisReply *reply,
    clusterManagerNode *n, int bulk_idx)
{
    UNUSED(reply);
    UNUSED(n);
    /* Only raise error when ADDSLOTS fail (bulk_idx == 1). */
    return (bulk_idx != 1);
}

static int clusterManagerSetSlotOwner(clusterManagerNode *owner,
                                      int slot,
                                      int do_clear)
{
    int success = clusterManagerStartTransaction(owner);
    if (!success) return 0;
    /* Ensure the slot is not already assigned. */
    clusterManagerDelSlot(owner, slot, 1);
    /* Add the slot and bump epoch. */
    clusterManagerAddSlot(owner, slot);
    if (do_clear) clusterManagerClearSlotStatus(owner, slot);
    clusterManagerBumpEpoch(owner);
    success = clusterManagerExecTransaction(owner, clusterManagerOnSetOwnerErr);
    return success;
}

/* Get the hash for the values of the specified keys in *keys_reply for the
 * specified nodes *n1 and *n2, by calling DEBUG DIGEST-VALUE redis command
 * on both nodes. Every key with same name on both nodes but having different
 * values will be added to the *diffs list. Return 0 in case of reply
 * error. */
static int clusterManagerCompareKeysValues(clusterManagerNode *n1,
                                          clusterManagerNode *n2,
                                          redisReply *keys_reply,
                                          list *diffs)
{
    size_t i, argc = keys_reply->elements + 2;
    static const char *hash_zero = "0000000000000000000000000000000000000000";
    char **argv = zcalloc(argc * sizeof(char *));
    size_t  *argv_len = zcalloc(argc * sizeof(size_t));
    argv[0] = "DEBUG";
    argv_len[0] = 5;
    argv[1] = "DIGEST-VALUE";
    argv_len[1] = 12;
    for (i = 0; i < keys_reply->elements; i++) {
        redisReply *entry = keys_reply->element[i];
        int idx = i + 2;
        argv[idx] = entry->str;
        argv_len[idx] = entry->len;
    }
    int success = 0;
    void *_reply1 = NULL, *_reply2 = NULL;
    redisReply *r1 = NULL, *r2 = NULL;
    redisAppendCommandArgv(n1->context,argc, (const char**)argv,argv_len);
    success = (redisGetReply(n1->context, &_reply1) == REDIS_OK);
    if (!success) goto cleanup;
    r1 = (redisReply *) _reply1;
    redisAppendCommandArgv(n2->context,argc, (const char**)argv,argv_len);
    success = (redisGetReply(n2->context, &_reply2) == REDIS_OK);
    if (!success) goto cleanup;
    r2 = (redisReply *) _reply2;
    success = (r1->type != REDIS_REPLY_ERROR && r2->type != REDIS_REPLY_ERROR);
    if (r1->type == REDIS_REPLY_ERROR) {
        CLUSTER_MANAGER_PRINT_REPLY_ERROR(n1, r1->str);
        success = 0;
    }
    if (r2->type == REDIS_REPLY_ERROR) {
        CLUSTER_MANAGER_PRINT_REPLY_ERROR(n2, r2->str);
        success = 0;
    }
    if (!success) goto cleanup;
    assert(keys_reply->elements == r1->elements &&
           keys_reply->elements == r2->elements);
    for (i = 0; i < keys_reply->elements; i++) {
        char *key = keys_reply->element[i]->str;
        char *hash1 = r1->element[i]->str;
        char *hash2 = r2->element[i]->str;
        /* Ignore keys that don't exist in both nodes. */
        if (strcmp(hash1, hash_zero) == 0 || strcmp(hash2, hash_zero) == 0)
            continue;
        if (strcmp(hash1, hash2) != 0) listAddNodeTail(diffs, key);
    }
cleanup:
    if (r1) freeReplyObject(r1);
    if (r2) freeReplyObject(r2);
    zfree(argv);
    zfree(argv_len);
    return success;
}

/* Migrate keys taken from reply->elements. It returns the reply from the
 * MIGRATE command, or NULL if something goes wrong. If the argument 'dots'
 * is not NULL, a dot will be printed for every migrated key. */
static redisReply *clusterManagerMigrateKeysInReply(clusterManagerNode *source,
                                                    clusterManagerNode *target,
                                                    redisReply *reply,
                                                    int replace, int timeout,
                                                    char *dots)
{
    redisReply *migrate_reply = NULL;
    char **argv = NULL;
    size_t *argv_len = NULL;
    int c = (replace ? 8 : 7);
    if (config.conn_info.auth) c += 2;
    if (config.conn_info.user) c += 1;
    size_t argc = c + reply->elements;
    size_t i, offset = 6; // Keys Offset
    argv = zcalloc(argc * sizeof(char *));
    argv_len = zcalloc(argc * sizeof(size_t));
    char portstr[255];
    char timeoutstr[255];
    snprintf(portstr, 10, "%d", target->port);
    snprintf(timeoutstr, 10, "%d", timeout);
    argv[0] = "MIGRATE";
    argv_len[0] = 7;
    argv[1] = target->ip;
    argv_len[1] = strlen(target->ip);
    argv[2] = portstr;
    argv_len[2] = strlen(portstr);
    argv[3] = "";
    argv_len[3] = 0;
    argv[4] = "0";
    argv_len[4] = 1;
    argv[5] = timeoutstr;
    argv_len[5] = strlen(timeoutstr);
    if (replace) {
        argv[offset] = "REPLACE";
        argv_len[offset] = 7;
        offset++;
    }
    if (config.conn_info.auth) {
        if (config.conn_info.user) {
            argv[offset] = "AUTH2";
            argv_len[offset] = 5;
            offset++;
            argv[offset] = config.conn_info.user;
            argv_len[offset] = strlen(config.conn_info.user);
            offset++;
            argv[offset] = config.conn_info.auth;
            argv_len[offset] = strlen(config.conn_info.auth);
            offset++;
        } else {
            argv[offset] = "AUTH";
            argv_len[offset] = 4;
            offset++;
            argv[offset] = config.conn_info.auth;
            argv_len[offset] = strlen(config.conn_info.auth);
            offset++;
        }
    }
    argv[offset] = "KEYS";
    argv_len[offset] = 4;
    offset++;
    for (i = 0; i < reply->elements; i++) {
        redisReply *entry = reply->element[i];
        size_t idx = i + offset;
        assert(entry->type == REDIS_REPLY_STRING);
        argv[idx] = (char *) sdsnewlen(entry->str, entry->len);
        argv_len[idx] = entry->len;
        if (dots) dots[i] = '.';
    }
    if (dots) dots[reply->elements] = '\0';
    void *_reply = NULL;
    redisAppendCommandArgv(source->context,argc,
                           (const char**)argv,argv_len);
    int success = (redisGetReply(source->context, &_reply) == REDIS_OK);
    for (i = 0; i < reply->elements; i++) sdsfree(argv[i + offset]);
    if (!success) goto cleanup;
    migrate_reply = (redisReply *) _reply;
cleanup:
    zfree(argv);
    zfree(argv_len);
    return migrate_reply;
}

/* Migrate all keys in the given slot from source to target.*/
static int clusterManagerMigrateKeysInSlot(clusterManagerNode *source,
                                           clusterManagerNode *target,
                                           int slot, int timeout,
                                           int pipeline, int verbose,
                                           char **err)
{
    int success = 1;
    int do_fix = config.cluster_manager_command.flags &
                 CLUSTER_MANAGER_CMD_FLAG_FIX;
    int do_replace = config.cluster_manager_command.flags &
                     CLUSTER_MANAGER_CMD_FLAG_REPLACE;
    while (1) {
        char *dots = NULL;
        redisReply *reply = NULL, *migrate_reply = NULL;
        reply = CLUSTER_MANAGER_COMMAND(source, "CLUSTER "
                                        "GETKEYSINSLOT %d %d", slot,
                                        pipeline);
        success = (reply != NULL);
        if (!success) return 0;
        if (reply->type == REDIS_REPLY_ERROR) {
            success = 0;
            if (err != NULL) {
                *err = zmalloc((reply->len + 1) * sizeof(char));
                redis_strlcpy(*err, reply->str,(reply->len + 1));
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(source, *err);
            }
            goto next;
        }
        assert(reply->type == REDIS_REPLY_ARRAY);
        size_t count = reply->elements;
        if (count == 0) {
            freeReplyObject(reply);
            break;
        }
        if (verbose) dots = zmalloc((count+1) * sizeof(char));
        /* Calling MIGRATE command. */
        migrate_reply = clusterManagerMigrateKeysInReply(source, target,
                                                         reply, 0, timeout,
                                                         dots);
        if (migrate_reply == NULL) goto next;
        if (migrate_reply->type == REDIS_REPLY_ERROR) {
            int is_busy = strstr(migrate_reply->str, "BUSYKEY") != NULL;
            int not_served = 0;
            if (!is_busy) {
                /* Check if the slot is unassigned (not served) in the
                 * source node's configuration. */
                char *get_owner_err = NULL;
                clusterManagerNode *served_by =
                    clusterManagerGetSlotOwner(source, slot, &get_owner_err);
                if (!served_by) {
                    if (get_owner_err == NULL) not_served = 1;
                    else {
                        CLUSTER_MANAGER_PRINT_REPLY_ERROR(source,
                                                          get_owner_err);
                        zfree(get_owner_err);
                    }
                }
            }
            /* Try to handle errors. */
            if (is_busy || not_served) {
                /* If the key's slot is not served, try to assign slot
                 * to the target node. */
                if (do_fix && not_served) {
                    clusterManagerLogWarn("*** Slot was not served, setting "
                                          "owner to node %s:%d.\n",
                                          target->ip, target->port);
                    clusterManagerSetSlot(source, target, slot, "node", NULL);
                }
                /* If the key already exists in the target node (BUSYKEY),
                 * check whether its value is the same in both nodes.
                 * In case of equal values, retry migration with the
                 * REPLACE option.
                 * In case of different values:
                 *  - If the migration is requested by the fix command, stop
                 *    and warn the user.
                 *  - In other cases (ie. reshard), proceed only if the user
                 *    launched the command with the --cluster-replace option.*/
                if (is_busy) {
                    clusterManagerLogWarn("\n*** Target key exists\n");
                    if (!do_replace) {
                        clusterManagerLogWarn("*** Checking key values on "
                                              "both nodes...\n");
                        list *diffs = listCreate();
                        success = clusterManagerCompareKeysValues(source,
                            target, reply, diffs);
                        if (!success) {
                            clusterManagerLogErr("*** Value check failed!\n");
                            listRelease(diffs);
                            goto next;
                        }
                        if (listLength(diffs) > 0) {
                            success = 0;
                            clusterManagerLogErr(
                                "*** Found %d key(s) in both source node and "
                                "target node having different values.\n"
                                "    Source node: %s:%d\n"
                                "    Target node: %s:%d\n"
                                "    Keys(s):\n",
                                listLength(diffs),
                                source->ip, source->port,
                                target->ip, target->port);
                            listIter dli;
                            listNode *dln;
                            listRewind(diffs, &dli);
                            while((dln = listNext(&dli)) != NULL) {
                                char *k = dln->value;
                                clusterManagerLogErr("    - %s\n", k);
                            }
                            clusterManagerLogErr("Please fix the above key(s) "
                                                 "manually and try again "
                                                 "or relaunch the command \n"
                                                 "with --cluster-replace "
                                                 "option to force key "
                                                 "overriding.\n");
                            listRelease(diffs);
                            goto next;
                        }
                        listRelease(diffs);
                    }
                    clusterManagerLogWarn("*** Replacing target keys...\n");
                }
                freeReplyObject(migrate_reply);
                migrate_reply = clusterManagerMigrateKeysInReply(source,
                                                                 target,
                                                                 reply,
                                                                 is_busy,
                                                                 timeout,
                                                                 NULL);
                success = (migrate_reply != NULL &&
                           migrate_reply->type != REDIS_REPLY_ERROR);
            } else success = 0;
            if (!success) {
                if (migrate_reply != NULL) {
                    if (err) {
                        *err = zmalloc((migrate_reply->len + 1) * sizeof(char));
                        redis_strlcpy(*err, migrate_reply->str, (migrate_reply->len + 1));
                    }
                    printf("\n");
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(source,
                                                      migrate_reply->str);
                }
                goto next;
            }
        }
        if (verbose) {
            printf("%s", dots);
            fflush(stdout);
        }
next:
        if (reply != NULL) freeReplyObject(reply);
        if (migrate_reply != NULL) freeReplyObject(migrate_reply);
        if (dots) zfree(dots);
        if (!success) break;
    }
    return success;
}

/* Move slots between source and target nodes using MIGRATE.
 *
 * Options:
 * CLUSTER_MANAGER_OPT_VERBOSE -- Print a dot for every moved key.
 * CLUSTER_MANAGER_OPT_COLD    -- Move keys without opening slots /
 *                                reconfiguring the nodes.
 * CLUSTER_MANAGER_OPT_UPDATE  -- Update node->slots for source/target nodes.
 * CLUSTER_MANAGER_OPT_QUIET   -- Don't print info messages.
*/
static int clusterManagerMoveSlot(clusterManagerNode *source,
                                  clusterManagerNode *target,
                                  int slot, int opts,  char**err)
{
    if (!(opts & CLUSTER_MANAGER_OPT_QUIET)) {
        printf("Moving slot %d from %s:%d to %s:%d: ", slot, source->ip,
               source->port, target->ip, target->port);
        fflush(stdout);
    }
    if (err != NULL) *err = NULL;
    int pipeline = config.cluster_manager_command.pipeline,
        timeout = config.cluster_manager_command.timeout,
        print_dots = (opts & CLUSTER_MANAGER_OPT_VERBOSE),
        option_cold = (opts & CLUSTER_MANAGER_OPT_COLD),
        success = 1;
    if (!option_cold) {
        success = clusterManagerSetSlot(target, source, slot,
                                        "importing", err);
        if (!success) return 0;
        success = clusterManagerSetSlot(source, target, slot,
                                        "migrating", err);
        if (!success) return 0;
    }
    success = clusterManagerMigrateKeysInSlot(source, target, slot, timeout,
                                              pipeline, print_dots, err);
    if (!(opts & CLUSTER_MANAGER_OPT_QUIET)) printf("\n");
    if (!success) return 0;
    if (!option_cold) {
        /* Set the new node as the owner of the slot in all the known nodes.
         *
         * We inform the target node first. It will propagate the information to
         * the rest of the cluster.
         *
         * If we inform any other node first, it can happen that the target node
         * crashes before it is set as the new owner and then the slot is left
         * without an owner which results in redirect loops. See issue #7116. */
        success = clusterManagerSetSlot(target, target, slot, "node", err);
        if (!success) return 0;

        /* Inform the source node. If the source node has just lost its last
         * slot and the target node has already informed the source node, the
         * source node has turned itself into a replica. This is not an error in
         * this scenario so we ignore it. See issue #9223. */
        success = clusterManagerSetSlot(source, target, slot, "node", err);
        const char *acceptable = "ERR Please use SETSLOT only with masters.";
        if (!success && err && !strncmp(*err, acceptable, strlen(acceptable))) {
            zfree(*err);
            *err = NULL;
        } else if (!success && err) {
            return 0;
        }

        /* We also inform the other nodes to avoid redirects in case the target
         * node is slow to propagate the change to the entire cluster. */
        listIter li;
        listNode *ln;
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n == target || n == source) continue; /* already done */
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
            success = clusterManagerSetSlot(n, target, slot, "node", err);
            if (!success) return 0;
        }
    }
    /* Update the node logical config */
    if (opts & CLUSTER_MANAGER_OPT_UPDATE) {
        source->slots[slot] = 0;
        target->slots[slot] = 1;
    }
    return 1;
}

/* Flush the dirty node configuration by calling replicate for slaves or
 * adding the slots defined in the masters. */
static int clusterManagerFlushNodeConfig(clusterManagerNode *node, char **err) {
    if (!node->dirty) return 0;
    redisReply *reply = NULL;
    int is_err = 0, success = 1;
    if (err != NULL) *err = NULL;
    if (node->replicate != NULL) {
        reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER REPLICATE %s",
                                        node->replicate);
        if (reply == NULL || (is_err = (reply->type == REDIS_REPLY_ERROR))) {
            if (is_err && err != NULL) {
                *err = zmalloc((reply->len + 1) * sizeof(char));
                redis_strlcpy(*err, reply->str, (reply->len + 1));
            }
            success = 0;
            /* If the cluster did not already joined it is possible that
             * the slave does not know the master node yet. So on errors
             * we return ASAP leaving the dirty flag set, to flush the
             * config later. */
            goto cleanup;
        }
    } else {
        int added = clusterManagerAddSlots(node, err);
        if (!added || *err != NULL) success = 0;
    }
    node->dirty = 0;
cleanup:
    if (reply != NULL) freeReplyObject(reply);
    return success;
}

/* Wait until the cluster configuration is consistent. */
static void clusterManagerWaitForClusterJoin(void) {
    printf("Waiting for the cluster to join\n");
    int counter = 0,
        check_after = CLUSTER_JOIN_CHECK_AFTER +
                      (int)(listLength(cluster_manager.nodes) * 0.15f);
    while(!clusterManagerIsConfigConsistent()) {
        printf(".");
        fflush(stdout);
        sleep(1);
        if (++counter > check_after) {
            dict *status = clusterManagerGetLinkStatus();
            dictIterator *iter = NULL;
            if (status != NULL && dictSize(status) > 0) {
                printf("\n");
                clusterManagerLogErr("Warning: %d node(s) may "
                                     "be unreachable\n", dictSize(status));
                iter = dictGetIterator(status);
                dictEntry *entry;
                while ((entry = dictNext(iter)) != NULL) {
                    sds nodeaddr = (sds) dictGetKey(entry);
                    char *node_ip = NULL;
                    int node_port = 0, node_bus_port = 0;
                    list *from = (list *) dictGetVal(entry);
                    if (parseClusterNodeAddress(nodeaddr, &node_ip,
                        &node_port, &node_bus_port) && node_bus_port) {
                        clusterManagerLogErr(" - The port %d of node %s may "
                                             "be unreachable from:\n",
                                             node_bus_port, node_ip);
                    } else {
                        clusterManagerLogErr(" - Node %s may be unreachable "
                                             "from:\n", nodeaddr);
                    }
                    listIter li;
                    listNode *ln;
                    listRewind(from, &li);
                    while ((ln = listNext(&li)) != NULL) {
                        sds from_addr = ln->value;
                        clusterManagerLogErr("   %s\n", from_addr);
                        sdsfree(from_addr);
                    }
                    clusterManagerLogErr("Cluster bus ports must be reachable "
                                         "by every node.\nRemember that "
                                         "cluster bus ports are different "
                                         "from standard instance ports.\n");
                    listEmpty(from);
                }
            }
            if (iter != NULL) dictReleaseIterator(iter);
            if (status != NULL) dictRelease(status);
            counter = 0;
        }
    }
    printf("\n");
}

/* Load node's cluster configuration by calling "CLUSTER NODES" command.
 * Node's configuration (name, replicate, slots, ...) is then updated.
 * If CLUSTER_MANAGER_OPT_GETFRIENDS flag is set into 'opts' argument,
 * and node already knows other nodes, the node's friends list is populated
 * with the other nodes info. */
static int clusterManagerNodeLoadInfo(clusterManagerNode *node, int opts,
                                      char **err)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER NODES");
    int success = 1;
    *err = NULL;
    if (!clusterManagerCheckRedisReply(node, reply, err)) {
        success = 0;
        goto cleanup;
    }
    int getfriends = (opts & CLUSTER_MANAGER_OPT_GETFRIENDS);
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *name = NULL, *addr = NULL, *flags = NULL, *master_id = NULL,
             *ping_sent = NULL, *ping_recv = NULL, *config_epoch = NULL,
             *link_status = NULL;
        UNUSED(link_status);
        int i = 0;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            switch(i++){
            case 0: name = token; break;
            case 1: addr = token; break;
            case 2: flags = token; break;
            case 3: master_id = token; break;
            case 4: ping_sent = token; break;
            case 5: ping_recv = token; break;
            case 6: config_epoch = token; break;
            case 7: link_status = token; break;
            }
            if (i == 8) break; // Slots
        }
        if (!flags) {
            success = 0;
            goto cleanup;
        }

        char *ip = NULL;
        int port = 0, bus_port = 0;
        if (addr == NULL || !parseClusterNodeAddress(addr, &ip, &port, &bus_port)) {
            fprintf(stderr, "Error: invalid CLUSTER NODES reply\n");
            success = 0;
            goto cleanup;
        }

        int myself = (strstr(flags, "myself") != NULL);
        clusterManagerNode *currentNode = NULL;
        if (myself) {
            /* bus-port could be wrong, correct it here, see clusterManagerNewNode. */
            node->bus_port = bus_port;
            node->flags |= CLUSTER_MANAGER_FLAG_MYSELF;
            currentNode = node;
            clusterManagerNodeResetSlots(node);
            if (i == 8) {
                int remaining = strlen(line);
                while (remaining > 0) {
                    p = strchr(line, ' ');
                    if (p == NULL) p = line + remaining;
                    remaining -= (p - line);

                    char *slotsdef = line;
                    *p = '\0';
                    if (remaining) {
                        line = p + 1;
                        remaining--;
                    } else line = p;
                    char *dash = NULL;
                    if (slotsdef[0] == '[') {
                        slotsdef++;
                        if ((p = strstr(slotsdef, "->-"))) { // Migrating
                            *p = '\0';
                            p += 3;
                            char *closing_bracket = strchr(p, ']');
                            if (closing_bracket) *closing_bracket = '\0';
                            sds slot = sdsnew(slotsdef);
                            sds dst = sdsnew(p);
                            node->migrating_count += 2;
                            node->migrating = zrealloc(node->migrating,
                                (node->migrating_count * sizeof(sds)));
                            node->migrating[node->migrating_count - 2] =
                                slot;
                            node->migrating[node->migrating_count - 1] =
                                dst;
                        }  else if ((p = strstr(slotsdef, "-<-"))) {//Importing
                            *p = '\0';
                            p += 3;
                            char *closing_bracket = strchr(p, ']');
                            if (closing_bracket) *closing_bracket = '\0';
                            sds slot = sdsnew(slotsdef);
                            sds src = sdsnew(p);
                            node->importing_count += 2;
                            node->importing = zrealloc(node->importing,
                                (node->importing_count * sizeof(sds)));
                            node->importing[node->importing_count - 2] =
                                slot;
                            node->importing[node->importing_count - 1] =
                                src;
                        }
                    } else if ((dash = strchr(slotsdef, '-')) != NULL) {
                        p = dash;
                        int start, stop;
                        *p = '\0';
                        start = atoi(slotsdef);
                        stop = atoi(p + 1);
                        node->slots_count += (stop - (start - 1));
                        while (start <= stop) node->slots[start++] = 1;
                    } else if (p > slotsdef) {
                        node->slots[atoi(slotsdef)] = 1;
                        node->slots_count++;
                    }
                }
            }
            node->dirty = 0;
        } else if (!getfriends) {
            if (!(node->flags & CLUSTER_MANAGER_FLAG_MYSELF)) continue;
            else break;
        } else {
            currentNode = clusterManagerNewNode(sdsnew(ip), port, bus_port);
            currentNode->flags |= CLUSTER_MANAGER_FLAG_FRIEND;
            if (node->friends == NULL) node->friends = listCreate();
            listAddNodeTail(node->friends, currentNode);
        }
        if (name != NULL) {
            if (currentNode->name) sdsfree(currentNode->name);
            currentNode->name = sdsnew(name);
        }
        if (currentNode->flags_str != NULL)
            freeClusterManagerNodeFlags(currentNode->flags_str);
        currentNode->flags_str = listCreate();
        int flag_len;
        while ((flag_len = strlen(flags)) > 0) {
            sds flag = NULL;
            char *fp = strchr(flags, ',');
            if (fp) {
                *fp = '\0';
                flag = sdsnew(flags);
                flags = fp + 1;
            } else {
                flag = sdsnew(flags);
                flags += flag_len;
            }
            if (strcmp(flag, "noaddr") == 0)
                currentNode->flags |= CLUSTER_MANAGER_FLAG_NOADDR;
            else if (strcmp(flag, "disconnected") == 0)
                currentNode->flags |= CLUSTER_MANAGER_FLAG_DISCONNECT;
            else if (strcmp(flag, "fail") == 0)
                currentNode->flags |= CLUSTER_MANAGER_FLAG_FAIL;
            else if (strcmp(flag, "slave") == 0) {
                currentNode->flags |= CLUSTER_MANAGER_FLAG_SLAVE;
                if (master_id != NULL) {
                    if (currentNode->replicate) sdsfree(currentNode->replicate);
                    currentNode->replicate = sdsnew(master_id);
                }
            }
            listAddNodeTail(currentNode->flags_str, flag);
        }
        if (config_epoch != NULL)
            currentNode->current_epoch = atoll(config_epoch);
        if (ping_sent != NULL) currentNode->ping_sent = atoll(ping_sent);
        if (ping_recv != NULL) currentNode->ping_recv = atoll(ping_recv);
        if (!getfriends && myself) break;
    }
cleanup:
    if (reply) freeReplyObject(reply);
    return success;
}

/* Retrieves info about the cluster using argument 'node' as the starting
 * point. All nodes will be loaded inside the cluster_manager.nodes list.
 * Warning: if something goes wrong, it will free the starting node before
 * returning 0. */
static int clusterManagerLoadInfoFromNode(clusterManagerNode *node) {
    if (node->context == NULL && !clusterManagerNodeConnect(node)) {
        freeClusterManagerNode(node);
        return 0;
    }
    char *e = NULL;
    if (!clusterManagerNodeIsCluster(node, &e)) {
        clusterManagerPrintNotClusterNodeError(node, e);
        if (e) zfree(e);
        freeClusterManagerNode(node);
        return 0;
    }
    e = NULL;
    if (!clusterManagerNodeLoadInfo(node, CLUSTER_MANAGER_OPT_GETFRIENDS, &e)) {
        if (e) {
            CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, e);
            zfree(e);
        }
        freeClusterManagerNode(node);
        return 0;
    }
    listIter li;
    listNode *ln;
    if (cluster_manager.nodes != NULL) {
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL)
            freeClusterManagerNode((clusterManagerNode *) ln->value);
        listRelease(cluster_manager.nodes);
    }
    cluster_manager.nodes = listCreate();
    listAddNodeTail(cluster_manager.nodes, node);
    if (node->friends != NULL) {
        listRewind(node->friends, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *friend = ln->value;
            if (!friend->ip || !friend->port) goto invalid_friend;
            if (!friend->context && !clusterManagerNodeConnect(friend))
                goto invalid_friend;
            e = NULL;
            if (clusterManagerNodeLoadInfo(friend, 0, &e)) {
                if (friend->flags & (CLUSTER_MANAGER_FLAG_NOADDR |
                                     CLUSTER_MANAGER_FLAG_DISCONNECT |
                                     CLUSTER_MANAGER_FLAG_FAIL))
                {
                    goto invalid_friend;
                }
                listAddNodeTail(cluster_manager.nodes, friend);
            } else {
                clusterManagerLogErr("[ERR] Unable to load info for "
                                     "node %s:%d\n",
                                     friend->ip, friend->port);
                goto invalid_friend;
            }
            continue;
invalid_friend:
            if (!(friend->flags & CLUSTER_MANAGER_FLAG_SLAVE))
                cluster_manager.unreachable_masters++;
            freeClusterManagerNode(friend);
        }
        listRelease(node->friends);
        node->friends = NULL;
    }
    // Count replicas for each node
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->replicate != NULL) {
            clusterManagerNode *master = clusterManagerNodeByName(n->replicate);
            if (master == NULL) {
                clusterManagerLogWarn("*** WARNING: %s:%d claims to be "
                                      "slave of unknown node ID %s.\n",
                                      n->ip, n->port, n->replicate);
            } else master->replicas_count++;
        }
    }
    return 1;
}

/* Compare functions used by various sorting operations. */
int clusterManagerSlotCompare(const void *slot1, const void *slot2) {
    const char **i1 = (const char **)slot1;
    const char **i2 = (const char **)slot2;
    return strcmp(*i1, *i2);
}

int clusterManagerSlotCountCompareDesc(const void *n1, const void *n2) {
    clusterManagerNode *node1 = *((clusterManagerNode **) n1);
    clusterManagerNode *node2 = *((clusterManagerNode **) n2);
    return node2->slots_count - node1->slots_count;
}

int clusterManagerCompareNodeBalance(const void *n1, const void *n2) {
    clusterManagerNode *node1 = *((clusterManagerNode **) n1);
    clusterManagerNode *node2 = *((clusterManagerNode **) n2);
    return node1->balance - node2->balance;
}

static sds clusterManagerGetConfigSignature(clusterManagerNode *node) {
    sds signature = NULL;
    int node_count = 0, i = 0, name_len = 0;
    char **node_configs = NULL;
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER NODES");
    if (reply == NULL || reply->type == REDIS_REPLY_ERROR)
        goto cleanup;
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        i = 0;
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *nodename = NULL;
        int tot_size = 0;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            if (i == 0) {
                nodename = token;
                tot_size = (p - token);
                name_len = tot_size++; // Make room for ':' in tot_size
            }
            if (++i == 8) break;
        }
        if (i != 8) continue;
        if (nodename == NULL) continue;
        int remaining = strlen(line);
        if (remaining == 0) continue;
        char **slots = NULL;
        int c = 0;
        while (remaining > 0) {
            p = strchr(line, ' ');
            if (p == NULL) p = line + remaining;
            int size = (p - line);
            remaining -= size;
            tot_size += size;
            char *slotsdef = line;
            *p = '\0';
            if (remaining) {
                line = p + 1;
                remaining--;
            } else line = p;
            if (slotsdef[0] != '[') {
                c++;
                slots = zrealloc(slots, (c * sizeof(char *)));
                slots[c - 1] = slotsdef;
            }
        }
        if (c > 0) {
            if (c > 1)
                qsort(slots, c, sizeof(char *), clusterManagerSlotCompare);
            node_count++;
            node_configs =
                zrealloc(node_configs, (node_count * sizeof(char *)));
            /* Make room for '|' separators. */
            tot_size += (sizeof(char) * (c - 1));
            char *cfg = zmalloc((sizeof(char) * tot_size) + 1);
            memcpy(cfg, nodename, name_len);
            char *sp = cfg + name_len;
            *(sp++) = ':';
            for (i = 0; i < c; i++) {
                if (i > 0) *(sp++) = ',';
                int slen = strlen(slots[i]);
                memcpy(sp, slots[i], slen);
                sp += slen;
            }
            *(sp++) = '\0';
            node_configs[node_count - 1] = cfg;
        }
        zfree(slots);
    }
    if (node_count > 0) {
        if (node_count > 1) {
            qsort(node_configs, node_count, sizeof(char *),
                  clusterManagerSlotCompare);
        }
        signature = sdsempty();
        for (i = 0; i < node_count; i++) {
            if (i > 0) signature = sdscatprintf(signature, "%c", '|');
            signature = sdscatfmt(signature, "%s", node_configs[i]);
        }
    }
cleanup:
    if (reply != NULL) freeReplyObject(reply);
    if (node_configs != NULL) {
        for (i = 0; i < node_count; i++) zfree(node_configs[i]);
        zfree(node_configs);
    }
    return signature;
}

static int clusterManagerIsConfigConsistent(void) {
    if (cluster_manager.nodes == NULL) return 0;
    int consistent = (listLength(cluster_manager.nodes) <= 1);
    // If the Cluster has only one node, it's always consistent
    if (consistent) return 1;
    sds first_cfg = NULL;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        sds cfg = clusterManagerGetConfigSignature(node);
        if (cfg == NULL) {
            consistent = 0;
            break;
        }
        if (first_cfg == NULL) first_cfg = cfg;
        else {
            consistent = !sdscmp(first_cfg, cfg);
            sdsfree(cfg);
            if (!consistent) break;
        }
    }
    if (first_cfg != NULL) sdsfree(first_cfg);
    return consistent;
}

static list *clusterManagerGetDisconnectedLinks(clusterManagerNode *node) {
    list *links = NULL;
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER NODES");
    if (!clusterManagerCheckRedisReply(node, reply, NULL)) goto cleanup;
    links = listCreate();
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        int i = 0;
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *nodename = NULL, *addr = NULL, *flags = NULL, *link_status = NULL;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            if (i == 0) nodename = token;
            else if (i == 1) addr = token;
            else if (i == 2) flags = token;
            else if (i == 7) link_status = token;
            else if (i == 8) break;
            i++;
        }
        if (i == 7) link_status = line;
        if (nodename == NULL || addr == NULL || flags == NULL ||
            link_status == NULL) continue;
        if (strstr(flags, "myself") != NULL) continue;
        int disconnected = ((strstr(flags, "disconnected") != NULL) ||
                            (strstr(link_status, "disconnected")));
        int handshaking = (strstr(flags, "handshake") != NULL);
        if (disconnected || handshaking) {
            clusterManagerLink *link = zmalloc(sizeof(*link));
            link->node_name = sdsnew(nodename);
            link->node_addr = sdsnew(addr);
            link->connected = 0;
            link->handshaking = handshaking;
            listAddNodeTail(links, link);
        }
    }
cleanup:
    if (reply != NULL) freeReplyObject(reply);
    return links;
}

/* Check for disconnected cluster links. It returns a dict whose keys
 * are the unreachable node addresses and the values are lists of
 * node addresses that cannot reach the unreachable node. */
static dict *clusterManagerGetLinkStatus(void) {
    if (cluster_manager.nodes == NULL) return NULL;
    dict *status = dictCreate(&clusterManagerLinkDictType);
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        list *links = clusterManagerGetDisconnectedLinks(node);
        if (links) {
            listIter lli;
            listNode *lln;
            listRewind(links, &lli);
            while ((lln = listNext(&lli)) != NULL) {
                clusterManagerLink *link = lln->value;
                list *from = NULL;
                dictEntry *entry = dictFind(status, link->node_addr);
                if (entry) from = dictGetVal(entry);
                else {
                    from = listCreate();
                    dictAdd(status, sdsdup(link->node_addr), from);
                }
                sds myaddr = sdsempty();
                myaddr = sdscatfmt(myaddr, "%s:%u", node->ip, node->port);
                listAddNodeTail(from, myaddr);
                sdsfree(link->node_name);
                sdsfree(link->node_addr);
                zfree(link);
            }
            listRelease(links);
        }
    }
    return status;
}

/* Add the error string to cluster_manager.errors and print it. */
static void clusterManagerOnError(sds err) {
    if (cluster_manager.errors == NULL)
        cluster_manager.errors = listCreate();
    listAddNodeTail(cluster_manager.errors, err);
    clusterManagerLogErr("%s\n", (char *) err);
}

/* Check the slots coverage of the cluster. The 'all_slots' argument must be
 * and array of 16384 bytes. Every covered slot will be set to 1 in the
 * 'all_slots' array. The function returns the total number if covered slots.*/
static int clusterManagerGetCoveredSlots(char *all_slots) {
    if (cluster_manager.nodes == NULL) return 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    int totslots = 0, i;
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
            if (node->slots[i] && !all_slots[i]) {
                all_slots[i] = 1;
                totslots++;
            }
        }
    }
    return totslots;
}

static void clusterManagerPrintSlotsList(list *slots) {
    clusterManagerNode n = {0};
    listIter li;
    listNode *ln;
    listRewind(slots, &li);
    while ((ln = listNext(&li)) != NULL) {
        int slot = atoi(ln->value);
        if (slot >= 0 && slot < CLUSTER_MANAGER_SLOTS)
            n.slots[slot] = 1;
    }
    sds nodeslist = clusterManagerNodeSlotsString(&n);
    printf("%s\n", nodeslist);
    sdsfree(nodeslist);
}

/* Return the node, among 'nodes' with the greatest number of keys
 * in the specified slot. */
static clusterManagerNode * clusterManagerGetNodeWithMostKeysInSlot(list *nodes,
                                                                    int slot,
                                                                    char **err)
{
    clusterManagerNode *node = NULL;
    int numkeys = 0;
    listIter li;
    listNode *ln;
    listRewind(nodes, &li);
    if (err) *err = NULL;
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
            continue;
        redisReply *r =
            CLUSTER_MANAGER_COMMAND(n, "CLUSTER COUNTKEYSINSLOT %d", slot);
        int success = clusterManagerCheckRedisReply(n, r, err);
        if (success) {
            if (r->integer > numkeys || node == NULL) {
                numkeys = r->integer;
                node = n;
            }
        }
        if (r != NULL) freeReplyObject(r);
        /* If the reply contains errors */
        if (!success) {
            if (err != NULL && *err != NULL)
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(n, err);
            node = NULL;
            break;
        }
    }
    return node;
}

/* This function returns the master that has the least number of replicas
 * in the cluster. If there are multiple masters with the same smaller
 * number of replicas, one at random is returned. */

static clusterManagerNode *clusterManagerNodeWithLeastReplicas(void) {
    clusterManagerNode *node = NULL;
    int lowest_count = 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        if (node == NULL || n->replicas_count < lowest_count) {
            node = n;
            lowest_count = n->replicas_count;
        }
    }
    return node;
}

/* This function returns a random master node, return NULL if none */

static clusterManagerNode *clusterManagerNodeMasterRandom(void) {
    int master_count = 0;
    int idx;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        master_count++;
    }

    assert(master_count > 0);
    srand(time(NULL));
    idx = rand() % master_count;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        if (!idx--) {
            return n;
        }
    }
    /* Can not be reached */
    assert(0);
}

static int clusterManagerFixSlotsCoverage(char *all_slots) {
    int force_fix = config.cluster_manager_command.flags &
                    CLUSTER_MANAGER_CMD_FLAG_FIX_WITH_UNREACHABLE_MASTERS;

    if (cluster_manager.unreachable_masters > 0 && !force_fix) {
        clusterManagerLogWarn("*** Fixing slots coverage with %d unreachable masters is dangerous: redis-cli will assume that slots about masters that are not reachable are not covered, and will try to reassign them to the reachable nodes. This can cause data loss and is rarely what you want to do. If you really want to proceed use the --cluster-fix-with-unreachable-masters option.\n", cluster_manager.unreachable_masters);
        exit(1);
    }

    int i, fixed = 0;
    list *none = NULL, *single = NULL, *multi = NULL;
    clusterManagerLogInfo(">>> Fixing slots coverage...\n");
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        int covered = all_slots[i];
        if (!covered) {
            sds slot = sdsfromlonglong((long long) i);
            list *slot_nodes = listCreate();
            sds slot_nodes_str = sdsempty();
            listIter li;
            listNode *ln;
            listRewind(cluster_manager.nodes, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
                    continue;
                redisReply *reply = CLUSTER_MANAGER_COMMAND(n,
                    "CLUSTER GETKEYSINSLOT %d %d", i, 1);
                if (!clusterManagerCheckRedisReply(n, reply, NULL)) {
                    fixed = -1;
                    if (reply) freeReplyObject(reply);
                    goto cleanup;
                }
                assert(reply->type == REDIS_REPLY_ARRAY);
                if (reply->elements > 0) {
                    listAddNodeTail(slot_nodes, n);
                    if (listLength(slot_nodes) > 1)
                        slot_nodes_str = sdscat(slot_nodes_str, ", ");
                    slot_nodes_str = sdscatfmt(slot_nodes_str,
                                               "%s:%u", n->ip, n->port);
                }
                freeReplyObject(reply);
            }
            sdsfree(slot_nodes_str);
            dictAdd(clusterManagerUncoveredSlots, slot, slot_nodes);
        }
    }

    /* For every slot, take action depending on the actual condition:
     * 1) No node has keys for this slot.
     * 2) A single node has keys for this slot.
     * 3) Multiple nodes have keys for this slot. */
    none = listCreate();
    single = listCreate();
    multi = listCreate();
    dictIterator *iter = dictGetIterator(clusterManagerUncoveredSlots);
    dictEntry *entry;
    while ((entry = dictNext(iter)) != NULL) {
        sds slot = (sds) dictGetKey(entry);
        list *nodes = (list *) dictGetVal(entry);
        switch (listLength(nodes)){
        case 0: listAddNodeTail(none, slot); break;
        case 1: listAddNodeTail(single, slot); break;
        default: listAddNodeTail(multi, slot); break;
        }
    }
    dictReleaseIterator(iter);

    /* we want explicit manual confirmation from users for all the fix cases */
    int ignore_force = 1;

    /*  Handle case "1": keys in no node. */
    if (listLength(none) > 0) {
        printf("The following uncovered slots have no keys "
               "across the cluster:\n");
        clusterManagerPrintSlotsList(none);
        if (confirmWithYes("Fix these slots by covering with a random node?",
                           ignore_force)) {
            listIter li;
            listNode *ln;
            listRewind(none, &li);
            while ((ln = listNext(&li)) != NULL) {
                sds slot = ln->value;
                int s = atoi(slot);
                clusterManagerNode *n = clusterManagerNodeMasterRandom();
                clusterManagerLogInfo(">>> Covering slot %s with %s:%d\n",
                                      slot, n->ip, n->port);
                if (!clusterManagerSetSlotOwner(n, s, 0)) {
                    fixed = -1;
                    goto cleanup;
                }
                /* Since CLUSTER ADDSLOTS succeeded, we also update the slot
                 * info into the node struct, in order to keep it synced */
                n->slots[s] = 1;
                fixed++;
            }
        }
    }

    /*  Handle case "2": keys only in one node. */
    if (listLength(single) > 0) {
        printf("The following uncovered slots have keys in just one node:\n");
        clusterManagerPrintSlotsList(single);
        if (confirmWithYes("Fix these slots by covering with those nodes?",
                           ignore_force)) {
            listIter li;
            listNode *ln;
            listRewind(single, &li);
            while ((ln = listNext(&li)) != NULL) {
                sds slot = ln->value;
                int s = atoi(slot);
                dictEntry *entry = dictFind(clusterManagerUncoveredSlots, slot);
                assert(entry != NULL);
                list *nodes = (list *) dictGetVal(entry);
                listNode *fn = listFirst(nodes);
                assert(fn != NULL);
                clusterManagerNode *n = fn->value;
                clusterManagerLogInfo(">>> Covering slot %s with %s:%d\n",
                                      slot, n->ip, n->port);
                if (!clusterManagerSetSlotOwner(n, s, 0)) {
                    fixed = -1;
                    goto cleanup;
                }
                /* Since CLUSTER ADDSLOTS succeeded, we also update the slot
                 * info into the node struct, in order to keep it synced */
                n->slots[atoi(slot)] = 1;
                fixed++;
            }
        }
    }

    /* Handle case "3": keys in multiple nodes. */
    if (listLength(multi) > 0) {
        printf("The following uncovered slots have keys in multiple nodes:\n");
        clusterManagerPrintSlotsList(multi);
        if (confirmWithYes("Fix these slots by moving keys "
                           "into a single node?", ignore_force)) {
            listIter li;
            listNode *ln;
            listRewind(multi, &li);
            while ((ln = listNext(&li)) != NULL) {
                sds slot = ln->value;
                dictEntry *entry = dictFind(clusterManagerUncoveredSlots, slot);
                assert(entry != NULL);
                list *nodes = (list *) dictGetVal(entry);
                int s = atoi(slot);
                clusterManagerNode *target =
                    clusterManagerGetNodeWithMostKeysInSlot(nodes, s, NULL);
                if (target == NULL) {
                    fixed = -1;
                    goto cleanup;
                }
                clusterManagerLogInfo(">>> Covering slot %s moving keys "
                                      "to %s:%d\n", slot,
                                      target->ip, target->port);
                if (!clusterManagerSetSlotOwner(target, s, 1)) {
                    fixed = -1;
                    goto cleanup;
                }
                /* Since CLUSTER ADDSLOTS succeeded, we also update the slot
                 * info into the node struct, in order to keep it synced */
                target->slots[atoi(slot)] = 1;
                listIter nli;
                listNode *nln;
                listRewind(nodes, &nli);
                while ((nln = listNext(&nli)) != NULL) {
                    clusterManagerNode *src = nln->value;
                    if (src == target) continue;
                    /* Assign the slot to target node in the source node. */
                    if (!clusterManagerSetSlot(src, target, s, "NODE", NULL))
                        fixed = -1;
                    if (fixed < 0) goto cleanup;
                    /* Set the source node in 'importing' state
                     * (even if we will actually migrate keys away)
                     * in order to avoid receiving redirections
                     * for MIGRATE. */
                    if (!clusterManagerSetSlot(src, target, s,
                                               "IMPORTING", NULL)) fixed = -1;
                    if (fixed < 0) goto cleanup;
                    int opts = CLUSTER_MANAGER_OPT_VERBOSE |
                               CLUSTER_MANAGER_OPT_COLD;
                    if (!clusterManagerMoveSlot(src, target, s, opts, NULL)) {
                        fixed = -1;
                        goto cleanup;
                    }
                    if (!clusterManagerClearSlotStatus(src, s))
                        fixed = -1;
                    if (fixed < 0) goto cleanup;
                }
                fixed++;
            }
        }
    }
cleanup:
    if (none) listRelease(none);
    if (single) listRelease(single);
    if (multi) listRelease(multi);
    return fixed;
}

/* Slot 'slot' was found to be in importing or migrating state in one or
 * more nodes. This function fixes this condition by migrating keys where
 * it seems more sensible. */
static int clusterManagerFixOpenSlot(int slot) {
    int force_fix = config.cluster_manager_command.flags &
                    CLUSTER_MANAGER_CMD_FLAG_FIX_WITH_UNREACHABLE_MASTERS;

    if (cluster_manager.unreachable_masters > 0 && !force_fix) {
        clusterManagerLogWarn("*** Fixing open slots with %d unreachable masters is dangerous: redis-cli will assume that slots about masters that are not reachable are not covered, and will try to reassign them to the reachable nodes. This can cause data loss and is rarely what you want to do. If you really want to proceed use the --cluster-fix-with-unreachable-masters option.\n", cluster_manager.unreachable_masters);
        exit(1);
    }

    clusterManagerLogInfo(">>> Fixing open slot %d\n", slot);
    /* Try to obtain the current slot owner, according to the current
     * nodes configuration. */
    int success = 1;
    list *owners = listCreate();    /* List of nodes claiming some ownership.
                                       it could be stating in the configuration
                                       to have the node ownership, or just
                                       holding keys for such slot. */
    list *migrating = listCreate();
    list *importing = listCreate();
    sds migrating_str = sdsempty();
    sds importing_str = sdsempty();
    clusterManagerNode *owner = NULL; /* The obvious slot owner if any. */

    /* Iterate all the nodes, looking for potential owners of this slot. */
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        if (n->slots[slot]) {
            listAddNodeTail(owners, n);
        } else {
            redisReply *r = CLUSTER_MANAGER_COMMAND(n,
                "CLUSTER COUNTKEYSINSLOT %d", slot);
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (success && r->integer > 0) {
                clusterManagerLogWarn("*** Found keys about slot %d "
                                      "in non-owner node %s:%d!\n", slot,
                                      n->ip, n->port);
                listAddNodeTail(owners, n);
            }
            if (r) freeReplyObject(r);
            if (!success) goto cleanup;
        }
    }

    /* If we have only a single potential owner for this slot,
     * set it as "owner". */
    if (listLength(owners) == 1) owner = listFirst(owners)->value;

    /* Scan the list of nodes again, in order to populate the
     * list of nodes in importing or migrating state for
     * this slot. */
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        int is_migrating = 0, is_importing = 0;
        if (n->migrating) {
            for (int i = 0; i < n->migrating_count; i += 2) {
                sds migrating_slot = n->migrating[i];
                if (atoi(migrating_slot) == slot) {
                    char *sep = (listLength(migrating) == 0 ? "" : ",");
                    migrating_str = sdscatfmt(migrating_str, "%s%s:%u",
                                              sep, n->ip, n->port);
                    listAddNodeTail(migrating, n);
                    is_migrating = 1;
                    break;
                }
            }
        }
        if (!is_migrating && n->importing) {
            for (int i = 0; i < n->importing_count; i += 2) {
                sds importing_slot = n->importing[i];
                if (atoi(importing_slot) == slot) {
                    char *sep = (listLength(importing) == 0 ? "" : ",");
                    importing_str = sdscatfmt(importing_str, "%s%s:%u",
                                              sep, n->ip, n->port);
                    listAddNodeTail(importing, n);
                    is_importing = 1;
                    break;
                }
            }
        }

        /* If the node is neither migrating nor importing and it's not
         * the owner, then is added to the importing list in case
         * it has keys in the slot. */
        if (!is_migrating && !is_importing && n != owner) {
            redisReply *r = CLUSTER_MANAGER_COMMAND(n,
                "CLUSTER COUNTKEYSINSLOT %d", slot);
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (success && r->integer > 0) {
                clusterManagerLogWarn("*** Found keys about slot %d "
                                      "in node %s:%d!\n", slot, n->ip,
                                      n->port);
                char *sep = (listLength(importing) == 0 ? "" : ",");
                importing_str = sdscatfmt(importing_str, "%s%s:%u",
                                          sep, n->ip, n->port);
                listAddNodeTail(importing, n);
            }
            if (r) freeReplyObject(r);
            if (!success) goto cleanup;
        }
    }
    if (sdslen(migrating_str) > 0)
        printf("Set as migrating in: %s\n", migrating_str);
    if (sdslen(importing_str) > 0)
        printf("Set as importing in: %s\n", importing_str);

    /* If there is no slot owner, set as owner the node with the biggest
     * number of keys, among the set of migrating / importing nodes. */
    if (owner == NULL) {
        clusterManagerLogInfo(">>> No single clear owner for the slot, "
                              "selecting an owner by # of keys...\n");
        owner = clusterManagerGetNodeWithMostKeysInSlot(cluster_manager.nodes,
                                                        slot, NULL);
        // If we still don't have an owner, we can't fix it.
        if (owner == NULL) {
            clusterManagerLogErr("[ERR] Can't select a slot owner. "
                                 "Impossible to fix.\n");
            success = 0;
            goto cleanup;
        }

        // Use ADDSLOTS to assign the slot.
        clusterManagerLogWarn("*** Configuring %s:%d as the slot owner\n",
                              owner->ip, owner->port);
        success = clusterManagerClearSlotStatus(owner, slot);
        if (!success) goto cleanup;
        success = clusterManagerSetSlotOwner(owner, slot, 0);
        if (!success) goto cleanup;
        /* Since CLUSTER ADDSLOTS succeeded, we also update the slot
         * info into the node struct, in order to keep it synced */
        owner->slots[slot] = 1;
        /* Remove the owner from the list of migrating/importing
         * nodes. */
        clusterManagerRemoveNodeFromList(migrating, owner);
        clusterManagerRemoveNodeFromList(importing, owner);
    }

    /* If there are multiple owners of the slot, we need to fix it
     * so that a single node is the owner and all the other nodes
     * are in importing state. Later the fix can be handled by one
     * of the base cases above.
     *
     * Note that this case also covers multiple nodes having the slot
     * in migrating state, since migrating is a valid state only for
     * slot owners. */
    if (listLength(owners) > 1) {
        /* Owner cannot be NULL at this point, since if there are more owners,
         * the owner has been set in the previous condition (owner == NULL). */
        assert(owner != NULL);
        listRewind(owners, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n == owner) continue;
            success = clusterManagerDelSlot(n, slot, 1);
            if (!success) goto cleanup;
            n->slots[slot] = 0;
            /* Assign the slot to the owner in the node 'n' configuration.' */
            success = clusterManagerSetSlot(n, owner, slot, "node", NULL);
            if (!success) goto cleanup;
            success = clusterManagerSetSlot(n, owner, slot, "importing", NULL);
            if (!success) goto cleanup;
            /* Avoid duplicates. */
            clusterManagerRemoveNodeFromList(importing, n);
            listAddNodeTail(importing, n);
            /* Ensure that the node is not in the migrating list. */
            clusterManagerRemoveNodeFromList(migrating, n);
        }
    }
    int move_opts = CLUSTER_MANAGER_OPT_VERBOSE;

    /* Case 1: The slot is in migrating state in one node, and in
     *         importing state in 1 node. That's trivial to address. */
    if (listLength(migrating) == 1 && listLength(importing) == 1) {
        clusterManagerNode *src = listFirst(migrating)->value;
        clusterManagerNode *dst = listFirst(importing)->value;
        clusterManagerLogInfo(">>> Case 1: Moving slot %d from "
                              "%s:%d to %s:%d\n", slot,
                              src->ip, src->port, dst->ip, dst->port);
        move_opts |= CLUSTER_MANAGER_OPT_UPDATE;
        success = clusterManagerMoveSlot(src, dst, slot, move_opts, NULL);
    }

    /* Case 2: There are multiple nodes that claim the slot as importing,
     * they probably got keys about the slot after a restart so opened
     * the slot. In this case we just move all the keys to the owner
     * according to the configuration. */
    else if (listLength(migrating) == 0 && listLength(importing) > 0) {
        clusterManagerLogInfo(">>> Case 2: Moving all the %d slot keys to its "
                              "owner %s:%d\n", slot, owner->ip, owner->port);
        move_opts |= CLUSTER_MANAGER_OPT_COLD;
        listRewind(importing, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n == owner) continue;
            success = clusterManagerMoveSlot(n, owner, slot, move_opts, NULL);
            if (!success) goto cleanup;
            clusterManagerLogInfo(">>> Setting %d as STABLE in "
                                  "%s:%d\n", slot, n->ip, n->port);
            success = clusterManagerClearSlotStatus(n, slot);
            if (!success) goto cleanup;
        }
        /* Since the slot has been moved in "cold" mode, ensure that all the
         * other nodes update their own configuration about the slot itself. */
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n == owner) continue;
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
            success = clusterManagerSetSlot(n, owner, slot, "NODE", NULL);
            if (!success) goto cleanup;
        }
    }

    /* Case 3: The slot is in migrating state in one node but multiple
     * other nodes claim to be in importing state and don't have any key in
     * the slot. We search for the importing node having the same ID as
     * the destination node of the migrating node.
     * In that case we move the slot from the migrating node to this node and
     * we close the importing states on all the other importing nodes.
     * If no importing node has the same ID as the destination node of the
     * migrating node, the slot's state is closed on both the migrating node
     * and the importing nodes. */
    else if (listLength(migrating) == 1 && listLength(importing) > 1) {
        int try_to_fix = 1;
        clusterManagerNode *src = listFirst(migrating)->value;
        clusterManagerNode *dst = NULL;
        sds target_id = NULL;
        for (int i = 0; i < src->migrating_count; i += 2) {
            sds migrating_slot = src->migrating[i];
            if (atoi(migrating_slot) == slot) {
                target_id = src->migrating[i + 1];
                break;
            }
        }
        assert(target_id != NULL);
        listIter li;
        listNode *ln;
        listRewind(importing, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            int count = clusterManagerCountKeysInSlot(n, slot);
            if (count > 0) {
                try_to_fix = 0;
                break;
            }
            if (strcmp(n->name, target_id) == 0) dst = n;
        }
        if (!try_to_fix) goto unhandled_case;
        if (dst != NULL) {
            clusterManagerLogInfo(">>> Case 3: Moving slot %d from %s:%d to "
                                  "%s:%d and closing it on all the other "
                                  "importing nodes.\n",
                                  slot, src->ip, src->port,
                                  dst->ip, dst->port);
            /* Move the slot to the destination node. */
            success = clusterManagerMoveSlot(src, dst, slot, move_opts, NULL);
            if (!success) goto cleanup;
            /* Close slot on all the other importing nodes. */
            listRewind(importing, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (dst == n) continue;
                success = clusterManagerClearSlotStatus(n, slot);
                if (!success) goto cleanup;
            }
        } else {
            clusterManagerLogInfo(">>> Case 3: Closing slot %d on both "
                                  "migrating and importing nodes.\n", slot);
            /* Close the slot on both the migrating node and the importing
             * nodes. */
            success = clusterManagerClearSlotStatus(src, slot);
            if (!success) goto cleanup;
            listRewind(importing, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                success = clusterManagerClearSlotStatus(n, slot);
                if (!success) goto cleanup;
            }
        }
    } else {
        int try_to_close_slot = (listLength(importing) == 0 &&
                                 listLength(migrating) == 1);
        if (try_to_close_slot) {
            clusterManagerNode *n = listFirst(migrating)->value;
            if (!owner || owner != n) {
                redisReply *r = CLUSTER_MANAGER_COMMAND(n,
                    "CLUSTER GETKEYSINSLOT %d %d", slot, 10);
                success = clusterManagerCheckRedisReply(n, r, NULL);
                if (r) {
                    if (success) try_to_close_slot = (r->elements == 0);
                    freeReplyObject(r);
                }
                if (!success) goto cleanup;
            }
        }
        /* Case 4: There are no slots claiming to be in importing state, but
         * there is a migrating node that actually don't have any key or is the
         * slot owner. We can just close the slot, probably a reshard
         * interrupted in the middle. */
        if (try_to_close_slot) {
            clusterManagerNode *n = listFirst(migrating)->value;
            clusterManagerLogInfo(">>> Case 4: Closing slot %d on %s:%d\n",
                                  slot, n->ip, n->port);
            redisReply *r = CLUSTER_MANAGER_COMMAND(n, "CLUSTER SETSLOT %d %s",
                                                    slot, "STABLE");
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (r) freeReplyObject(r);
            if (!success) goto cleanup;
        } else {
unhandled_case:
            success = 0;
            clusterManagerLogErr("[ERR] Sorry, redis-cli can't fix this slot "
                                 "yet (work in progress). Slot is set as "
                                 "migrating in %s, as importing in %s, "
                                 "owner is %s:%d\n", migrating_str,
                                 importing_str, owner->ip, owner->port);
        }
    }
cleanup:
    listRelease(owners);
    listRelease(migrating);
    listRelease(importing);
    sdsfree(migrating_str);
    sdsfree(importing_str);
    return success;
}

static int clusterManagerFixMultipleSlotOwners(int slot, list *owners) {
    clusterManagerLogInfo(">>> Fixing multiple owners for slot %d...\n", slot);
    int success = 0;
    assert(listLength(owners) > 1);
    clusterManagerNode *owner = clusterManagerGetNodeWithMostKeysInSlot(owners,
                                                                        slot,
                                                                        NULL);
    if (!owner) owner = listFirst(owners)->value;
    clusterManagerLogInfo(">>> Setting slot %d owner: %s:%d\n",
                          slot, owner->ip, owner->port);
    /* Set the slot owner. */
    if (!clusterManagerSetSlotOwner(owner, slot, 0)) return 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    /* Update configuration in all the other master nodes by assigning the slot
     * itself to the new owner, and by eventually migrating keys if the node
     * has keys for the slot. */
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n == owner) continue;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        int count = clusterManagerCountKeysInSlot(n, slot);
        success = (count >= 0);
        if (!success) break;
        clusterManagerDelSlot(n, slot, 1);
        if (!clusterManagerSetSlot(n, owner, slot, "node", NULL)) return 0;
        if (count > 0) {
            int opts = CLUSTER_MANAGER_OPT_VERBOSE |
                       CLUSTER_MANAGER_OPT_COLD;
            success = clusterManagerMoveSlot(n, owner, slot, opts, NULL);
            if (!success) break;
        }
    }
    return success;
}

static int clusterManagerCheckCluster(int quiet) {
    listNode *ln = listFirst(cluster_manager.nodes);
    if (!ln) return 0;
    clusterManagerNode *node = ln->value;
    clusterManagerLogInfo(">>> Performing Cluster Check (using node %s:%d)\n",
                          node->ip, node->port);
    int result = 1, consistent = 0;
    int do_fix = config.cluster_manager_command.flags &
                 CLUSTER_MANAGER_CMD_FLAG_FIX;
    if (!quiet) clusterManagerShowNodes();
    consistent = clusterManagerIsConfigConsistent();
    if (!consistent) {
        sds err = sdsnew("[ERR] Nodes don't agree about configuration!");
        clusterManagerOnError(err);
        result = 0;
    } else {
        clusterManagerLogOk("[OK] All nodes agree about slots "
                            "configuration.\n");
    }
    /* Check open slots */
    clusterManagerLogInfo(">>> Check for open slots...\n");
    listIter li;
    listRewind(cluster_manager.nodes, &li);
    int i;
    dict *open_slots = NULL;
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->migrating != NULL) {
            if (open_slots == NULL)
                open_slots = dictCreate(&clusterManagerDictType);
            sds errstr = sdsempty();
            errstr = sdscatprintf(errstr,
                                "[WARNING] Node %s:%d has slots in "
                                "migrating state ",
                                n->ip,
                                n->port);
            for (i = 0; i < n->migrating_count; i += 2) {
                sds slot = n->migrating[i];
                dictReplace(open_slots, slot, sdsdup(n->migrating[i + 1]));
                char *fmt = (i > 0 ? ",%S" : "%S");
                errstr = sdscatfmt(errstr, fmt, slot);
            }
            errstr = sdscat(errstr, ".");
            clusterManagerOnError(errstr);
        }
        if (n->importing != NULL) {
            if (open_slots == NULL)
                open_slots = dictCreate(&clusterManagerDictType);
            sds errstr = sdsempty();
            errstr = sdscatprintf(errstr,
                                "[WARNING] Node %s:%d has slots in "
                                "importing state ",
                                n->ip,
                                n->port);
            for (i = 0; i < n->importing_count; i += 2) {
                sds slot = n->importing[i];
                dictReplace(open_slots, slot, sdsdup(n->importing[i + 1]));
                char *fmt = (i > 0 ? ",%S" : "%S");
                errstr = sdscatfmt(errstr, fmt, slot);
            }
            errstr = sdscat(errstr, ".");
            clusterManagerOnError(errstr);
        }
    }
    if (open_slots != NULL) {
        result = 0;
        dictIterator *iter = dictGetIterator(open_slots);
        dictEntry *entry;
        sds errstr = sdsnew("[WARNING] The following slots are open: ");
        i = 0;
        while ((entry = dictNext(iter)) != NULL) {
            sds slot = (sds) dictGetKey(entry);
            char *fmt = (i++ > 0 ? ",%S" : "%S");
            errstr = sdscatfmt(errstr, fmt, slot);
        }
        clusterManagerLogErr("%s.\n", (char *) errstr);
        sdsfree(errstr);
        if (do_fix) {
            /* Fix open slots. */
            dictReleaseIterator(iter);
            iter = dictGetIterator(open_slots);
            while ((entry = dictNext(iter)) != NULL) {
                sds slot = (sds) dictGetKey(entry);
                result = clusterManagerFixOpenSlot(atoi(slot));
                if (!result) break;
            }
        }
        dictReleaseIterator(iter);
        dictRelease(open_slots);
    }
    clusterManagerLogInfo(">>> Check slots coverage...\n");
    char slots[CLUSTER_MANAGER_SLOTS];
    memset(slots, 0, CLUSTER_MANAGER_SLOTS);
    int coverage = clusterManagerGetCoveredSlots(slots);
    if (coverage == CLUSTER_MANAGER_SLOTS) {
        clusterManagerLogOk("[OK] All %d slots covered.\n",
                            CLUSTER_MANAGER_SLOTS);
    } else {
        sds err = sdsempty();
        err = sdscatprintf(err, "[ERR] Not all %d slots are "
                                "covered by nodes.\n",
                                CLUSTER_MANAGER_SLOTS);
        clusterManagerOnError(err);
        result = 0;
        if (do_fix/* && result*/) {
            dictType dtype = clusterManagerDictType;
            dtype.keyDestructor = dictSdsDestructor;
            dtype.valDestructor = dictListDestructor;
            clusterManagerUncoveredSlots = dictCreate(&dtype);
            int fixed = clusterManagerFixSlotsCoverage(slots);
            if (fixed > 0) result = 1;
        }
    }
    int search_multiple_owners = config.cluster_manager_command.flags &
                                 CLUSTER_MANAGER_CMD_FLAG_CHECK_OWNERS;
    if (search_multiple_owners) {
        /* Check whether there are multiple owners, even when slots are
         * fully covered and there are no open slots. */
        clusterManagerLogInfo(">>> Check for multiple slot owners...\n");
        int slot = 0, slots_with_multiple_owners = 0;
        for (; slot < CLUSTER_MANAGER_SLOTS; slot++) {
            listIter li;
            listNode *ln;
            listRewind(cluster_manager.nodes, &li);
            list *owners = listCreate();
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
                if (n->slots[slot]) listAddNodeTail(owners, n);
                else {
                    /* Nodes having keys for the slot will be considered
                     * owners too. */
                    int count = clusterManagerCountKeysInSlot(n, slot);
                    if (count > 0) listAddNodeTail(owners, n);
                }
            }
            if (listLength(owners) > 1) {
                result = 0;
                clusterManagerLogErr("[WARNING] Slot %d has %d owners:\n",
                                     slot, listLength(owners));
                listRewind(owners, &li);
                while ((ln = listNext(&li)) != NULL) {
                    clusterManagerNode *n = ln->value;
                    clusterManagerLogErr("    %s:%d\n", n->ip, n->port);
                }
                slots_with_multiple_owners++;
                if (do_fix) {
                    result = clusterManagerFixMultipleSlotOwners(slot, owners);
                    if (!result) {
                        clusterManagerLogErr("Failed to fix multiple owners "
                                             "for slot %d\n", slot);
                        listRelease(owners);
                        break;
                    } else slots_with_multiple_owners--;
                }
            }
            listRelease(owners);
        }
        if (slots_with_multiple_owners == 0)
            clusterManagerLogOk("[OK] No multiple owners found.\n");
    }
    return result;
}

static clusterManagerNode *clusterNodeForResharding(char *id,
                                                    clusterManagerNode *target,
                                                    int *raise_err)
{
    clusterManagerNode *node = NULL;
    const char *invalid_node_msg = "*** The specified node (%s) is not known "
                                   "or not a master, please retry.\n";
    node = clusterManagerNodeByName(id);
    *raise_err = 0;
    if (!node || node->flags & CLUSTER_MANAGER_FLAG_SLAVE) {
        clusterManagerLogErr(invalid_node_msg, id);
        *raise_err = 1;
        return NULL;
    } else if (target != NULL) {
        if (!strcmp(node->name, target->name)) {
            clusterManagerLogErr( "*** It is not possible to use "
                                  "the target node as "
                                  "source node.\n");
            return NULL;
        }
    }
    return node;
}

static list *clusterManagerComputeReshardTable(list *sources, int numslots) {
    list *moved = listCreate();
    int src_count = listLength(sources), i = 0, tot_slots = 0, j;
    clusterManagerNode **sorted = zmalloc(src_count * sizeof(*sorted));
    listIter li;
    listNode *ln;
    listRewind(sources, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        tot_slots += node->slots_count;
        sorted[i++] = node;
    }
    qsort(sorted, src_count, sizeof(clusterManagerNode *),
          clusterManagerSlotCountCompareDesc);
    for (i = 0; i < src_count; i++) {
        clusterManagerNode *node = sorted[i];
        float n = ((float) numslots / tot_slots * node->slots_count);
        if (i == 0) n = ceil(n);
        else n = floor(n);
        int max = (int) n, count = 0;
        for (j = 0; j < CLUSTER_MANAGER_SLOTS; j++) {
            int slot = node->slots[j];
            if (!slot) continue;
            if (count >= max || (int)listLength(moved) >= numslots) break;
            clusterManagerReshardTableItem *item = zmalloc(sizeof(*item));
            item->source = node;
            item->slot = j;
            listAddNodeTail(moved, item);
            count++;
        }
    }
    zfree(sorted);
    return moved;
}

static void clusterManagerShowReshardTable(list *table) {
    listIter li;
    listNode *ln;
    listRewind(table, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerReshardTableItem *item = ln->value;
        clusterManagerNode *n = item->source;
        printf("    Moving slot %d from %s\n", item->slot, (char *) n->name);
    }
}

static void clusterManagerReleaseReshardTable(list *table) {
    if (table != NULL) {
        listIter li;
        listNode *ln;
        listRewind(table, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerReshardTableItem *item = ln->value;
            zfree(item);
        }
        listRelease(table);
    }
}

static void clusterManagerLog(int level, const char* fmt, ...) {
    int use_colors =
        (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_COLOR);
    if (use_colors) {
        printf("\033[");
        switch (level) {
        case CLUSTER_MANAGER_LOG_LVL_INFO: printf(LOG_COLOR_BOLD); break;
        case CLUSTER_MANAGER_LOG_LVL_WARN: printf(LOG_COLOR_YELLOW); break;
        case CLUSTER_MANAGER_LOG_LVL_ERR: printf(LOG_COLOR_RED); break;
        case CLUSTER_MANAGER_LOG_LVL_SUCCESS: printf(LOG_COLOR_GREEN); break;
        default: printf(LOG_COLOR_RESET); break;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (use_colors) printf("\033[" LOG_COLOR_RESET);
}

static void clusterManagerNodeArrayInit(clusterManagerNodeArray *array,
                                        int alloc_len)
{
    array->nodes = zcalloc(alloc_len * sizeof(clusterManagerNode*));
    array->alloc = array->nodes;
    array->len = alloc_len;
    array->count = 0;
}

/* Reset array->nodes to the original array allocation and re-count non-NULL
 * nodes. */
static void clusterManagerNodeArrayReset(clusterManagerNodeArray *array) {
    if (array->nodes > array->alloc) {
        array->len = array->nodes - array->alloc;
        array->nodes = array->alloc;
        array->count = 0;
        int i = 0;
        for(; i < array->len; i++) {
            if (array->nodes[i] != NULL) array->count++;
        }
    }
}

/* Shift array->nodes and store the shifted node into 'nodeptr'. */
static void clusterManagerNodeArrayShift(clusterManagerNodeArray *array,
                                         clusterManagerNode **nodeptr)
{
    assert(array->len > 0);
    /* If the first node to be shifted is not NULL, decrement count. */
    if (*array->nodes != NULL) array->count--;
    /* Store the first node to be shifted into 'nodeptr'. */
    *nodeptr = *array->nodes;
    /* Shift the nodes array and decrement length. */
    array->nodes++;
    array->len--;
}

static void clusterManagerNodeArrayAdd(clusterManagerNodeArray *array,
                                       clusterManagerNode *node)
{
    assert(array->len > 0);
    assert(node != NULL);
    assert(array->count < array->len);
    array->nodes[array->count++] = node;
}

static void clusterManagerPrintNotEmptyNodeError(clusterManagerNode *node,
                                                 char *err)
{
    char *msg;
    if (err) msg = err;
    else {
        msg = "is not empty. Either the node already knows other "
              "nodes (check with CLUSTER NODES) or contains some "
              "key in database 0.";
    }
    clusterManagerLogErr("[ERR] Node %s:%d %s\n", node->ip, node->port, msg);
}

static void clusterManagerPrintNotClusterNodeError(clusterManagerNode *node,
                                                   char *err)
{
    char *msg = (err ? err : "is not configured as a cluster node.");
    clusterManagerLogErr("[ERR] Node %s:%d %s\n", node->ip, node->port, msg);
}

/* Execute redis-cli in Cluster Manager mode */
static void clusterManagerMode(clusterManagerCommandProc *proc) {
    int argc = config.cluster_manager_command.argc;
    char **argv = config.cluster_manager_command.argv;
    cluster_manager.nodes = NULL;
    int success = proc(argc, argv);

    /* Initialized in createClusterManagerCommand. */
    if (config.stdin_lastarg) {
        zfree(config.cluster_manager_command.argv);
        sdsfree(config.cluster_manager_command.stdin_arg);
    } else if (config.stdin_tag_arg) {
        sdsfree(config.cluster_manager_command.stdin_arg);
    }
    freeClusterManager();

    exit(success ? 0 : 1);
}

/* Cluster Manager Commands */

static int clusterManagerCommandCreate(int argc, char **argv) {
    int i, j, success = 1;
    cluster_manager.nodes = listCreate();
    for (i = 0; i < argc; i++) {
        char *addr = argv[i];
        char *ip = NULL;
        int port = 0;
        if (!parseClusterNodeAddress(addr, &ip, &port, NULL)) {
            fprintf(stderr, "Invalid address format: %s\n", addr);
            return 0;
        }

        clusterManagerNode *node = clusterManagerNewNode(ip, port, 0);
        if (!clusterManagerNodeConnect(node)) {
            freeClusterManagerNode(node);
            return 0;
        }
        char *err = NULL;
        if (!clusterManagerNodeIsCluster(node, &err)) {
            clusterManagerPrintNotClusterNodeError(node, err);
            if (err) zfree(err);
            freeClusterManagerNode(node);
            return 0;
        }
        err = NULL;
        if (!clusterManagerNodeLoadInfo(node, 0, &err)) {
            if (err) {
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                zfree(err);
            }
            freeClusterManagerNode(node);
            return 0;
        }
        err = NULL;
        if (!clusterManagerNodeIsEmpty(node, &err)) {
            clusterManagerPrintNotEmptyNodeError(node, err);
            if (err) zfree(err);
            freeClusterManagerNode(node);
            return 0;
        }
        listAddNodeTail(cluster_manager.nodes, node);
    }
    int node_len = cluster_manager.nodes->len;
    int replicas = config.cluster_manager_command.replicas;
    int masters_count = CLUSTER_MANAGER_MASTERS_COUNT(node_len, replicas);
    if (masters_count < 3) {
        clusterManagerLogErr(
            "*** ERROR: Invalid configuration for cluster creation.\n"
            "*** Redis Cluster requires at least 3 master nodes.\n"
            "*** This is not possible with %d nodes and %d replicas per node.",
            node_len, replicas);
        clusterManagerLogErr("\n*** At least %d nodes are required.\n",
                             3 * (replicas + 1));
        return 0;
    }
    clusterManagerLogInfo(">>> Performing hash slots allocation "
                          "on %d nodes...\n", node_len);
    int interleaved_len = 0, ip_count = 0;
    clusterManagerNode **interleaved = zcalloc(node_len*sizeof(**interleaved));
    char **ips = zcalloc(node_len * sizeof(char*));
    clusterManagerNodeArray *ip_nodes = zcalloc(node_len * sizeof(*ip_nodes));
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        int found = 0;
        for (i = 0; i < ip_count; i++) {
            char *ip = ips[i];
            if (!strcmp(ip, n->ip)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            ips[ip_count++] = n->ip;
        }
        clusterManagerNodeArray *node_array = &(ip_nodes[i]);
        if (node_array->nodes == NULL)
            clusterManagerNodeArrayInit(node_array, node_len);
        clusterManagerNodeArrayAdd(node_array, n);
    }
    while (interleaved_len < node_len) {
        for (i = 0; i < ip_count; i++) {
            clusterManagerNodeArray *node_array = &(ip_nodes[i]);
            if (node_array->count > 0) {
                clusterManagerNode *n = NULL;
                clusterManagerNodeArrayShift(node_array, &n);
                interleaved[interleaved_len++] = n;
            }
        }
    }
    clusterManagerNode **masters = interleaved;
    interleaved += masters_count;
    interleaved_len -= masters_count;
    float slots_per_node = CLUSTER_MANAGER_SLOTS / (float) masters_count;
    long first = 0;
    float cursor = 0.0f;
    for (i = 0; i < masters_count; i++) {
        clusterManagerNode *master = masters[i];
        long last = lround(cursor + slots_per_node - 1);
        if (last > CLUSTER_MANAGER_SLOTS || i == (masters_count - 1))
            last = CLUSTER_MANAGER_SLOTS - 1;
        if (last < first) last = first;
        printf("Master[%d] -> Slots %ld - %ld\n", i, first, last);
        master->slots_count = 0;
        for (j = first; j <= last; j++) {
            master->slots[j] = 1;
            master->slots_count++;
        }
        master->dirty = 1;
        first = last + 1;
        cursor += slots_per_node;
    }

    /* Rotating the list sometimes helps to get better initial
     * anti-affinity before the optimizer runs. */
    clusterManagerNode *first_node = interleaved[0];
    for (i = 0; i < (interleaved_len - 1); i++)
        interleaved[i] = interleaved[i + 1];
    interleaved[interleaved_len - 1] = first_node;
    int assign_unused = 0, available_count = interleaved_len;
assign_replicas:
    for (i = 0; i < masters_count; i++) {
        clusterManagerNode *master = masters[i];
        int assigned_replicas = 0;
        while (assigned_replicas < replicas) {
            if (available_count == 0) break;
            clusterManagerNode *found = NULL, *slave = NULL;
            int firstNodeIdx = -1;
            for (j = 0; j < interleaved_len; j++) {
                clusterManagerNode *n = interleaved[j];
                if (n == NULL) continue;
                if (strcmp(n->ip, master->ip)) {
                    found = n;
                    interleaved[j] = NULL;
                    break;
                }
                if (firstNodeIdx < 0) firstNodeIdx = j;
            }
            if (found) slave = found;
            else if (firstNodeIdx >= 0) {
                slave = interleaved[firstNodeIdx];
                interleaved_len -= (firstNodeIdx + 1);
                interleaved += (firstNodeIdx + 1);
            }
            if (slave != NULL) {
                assigned_replicas++;
                available_count--;
                if (slave->replicate) sdsfree(slave->replicate);
                slave->replicate = sdsnew(master->name);
                slave->dirty = 1;
            } else break;
            printf("Adding replica %s:%d to %s:%d\n", slave->ip, slave->port,
                   master->ip, master->port);
            if (assign_unused) break;
        }
    }
    if (!assign_unused && available_count > 0) {
        assign_unused = 1;
        printf("Adding extra replicas...\n");
        goto assign_replicas;
    }
    for (i = 0; i < ip_count; i++) {
        clusterManagerNodeArray *node_array = ip_nodes + i;
        clusterManagerNodeArrayReset(node_array);
    }
    clusterManagerOptimizeAntiAffinity(ip_nodes, ip_count);
    clusterManagerShowNodes();
    int ignore_force = 0;
    if (confirmWithYes("Can I set the above configuration?", ignore_force)) {
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            char *err = NULL;
            int flushed = clusterManagerFlushNodeConfig(node, &err);
            if (!flushed && node->dirty && !node->replicate) {
                if (err != NULL) {
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                    zfree(err);
                }
                success = 0;
                goto cleanup;
            } else if (err != NULL) zfree(err);
        }
        clusterManagerLogInfo(">>> Nodes configuration updated\n");
        clusterManagerLogInfo(">>> Assign a different config epoch to "
                              "each node\n");
        int config_epoch = 1;
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            redisReply *reply = NULL;
            reply = CLUSTER_MANAGER_COMMAND(node,
                                            "cluster set-config-epoch %d",
                                            config_epoch++);
            if (reply != NULL) freeReplyObject(reply);
        }
        clusterManagerLogInfo(">>> Sending CLUSTER MEET messages to join "
                              "the cluster\n");
        clusterManagerNode *first = NULL;
        char first_ip[NET_IP_STR_LEN]; /* first->ip may be a hostname */
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            if (first == NULL) {
                first = node;
                /* Although hiredis supports connecting to a hostname, CLUSTER
                 * MEET requires an IP address, so we do a DNS lookup here. */
                int anet_flags = ANET_NONE;
                if (config.prefer_ipv4) anet_flags |= ANET_PREFER_IPV4;
                if (config.prefer_ipv6) anet_flags |= ANET_PREFER_IPV6;
                if (anetResolve(NULL, first->ip, first_ip, sizeof(first_ip), anet_flags)
                    == ANET_ERR)
                {
                    fprintf(stderr, "Invalid IP address or hostname specified: %s\n", first->ip);
                    success = 0;
                    goto cleanup;
                }
                continue;
            }
            redisReply *reply = NULL;
            if (first->bus_port == 0 || (first->bus_port == first->port + CLUSTER_MANAGER_PORT_INCR)) {
                /* CLUSTER MEET bus-port parameter was added in 4.0.
                 * So if (bus_port == 0) or (bus_port == port + CLUSTER_MANAGER_PORT_INCR),
                 * we just call CLUSTER MEET with 2 arguments, using the old form. */
                reply = CLUSTER_MANAGER_COMMAND(node, "cluster meet %s %d",
                                                first_ip, first->port);
            } else {
                reply = CLUSTER_MANAGER_COMMAND(node, "cluster meet %s %d %d",
                                                first_ip, first->port, first->bus_port);
            }
            int is_err = 0;
            if (reply != NULL) {
                if ((is_err = reply->type == REDIS_REPLY_ERROR))
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, reply->str);
                freeReplyObject(reply);
            } else {
                is_err = 1;
                fprintf(stderr, "Failed to send CLUSTER MEET command.\n");
            }
            if (is_err) {
                success = 0;
                goto cleanup;
            }
        }
        /* Give one second for the join to start, in order to avoid that
         * waiting for cluster join will find all the nodes agree about
         * the config as they are still empty with unassigned slots. */
        sleep(1);
        clusterManagerWaitForClusterJoin();
        /* Useful for the replicas */
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            if (!node->dirty) continue;
            char *err = NULL;
            int flushed = clusterManagerFlushNodeConfig(node, &err);
            if (!flushed && !node->replicate) {
                if (err != NULL) {
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                    zfree(err);
                }
                success = 0;
                goto cleanup;
            } else if (err != NULL) {
                zfree(err);
            }
        }
        // Reset Nodes
        listRewind(cluster_manager.nodes, &li);
        clusterManagerNode *first_node = NULL;
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            if (!first_node) first_node = node;
            else freeClusterManagerNode(node);
        }
        listEmpty(cluster_manager.nodes);
        if (!clusterManagerLoadInfoFromNode(first_node)) {
            success = 0;
            goto cleanup;
        }
        clusterManagerCheckCluster(0);
    }
cleanup:
    /* Free everything */
    zfree(masters);
    zfree(ips);
    for (i = 0; i < node_len; i++) {
        clusterManagerNodeArray *node_array = ip_nodes + i;
        CLUSTER_MANAGER_NODE_ARRAY_FREE(node_array);
    }
    zfree(ip_nodes);
    return success;
}

static int clusterManagerCommandAddNode(int argc, char **argv) {
    int success = 1;
    redisReply *reply = NULL;
    redisReply *function_restore_reply = NULL;
    redisReply *function_list_reply = NULL;
    char *ref_ip = NULL, *ip = NULL;
    int ref_port = 0, port = 0;
    if (!getClusterHostFromCmdArgs(argc - 1, argv + 1, &ref_ip, &ref_port))
        goto invalid_args;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port))
        goto invalid_args;
    clusterManagerLogInfo(">>> Adding node %s:%d to cluster %s:%d\n", ip, port,
                          ref_ip, ref_port);
    // Check the existing cluster
    clusterManagerNode *refnode = clusterManagerNewNode(ref_ip, ref_port, 0);
    if (!clusterManagerLoadInfoFromNode(refnode)) return 0;
    if (!clusterManagerCheckCluster(0)) return 0;

    /* If --cluster-master-id was specified, try to resolve it now so that we
     * abort before starting with the node configuration. */
    clusterManagerNode *master_node = NULL;
    if (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_SLAVE) {
        char *master_id = config.cluster_manager_command.master_id;
        if (master_id != NULL) {
            master_node = clusterManagerNodeByName(master_id);
            if (master_node == NULL) {
                clusterManagerLogErr("[ERR] No such master ID %s\n", master_id);
                return 0;
            }
        } else {
            master_node = clusterManagerNodeWithLeastReplicas();
            assert(master_node != NULL);
            printf("Automatically selected master %s:%d\n", master_node->ip,
                   master_node->port);
        }
    }

    // Add the new node
    clusterManagerNode *new_node = clusterManagerNewNode(ip, port, 0);
    int added = 0;
    if (!clusterManagerNodeConnect(new_node)) {
        clusterManagerLogErr("[ERR] Sorry, can't connect to node %s:%d\n",
                             ip, port);
        success = 0;
        goto cleanup;
    }
    char *err = NULL;
    if (!(success = clusterManagerNodeIsCluster(new_node, &err))) {
        clusterManagerPrintNotClusterNodeError(new_node, err);
        if (err) zfree(err);
        goto cleanup;
    }
    if (!clusterManagerNodeLoadInfo(new_node, 0, &err)) {
        if (err) {
            CLUSTER_MANAGER_PRINT_REPLY_ERROR(new_node, err);
            zfree(err);
        }
        success = 0;
        goto cleanup;
    }
    if (!(success = clusterManagerNodeIsEmpty(new_node, &err))) {
        clusterManagerPrintNotEmptyNodeError(new_node, err);
        if (err) zfree(err);
        goto cleanup;
    }
    clusterManagerNode *first = listFirst(cluster_manager.nodes)->value;
    listAddNodeTail(cluster_manager.nodes, new_node);
    added = 1;

    if (!master_node) {
        /* Send functions to the new node, if new node is a replica it will get the functions from its primary. */
        clusterManagerLogInfo(">>> Getting functions from cluster\n");
        reply = CLUSTER_MANAGER_COMMAND(refnode, "FUNCTION DUMP");
        if (!clusterManagerCheckRedisReply(refnode, reply, &err)) {
            clusterManagerLogInfo(">>> Failed retrieving Functions from the cluster, "
                    "skip this step as Redis version do not support function command (error = '%s')\n", err? err : "NULL reply");
            if (err) zfree(err);
        } else {
            assert(reply->type == REDIS_REPLY_STRING);
            clusterManagerLogInfo(">>> Send FUNCTION LIST to %s:%d to verify there is no functions in it\n", ip, port);
            function_list_reply = CLUSTER_MANAGER_COMMAND(new_node, "FUNCTION LIST");
            if (!clusterManagerCheckRedisReply(new_node, function_list_reply, &err)) {
                clusterManagerLogErr(">>> Failed on CLUSTER LIST (error = '%s')\r\n", err? err : "NULL reply");
                if (err) zfree(err);
                success = 0;
                goto cleanup;
            }
            assert(function_list_reply->type == REDIS_REPLY_ARRAY);
            if (function_list_reply->elements > 0) {
                clusterManagerLogErr(">>> New node already contains functions and can not be added to the cluster. Use FUNCTION FLUSH and try again.\r\n");
                success = 0;
                goto cleanup;
            }
            clusterManagerLogInfo(">>> Send FUNCTION RESTORE to %s:%d\n", ip, port);
            function_restore_reply = CLUSTER_MANAGER_COMMAND(new_node, "FUNCTION RESTORE %b", reply->str, reply->len);
            if (!clusterManagerCheckRedisReply(new_node, function_restore_reply, &err)) {
                clusterManagerLogErr(">>> Failed loading functions to the new node (error = '%s')\r\n", err? err : "NULL reply");
                if (err) zfree(err);
                success = 0;
                goto cleanup;
            }
        }
    }

    if (reply) freeReplyObject(reply);

    // Send CLUSTER MEET command to the new node
    clusterManagerLogInfo(">>> Send CLUSTER MEET to node %s:%d to make it "
                          "join the cluster.\n", ip, port);
    /* CLUSTER MEET requires an IP address, so we do a DNS lookup here. */
    char first_ip[NET_IP_STR_LEN];
    int anet_flags = ANET_NONE;
    if (config.prefer_ipv4) anet_flags |= ANET_PREFER_IPV4;
    if (config.prefer_ipv6) anet_flags |= ANET_PREFER_IPV6;
    if (anetResolve(NULL, first->ip, first_ip, sizeof(first_ip), anet_flags) == ANET_ERR) {
        fprintf(stderr, "Invalid IP address or hostname specified: %s\n", first->ip);
        success = 0;
        goto cleanup;
    }

    if (first->bus_port == 0 || (first->bus_port == first->port + CLUSTER_MANAGER_PORT_INCR)) {
        /* CLUSTER MEET bus-port parameter was added in 4.0.
         * So if (bus_port == 0) or (bus_port == port + CLUSTER_MANAGER_PORT_INCR),
         * we just call CLUSTER MEET with 2 arguments, using the old form. */
        reply = CLUSTER_MANAGER_COMMAND(new_node, "CLUSTER MEET %s %d",
                                        first_ip, first->port);
    } else {
        reply = CLUSTER_MANAGER_COMMAND(new_node, "CLUSTER MEET %s %d %d",
                                        first_ip, first->port, first->bus_port);
    }

    if (!(success = clusterManagerCheckRedisReply(new_node, reply, NULL)))
        goto cleanup;

    /* Additional configuration is needed if the node is added as a slave. */
    if (master_node) {
        sleep(1);
        clusterManagerWaitForClusterJoin();
        clusterManagerLogInfo(">>> Configure node as replica of %s:%d.\n",
                              master_node->ip, master_node->port);
        freeReplyObject(reply);
        reply = CLUSTER_MANAGER_COMMAND(new_node, "CLUSTER REPLICATE %s",
                                        master_node->name);
        if (!(success = clusterManagerCheckRedisReply(new_node, reply, NULL)))
            goto cleanup;
    }
    clusterManagerLogOk("[OK] New node added correctly.\n");
cleanup:
    if (!added && new_node) freeClusterManagerNode(new_node);
    if (reply) freeReplyObject(reply);
    if (function_restore_reply) freeReplyObject(function_restore_reply);
    if (function_list_reply) freeReplyObject(function_list_reply);
    return success;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandDeleteNode(int argc, char **argv) {
    UNUSED(argc);
    int success = 1;
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    char *node_id = argv[1];
    clusterManagerLogInfo(">>> Removing node %s from cluster %s:%d\n",
                          node_id, ip, port);
    clusterManagerNode *ref_node = clusterManagerNewNode(ip, port, 0);
    clusterManagerNode *node = NULL;

    // Load cluster information
    if (!clusterManagerLoadInfoFromNode(ref_node)) return 0;

    // Check if the node exists and is not empty
    node = clusterManagerNodeByName(node_id);
    if (node == NULL) {
        clusterManagerLogErr("[ERR] No such node ID %s\n", node_id);
        return 0;
    }
    if (node->slots_count != 0) {
        clusterManagerLogErr("[ERR] Node %s:%d is not empty! Reshard data "
                             "away and try again.\n", node->ip, node->port);
        return 0;
    }

    // Send CLUSTER FORGET to all the nodes but the node to remove
    clusterManagerLogInfo(">>> Sending CLUSTER FORGET messages to the "
                          "cluster...\n");
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n == node) continue;
        if (n->replicate && !strcasecmp(n->replicate, node_id)) {
            // Reconfigure the slave to replicate with some other node
            clusterManagerNode *master = clusterManagerNodeWithLeastReplicas();
            assert(master != NULL);
            clusterManagerLogInfo(">>> %s:%d as replica of %s:%d\n",
                                  n->ip, n->port, master->ip, master->port);
            redisReply *r = CLUSTER_MANAGER_COMMAND(n, "CLUSTER REPLICATE %s",
                                                    master->name);
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (r) freeReplyObject(r);
            if (!success) return 0;
        }
        redisReply *r = CLUSTER_MANAGER_COMMAND(n, "CLUSTER FORGET %s",
                                                node_id);
        success = clusterManagerCheckRedisReply(n, r, NULL);
        if (r) freeReplyObject(r);
        if (!success) return 0;
    }

    /* Finally send CLUSTER RESET to the node. */
    clusterManagerLogInfo(">>> Sending CLUSTER RESET SOFT to the "
                          "deleted node.\n");
    redisReply *r = redisCommand(node->context, "CLUSTER RESET %s", "SOFT");
    success = clusterManagerCheckRedisReply(node, r, NULL);
    if (r) freeReplyObject(r);
    return success;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandInfo(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(node)) return 0;
    clusterManagerShowClusterInfo();
    return 1;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandCheck(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(node)) return 0;
    clusterManagerShowClusterInfo();
    return clusterManagerCheckCluster(0);
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandFix(int argc, char **argv) {
    config.cluster_manager_command.flags |= CLUSTER_MANAGER_CMD_FLAG_FIX;
    return clusterManagerCommandCheck(argc, argv);
}

static int clusterManagerCommandReshard(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(node)) return 0;
    clusterManagerCheckCluster(0);
    if (cluster_manager.errors && listLength(cluster_manager.errors) > 0) {
        fflush(stdout);
        fprintf(stderr,
                "*** Please fix your cluster problems before resharding\n");
        return 0;
    }
    int slots = config.cluster_manager_command.slots;
    if (!slots) {
        while (slots <= 0 || slots > CLUSTER_MANAGER_SLOTS) {
            printf("How many slots do you want to move (from 1 to %d)? ",
                   CLUSTER_MANAGER_SLOTS);
            fflush(stdout);
            char buf[6];
            int nread = read(fileno(stdin),buf,6);
            if (nread <= 0) continue;
            int last_idx = nread - 1;
            if (buf[last_idx] != '\n') {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {}
            }
            buf[last_idx] = '\0';
            slots = atoi(buf);
        }
    }
    char buf[255];
    char *to = config.cluster_manager_command.to,
         *from = config.cluster_manager_command.from;
    while (to == NULL) {
        printf("What is the receiving node ID? ");
        fflush(stdout);
        int nread = read(fileno(stdin),buf,255);
        if (nread <= 0) continue;
        int last_idx = nread - 1;
        if (buf[last_idx] != '\n') {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
        }
        buf[last_idx] = '\0';
        if (strlen(buf) > 0) to = buf;
    }
    int raise_err = 0;
    clusterManagerNode *target = clusterNodeForResharding(to, NULL, &raise_err);
    if (target == NULL) return 0;
    list *sources = listCreate();
    list *table = NULL;
    int all = 0, result = 1;
    if (from == NULL) {
        printf("Please enter all the source node IDs.\n");
        printf("  Type 'all' to use all the nodes as source nodes for "
               "the hash slots.\n");
        printf("  Type 'done' once you entered all the source nodes IDs.\n");
        while (1) {
            printf("Source node #%lu: ", listLength(sources) + 1);
            fflush(stdout);
            int nread = read(fileno(stdin),buf,255);
            if (nread <= 0) continue;
            int last_idx = nread - 1;
            if (buf[last_idx] != '\n') {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {}
            }
            buf[last_idx] = '\0';
            if (!strcmp(buf, "done")) break;
            else if (!strcmp(buf, "all")) {
                all = 1;
                break;
            } else {
                clusterManagerNode *src =
                    clusterNodeForResharding(buf, target, &raise_err);
                if (src != NULL) listAddNodeTail(sources, src);
                else if (raise_err) {
                    result = 0;
                    goto cleanup;
                }
            }
        }
    } else {
        char *p;
        while((p = strchr(from, ',')) != NULL) {
            *p = '\0';
            if (!strcmp(from, "all")) {
                all = 1;
                break;
            } else {
                clusterManagerNode *src =
                    clusterNodeForResharding(from, target, &raise_err);
                if (src != NULL) listAddNodeTail(sources, src);
                else if (raise_err) {
                    result = 0;
                    goto cleanup;
                }
            }
            from = p + 1;
        }
        /* Check if there's still another source to process. */
        if (!all && strlen(from) > 0) {
            if (!strcmp(from, "all")) all = 1;
            if (!all) {
                clusterManagerNode *src =
                    clusterNodeForResharding(from, target, &raise_err);
                if (src != NULL) listAddNodeTail(sources, src);
                else if (raise_err) {
                    result = 0;
                    goto cleanup;
                }
            }
        }
    }
    listIter li;
    listNode *ln;
    if (all) {
        listEmpty(sources);
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
                continue;
            if (!sdscmp(n->name, target->name)) continue;
            listAddNodeTail(sources, n);
        }
    }
    if (listLength(sources) == 0) {
        fprintf(stderr, "*** No source nodes given, operation aborted.\n");
        result = 0;
        goto cleanup;
    }
    printf("\nReady to move %d slots.\n", slots);
    printf("  Source nodes:\n");
    listRewind(sources, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *src = ln->value;
        sds info = clusterManagerNodeInfo(src, 4);
        printf("%s\n", info);
        sdsfree(info);
    }
    printf("  Destination node:\n");
    sds info = clusterManagerNodeInfo(target, 4);
    printf("%s\n", info);
    sdsfree(info);
    table = clusterManagerComputeReshardTable(sources, slots);
    printf("  Resharding plan:\n");
    clusterManagerShowReshardTable(table);
    if (!(config.cluster_manager_command.flags &
          CLUSTER_MANAGER_CMD_FLAG_YES))
    {
        printf("Do you want to proceed with the proposed "
               "reshard plan (yes/no)? ");
        fflush(stdout);
        char buf[4];
        int nread = read(fileno(stdin),buf,4);
        buf[3] = '\0';
        if (nread <= 0 || strcmp("yes", buf) != 0) {
            result = 0;
            goto cleanup;
        }
    }
    int opts = CLUSTER_MANAGER_OPT_VERBOSE;
    listRewind(table, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerReshardTableItem *item = ln->value;
        char *err = NULL;
        result = clusterManagerMoveSlot(item->source, target, item->slot,
                                        opts, &err);
        if (!result) {
            if (err != NULL) {
                clusterManagerLogErr("clusterManagerMoveSlot failed: %s\n", err);
                zfree(err);
            }
            goto cleanup;
        }
    }
cleanup:
    listRelease(sources);
    clusterManagerReleaseReshardTable(table);
    return result;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandRebalance(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    clusterManagerNode **weightedNodes = NULL;
    list *involved = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(node)) return 0;
    int result = 1, i;
    if (config.cluster_manager_command.weight != NULL) {
        for (i = 0; i < config.cluster_manager_command.weight_argc; i++) {
            char *name = config.cluster_manager_command.weight[i];
            char *p = strchr(name, '=');
            if (p == NULL) {
                clusterManagerLogErr("*** invalid input %s\n", name);
                result = 0;
                goto cleanup;
            }
            *p = '\0';
            float w = atof(++p);
            clusterManagerNode *n = clusterManagerNodeByAbbreviatedName(name);
            if (n == NULL) {
                clusterManagerLogErr("*** No such master node %s\n", name);
                result = 0;
                goto cleanup;
            }
            n->weight = w;
        }
    }
    float total_weight = 0;
    int nodes_involved = 0;
    int use_empty = config.cluster_manager_command.flags &
                    CLUSTER_MANAGER_CMD_FLAG_EMPTYMASTER;
    involved = listCreate();
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    /* Compute the total cluster weight. */
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
            continue;
        if (!use_empty && n->slots_count == 0) {
            n->weight = 0;
            continue;
        }
        total_weight += n->weight;
        nodes_involved++;
        listAddNodeTail(involved, n);
    }
    weightedNodes = zmalloc(nodes_involved * sizeof(clusterManagerNode *));
    if (weightedNodes == NULL) goto cleanup;
    /* Check cluster, only proceed if it looks sane. */
    clusterManagerCheckCluster(1);
    if (cluster_manager.errors && listLength(cluster_manager.errors) > 0) {
        clusterManagerLogErr("*** Please fix your cluster problems "
                             "before rebalancing\n");
        result = 0;
        goto cleanup;
    }
    /* Calculate the slots balance for each node. It's the number of
     * slots the node should lose (if positive) or gain (if negative)
     * in order to be balanced. */
    int threshold_reached = 0, total_balance = 0;
    float threshold = config.cluster_manager_command.threshold;
    i = 0;
    listRewind(involved, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        weightedNodes[i++] = n;
        int expected = (int) (((float)CLUSTER_MANAGER_SLOTS / total_weight) *
                        n->weight);
        n->balance = n->slots_count - expected;
        total_balance += n->balance;
        /* Compute the percentage of difference between the
         * expected number of slots and the real one, to see
         * if it's over the threshold specified by the user. */
        int over_threshold = 0;
        if (threshold > 0) {
            if (n->slots_count > 0) {
                float err_perc = fabs((100-(100.0*expected/n->slots_count)));
                if (err_perc > threshold) over_threshold = 1;
            } else if (expected > 1) {
                over_threshold = 1;
            }
        }
        if (over_threshold) threshold_reached = 1;
    }
    if (!threshold_reached) {
        clusterManagerLogWarn("*** No rebalancing needed! "
                             "All nodes are within the %.2f%% threshold.\n",
                             config.cluster_manager_command.threshold);
        goto cleanup;
    }
    /* Because of rounding, it is possible that the balance of all nodes
     * summed does not give 0. Make sure that nodes that have to provide
     * slots are always matched by nodes receiving slots. */
    while (total_balance > 0) {
        listRewind(involved, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n->balance <= 0 && total_balance > 0) {
                n->balance--;
                total_balance--;
            }
        }
    }
    /* Sort nodes by their slots balance. */
    qsort(weightedNodes, nodes_involved, sizeof(clusterManagerNode *),
          clusterManagerCompareNodeBalance);
    clusterManagerLogInfo(">>> Rebalancing across %d nodes. "
                          "Total weight = %.2f\n",
                          nodes_involved, total_weight);
    if (config.verbose) {
        for (i = 0; i < nodes_involved; i++) {
            clusterManagerNode *n = weightedNodes[i];
            printf("%s:%d balance is %d slots\n", n->ip, n->port, n->balance);
        }
    }
    /* Now we have at the start of the 'sn' array nodes that should get
     * slots, at the end nodes that must give slots.
     * We take two indexes, one at the start, and one at the end,
     * incrementing or decrementing the indexes accordingly til we
     * find nodes that need to get/provide slots. */
    int dst_idx = 0;
    int src_idx = nodes_involved - 1;
    int simulate = config.cluster_manager_command.flags &
                   CLUSTER_MANAGER_CMD_FLAG_SIMULATE;
    while (dst_idx < src_idx) {
        clusterManagerNode *dst = weightedNodes[dst_idx];
        clusterManagerNode *src = weightedNodes[src_idx];
        int db = abs(dst->balance);
        int sb = abs(src->balance);
        int numslots = (db < sb ? db : sb);
        if (numslots > 0) {
            printf("Moving %d slots from %s:%d to %s:%d\n", numslots,
                                                            src->ip,
                                                            src->port,
                                                            dst->ip,
                                                            dst->port);
            /* Actually move the slots. */
            list *lsrc = listCreate(), *table = NULL;
            listAddNodeTail(lsrc, src);
            table = clusterManagerComputeReshardTable(lsrc, numslots);
            listRelease(lsrc);
            int table_len = (int) listLength(table);
            if (!table || table_len != numslots) {
                clusterManagerLogErr("*** Assertion failed: Reshard table "
                                     "!= number of slots");
                result = 0;
                goto end_move;
            }
            if (simulate) {
                for (i = 0; i < table_len; i++) printf("#");
            } else {
                int opts = CLUSTER_MANAGER_OPT_QUIET |
                           CLUSTER_MANAGER_OPT_UPDATE;
                listRewind(table, &li);
                while ((ln = listNext(&li)) != NULL) {
                    clusterManagerReshardTableItem *item = ln->value;
                    char *err;
                    result = clusterManagerMoveSlot(item->source,
                                                    dst,
                                                    item->slot,
                                                    opts, &err);
                    if (!result) {
                        clusterManagerLogErr("*** clusterManagerMoveSlot: %s\n", err);
                        zfree(err);
                        goto end_move;
                    }
                    printf("#");
                    fflush(stdout);
                }

            }
            printf("\n");
end_move:
            clusterManagerReleaseReshardTable(table);
            if (!result) goto cleanup;
        }
        /* Update nodes balance. */
        dst->balance += numslots;
        src->balance -= numslots;
        if (dst->balance == 0) dst_idx++;
        if (src->balance == 0) src_idx --;
    }
cleanup:
    if (involved != NULL) listRelease(involved);
    if (weightedNodes != NULL) zfree(weightedNodes);
    return result;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandSetTimeout(int argc, char **argv) {
    UNUSED(argc);
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    int timeout = atoi(argv[1]);
    if (timeout < 100) {
        fprintf(stderr, "Setting a node timeout of less than 100 "
                "milliseconds is a bad idea.\n");
        return 0;
    }
    // Load cluster information
    clusterManagerNode *node = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(node)) return 0;
    int ok_count = 0, err_count = 0;

    clusterManagerLogInfo(">>> Reconfiguring node timeout in every "
                          "cluster node...\n");
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        char *err = NULL;
        redisReply *reply = CLUSTER_MANAGER_COMMAND(n, "CONFIG %s %s %d",
                                                    "SET",
                                                    "cluster-node-timeout",
                                                    timeout);
        if (reply == NULL) goto reply_err;
        int ok = clusterManagerCheckRedisReply(n, reply, &err);
        freeReplyObject(reply);
        if (!ok) goto reply_err;
        reply = CLUSTER_MANAGER_COMMAND(n, "CONFIG %s", "REWRITE");
        if (reply == NULL) goto reply_err;
        ok = clusterManagerCheckRedisReply(n, reply, &err);
        freeReplyObject(reply);
        if (!ok) goto reply_err;
        clusterManagerLogWarn("*** New timeout set for %s:%d\n", n->ip,
                              n->port);
        ok_count++;
        continue;
reply_err:;
        int need_free = 0;
        if (err == NULL) err = "";
        else need_free = 1;
        clusterManagerLogErr("ERR setting node-timeout for %s:%d: %s\n", n->ip,
                             n->port, err);
        if (need_free) zfree(err);
        err_count++;
    }
    clusterManagerLogInfo(">>> New node timeout set. %d OK, %d ERR.\n",
                          ok_count, err_count);
    return 1;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandImport(int argc, char **argv) {
    int success = 1;
    int port = 0, src_port = 0;
    char *ip = NULL, *src_ip = NULL;
    char *invalid_args_msg = NULL;
    sds cmdfmt = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) {
        invalid_args_msg = CLUSTER_MANAGER_INVALID_HOST_ARG;
        goto invalid_args;
    }
    if (config.cluster_manager_command.from == NULL) {
        invalid_args_msg = "[ERR] Option '--cluster-from' is required for "
                           "subcommand 'import'.\n";
        goto invalid_args;
    }
    char *src_host[] = {config.cluster_manager_command.from};
    if (!getClusterHostFromCmdArgs(1, src_host, &src_ip, &src_port)) {
        invalid_args_msg = "[ERR] Invalid --cluster-from host. You need to "
                           "pass a valid address (ie. 120.0.0.1:7000).\n";
        goto invalid_args;
    }
    clusterManagerLogInfo(">>> Importing data from %s:%d to cluster %s:%d\n",
                          src_ip, src_port, ip, port);

    clusterManagerNode *refnode = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(refnode)) return 0;
    if (!clusterManagerCheckCluster(0)) return 0;
    char *reply_err = NULL;
    redisReply *src_reply = NULL;
    // Connect to the source node.
    redisContext *src_ctx = redisConnectWrapper(src_ip, src_port, config.connect_timeout);
    if (src_ctx->err) {
        success = 0;
        fprintf(stderr,"Could not connect to Redis at %s:%d: %s.\n", src_ip,
                src_port, src_ctx->errstr);
        goto cleanup;
    }
    // Auth for the source node. 
    char *from_user = config.cluster_manager_command.from_user;
    char *from_pass = config.cluster_manager_command.from_pass;
    if (cliAuth(src_ctx, from_user, from_pass) == REDIS_ERR) {
        success = 0;
        goto cleanup;
    }

    src_reply = reconnectingRedisCommand(src_ctx, "INFO");
    if (!src_reply || src_reply->type == REDIS_REPLY_ERROR) {
        if (src_reply && src_reply->str) reply_err = src_reply->str;
        success = 0;
        goto cleanup;
    }
    if (getLongInfoField(src_reply->str, "cluster_enabled")) {
        clusterManagerLogErr("[ERR] The source node should not be a "
                             "cluster node.\n");
        success = 0;
        goto cleanup;
    }
    freeReplyObject(src_reply);
    src_reply = reconnectingRedisCommand(src_ctx, "DBSIZE");
    if (!src_reply || src_reply->type == REDIS_REPLY_ERROR) {
        if (src_reply && src_reply->str) reply_err = src_reply->str;
        success = 0;
        goto cleanup;
    }
    int size = src_reply->integer, i;
    clusterManagerLogWarn("*** Importing %d keys from DB 0\n", size);

    // Build a slot -> node map
    clusterManagerNode  *slots_map[CLUSTER_MANAGER_SLOTS];
    memset(slots_map, 0, sizeof(slots_map));
    listIter li;
    listNode *ln;
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
            if (n->slots_count == 0) continue;
            if (n->slots[i]) {
                slots_map[i] = n;
                break;
            }
        }
    }
    cmdfmt = sdsnew("MIGRATE %s %d %s %d %d");
    if (config.conn_info.auth) {
        if (config.conn_info.user) {
            cmdfmt = sdscatfmt(cmdfmt," AUTH2 %s %s", config.conn_info.user, config.conn_info.auth); 
        } else {
            cmdfmt = sdscatfmt(cmdfmt," AUTH %s", config.conn_info.auth);
        }
    }

    if (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_COPY)
        cmdfmt = sdscat(cmdfmt," COPY");
    if (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_REPLACE)
        cmdfmt = sdscat(cmdfmt," REPLACE");

    /* Use SCAN to iterate over the keys, migrating to the
     * right node as needed. */
    int cursor = -999, timeout = config.cluster_manager_command.timeout;
    while (cursor != 0) {
        if (cursor < 0) cursor = 0;
        freeReplyObject(src_reply);
        src_reply = reconnectingRedisCommand(src_ctx, "SCAN %d COUNT %d",
                                             cursor, 1000);
        if (!src_reply || src_reply->type == REDIS_REPLY_ERROR) {
            if (src_reply && src_reply->str) reply_err = src_reply->str;
            success = 0;
            goto cleanup;
        }
        assert(src_reply->type == REDIS_REPLY_ARRAY);
        assert(src_reply->elements >= 2);
        assert(src_reply->element[1]->type == REDIS_REPLY_ARRAY);
        if (src_reply->element[0]->type == REDIS_REPLY_STRING)
            cursor = atoi(src_reply->element[0]->str);
        else if (src_reply->element[0]->type == REDIS_REPLY_INTEGER)
            cursor = src_reply->element[0]->integer;
        int keycount = src_reply->element[1]->elements;
        for (i = 0; i < keycount; i++) {
            redisReply *kr = src_reply->element[1]->element[i];
            assert(kr->type == REDIS_REPLY_STRING);
            char *key = kr->str;
            uint16_t slot = clusterManagerKeyHashSlot(key, kr->len);
            clusterManagerNode *target = slots_map[slot];
            printf("Migrating %s to %s:%d: ", key, target->ip, target->port);
            redisReply *r = reconnectingRedisCommand(src_ctx, cmdfmt,
                                                     target->ip, target->port,
                                                     key, 0, timeout);
            if (!r || r->type == REDIS_REPLY_ERROR) {
                if (r && r->str) {
                    clusterManagerLogErr("Source %s:%d replied with "
                                         "error:\n%s\n", src_ip, src_port,
                                         r->str);
                }
                success = 0;
            }
            freeReplyObject(r);
            if (!success) goto cleanup;
            clusterManagerLogOk("OK\n");
        }
    }
cleanup:
    if (reply_err)
        clusterManagerLogErr("Source %s:%d replied with error:\n%s\n",
                             src_ip, src_port, reply_err);
    if (src_ctx) redisFree(src_ctx);
    if (src_reply) freeReplyObject(src_reply);
    if (cmdfmt) sdsfree(cmdfmt);
    return success;
invalid_args:
    fprintf(stderr, "%s", invalid_args_msg);
    return 0;
}

static int clusterManagerCommandCall(int argc, char **argv) {
    int port = 0, i;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *refnode = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(refnode)) return 0;
    argc--;
    argv++;
    size_t *argvlen = zmalloc(argc*sizeof(size_t));
    clusterManagerLogInfo(">>> Calling");
    for (i = 0; i < argc; i++) {
        argvlen[i] = strlen(argv[i]);
        printf(" %s", argv[i]);
    }
    printf("\n");
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if ((config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_MASTERS_ONLY)
              && (n->replicate != NULL)) continue;  // continue if node is slave
        if ((config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_SLAVES_ONLY)
              && (n->replicate == NULL)) continue;   // continue if node is master
        if (!n->context && !clusterManagerNodeConnect(n)) continue;
        redisReply *reply = NULL;
        redisAppendCommandArgv(n->context, argc, (const char **) argv, argvlen);
        int status = redisGetReply(n->context, (void **)(&reply));
        if (status != REDIS_OK || reply == NULL )
            printf("%s:%d: Failed!\n", n->ip, n->port);
        else {
            sds formatted_reply = cliFormatReplyRaw(reply);
            printf("%s:%d: %s\n", n->ip, n->port, (char *) formatted_reply);
            sdsfree(formatted_reply);
        }
        if (reply != NULL) freeReplyObject(reply);
    }
    zfree(argvlen);
    return 1;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandBackup(int argc, char **argv) {
    UNUSED(argc);
    int success = 1, port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *refnode = clusterManagerNewNode(ip, port, 0);
    if (!clusterManagerLoadInfoFromNode(refnode)) return 0;
    int no_issues = clusterManagerCheckCluster(0);
    int cluster_errors_count = (no_issues ? 0 :
                                listLength(cluster_manager.errors));
    config.cluster_manager_command.backup_dir = argv[1];
    /* TODO: check if backup_dir is a valid directory. */
    sds json = sdsnew("[\n");
    int first_node = 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        if (!first_node) first_node = 1;
        else json = sdscat(json, ",\n");
        clusterManagerNode *node = ln->value;
        sds node_json = clusterManagerNodeGetJSON(node, cluster_errors_count);
        json = sdscat(json, node_json);
        sdsfree(node_json);
        if (node->replicate)
            continue;
        clusterManagerLogInfo(">>> Node %s:%d -> Saving RDB...\n",
                              node->ip, node->port);
        fflush(stdout);
        getRDB(node);
    }
    json = sdscat(json, "\n]");
    sds jsonpath = sdsnew(config.cluster_manager_command.backup_dir);
    if (jsonpath[sdslen(jsonpath) - 1] != '/')
        jsonpath = sdscat(jsonpath, "/");
    jsonpath = sdscat(jsonpath, "nodes.json");
    fflush(stdout);
    clusterManagerLogInfo("Saving cluster configuration to: %s\n", jsonpath);
    FILE *out = fopen(jsonpath, "w+");
    if (!out) {
        clusterManagerLogErr("Could not save nodes to: %s\n", jsonpath);
        success = 0;
        goto cleanup;
    }
    fputs(json, out);
    fclose(out);
cleanup:
    sdsfree(json);
    sdsfree(jsonpath);
    if (success) {
        if (!no_issues) {
            clusterManagerLogWarn("*** Cluster seems to have some problems, "
                                  "please be aware of it if you're going "
                                  "to restore this backup.\n");
        }
        clusterManagerLogOk("[OK] Backup created into: %s\n",
                            config.cluster_manager_command.backup_dir);
    } else clusterManagerLogOk("[ERR] Failed to back cluster!\n");
    return success;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}

static int clusterManagerCommandHelp(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);
    int commands_count = sizeof(clusterManagerCommands) /
                         sizeof(clusterManagerCommandDef);
    int i = 0, j;
    fprintf(stdout, "Cluster Manager Commands:\n");
    int padding = 15;
    for (; i < commands_count; i++) {
        clusterManagerCommandDef *def = &(clusterManagerCommands[i]);
        int namelen = strlen(def->name), padlen = padding - namelen;
        fprintf(stdout, "  %s", def->name);
        for (j = 0; j < padlen; j++) fprintf(stdout, " ");
        fprintf(stdout, "%s\n", (def->args ? def->args : ""));
        if (def->options != NULL) {
            int optslen = strlen(def->options);
            char *p = def->options, *eos = p + optslen;
            char *comma = NULL;
            while ((comma = strchr(p, ',')) != NULL) {
                int deflen = (int)(comma - p);
                char buf[255];
                memcpy(buf, p, deflen);
                buf[deflen] = '\0';
                for (j = 0; j < padding; j++) fprintf(stdout, " ");
                fprintf(stdout, "  --cluster-%s\n", buf);
                p = comma + 1;
                if (p >= eos) break;
            }
            if (p < eos) {
                for (j = 0; j < padding; j++) fprintf(stdout, " ");
                fprintf(stdout, "  --cluster-%s\n", p);
            }
        }
    }
    fprintf(stdout, "\nFor check, fix, reshard, del-node, set-timeout, "
                    "info, rebalance, call, import, backup you "
                    "can specify the host and port of any working node in "
                    "the cluster.\n");

    int options_count = sizeof(clusterManagerOptions) /
                        sizeof(clusterManagerOptionDef);
    i = 0;
    fprintf(stdout, "\nCluster Manager Options:\n");
    for (; i < options_count; i++) {
        clusterManagerOptionDef *def = &(clusterManagerOptions[i]);
        int namelen = strlen(def->name), padlen = padding - namelen;
        fprintf(stdout, "  %s", def->name);
        for (j = 0; j < padlen; j++) fprintf(stdout, " ");
        fprintf(stdout, "%s\n", def->desc);
    }

    fprintf(stdout, "\n");
    return 0;
}

/*------------------------------------------------------------------------------
 * Latency and latency history modes
 *--------------------------------------------------------------------------- */

static void latencyModePrint(long long min, long long max, double avg, long long count) {
    if (config.output == OUTPUT_STANDARD) {
        printf("min: %lld, max: %lld, avg: %.2f (%lld samples)",
                min, max, avg, count);
        fflush(stdout);
    } else if (config.output == OUTPUT_CSV) {
        printf("%lld,%lld,%.2f,%lld\n", min, max, avg, count);
    } else if (config.output == OUTPUT_RAW) {
        printf("%lld %lld %.2f %lld\n", min, max, avg, count);
    } else if (config.output == OUTPUT_JSON) {
        printf("{\"min\": %lld, \"max\": %lld, \"avg\": %.2f, \"count\": %lld}\n", min, max, avg, count);
    }
}

#define LATENCY_SAMPLE_RATE 10 /* milliseconds. */
#define LATENCY_HISTORY_DEFAULT_INTERVAL 15000 /* milliseconds. */
static void latencyMode(void) {
    redisReply *reply;
    long long start, latency, min = 0, max = 0, tot = 0, count = 0;
    long long history_interval =
        config.interval ? config.interval/1000 :
                          LATENCY_HISTORY_DEFAULT_INTERVAL;
    double avg;
    long long history_start = mstime();

    /* Set a default for the interval in case of --latency option
     * with --raw, --csv or when it is redirected to non tty. */
    if (config.interval == 0) {
        config.interval = 1000;
    } else {
        config.interval /= 1000; /* We need to convert to milliseconds. */
    }

    if (!context) exit(1);
    while(1) {
        start = mstime();
        reply = reconnectingRedisCommand(context,"PING");
        if (reply == NULL) {
            fprintf(stderr,"\nI/O error\n");
            exit(1);
        }
        latency = mstime()-start;
        freeReplyObject(reply);
        count++;
        if (count == 1) {
            min = max = tot = latency;
            avg = (double) latency;
        } else {
            if (latency < min) min = latency;
            if (latency > max) max = latency;
            tot += latency;
            avg = (double) tot/count;
        }

        if (config.output == OUTPUT_STANDARD) {
            printf("\x1b[0G\x1b[2K"); /* Clear the line. */
            latencyModePrint(min,max,avg,count);
        } else {
            if (config.latency_history) {
                latencyModePrint(min,max,avg,count);
            } else if (mstime()-history_start > config.interval) {
                latencyModePrint(min,max,avg,count);
                exit(0);
            }
        }

        if (config.latency_history && mstime()-history_start > history_interval)
        {
            printf(" -- %.2f seconds range\n", (float)(mstime()-history_start)/1000);
            history_start = mstime();
            min = max = tot = count = 0;
        }
        usleep(LATENCY_SAMPLE_RATE * 1000);
    }
}

/*------------------------------------------------------------------------------
 * Latency distribution mode -- requires 256 colors xterm
 *--------------------------------------------------------------------------- */

#define LATENCY_DIST_DEFAULT_INTERVAL 1000 /* milliseconds. */

/* Structure to store samples distribution. */
struct distsamples {
    long long max;   /* Max latency to fit into this interval (usec). */
    long long count; /* Number of samples in this interval. */
    int character;   /* Associated character in visualization. */
};

/* Helper function for latencyDistMode(). Performs the spectrum visualization
 * of the collected samples targeting an xterm 256 terminal.
 *
 * Takes an array of distsamples structures, ordered from smaller to bigger
 * 'max' value. Last sample max must be 0, to mean that it holds all the
 * samples greater than the previous one, and is also the stop sentinel.
 *
 * "tot' is the total number of samples in the different buckets, so it
 * is the SUM(samples[i].count) for i to 0 up to the max sample.
 *
 * As a side effect the function sets all the buckets count to 0. */
void showLatencyDistSamples(struct distsamples *samples, long long tot) {
    int j;

     /* We convert samples into an index inside the palette
     * proportional to the percentage a given bucket represents.
     * This way intensity of the different parts of the spectrum
     * don't change relative to the number of requests, which avoids to
     * pollute the visualization with non-latency related info. */
    printf("\033[38;5;0m"); /* Set foreground color to black. */
    for (j = 0; ; j++) {
        int coloridx =
            ceil((double) samples[j].count / tot * (spectrum_palette_size-1));
        int color = spectrum_palette[coloridx];
        printf("\033[48;5;%dm%c", (int)color, samples[j].character);
        samples[j].count = 0;
        if (samples[j].max == 0) break; /* Last sample. */
    }
    printf("\033[0m\n");
    fflush(stdout);
}

/* Show the legend: different buckets values and colors meaning, so
 * that the spectrum is more easily readable. */
void showLatencyDistLegend(void) {
    int j;

    printf("---------------------------------------------\n");
    printf(". - * #          .01 .125 .25 .5 milliseconds\n");
    printf("1,2,3,...,9      from 1 to 9     milliseconds\n");
    printf("A,B,C,D,E        10,20,30,40,50  milliseconds\n");
    printf("F,G,H,I,J        .1,.2,.3,.4,.5       seconds\n");
    printf("K,L,M,N,O,P,Q,?  1,2,4,8,16,30,60,>60 seconds\n");
    printf("From 0 to 100%%: ");
    for (j = 0; j < spectrum_palette_size; j++) {
        printf("\033[48;5;%dm ", spectrum_palette[j]);
    }
    printf("\033[0m\n");
    printf("---------------------------------------------\n");
}

static void latencyDistMode(void) {
    redisReply *reply;
    long long start, latency, count = 0;
    long long history_interval =
        config.interval ? config.interval/1000 :
                          LATENCY_DIST_DEFAULT_INTERVAL;
    long long history_start = ustime();
    int j, outputs = 0;

    struct distsamples samples[] = {
        /* We use a mostly logarithmic scale, with certain linear intervals
         * which are more interesting than others, like 1-10 milliseconds
         * range. */
        {10,0,'.'},         /* 0.01 ms */
        {125,0,'-'},        /* 0.125 ms */
        {250,0,'*'},        /* 0.25 ms */
        {500,0,'#'},        /* 0.5 ms */
        {1000,0,'1'},       /* 1 ms */
        {2000,0,'2'},       /* 2 ms */
        {3000,0,'3'},       /* 3 ms */
        {4000,0,'4'},       /* 4 ms */
        {5000,0,'5'},       /* 5 ms */
        {6000,0,'6'},       /* 6 ms */
        {7000,0,'7'},       /* 7 ms */
        {8000,0,'8'},       /* 8 ms */
        {9000,0,'9'},       /* 9 ms */
        {10000,0,'A'},      /* 10 ms */
        {20000,0,'B'},      /* 20 ms */
        {30000,0,'C'},      /* 30 ms */
        {40000,0,'D'},      /* 40 ms */
        {50000,0,'E'},      /* 50 ms */
        {100000,0,'F'},     /* 0.1 s */
        {200000,0,'G'},     /* 0.2 s */
        {300000,0,'H'},     /* 0.3 s */
        {400000,0,'I'},     /* 0.4 s */
        {500000,0,'J'},     /* 0.5 s */
        {1000000,0,'K'},    /* 1 s */
        {2000000,0,'L'},    /* 2 s */
        {4000000,0,'M'},    /* 4 s */
        {8000000,0,'N'},    /* 8 s */
        {16000000,0,'O'},   /* 16 s */
        {30000000,0,'P'},   /* 30 s */
        {60000000,0,'Q'},   /* 1 minute */
        {0,0,'?'},          /* > 1 minute */
    };

    if (!context) exit(1);
    while(1) {
        start = ustime();
        reply = reconnectingRedisCommand(context,"PING");
        if (reply == NULL) {
            fprintf(stderr,"\nI/O error\n");
            exit(1);
        }
        latency = ustime()-start;
        freeReplyObject(reply);
        count++;

        /* Populate the relevant bucket. */
        for (j = 0; ; j++) {
            if (samples[j].max == 0 || latency <= samples[j].max) {
                samples[j].count++;
                break;
            }
        }

        /* From time to time show the spectrum. */
        if (count && (ustime()-history_start)/1000 > history_interval) {
            if ((outputs++ % 20) == 0)
                showLatencyDistLegend();
            showLatencyDistSamples(samples,count);
            history_start = ustime();
            count = 0;
        }
        usleep(LATENCY_SAMPLE_RATE * 1000);
    }
}

/*------------------------------------------------------------------------------
 * Slave mode
 *--------------------------------------------------------------------------- */

#define RDB_EOF_MARK_SIZE 40

int sendReplconf(const char* arg1, const char* arg2) {
    int res = 1;
    fprintf(stderr, "sending REPLCONF %s %s\n", arg1, arg2);
    redisReply *reply = redisCommand(context, "REPLCONF %s %s", arg1, arg2);

    /* Handle any error conditions */
    if(reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        exit(1);
    } else if(reply->type == REDIS_REPLY_ERROR) {
        /* non fatal, old versions may not support it */
        fprintf(stderr, "REPLCONF %s error: %s\n", arg1, reply->str);
        res = 0;
    }
    freeReplyObject(reply);
    return res;
}

void sendCapa(void) {
    sendReplconf("capa", "eof");
}

void sendRdbOnly(void) {
    sendReplconf("rdb-only", "1");
}

/* Read raw bytes through a redisContext. The read operation is not greedy
 * and may not fill the buffer entirely.
 */
static ssize_t readConn(redisContext *c, char *buf, size_t len)
{
    return c->funcs->read(c, buf, len);
}

/* Sends SYNC and reads the number of bytes in the payload. Used both by
 * slaveMode() and getRDB().
 *
 * send_sync if 1 means we will explicitly send SYNC command. If 0 means
 * we will not send SYNC command, will send the command that in c->obuf.
 *
 * Returns the size of the RDB payload to read, or 0 in case an EOF marker is used and the size
 * is unknown, also returns 0 in case a PSYNC +CONTINUE was found (no RDB payload).
 *
 * The out_full_mode parameter if 1 means this is a full sync, if 0 means this is partial mode. */
unsigned long long sendSync(redisContext *c, int send_sync, char *out_eof, int *out_full_mode) {
    /* To start we need to send the SYNC command and return the payload.
     * The hiredis client lib does not understand this part of the protocol
     * and we don't want to mess with its buffers, so everything is performed
     * using direct low-level I/O. */
    char buf[4096], *p;
    ssize_t nread;

    if (out_full_mode) *out_full_mode = 1;

    if (send_sync) {
        /* Send the SYNC command. */
        if (cliWriteConn(c, "SYNC\r\n", 6) != 6) {
            fprintf(stderr,"Error writing to master\n");
            exit(1);
        }
    } else {
        /* We have written the command into c->obuf before. */
        if (cliWriteConn(c, "", 0) != 0) {
            fprintf(stderr,"Error writing to master\n");
            exit(1);
        }
    }

    /* Read $<payload>\r\n, making sure to read just up to "\n" */
    p = buf;
    while(1) {
        nread = readConn(c,p,1);
        if (nread <= 0) {
            fprintf(stderr,"Error reading bulk length while SYNCing\n");
            exit(1);
        }
        if (*p == '\n' && p != buf) break;
        if (*p != '\n') p++;
        if (p >= buf + sizeof(buf) - 1) break; /* Go back one more char for null-term. */
    }
    *p = '\0';
    if (buf[0] == '-') {
        fprintf(stderr, "SYNC with master failed: %s\n", buf);
        exit(1);
    }

    /* Handling PSYNC responses.
     * Read +FULLRESYNC <replid> <offset>\r\n, after that is the $<payload> or the $EOF:<40 bytes delimiter>
     * Read +CONTINUE <replid>\r\n or +CONTINUE\r\n, after that is the command stream */
    if (!strncmp(buf, "+FULLRESYNC", 11) ||
        !strncmp(buf, "+CONTINUE", 9))
    {
        int sync_partial = !strncmp(buf, "+CONTINUE", 9);
        fprintf(stderr, "PSYNC replied %s\n", buf);
        p = buf;
        while(1) {
            nread = readConn(c,p,1);
            if (nread <= 0) {
                fprintf(stderr,"Error reading bulk length while PSYNCing\n");
                exit(1);
            }
            if (*p == '\n' && p != buf) break;
            if (*p != '\n') p++;
            if (p >= buf + sizeof(buf) - 1) break; /* Go back one more char for null-term. */
        }
        *p = '\0';

        if (sync_partial) {
            if (out_full_mode) *out_full_mode = 0;
            return 0;
        }
    }

    if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= RDB_EOF_MARK_SIZE) {
        memcpy(out_eof, buf+5, RDB_EOF_MARK_SIZE);
        return 0;
    }
    return strtoull(buf+1,NULL,10);
}

static void slaveMode(int send_sync) {
    static char eofmark[RDB_EOF_MARK_SIZE];
    static char lastbytes[RDB_EOF_MARK_SIZE];
    static int usemark = 0;
    static int out_full_mode;
    unsigned long long payload = sendSync(context, send_sync, eofmark, &out_full_mode);
    char buf[1024];
    int original_output = config.output;
    char *info = out_full_mode ? "Full resync" : "Partial resync";

    if (out_full_mode == 1 && payload == 0) {
        /* SYNC with EOF marker or PSYNC +FULLRESYNC with EOF marker. */
        payload = ULLONG_MAX;
        memset(lastbytes,0,RDB_EOF_MARK_SIZE);
        usemark = 1;
        fprintf(stderr, "%s with master, discarding "
                        "bytes of bulk transfer until EOF marker...\n", info);
    } else if (out_full_mode == 1 && payload != 0) {
        /* SYNC without EOF marker or PSYNC +FULLRESYNC. */
        fprintf(stderr, "%s with master, discarding %llu "
                        "bytes of bulk transfer...\n", info, payload);
    } else if (out_full_mode == 0 && payload == 0) {
        /* PSYNC +CONTINUE (no RDB payload). */
        fprintf(stderr, "%s with master...\n", info);
    }

    /* Discard the payload. */
    while(payload) {
        ssize_t nread;

        nread = readConn(context,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"Error reading RDB payload while %sing\n", info);
            exit(1);
        }
        payload -= nread;

        if (usemark) {
            /* Update the last bytes array, and check if it matches our delimiter.*/
            if (nread >= RDB_EOF_MARK_SIZE) {
                memcpy(lastbytes,buf+nread-RDB_EOF_MARK_SIZE,RDB_EOF_MARK_SIZE);
            } else {
                int rem = RDB_EOF_MARK_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,RDB_EOF_MARK_SIZE) == 0)
                break;
        }
    }

    if (usemark) {
        unsigned long long offset = ULLONG_MAX - payload;
        fprintf(stderr,"%s done after %llu bytes. Logging commands from master.\n", info, offset);
        /* put the slave online */
        sleep(1);
        sendReplconf("ACK", "0");
    } else
        fprintf(stderr,"%s done. Logging commands from master.\n", info);

    /* Now we can use hiredis to read the incoming protocol. */
    config.output = OUTPUT_CSV;
    while (cliReadReply(0) == REDIS_OK);
    config.output = original_output;
}

/*------------------------------------------------------------------------------
 * RDB transfer mode
 *--------------------------------------------------------------------------- */

/* This function implements --rdb, so it uses the replication protocol in order
 * to fetch the RDB file from a remote server. */
static void getRDB(clusterManagerNode *node) {
    int fd;
    redisContext *s;
    char *filename;
    if (node != NULL) {
        assert(node->context);
        s = node->context;
        filename = clusterManagerGetNodeRDBFilename(node);
    } else {
        s = context;
        filename = config.rdb_filename;
    }
    static char eofmark[RDB_EOF_MARK_SIZE];
    static char lastbytes[RDB_EOF_MARK_SIZE];
    static int usemark = 0;
    unsigned long long payload = sendSync(s, 1, eofmark, NULL);
    char buf[4096];

    if (payload == 0) {
        payload = ULLONG_MAX;
        memset(lastbytes,0,RDB_EOF_MARK_SIZE);
        usemark = 1;
        fprintf(stderr,"SYNC sent to master, writing bytes of bulk transfer "
                "until EOF marker to '%s'\n", filename);
    } else {
        fprintf(stderr,"SYNC sent to master, writing %llu bytes to '%s'\n",
            payload, filename);
    }

    int write_to_stdout = !strcmp(filename,"-");
    /* Write to file. */
    if (write_to_stdout) {
        fd = STDOUT_FILENO;
    } else {
        fd = open(filename, O_CREAT|O_WRONLY, 0644);
        if (fd == -1) {
            fprintf(stderr, "Error opening '%s': %s\n", filename,
                strerror(errno));
            exit(1);
        }
    }

    while(payload) {
        ssize_t nread, nwritten;

        nread = readConn(s,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"I/O Error reading RDB payload from socket\n");
            exit(1);
        }
        nwritten = write(fd, buf, nread);
        if (nwritten != nread) {
            fprintf(stderr,"Error writing data to file: %s\n",
                (nwritten == -1) ? strerror(errno) : "short write");
            exit(1);
        }
        payload -= nread;

        if (usemark) {
            /* Update the last bytes array, and check if it matches our delimiter.*/
            if (nread >= RDB_EOF_MARK_SIZE) {
                memcpy(lastbytes,buf+nread-RDB_EOF_MARK_SIZE,RDB_EOF_MARK_SIZE);
            } else {
                int rem = RDB_EOF_MARK_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,RDB_EOF_MARK_SIZE) == 0)
                break;
        }
    }
    if (usemark) {
        payload = ULLONG_MAX - payload - RDB_EOF_MARK_SIZE;
        if (!write_to_stdout && ftruncate(fd, payload) == -1)
            fprintf(stderr,"ftruncate failed: %s.\n", strerror(errno));
        fprintf(stderr,"Transfer finished with success after %llu bytes\n", payload);
    } else {
        fprintf(stderr,"Transfer finished with success.\n");
    }
    redisFree(s); /* Close the connection ASAP as fsync() may take time. */
    if (node)
        node->context = NULL;
    if (!write_to_stdout && fsync(fd) == -1) {
        fprintf(stderr,"Fail to fsync '%s': %s\n", filename, strerror(errno));
        exit(1);
    }
    close(fd);
    if (node) {
        sdsfree(filename);
        return;
    }
    exit(0);
}

/*------------------------------------------------------------------------------
 * Bulk import (pipe) mode
 *--------------------------------------------------------------------------- */

#define PIPEMODE_WRITE_LOOP_MAX_BYTES (128*1024)
static void pipeMode(void) {
    long long errors = 0, replies = 0, obuf_len = 0, obuf_pos = 0;
    char obuf[1024*16]; /* Output buffer */
    char aneterr[ANET_ERR_LEN];
    redisReply *reply;
    int eof = 0; /* True once we consumed all the standard input. */
    int done = 0;
    char magic[20]; /* Special reply we recognize. */
    time_t last_read_time = time(NULL);

    srand(time(NULL));

    /* Use non blocking I/O. */
    if (anetNonBlock(aneterr,context->fd) == ANET_ERR) {
        fprintf(stderr, "Can't set the socket in non blocking mode: %s\n",
            aneterr);
        exit(1);
    }

    context->flags &= ~REDIS_BLOCK;

    /* Transfer raw protocol and read replies from the server at the same
     * time. */
    while(!done) {
        int mask = AE_READABLE;

        if (!eof || obuf_len != 0) mask |= AE_WRITABLE;
        mask = aeWait(context->fd,mask,1000);

        /* Handle the readable state: we can read replies from the server. */
        if (mask & AE_READABLE) {
            int read_error = 0;

            do {
                if (!read_error && redisBufferRead(context) == REDIS_ERR) {
                    read_error = 1;
                }

                reply = NULL;
                if (redisGetReply(context, (void **) &reply) == REDIS_ERR) {
                    fprintf(stderr, "Error reading replies from server\n");
                    exit(1);
                }
                if (reply) {
                    last_read_time = time(NULL);
                    if (reply->type == REDIS_REPLY_ERROR) {
                        fprintf(stderr,"%s\n", reply->str);
                        errors++;
                    } else if (eof && reply->type == REDIS_REPLY_STRING &&
                                      reply->len == 20) {
                        /* Check if this is the reply to our final ECHO
                         * command. If so everything was received
                         * from the server. */
                        if (memcmp(reply->str,magic,20) == 0) {
                            printf("Last reply received from server.\n");
                            done = 1;
                            replies--;
                        }
                    }
                    replies++;
                    freeReplyObject(reply);
                }
            } while(reply);

            /* Abort on read errors. We abort here because it is important
             * to consume replies even after a read error: this way we can
             * show a potential problem to the user. */
            if (read_error) exit(1);
        }

        /* Handle the writable state: we can send protocol to the server. */
        if (mask & AE_WRITABLE) {
            ssize_t loop_nwritten = 0;

            while(1) {
                /* Transfer current buffer to server. */
                if (obuf_len != 0) {
                    ssize_t nwritten = cliWriteConn(context,obuf+obuf_pos,obuf_len);

                    if (nwritten == -1) {
                        if (errno != EAGAIN && errno != EINTR) {
                            fprintf(stderr, "Error writing to the server: %s\n",
                                strerror(errno));
                            exit(1);
                        } else {
                            nwritten = 0;
                        }
                    }
                    obuf_len -= nwritten;
                    obuf_pos += nwritten;
                    loop_nwritten += nwritten;
                    if (obuf_len != 0) break; /* Can't accept more data. */
                }
                if (context->err) {
                    fprintf(stderr, "Server I/O Error: %s\n", context->errstr);
                    exit(1);
                }
                /* If buffer is empty, load from stdin. */
                if (obuf_len == 0 && !eof) {
                    ssize_t nread = read(STDIN_FILENO,obuf,sizeof(obuf));

                    if (nread == 0) {
                        /* The ECHO sequence starts with a "\r\n" so that if there
                         * is garbage in the protocol we read from stdin, the ECHO
                         * will likely still be properly formatted.
                         * CRLF is ignored by Redis, so it has no effects. */
                        char echo[] =
                        "\r\n*2\r\n$4\r\nECHO\r\n$20\r\n01234567890123456789\r\n";
                        int j;

                        eof = 1;
                        /* Everything transferred, so we queue a special
                         * ECHO command that we can match in the replies
                         * to make sure everything was read from the server. */
                        for (j = 0; j < 20; j++)
                            magic[j] = rand() & 0xff;
                        memcpy(echo+21,magic,20);
                        memcpy(obuf,echo,sizeof(echo)-1);
                        obuf_len = sizeof(echo)-1;
                        obuf_pos = 0;
                        printf("All data transferred. Waiting for the last reply...\n");
                    } else if (nread == -1) {
                        fprintf(stderr, "Error reading from stdin: %s\n",
                            strerror(errno));
                        exit(1);
                    } else {
                        obuf_len = nread;
                        obuf_pos = 0;
                    }
                }
                if ((obuf_len == 0 && eof) ||
                    loop_nwritten > PIPEMODE_WRITE_LOOP_MAX_BYTES) break;
            }
        }

        /* Handle timeout, that is, we reached EOF, and we are not getting
         * replies from the server for a few seconds, nor the final ECHO is
         * received. */
        if (eof && config.pipe_timeout > 0 &&
            time(NULL)-last_read_time > config.pipe_timeout)
        {
            fprintf(stderr,"No replies for %d seconds: exiting.\n",
                config.pipe_timeout);
            errors++;
            break;
        }
    }
    printf("errors: %lld, replies: %lld\n", errors, replies);
    if (errors)
        exit(1);
    else
        exit(0);
}

/*------------------------------------------------------------------------------
 * Find big keys
 *--------------------------------------------------------------------------- */

static redisReply *sendScan(unsigned long long *it) {
    redisReply *reply;

    if (config.pattern)
        reply = redisCommand(context, "SCAN %llu MATCH %b COUNT %d",
            *it, config.pattern, sdslen(config.pattern), config.count);
    else
        reply = redisCommand(context, "SCAN %llu COUNT %d",
            *it, config.count);

    /* Handle any error conditions */
    if(reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        exit(1);
    } else if(reply->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "SCAN error: %s\n", reply->str);
        exit(1);
    } else if(reply->type != REDIS_REPLY_ARRAY) {
        fprintf(stderr, "Non ARRAY response from SCAN!\n");
        exit(1);
    } else if(reply->elements != 2) {
        fprintf(stderr, "Invalid element count from SCAN!\n");
        exit(1);
    }

    /* Validate our types are correct */
    assert(reply->element[0]->type == REDIS_REPLY_STRING);
    assert(reply->element[1]->type == REDIS_REPLY_ARRAY);

    /* Update iterator */
    *it = strtoull(reply->element[0]->str, NULL, 10);

    return reply;
}

static int getDbSize(void) {
    redisReply *reply;
    int size;

    reply = redisCommand(context, "DBSIZE");

    if (reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        exit(1);
    } else if (reply->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "Couldn't determine DBSIZE: %s\n", reply->str);
        exit(1);
    } else if (reply->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "Non INTEGER response from DBSIZE!\n");
        exit(1);
    }

    /* Grab the number of keys and free our reply */
    size = reply->integer;
    freeReplyObject(reply);

    return size;
}

static int getDatabases(void) {
    redisReply *reply;
    int dbnum;

    reply = redisCommand(context, "CONFIG GET databases");

    if (reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        exit(1);
    } else if (reply->type == REDIS_REPLY_ERROR) {
        dbnum = 16;
        fprintf(stderr, "CONFIG GET databases fails: %s, use default value 16 instead\n", reply->str);
    } else {
        assert(reply->type == (config.current_resp3 ? REDIS_REPLY_MAP : REDIS_REPLY_ARRAY));
        assert(reply->elements == 2);
        dbnum = atoi(reply->element[1]->str);
    }

    freeReplyObject(reply);
    return dbnum;
}

typedef struct {
    char *name;
    char *sizecmd;
    char *sizeunit;
    unsigned long long biggest;
    unsigned long long count;
    unsigned long long totalsize;
    sds biggest_key;
} typeinfo;

typeinfo type_string = { "string", "STRLEN", "bytes" };
typeinfo type_list = { "list", "LLEN", "items" };
typeinfo type_set = { "set", "SCARD", "members" };
typeinfo type_hash = { "hash", "HLEN", "fields" };
typeinfo type_zset = { "zset", "ZCARD", "members" };
typeinfo type_stream = { "stream", "XLEN", "entries" };
typeinfo type_other = { "other", NULL, "?" };

static typeinfo* typeinfo_add(dict *types, char* name, typeinfo* type_template) {
    typeinfo *info = zmalloc(sizeof(typeinfo));
    *info = *type_template;
    info->name = sdsnew(name);
    dictAdd(types, info->name, info);
    return info;
}

void type_free(dict *d, void* val) {
    typeinfo *info = val;
    UNUSED(d);
    if (info->biggest_key)
        sdsfree(info->biggest_key);
    sdsfree(info->name);
    zfree(info);
}

static dictType typeinfoDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor (owned by the value)*/
    type_free,                 /* val destructor */
    NULL                       /* allow to expand */
};

static void getKeyTypes(dict *types_dict, redisReply *keys, typeinfo **types) {
    redisReply *reply;
    unsigned int i;

    /* Pipeline TYPE commands */
    for(i=0;i<keys->elements;i++) {
        const char* argv[] = {"TYPE", keys->element[i]->str};
        size_t lens[] = {4, keys->element[i]->len};
        redisAppendCommandArgv(context, 2, argv, lens);
    }

    /* Retrieve types */
    for(i=0;i<keys->elements;i++) {
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            fprintf(stderr, "Error getting type for key '%s' (%d: %s)\n",
                keys->element[i]->str, context->err, context->errstr);
            exit(1);
        } else if(reply->type != REDIS_REPLY_STATUS) {
            if(reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "TYPE returned an error: %s\n", reply->str);
            } else {
                fprintf(stderr,
                    "Invalid reply type (%d) for TYPE on key '%s'!\n",
                    reply->type, keys->element[i]->str);
            }
            exit(1);
        }

        sds typereply = sdsnew(reply->str);
        dictEntry *de = dictFind(types_dict, typereply);
        sdsfree(typereply);
        typeinfo *type = NULL;
        if (de)
            type = dictGetVal(de);
        else if (strcmp(reply->str, "none")) /* create new types for modules, (but not for deleted keys) */
            type = typeinfo_add(types_dict, reply->str, &type_other);
        types[i] = type;
        freeReplyObject(reply);
    }
}

static void getKeySizes(redisReply *keys, typeinfo **types,
                        unsigned long long *sizes, int memkeys,
                        unsigned memkeys_samples)
{
    redisReply *reply;
    unsigned int i;

    /* Pipeline size commands */
    for(i=0;i<keys->elements;i++) {
        /* Skip keys that disappeared between SCAN and TYPE (or unknown types when not in memkeys mode) */
        if(!types[i] || (!types[i]->sizecmd && !memkeys))
            continue;

        if (!memkeys) {
            const char* argv[] = {types[i]->sizecmd, keys->element[i]->str};
            size_t lens[] = {strlen(types[i]->sizecmd), keys->element[i]->len};
            redisAppendCommandArgv(context, 2, argv, lens);
        } else if (memkeys_samples==0) {
            const char* argv[] = {"MEMORY", "USAGE", keys->element[i]->str};
            size_t lens[] = {6, 5, keys->element[i]->len};
            redisAppendCommandArgv(context, 3, argv, lens);
        } else {
            sds samplesstr = sdsfromlonglong(memkeys_samples);
            const char* argv[] = {"MEMORY", "USAGE", keys->element[i]->str, "SAMPLES", samplesstr};
            size_t lens[] = {6, 5, keys->element[i]->len, 7, sdslen(samplesstr)};
            redisAppendCommandArgv(context, 5, argv, lens);
            sdsfree(samplesstr);
        }
    }

    /* Retrieve sizes */
    for(i=0;i<keys->elements;i++) {
        /* Skip keys that disappeared between SCAN and TYPE (or unknown types when not in memkeys mode) */
        if(!types[i] || (!types[i]->sizecmd && !memkeys)) {
            sizes[i] = 0;
            continue;
        }

        /* Retrieve size */
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            fprintf(stderr, "Error getting size for key '%s' (%d: %s)\n",
                keys->element[i]->str, context->err, context->errstr);
            exit(1);
        } else if(reply->type != REDIS_REPLY_INTEGER) {
            /* Theoretically the key could have been removed and
             * added as a different type between TYPE and SIZE */
            fprintf(stderr,
                "Warning:  %s on '%s' failed (may have changed type)\n",
                !memkeys? types[i]->sizecmd: "MEMORY USAGE",
                keys->element[i]->str);
            sizes[i] = 0;
        } else {
            sizes[i] = reply->integer;
        }

        freeReplyObject(reply);
    }
}

static void longStatLoopModeStop(int s) {
    UNUSED(s);
    force_cancel_loop = 1;
}

/* In cluster mode we may need to send the READONLY command.
   Ignore the error in case the server isn't using cluster mode. */
static void sendReadOnly(void) {
    redisReply *read_reply;
    read_reply = redisCommand(context, "READONLY");
    if (read_reply == NULL){
        fprintf(stderr, "\nI/O error\n");
        exit(1);
    } else if (read_reply->type == REDIS_REPLY_ERROR && strcmp(read_reply->str, "ERR This instance has cluster support disabled") != 0) {
        fprintf(stderr, "Error: %s\n", read_reply->str);
        exit(1);
    }
    freeReplyObject(read_reply);
}

static void findBigKeys(int memkeys, unsigned memkeys_samples) {
    unsigned long long sampled = 0, total_keys, totlen=0, *sizes=NULL, it=0, scan_loops = 0;
    redisReply *reply, *keys;
    unsigned int arrsize=0, i;
    dictIterator *di;
    dictEntry *de;
    typeinfo **types = NULL;
    double pct;

    dict *types_dict = dictCreate(&typeinfoDictType);
    typeinfo_add(types_dict, "string", &type_string);
    typeinfo_add(types_dict, "list", &type_list);
    typeinfo_add(types_dict, "set", &type_set);
    typeinfo_add(types_dict, "hash", &type_hash);
    typeinfo_add(types_dict, "zset", &type_zset);
    typeinfo_add(types_dict, "stream", &type_stream);

    signal(SIGINT, longStatLoopModeStop);
    /* Total keys pre scanning */
    total_keys = getDbSize();

    /* Status message */
    printf("\n# Scanning the entire keyspace to find biggest keys as well as\n");
    printf("# average sizes per key type.  You can use -i 0.1 to sleep 0.1 sec\n");
    printf("# per 100 SCAN commands (not usually needed).\n\n");
    
    /* Use readonly in cluster */
    sendReadOnly();

    /* SCAN loop */
    do {
        /* Calculate approximate percentage completion */
        pct = 100 * (double)sampled/total_keys;

        /* Grab some keys and point to the keys array */
        reply = sendScan(&it);
        scan_loops++;
        keys  = reply->element[1];

        /* Reallocate our type and size array if we need to */
        if(keys->elements > arrsize) {
            types = zrealloc(types, sizeof(typeinfo*)*keys->elements);
            sizes = zrealloc(sizes, sizeof(unsigned long long)*keys->elements);

            if(!types || !sizes) {
                fprintf(stderr, "Failed to allocate storage for keys!\n");
                exit(1);
            }

            arrsize = keys->elements;
        }

        /* Retrieve types and then sizes */
        getKeyTypes(types_dict, keys, types);
        getKeySizes(keys, types, sizes, memkeys, memkeys_samples);

        /* Now update our stats */
        for(i=0;i<keys->elements;i++) {
            typeinfo *type = types[i];
            /* Skip keys that disappeared between SCAN and TYPE */
            if(!type)
                continue;

            type->totalsize += sizes[i];
            type->count++;
            totlen += keys->element[i]->len;
            sampled++;

            if(type->biggest<sizes[i]) {
                /* Keep track of biggest key name for this type */
                if (type->biggest_key)
                    sdsfree(type->biggest_key);
                type->biggest_key = sdscatrepr(sdsempty(), keys->element[i]->str, keys->element[i]->len);
                if(!type->biggest_key) {
                    fprintf(stderr, "Failed to allocate memory for key!\n");
                    exit(1);
                }

                printf(
                   "[%05.2f%%] Biggest %-6s found so far '%s' with %llu %s\n",
                   pct, type->name, type->biggest_key, sizes[i],
                   !memkeys? type->sizeunit: "bytes");

                /* Keep track of the biggest size for this type */
                type->biggest = sizes[i];
            }

            /* Update overall progress */
            if(sampled % 1000000 == 0) {
                printf("[%05.2f%%] Sampled %llu keys so far\n", pct, sampled);
            }
        }

        /* Sleep if we've been directed to do so */
        if (config.interval && (scan_loops % 100) == 0) {
            usleep(config.interval);
        }

        freeReplyObject(reply);
    } while(force_cancel_loop == 0 && it != 0);

    if(types) zfree(types);
    if(sizes) zfree(sizes);

    /* We're done */
    printf("\n-------- summary -------\n\n");
    if (force_cancel_loop) printf("[%05.2f%%] ", pct);
    printf("Sampled %llu keys in the keyspace!\n", sampled);
    printf("Total key length in bytes is %llu (avg len %.2f)\n\n",
       totlen, totlen ? (double)totlen/sampled : 0);

    /* Output the biggest keys we found, for types we did find */
    di = dictGetIterator(types_dict);
    while ((de = dictNext(di))) {
        typeinfo *type = dictGetVal(de);
        if(type->biggest_key) {
            printf("Biggest %6s found '%s' has %llu %s\n", type->name, type->biggest_key,
               type->biggest, !memkeys? type->sizeunit: "bytes");
        }
    }
    dictReleaseIterator(di);

    printf("\n");

    di = dictGetIterator(types_dict);
    while ((de = dictNext(di))) {
        typeinfo *type = dictGetVal(de);
        printf("%llu %ss with %llu %s (%05.2f%% of keys, avg size %.2f)\n",
           type->count, type->name, type->totalsize, !memkeys? type->sizeunit: "bytes",
           sampled ? 100 * (double)type->count/sampled : 0,
           type->count ? (double)type->totalsize/type->count : 0);
    }
    dictReleaseIterator(di);

    dictRelease(types_dict);

    /* Success! */
    exit(0);
}

static void getKeyFreqs(redisReply *keys, unsigned long long *freqs) {
    redisReply *reply;
    unsigned int i;

    /* Pipeline OBJECT freq commands */
    for(i=0;i<keys->elements;i++) {
        const char* argv[] = {"OBJECT", "FREQ", keys->element[i]->str};
        size_t lens[] = {6, 4, keys->element[i]->len};
        redisAppendCommandArgv(context, 3, argv, lens);
    }

    /* Retrieve freqs */
    for(i=0;i<keys->elements;i++) {
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            sds keyname = sdscatrepr(sdsempty(), keys->element[i]->str, keys->element[i]->len);
            fprintf(stderr, "Error getting freq for key '%s' (%d: %s)\n",
                keyname, context->err, context->errstr);
            sdsfree(keyname);
            exit(1);
        } else if(reply->type != REDIS_REPLY_INTEGER) {
            if(reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
                exit(1);
            } else {
                sds keyname = sdscatrepr(sdsempty(), keys->element[i]->str, keys->element[i]->len);
                fprintf(stderr, "Warning: OBJECT freq on '%s' failed (may have been deleted)\n", keyname);
                sdsfree(keyname);
                freqs[i] = 0;
            }
        } else {
            freqs[i] = reply->integer;
        }
        freeReplyObject(reply);
    }
}

#define HOTKEYS_SAMPLE 16
static void findHotKeys(void) {
    redisReply *keys, *reply;
    unsigned long long counters[HOTKEYS_SAMPLE] = {0};
    sds hotkeys[HOTKEYS_SAMPLE] = {NULL};
    unsigned long long sampled = 0, total_keys, *freqs = NULL, it = 0, scan_loops = 0;
    unsigned int arrsize = 0, i, k;
    double pct;

    signal(SIGINT, longStatLoopModeStop);
    /* Total keys pre scanning */
    total_keys = getDbSize();

    /* Status message */
    printf("\n# Scanning the entire keyspace to find hot keys as well as\n");
    printf("# average sizes per key type.  You can use -i 0.1 to sleep 0.1 sec\n");
    printf("# per 100 SCAN commands (not usually needed).\n\n");

    /* Use readonly in cluster */
    sendReadOnly();
    
    /* SCAN loop */
    do {
        /* Calculate approximate percentage completion */
        pct = 100 * (double)sampled/total_keys;

        /* Grab some keys and point to the keys array */
        reply = sendScan(&it);
        scan_loops++;
        keys  = reply->element[1];

        /* Reallocate our freqs array if we need to */
        if(keys->elements > arrsize) {
            freqs = zrealloc(freqs, sizeof(unsigned long long)*keys->elements);

            if(!freqs) {
                fprintf(stderr, "Failed to allocate storage for keys!\n");
                exit(1);
            }

            arrsize = keys->elements;
        }

        getKeyFreqs(keys, freqs);

        /* Now update our stats */
        for(i=0;i<keys->elements;i++) {
            sampled++;
            /* Update overall progress */
            if(sampled % 1000000 == 0) {
                printf("[%05.2f%%] Sampled %llu keys so far\n", pct, sampled);
            }

            /* Use eviction pool here */
            k = 0;
            while (k < HOTKEYS_SAMPLE && freqs[i] > counters[k]) k++;
            if (k == 0) continue;
            k--;
            if (k == 0 || counters[k] == 0) {
                sdsfree(hotkeys[k]);
            } else {
                sdsfree(hotkeys[0]);
                memmove(counters,counters+1,sizeof(counters[0])*k);
                memmove(hotkeys,hotkeys+1,sizeof(hotkeys[0])*k);
            }
            counters[k] = freqs[i];
            hotkeys[k] = sdscatrepr(sdsempty(), keys->element[i]->str, keys->element[i]->len);
            printf(
               "[%05.2f%%] Hot key '%s' found so far with counter %llu\n",
               pct, hotkeys[k], freqs[i]);
        }

        /* Sleep if we've been directed to do so */
        if (config.interval && (scan_loops % 100) == 0) {
            usleep(config.interval);
        }

        freeReplyObject(reply);
    } while(force_cancel_loop ==0 && it != 0);

    if (freqs) zfree(freqs);

    /* We're done */
    printf("\n-------- summary -------\n\n");
    if(force_cancel_loop)printf("[%05.2f%%] ",pct);
    printf("Sampled %llu keys in the keyspace!\n", sampled);

    for (i=1; i<= HOTKEYS_SAMPLE; i++) {
        k = HOTKEYS_SAMPLE - i;
        if(counters[k]>0) {
            printf("hot key found with counter: %llu\tkeyname: %s\n", counters[k], hotkeys[k]);
            sdsfree(hotkeys[k]);
        }
    }

    exit(0);
}

/*------------------------------------------------------------------------------
 * Stats mode
 *--------------------------------------------------------------------------- */

/* Return the specified INFO field from the INFO command output "info".
 * A new buffer is allocated for the result, that needs to be free'd.
 * If the field is not found NULL is returned. */
static char *getInfoField(char *info, char *field) {
    char *p = strstr(info,field);
    char *n1, *n2;
    char *result;

    if (!p) return NULL;
    p += strlen(field)+1;
    n1 = strchr(p,'\r');
    n2 = strchr(p,',');
    if (n2 && n2 < n1) n1 = n2;
    result = zmalloc(sizeof(char)*(n1-p)+1);
    memcpy(result,p,(n1-p));
    result[n1-p] = '\0';
    return result;
}

/* Like the above function but automatically convert the result into
 * a long. On error (missing field) LONG_MIN is returned. */
static long getLongInfoField(char *info, char *field) {
    char *value = getInfoField(info,field);
    long l;

    if (!value) return LONG_MIN;
    l = strtol(value,NULL,10);
    zfree(value);
    return l;
}

/* Convert number of bytes into a human readable string of the form:
 * 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, size_t size, long long n) {
    double d;

    if (n < 0) {
        *s = '-';
        s++;
        n = -n;
    }
    if (n < 1024) {
        /* Bytes */
        snprintf(s,size,"%lldB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        snprintf(s,size,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        snprintf(s,size,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        snprintf(s,size,"%.2fG",d);
    }
}

static void statMode(void) {
    redisReply *reply;
    long aux, requests = 0;
    int dbnum = getDatabases();
    int i = 0;

    while(1) {
        char buf[64];
        int j;

        reply = reconnectingRedisCommand(context,"INFO");
        if (reply == NULL) {
            fprintf(stderr, "\nI/O error\n");
            exit(1);
        } else if (reply->type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "ERROR: %s\n", reply->str);
            exit(1);
        }

        if ((i++ % 20) == 0) {
            printf(
"------- data ------ --------------------- load -------------------- - child -\n"
"keys       mem      clients blocked requests            connections          \n");
        }

        /* Keys */
        aux = 0;
        for (j = 0; j < dbnum; j++) {
            long k;

            snprintf(buf,sizeof(buf),"db%d:keys",j);
            k = getLongInfoField(reply->str,buf);
            if (k == LONG_MIN) continue;
            aux += k;
        }
        snprintf(buf,sizeof(buf),"%ld",aux);
        printf("%-11s",buf);

        /* Used memory */
        aux = getLongInfoField(reply->str,"used_memory");
        bytesToHuman(buf,sizeof(buf),aux);
        printf("%-8s",buf);

        /* Clients */
        aux = getLongInfoField(reply->str,"connected_clients");
        snprintf(buf,sizeof(buf),"%ld",aux);
        printf(" %-8s",buf);

        /* Blocked (BLPOPPING) Clients */
        aux = getLongInfoField(reply->str,"blocked_clients");
        snprintf(buf,sizeof(buf),"%ld",aux);
        printf("%-8s",buf);

        /* Requests */
        aux = getLongInfoField(reply->str,"total_commands_processed");
        snprintf(buf,sizeof(buf),"%ld (+%ld)",aux,requests == 0 ? 0 : aux-requests);
        printf("%-19s",buf);
        requests = aux;

        /* Connections */
        aux = getLongInfoField(reply->str,"total_connections_received");
        snprintf(buf,sizeof(buf),"%ld",aux);
        printf(" %-12s",buf);

        /* Children */
        aux = getLongInfoField(reply->str,"bgsave_in_progress");
        aux |= getLongInfoField(reply->str,"aof_rewrite_in_progress") << 1;
        aux |= getLongInfoField(reply->str,"loading") << 2;
        switch(aux) {
        case 0: break;
        case 1:
            printf("SAVE");
            break;
        case 2:
            printf("AOF");
            break;
        case 3:
            printf("SAVE+AOF");
            break;
        case 4:
            printf("LOAD");
            break;
        }

        printf("\n");
        freeReplyObject(reply);
        usleep(config.interval);
    }
}

/*------------------------------------------------------------------------------
 * Scan mode
 *--------------------------------------------------------------------------- */

static void scanMode(void) {
    redisReply *reply;
    unsigned long long cur = 0;
    signal(SIGINT, longStatLoopModeStop);
    do {
        reply = sendScan(&cur);
        for (unsigned int j = 0; j < reply->element[1]->elements; j++) {
            if (config.output == OUTPUT_STANDARD) {
                sds out = sdscatrepr(sdsempty(), reply->element[1]->element[j]->str,
                                     reply->element[1]->element[j]->len);
                printf("%s\n", out);
                sdsfree(out);
            } else {
                printf("%s\n", reply->element[1]->element[j]->str);
            }
        }
        freeReplyObject(reply);
        if (config.interval) usleep(config.interval);
    } while(force_cancel_loop == 0 && cur != 0);

    exit(0);
}

/*------------------------------------------------------------------------------
 * LRU test mode
 *--------------------------------------------------------------------------- */

/* Return an integer from min to max (both inclusive) using a power-law
 * distribution, depending on the value of alpha: the greater the alpha
 * the more bias towards lower values.
 *
 * With alpha = 6.2 the output follows the 80-20 rule where 20% of
 * the returned numbers will account for 80% of the frequency. */
long long powerLawRand(long long min, long long max, double alpha) {
    double pl, r;

    max += 1;
    r = ((double)rand()) / RAND_MAX;
    pl = pow(
        ((pow(max,alpha+1) - pow(min,alpha+1))*r + pow(min,alpha+1)),
        (1.0/(alpha+1)));
    return (max-1-(long long)pl)+min;
}

/* Generates a key name among a set of lru_test_sample_size keys, using
 * an 80-20 distribution. */
void LRUTestGenKey(char *buf, size_t buflen) {
    snprintf(buf, buflen, "lru:%lld",
        powerLawRand(1, config.lru_test_sample_size, 6.2));
}

#define LRU_CYCLE_PERIOD 1000 /* 1000 milliseconds. */
#define LRU_CYCLE_PIPELINE_SIZE 250
static void LRUTestMode(void) {
    redisReply *reply;
    char key[128];
    long long start_cycle;
    int j;

    srand(time(NULL)^getpid());
    while(1) {
        /* Perform cycles of 1 second with 50% writes and 50% reads.
         * We use pipelining batching writes / reads N times per cycle in order
         * to fill the target instance easily. */
        start_cycle = mstime();
        long long hits = 0, misses = 0;
        while(mstime() - start_cycle < LRU_CYCLE_PERIOD) {
            /* Write cycle. */
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++) {
                char val[6];
                val[5] = '\0';
                for (int i = 0; i < 5; i++) val[i] = 'A'+rand()%('z'-'A');
                LRUTestGenKey(key,sizeof(key));
                redisAppendCommand(context, "SET %s %s",key,val);
            }
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++)
                redisGetReply(context, (void**)&reply);

            /* Read cycle. */
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++) {
                LRUTestGenKey(key,sizeof(key));
                redisAppendCommand(context, "GET %s",key);
            }
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++) {
                if (redisGetReply(context, (void**)&reply) == REDIS_OK) {
                    switch(reply->type) {
                        case REDIS_REPLY_ERROR:
                            fprintf(stderr, "%s\n", reply->str);
                            break;
                        case REDIS_REPLY_NIL:
                            misses++;
                            break;
                        default:
                            hits++;
                            break;
                    }
                }
            }

            if (context->err) {
                fprintf(stderr,"I/O error during LRU test\n");
                exit(1);
            }
        }
        /* Print stats. */
        printf(
            "%lld Gets/sec | Hits: %lld (%.2f%%) | Misses: %lld (%.2f%%)\n",
            hits+misses,
            hits, (double)hits/(hits+misses)*100,
            misses, (double)misses/(hits+misses)*100);
    }
    exit(0);
}

/*------------------------------------------------------------------------------
 * Intrinsic latency mode.
 *
 * Measure max latency of a running process that does not result from
 * syscalls. Basically this software should provide a hint about how much
 * time the kernel leaves the process without a chance to run.
 *--------------------------------------------------------------------------- */

/* This is just some computation the compiler can't optimize out.
 * Should run in less than 100-200 microseconds even using very
 * slow hardware. Runs in less than 10 microseconds in modern HW. */
unsigned long compute_something_fast(void) {
    unsigned char s[256], i, j, t;
    int count = 1000, k;
    unsigned long output = 0;

    for (k = 0; k < 256; k++) s[k] = k;

    i = 0;
    j = 0;
    while(count--) {
        i++;
        j = j + s[i];
        t = s[i];
        s[i] = s[j];
        s[j] = t;
        output += s[(s[i]+s[j])&255];
    }
    return output;
}

static void sigIntHandler(int s) {
    UNUSED(s);

    if (config.monitor_mode || config.pubsub_mode) {
        close(context->fd);
        context->fd = REDIS_INVALID_FD;
        config.blocking_state_aborted = 1;
    } else {
        exit(1);
    }
}

static void intrinsicLatencyMode(void) {
    long long test_end, run_time, max_latency = 0, runs = 0;

    run_time = (long long)config.intrinsic_latency_duration * 1000000;
    test_end = ustime() + run_time;
    signal(SIGINT, longStatLoopModeStop);

    while(1) {
        long long start, end, latency;

        start = ustime();
        compute_something_fast();
        end = ustime();
        latency = end-start;
        runs++;
        if (latency <= 0) continue;

        /* Reporting */
        if (latency > max_latency) {
            max_latency = latency;
            printf("Max latency so far: %lld microseconds.\n", max_latency);
        }

        double avg_us = (double)run_time/runs;
        double avg_ns = avg_us * 1e3;
        if (force_cancel_loop || end > test_end) {
            printf("\n%lld total runs "
                "(avg latency: "
                "%.4f microseconds / %.2f nanoseconds per run).\n",
                runs, avg_us, avg_ns);
            printf("Worst run took %.0fx longer than the average latency.\n",
                max_latency / avg_us);
            exit(0);
        }
    }
}

static sds askPassword(const char *msg) {
    linenoiseMaskModeEnable();
    sds auth = linenoise(msg);
    linenoiseMaskModeDisable();
    return auth;
}

/* Prints out the hint completion string for a given input prefix string. */
void testHint(const char *input) {
    cliInitHelp();

    sds hint = getHintForInput(input);
    printf("%s\n", hint);
    exit(0);
}

sds readHintSuiteLine(char buf[], size_t size, FILE *fp) {
    while (fgets(buf, size, fp) != NULL) {
        if (buf[0] != '#') {
            sds input = sdsnew(buf);

            /* Strip newline. */
            input = sdstrim(input, "\n");
            return input;
        }
    }
    return NULL;
}

/* Runs a suite of hint completion tests contained in a file. */
void testHintSuite(char *filename) {
    FILE *fp;
    char buf[256];
    sds line, input, expected, hint;
    int pass=0, fail=0;
    int argc;
    char **argv;

    fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr,
            "Can't open file '%s': %s\n", filename, strerror(errno));
        exit(-1);
    }

    cliInitHelp();

    while (1) {
        line = readHintSuiteLine(buf, sizeof(buf), fp);
        if (line == NULL) break;
        argv = sdssplitargs(line, &argc);
        sdsfree(line);
        if (argc == 0) {
            sdsfreesplitres(argv, argc);
            continue;
        }

        if (argc == 1) {
            fprintf(stderr,
                "Missing expected hint for input '%s'\n", argv[0]);
            exit(-1);
        }
        input = argv[0];
        expected = argv[1];
        hint = getHintForInput(input);
        if (config.verbose) {
            printf("Input: '%s', Expected: '%s', Hint: '%s'\n", input, expected, hint);
        }

        /* Strip trailing spaces from hint - they don't matter. */
        while (hint != NULL && sdslen(hint) > 0 && hint[sdslen(hint) - 1] == ' ') {            
            sdssetlen(hint, sdslen(hint) - 1);
            hint[sdslen(hint)] = '\0';
        }

        if (hint == NULL || strcmp(hint, expected) != 0) {
            fprintf(stderr, "Test case '%s' FAILED: expected '%s', got '%s'\n", input, expected, hint);
            ++fail;
        }
        else {
            ++pass;
        }
        sdsfreesplitres(argv, argc);
        sdsfree(hint);
    }
    fclose(fp);
    
    printf("%s: %d/%d passed\n", fail == 0 ? "SUCCESS" : "FAILURE", pass, pass + fail);
    exit(fail);
}

/*------------------------------------------------------------------------------
 * Program main()
 *--------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    int firstarg;
    struct timeval tv;

    memset(&config.sslconfig, 0, sizeof(config.sslconfig));
    config.conn_info.hostip = sdsnew("127.0.0.1");
    config.conn_info.hostport = 6379;
    config.connect_timeout.tv_sec = 0;
    config.connect_timeout.tv_usec = 0;
    config.hostsocket = NULL;
    config.repeat = 1;
    config.interval = 0;
    config.dbnum = 0;
    config.conn_info.input_dbnum = 0;
    config.interactive = 0;
    config.shutdown = 0;
    config.monitor_mode = 0;
    config.pubsub_mode = 0;
    config.blocking_state_aborted = 0;
    config.latency_mode = 0;
    config.latency_dist_mode = 0;
    config.latency_history = 0;
    config.lru_test_mode = 0;
    config.lru_test_sample_size = 0;
    config.cluster_mode = 0;
    config.cluster_send_asking = 0;
    config.slave_mode = 0;
    config.getrdb_mode = 0;
    config.get_functions_rdb_mode = 0;
    config.stat_mode = 0;
    config.scan_mode = 0;
    config.count = 10;
    config.intrinsic_latency_mode = 0;
    config.pattern = NULL;
    config.rdb_filename = NULL;
    config.pipe_mode = 0;
    config.pipe_timeout = REDIS_CLI_DEFAULT_PIPE_TIMEOUT;
    config.bigkeys = 0;
    config.memkeys = 0;
    config.hotkeys = 0;
    config.stdin_lastarg = 0;
    config.stdin_tag_arg = 0;
    config.stdin_tag_name = NULL;
    config.conn_info.auth = NULL;
    config.askpass = 0;
    config.conn_info.user = NULL;
    config.eval = NULL;
    config.eval_ldb = 0;
    config.eval_ldb_end = 0;
    config.eval_ldb_sync = 0;
    config.enable_ldb_on_eval = 0;
    config.last_cmd_type = -1;
    config.last_reply = NULL;
    config.verbose = 0;
    config.set_errcode = 0;
    config.no_auth_warning = 0;
    config.in_multi = 0;
    config.server_version = NULL;
    config.prefer_ipv4 = 0;
    config.prefer_ipv6 = 0;
    config.cluster_manager_command.name = NULL;
    config.cluster_manager_command.argc = 0;
    config.cluster_manager_command.argv = NULL;
    config.cluster_manager_command.stdin_arg = NULL;
    config.cluster_manager_command.flags = 0;
    config.cluster_manager_command.replicas = 0;
    config.cluster_manager_command.from = NULL;
    config.cluster_manager_command.to = NULL;
    config.cluster_manager_command.from_user = NULL;
    config.cluster_manager_command.from_pass = NULL;
    config.cluster_manager_command.from_askpass = 0;
    config.cluster_manager_command.weight = NULL;
    config.cluster_manager_command.weight_argc = 0;
    config.cluster_manager_command.slots = 0;
    config.cluster_manager_command.timeout = CLUSTER_MANAGER_MIGRATE_TIMEOUT;
    config.cluster_manager_command.pipeline = CLUSTER_MANAGER_MIGRATE_PIPELINE;
    config.cluster_manager_command.threshold =
        CLUSTER_MANAGER_REBALANCE_THRESHOLD;
    config.cluster_manager_command.backup_dir = NULL;
    pref.hints = 1;

    spectrum_palette = spectrum_palette_color;
    spectrum_palette_size = spectrum_palette_color_size;

    if (!isatty(fileno(stdout)) && (getenv("FAKETTY") == NULL)) {
        config.output = OUTPUT_RAW;
        config.push_output = 0;
    } else {
        config.output = OUTPUT_STANDARD;
        config.push_output = 1;
    }
    config.mb_delim = sdsnew("\n");
    config.cmd_delim = sdsnew("\n");

    firstarg = parseOptions(argc,argv);
    argc -= firstarg;
    argv += firstarg;

    parseEnv();

    if (config.askpass) {
        config.conn_info.auth = askPassword("Please input password: ");
    }

    if (config.cluster_manager_command.from_askpass) {
        config.cluster_manager_command.from_pass = askPassword(
            "Please input import source node password: ");
    }

#ifdef USE_OPENSSL
    if (config.tls) {
        cliSecureInit();
    }
#endif

    gettimeofday(&tv, NULL);
    init_genrand64(((long long) tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());

    /* Cluster Manager mode */
    if (CLUSTER_MANAGER_MODE()) {
        clusterManagerCommandProc *proc = validateClusterManagerCommand();
        if (!proc) {
            exit(1);
        }
        clusterManagerMode(proc);
    }

    /* Latency mode */
    if (config.latency_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        latencyMode();
    }

    /* Latency distribution mode */
    if (config.latency_dist_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        latencyDistMode();
    }

    /* Slave mode */
    if (config.slave_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        sendCapa();
        sendReplconf("rdb-filter-only", "");
        slaveMode(1);
    }

    /* Get RDB/functions mode. */
    if (config.getrdb_mode || config.get_functions_rdb_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        sendCapa();
        sendRdbOnly();
        if (config.get_functions_rdb_mode && !sendReplconf("rdb-filter-only", "functions")) {
            fprintf(stderr, "Failed requesting functions only RDB from server, aborting\n");
            exit(1);
        }
        getRDB(NULL);
    }

    /* Pipe mode */
    if (config.pipe_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        pipeMode();
    }

    /* Find big keys */
    if (config.bigkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findBigKeys(0, 0);
    }

    /* Find large keys */
    if (config.memkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findBigKeys(1, config.memkeys_samples);
    }

    /* Find hot keys */
    if (config.hotkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findHotKeys();
    }

    /* Stat mode */
    if (config.stat_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        if (config.interval == 0) config.interval = 1000000;
        statMode();
    }

    /* Scan mode */
    if (config.scan_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        scanMode();
    }

    /* LRU test mode */
    if (config.lru_test_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        LRUTestMode();
    }

    /* Intrinsic latency mode */
    if (config.intrinsic_latency_mode) intrinsicLatencyMode();

    /* Print command-line hint for an input prefix string */
    if (config.test_hint) {
        testHint(config.test_hint);
    }
    /* Run test suite for command-line hints */
    if (config.test_hint_file) {
        testHintSuite(config.test_hint_file);
    }

    /* Start interactive mode when no command is provided */
    if (argc == 0 && !config.eval) {
        /* Ignore SIGPIPE in interactive mode to force a reconnect */
        signal(SIGPIPE, SIG_IGN);
        signal(SIGINT, sigIntHandler);

        /* Note that in repl mode we don't abort on connection error.
         * A new attempt will be performed for every command send. */
        cliConnect(0);
        repl();
    }

    /* Otherwise, we have some arguments to execute */
    if (config.eval) {
        if (cliConnect(0) != REDIS_OK) exit(1);
        return evalMode(argc,argv);
    } else {
        cliConnect(CC_QUIET);
        return noninteractive(argc,argv);
    }
}
