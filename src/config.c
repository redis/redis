/* Configuration file parsing and CONFIG GET/SET commands implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "cluster.h"
#include "connection.h"
#include "bio.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <glob.h>
#include <string.h>
#include <locale.h>

/*-----------------------------------------------------------------------------
 * Config file name-value maps.
 *----------------------------------------------------------------------------*/

typedef struct deprecatedConfig {
    const char *name;
    const int argc_min;
    const int argc_max;
} deprecatedConfig;

configEnum maxmemory_policy_enum[] = {
    {"volatile-lru", MAXMEMORY_VOLATILE_LRU},
    {"volatile-lfu", MAXMEMORY_VOLATILE_LFU},
    {"volatile-random",MAXMEMORY_VOLATILE_RANDOM},
    {"volatile-ttl",MAXMEMORY_VOLATILE_TTL},
    {"allkeys-lru",MAXMEMORY_ALLKEYS_LRU},
    {"allkeys-lfu",MAXMEMORY_ALLKEYS_LFU},
    {"allkeys-random",MAXMEMORY_ALLKEYS_RANDOM},
    {"noeviction",MAXMEMORY_NO_EVICTION},
    {NULL, 0}
};

configEnum syslog_facility_enum[] = {
    {"user",    LOG_USER},
    {"local0",  LOG_LOCAL0},
    {"local1",  LOG_LOCAL1},
    {"local2",  LOG_LOCAL2},
    {"local3",  LOG_LOCAL3},
    {"local4",  LOG_LOCAL4},
    {"local5",  LOG_LOCAL5},
    {"local6",  LOG_LOCAL6},
    {"local7",  LOG_LOCAL7},
    {NULL, 0}
};

configEnum loglevel_enum[] = {
    {"debug", LL_DEBUG},
    {"verbose", LL_VERBOSE},
    {"notice", LL_NOTICE},
    {"warning", LL_WARNING},
    {"nothing", LL_NOTHING},
    {NULL,0}
};

configEnum supervised_mode_enum[] = {
    {"upstart", SUPERVISED_UPSTART},
    {"systemd", SUPERVISED_SYSTEMD},
    {"auto", SUPERVISED_AUTODETECT},
    {"no", SUPERVISED_NONE},
    {NULL, 0}
};

configEnum aof_fsync_enum[] = {
    {"everysec", AOF_FSYNC_EVERYSEC},
    {"always", AOF_FSYNC_ALWAYS},
    {"no", AOF_FSYNC_NO},
    {NULL, 0}
};

configEnum shutdown_on_sig_enum[] = {
    {"default", 0},
    {"save", SHUTDOWN_SAVE},
    {"nosave", SHUTDOWN_NOSAVE},
    {"now", SHUTDOWN_NOW},
    {"force", SHUTDOWN_FORCE},
    {NULL, 0}
};

configEnum repl_diskless_load_enum[] = {
    {"disabled", REPL_DISKLESS_LOAD_DISABLED},
    {"on-empty-db", REPL_DISKLESS_LOAD_WHEN_DB_EMPTY},
    {"swapdb", REPL_DISKLESS_LOAD_SWAPDB},
    {NULL, 0}
};

configEnum tls_auth_clients_enum[] = {
    {"no", TLS_CLIENT_AUTH_NO},
    {"yes", TLS_CLIENT_AUTH_YES},
    {"optional", TLS_CLIENT_AUTH_OPTIONAL},
    {NULL, 0}
};

configEnum oom_score_adj_enum[] = {
    {"no", OOM_SCORE_ADJ_NO},
    {"yes", OOM_SCORE_RELATIVE},
    {"relative", OOM_SCORE_RELATIVE},
    {"absolute", OOM_SCORE_ADJ_ABSOLUTE},
    {NULL, 0}
};

configEnum acl_pubsub_default_enum[] = {
    {"allchannels", SELECTOR_FLAG_ALLCHANNELS},
    {"resetchannels", 0},
    {NULL, 0}
};

configEnum sanitize_dump_payload_enum[] = {
    {"no", SANITIZE_DUMP_NO},
    {"yes", SANITIZE_DUMP_YES},
    {"clients", SANITIZE_DUMP_CLIENTS},
    {NULL, 0}
};

configEnum protected_action_enum[] = {
    {"no", PROTECTED_ACTION_ALLOWED_NO},
    {"yes", PROTECTED_ACTION_ALLOWED_YES},
    {"local", PROTECTED_ACTION_ALLOWED_LOCAL},
    {NULL, 0}
};

configEnum cluster_preferred_endpoint_type_enum[] = {
    {"ip", CLUSTER_ENDPOINT_TYPE_IP},
    {"hostname", CLUSTER_ENDPOINT_TYPE_HOSTNAME},
    {"unknown-endpoint", CLUSTER_ENDPOINT_TYPE_UNKNOWN_ENDPOINT},
    {NULL, 0}
};

configEnum propagation_error_behavior_enum[] = {
    {"ignore", PROPAGATION_ERR_BEHAVIOR_IGNORE},
    {"panic", PROPAGATION_ERR_BEHAVIOR_PANIC},
    {"panic-on-replicas", PROPAGATION_ERR_BEHAVIOR_PANIC_ON_REPLICAS},
    {NULL, 0}
};

/* Output buffer limits presets. */
clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT] = {
    {0, 0, 0}, /* normal */
    {1024*1024*256, 1024*1024*64, 60}, /* slave */
    {1024*1024*32, 1024*1024*8, 60}  /* pubsub */
};

/* OOM Score defaults */
int configOOMScoreAdjValuesDefaults[CONFIG_OOM_COUNT] = { 0, 200, 800 };

/* Generic config infrastructure function pointers
 * int is_valid_fn(val, err)
 *     Return 1 when val is valid, and 0 when invalid.
 *     Optionally set err to a static error string.
 */

/* Configuration values that require no special handling to set, get, load or
 * rewrite. */
typedef struct boolConfigData {
    int *config; /* The pointer to the server config this value is stored in */
    int default_value; /* The default value of the config on rewrite */
    int (*is_valid_fn)(int val, const char **err); /* Optional function to check validity of new value (generic doc above) */
} boolConfigData;

typedef struct stringConfigData {
    char **config; /* Pointer to the server config this value is stored in. */
    const char *default_value; /* Default value of the config on rewrite. */
    int (*is_valid_fn)(char* val, const char **err); /* Optional function to check validity of new value (generic doc above) */
    int convert_empty_to_null; /* Boolean indicating if empty strings should
                                  be stored as a NULL value. */
} stringConfigData;

typedef struct sdsConfigData {
    sds *config; /* Pointer to the server config this value is stored in. */
    char *default_value; /* Default value of the config on rewrite. */
    int (*is_valid_fn)(sds val, const char **err); /* Optional function to check validity of new value (generic doc above) */
    int convert_empty_to_null; /* Boolean indicating if empty SDS strings should
                                  be stored as a NULL value. */
} sdsConfigData;

typedef struct enumConfigData {
    int *config; /* The pointer to the server config this value is stored in */
    configEnum *enum_value; /* The underlying enum type this data represents */
    int default_value; /* The default value of the config on rewrite */
    int (*is_valid_fn)(int val, const char **err); /* Optional function to check validity of new value (generic doc above) */
} enumConfigData;

typedef enum numericType {
    NUMERIC_TYPE_INT,
    NUMERIC_TYPE_UINT,
    NUMERIC_TYPE_LONG,
    NUMERIC_TYPE_ULONG,
    NUMERIC_TYPE_LONG_LONG,
    NUMERIC_TYPE_ULONG_LONG,
    NUMERIC_TYPE_SIZE_T,
    NUMERIC_TYPE_SSIZE_T,
    NUMERIC_TYPE_OFF_T,
    NUMERIC_TYPE_TIME_T,
} numericType;

typedef struct numericConfigData {
    union {
        int *i;
        unsigned int *ui;
        long *l;
        unsigned long *ul;
        long long *ll;
        unsigned long long *ull;
        size_t *st;
        ssize_t *sst;
        off_t *ot;
        time_t *tt;
    } config; /* The pointer to the numeric config this value is stored in */
    unsigned int flags;
    numericType numeric_type; /* An enum indicating the type of this value */
    long long lower_bound; /* The lower bound of this numeric value */
    long long upper_bound; /* The upper bound of this numeric value */
    long long default_value; /* The default value of the config on rewrite */
    int (*is_valid_fn)(long long val, const char **err); /* Optional function to check validity of new value (generic doc above) */
} numericConfigData;

typedef union typeData {
    boolConfigData yesno;
    stringConfigData string;
    sdsConfigData sds;
    enumConfigData enumd;
    numericConfigData numeric;
} typeData;

typedef struct standardConfig standardConfig;

typedef int (*apply_fn)(const char **err);
typedef struct typeInterface {
    /* Called on server start, to init the server with default value */
    void (*init)(standardConfig *config);
    /* Called on server startup and CONFIG SET, returns 1 on success,
     * 2 meaning no actual change done, 0 on error and can set a verbose err
     * string */
    int (*set)(standardConfig *config, sds *argv, int argc, const char **err);
    /* Optional: called after `set()` to apply the config change. Used only in
     * the context of CONFIG SET. Returns 1 on success, 0 on failure.
     * Optionally set err to a static error string. */
    apply_fn apply;
    /* Called on CONFIG GET, returns sds to be used in reply */
    sds (*get)(standardConfig *config);
    /* Called on CONFIG REWRITE, required to rewrite the config state */
    void (*rewrite)(standardConfig *config, const char *name, struct rewriteConfigState *state);
} typeInterface;

struct standardConfig {
    const char *name; /* The user visible name of this config */
    const char *alias; /* An alias that can also be used for this config */
    unsigned int flags; /* Flags for this specific config */
    typeInterface interface; /* The function pointers that define the type interface */
    typeData data; /* The type specific data exposed used by the interface */
    configType type; /* The type of config this is. */
    void *privdata; /* privdata for this config, for module configs this is a ModuleConfig struct */
};

dict *configs = NULL; /* Runtime config values */

/* Lookup a config by the provided sds string name, or return NULL
 * if the config does not exist */
static standardConfig *lookupConfig(sds name) {
    dictEntry *de = dictFind(configs, name);
    return de ? dictGetVal(de) : NULL;
}

/*-----------------------------------------------------------------------------
 * Enum access functions
 *----------------------------------------------------------------------------*/

/* Get enum value from name. If there is no match INT_MIN is returned. */
int configEnumGetValue(configEnum *ce, sds *argv, int argc, int bitflags) {
    if (argc == 0 || (!bitflags && argc != 1)) return INT_MIN;
    int values = 0;
    for (int i = 0; i < argc; i++) {
        int matched = 0;
        for (configEnum *ceItem = ce; ceItem->name != NULL; ceItem++) {
            if (!strcasecmp(argv[i],ceItem->name)) {
                values |= ceItem->val;
                matched = 1;
            }
        }
        if (!matched) return INT_MIN;
    }
    return values;
}

/* Get enum name/s from value. If no matches are found "unknown" is returned. */
static sds configEnumGetName(configEnum *ce, int values, int bitflags) {
    sds names = NULL;
    int unmatched = values;
    for( ; ce->name != NULL; ce++) {
        if (values == ce->val) { /* Short path for perfect match */
            sdsfree(names);
            return sdsnew(ce->name);
        }

        /* Note: for bitflags, we want them sorted from high to low, so that if there are several / partially
         * overlapping entries, we'll prefer the ones matching more bits. */
        if (bitflags && ce->val && ce->val == (unmatched & ce->val)) {
            names = names ? sdscatfmt(names, " %s", ce->name) : sdsnew(ce->name);
            unmatched &= ~ce->val;
        }
    }
    if (!names || unmatched) {
        sdsfree(names);
        return sdsnew("unknown");
    }
    return names;
}

/* Used for INFO generation. */
const char *evictPolicyToString(void) {
    for (configEnum *ce = maxmemory_policy_enum; ce->name != NULL; ce++) {
        if (server.maxmemory_policy == ce->val)
            return ce->name;
    }
    serverPanic("unknown eviction policy");
}

/*-----------------------------------------------------------------------------
 * Config file parsing
 *----------------------------------------------------------------------------*/

int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}

void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}

void resetServerSaveParams(void) {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}

void queueLoadModule(sds path, sds *argv, int argc) {
    int i;
    struct moduleLoadQueueEntry *loadmod;

    loadmod = zmalloc(sizeof(struct moduleLoadQueueEntry));
    loadmod->argv = argc ? zmalloc(sizeof(robj*)*argc) : NULL;
    loadmod->path = sdsnew(path);
    loadmod->argc = argc;
    for (i = 0; i < argc; i++) {
        loadmod->argv[i] = createRawStringObject(argv[i],sdslen(argv[i]));
    }
    listAddNodeTail(server.loadmodule_queue,loadmod);
}

/* Parse an array of `arg_len` sds strings, validate and populate
 * server.client_obuf_limits if valid.
 * Used in CONFIG SET and configuration file parsing. */
static int updateClientOutputBufferLimit(sds *args, int arg_len, const char **err) {
    int j;
    int class;
    unsigned long long hard, soft;
    int hard_err, soft_err;
    int soft_seconds;
    char *soft_seconds_eptr;
    clientBufferLimitsConfig values[CLIENT_TYPE_OBUF_COUNT];
    int classes[CLIENT_TYPE_OBUF_COUNT] = {0};

    /* We need a multiple of 4: <class> <hard> <soft> <soft_seconds> */
    if (arg_len % 4) {
        if (err) *err = "Wrong number of arguments in "
                        "buffer limit configuration.";
        return 0;
    }

    /* Sanity check of single arguments, so that we either refuse the
     * whole configuration string or accept it all, even if a single
     * error in a single client class is present. */
    for (j = 0; j < arg_len; j += 4) {
        class = getClientTypeByName(args[j]);
        if (class == -1 || class == CLIENT_TYPE_MASTER) {
            if (err) *err = "Invalid client class specified in "
                            "buffer limit configuration.";
            return 0;
        }

        hard = memtoull(args[j+1], &hard_err);
        soft = memtoull(args[j+2], &soft_err);
        soft_seconds = strtoll(args[j+3], &soft_seconds_eptr, 10);
        if (hard_err || soft_err ||
            soft_seconds < 0 || *soft_seconds_eptr != '\0')
        {
            if (err) *err = "Error in hard, soft or soft_seconds setting in "
                            "buffer limit configuration.";
            return 0;
        }

        values[class].hard_limit_bytes = hard;
        values[class].soft_limit_bytes = soft;
        values[class].soft_limit_seconds = soft_seconds;
        classes[class] = 1;
    }

    /* Finally set the new config. */
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++) {
        if (classes[j]) server.client_obuf_limits[j] = values[j];
    }

    return 1;
}

/* Note this is here to support detecting we're running a config set from
 * within conf file parsing. This is only needed to support the deprecated
 * abnormal aggregate `save T C` functionality. Remove in the future. */
static int reading_config_file;

void loadServerConfigFromString(char *config) {
    deprecatedConfig deprecated_configs[] = {
        {"list-max-ziplist-entries", 2, 2},
        {"list-max-ziplist-value", 2, 2},
        {"lua-replicate-commands", 2, 2},
        {NULL, 0},
    };
    char buf[1024];
    const char *err = NULL;
    int linenum = 0, totlines, i;
    sds *lines;

    reading_config_file = 1;
    lines = sdssplitlen(config,strlen(config),"\n",1,&totlines);

    for (i = 0; i < totlines; i++) {
        sds *argv;
        int argc;

        linenum = i+1;
        lines[i] = sdstrim(lines[i]," \t\r\n");

        /* Skip comments and blank lines */
        if (lines[i][0] == '#' || lines[i][0] == '\0') continue;

        /* Split into arguments */
        argv = sdssplitargs(lines[i],&argc);
        if (argv == NULL) {
            err = "Unbalanced quotes in configuration line";
            goto loaderr;
        }

        /* Skip this line if the resulting command vector is empty. */
        if (argc == 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }
        sdstolower(argv[0]);

        /* Iterate the configs that are standard */
        standardConfig *config = lookupConfig(argv[0]);
        if (config) {
            /* For normal single arg configs enforce we have a single argument.
             * Note that MULTI_ARG_CONFIGs need to validate arg count on their own */
            if (!(config->flags & MULTI_ARG_CONFIG) && argc != 2) {
                err = "wrong number of arguments";
                goto loaderr;
            }

            if ((config->flags & MULTI_ARG_CONFIG) && argc == 2 && sdslen(argv[1])) {
                /* For MULTI_ARG_CONFIGs, if we only have one argument, try to split it by spaces.
                 * Only if the argument is not empty, otherwise something like --save "" will fail.
                 * So that we can support something like --config "arg1 arg2 arg3". */
                sds *new_argv;
                int new_argc;
                new_argv = sdssplitargs(argv[1], &new_argc);
                if (!config->interface.set(config, new_argv, new_argc, &err)) {
                    goto loaderr;
                }
                sdsfreesplitres(new_argv, new_argc);
            } else {
                /* Set config using all arguments that follows */
                if (!config->interface.set(config, &argv[1], argc-1, &err)) {
                    goto loaderr;
                }
            }

            sdsfreesplitres(argv,argc);
            continue;
        } else {
            int match = 0;
            for (deprecatedConfig *config = deprecated_configs; config->name != NULL; config++) {
                if (!strcasecmp(argv[0], config->name) && 
                    config->argc_min <= argc && 
                    argc <= config->argc_max) 
                {
                    match = 1;
                    break;
                }
            }
            if (match) {
                sdsfreesplitres(argv,argc);
                continue;
            }
        }

        /* Execute config directives */
        if (!strcasecmp(argv[0],"include") && argc == 2) {
            loadServerConfig(argv[1], 0, NULL);
        } else if (!strcasecmp(argv[0],"rename-command") && argc == 3) {
            struct redisCommand *cmd = lookupCommandBySds(argv[1]);
            int retval;

            if (!cmd) {
                err = "No such command in rename-command";
                goto loaderr;
            }

            /* If the target command name is the empty string we just
             * remove it from the command table. */
            retval = dictDelete(server.commands, argv[1]);
            serverAssert(retval == DICT_OK);

            /* Otherwise we re-add the command under a different name. */
            if (sdslen(argv[2]) != 0) {
                sds copy = sdsdup(argv[2]);

                retval = dictAdd(server.commands, copy, cmd);
                if (retval != DICT_OK) {
                    sdsfree(copy);
                    err = "Target command name already exists"; goto loaderr;
                }
            }
        } else if (!strcasecmp(argv[0],"user") && argc >= 2) {
            int argc_err;
            if (ACLAppendUserForLoading(argv,argc,&argc_err) == C_ERR) {
                const char *errmsg = ACLSetUserStringError();
                snprintf(buf,sizeof(buf),"Error in user declaration '%s': %s",
                    argv[argc_err],errmsg);
                err = buf;
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"loadmodule") && argc >= 2) {
            queueLoadModule(argv[1],&argv[2],argc-2);
        } else if (strchr(argv[0], '.')) {
            if (argc < 2) {
                err = "Module config specified without value";
                goto loaderr;
            }
            sds name = sdsdup(argv[0]);
            sds val = sdsdup(argv[1]);
            for (int i = 2; i < argc; i++)
                val = sdscatfmt(val, " %S", argv[i]);
            if (!dictReplace(server.module_configs_queue, name, val)) sdsfree(name);
        } else if (!strcasecmp(argv[0],"sentinel")) {
            /* argc == 1 is handled by main() as we need to enter the sentinel
             * mode ASAP. */
            if (argc != 1) {
                if (!server.sentinel_mode) {
                    err = "sentinel directive while not in sentinel mode";
                    goto loaderr;
                }
                queueSentinelConfig(argv+1,argc-1,linenum,lines[i]);
            }
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        sdsfreesplitres(argv,argc);
    }

    if (server.logfile[0] != '\0') {
        FILE *logfp;

        /* Test if we are able to open the file. The server will not
         * be able to abort just for this problem later... */
        logfp = fopen(server.logfile,"a");
        if (logfp == NULL) {
            err = sdscatprintf(sdsempty(),
                               "Can't open the log file: %s", strerror(errno));
            goto loaderr;
        }
        fclose(logfp);
    }

    /* Sanity checks. */
    if (server.cluster_enabled && server.masterhost) {
        err = "replicaof directive not allowed in cluster mode";
        goto loaderr;
    }

    /* in case cluster mode is enabled dbnum must be 1 */
    if (server.cluster_enabled && server.dbnum > 1) {
        serverLog(LL_WARNING, "WARNING: Changing databases number from %d to 1 since we are in cluster mode", server.dbnum);
        server.dbnum = 1;
    }

    /* To ensure backward compatibility and work while hz is out of range */
    if (server.config_hz < CONFIG_MIN_HZ) server.config_hz = CONFIG_MIN_HZ;
    if (server.config_hz > CONFIG_MAX_HZ) server.config_hz = CONFIG_MAX_HZ;

    sdsfreesplitres(lines,totlines);
    reading_config_file = 0;
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR (Redis %s) ***\n",
        REDIS_VERSION);
    if (i < totlines) {
        fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
        fprintf(stderr, ">>> '%s'\n", lines[i]);
    }
    fprintf(stderr, "%s\n", err);
    exit(1);
}

/* Load the server configuration from the specified filename.
 * The function appends the additional configuration directives stored
 * in the 'options' string to the config file before loading.
 *
 * Both filename and options can be NULL, in such a case are considered
 * empty. This way loadServerConfig can be used to just load a file or
 * just load a string. */
#define CONFIG_READ_LEN 1024
void loadServerConfig(char *filename, char config_from_stdin, char *options) {
    sds config = sdsempty();
    char buf[CONFIG_READ_LEN+1];
    FILE *fp;
    glob_t globbuf;

    /* Load the file content */
    if (filename) {

        /* The logic for handling wildcards has slightly different behavior in cases where
         * there is a failure to locate the included file.
         * Whether or not a wildcard is specified, we should ALWAYS log errors when attempting
         * to open included config files.
         *
         * However, we desire a behavioral difference between instances where a wildcard was
         * specified and those where it hasn't:
         *      no wildcards   : attempt to open the specified file and fail with a logged error
         *                       if the file cannot be found and opened.
         *      with wildcards : attempt to glob the specified pattern; if no files match the
         *                       pattern, then gracefully continue on to the next entry in the
         *                       config file, as if the current entry was never encountered.
         *                       This will allow for empty conf.d directories to be included. */

        if (strchr(filename, '*') || strchr(filename, '?') || strchr(filename, '[')) {
            /* A wildcard character detected in filename, so let us use glob */
            if (glob(filename, 0, NULL, &globbuf) == 0) {

                for (size_t i = 0; i < globbuf.gl_pathc; i++) {
                    if ((fp = fopen(globbuf.gl_pathv[i], "r")) == NULL) {
                        serverLog(LL_WARNING,
                                  "Fatal error, can't open config file '%s': %s",
                                  globbuf.gl_pathv[i], strerror(errno));
                        exit(1);
                    }
                    while(fgets(buf,CONFIG_READ_LEN+1,fp) != NULL)
                        config = sdscat(config,buf);
                    fclose(fp);
                }

                globfree(&globbuf);
            }
        } else {
            /* No wildcard in filename means we can use the original logic to read and
             * potentially fail traditionally */
            if ((fp = fopen(filename, "r")) == NULL) {
                serverLog(LL_WARNING,
                          "Fatal error, can't open config file '%s': %s",
                          filename, strerror(errno));
                exit(1);
            }
            while(fgets(buf,CONFIG_READ_LEN+1,fp) != NULL)
                config = sdscat(config,buf);
            fclose(fp);
        }
    }

    /* Append content from stdin */
    if (config_from_stdin) {
        serverLog(LL_NOTICE,"Reading config from stdin");
        fp = stdin;
        while(fgets(buf,CONFIG_READ_LEN+1,fp) != NULL)
            config = sdscat(config,buf);
    }

    /* Append the additional options */
    if (options) {
        config = sdscat(config,"\n");
        config = sdscat(config,options);
    }
    loadServerConfigFromString(config);
    sdsfree(config);
}

static int performInterfaceSet(standardConfig *config, sds value, const char **errstr) {
    sds *argv;
    int argc, res;

    if (config->flags & MULTI_ARG_CONFIG) {
        argv = sdssplitlen(value, sdslen(value), " ", 1, &argc);
    } else {
        argv = (char**)&value;
        argc = 1;
    }

    /* Set the config */
    res = config->interface.set(config, argv, argc, errstr);
    if (config->flags & MULTI_ARG_CONFIG) sdsfreesplitres(argv, argc);
    return res;
}

/* Find the config by name and attempt to set it to value. */
int performModuleConfigSetFromName(sds name, sds value, const char **err) {
    standardConfig *config = lookupConfig(name);
    if (!config || !(config->flags & MODULE_CONFIG)) {
        *err = "Config name not found";
        return 0;
    }
    return performInterfaceSet(config, value, err);
}

/* Find config by name and attempt to set it to its default value. */
int performModuleConfigSetDefaultFromName(sds name, const char **err) {
    standardConfig *config = lookupConfig(name);
    serverAssert(config);
    if (!(config->flags & MODULE_CONFIG)) {
        *err = "Config name not found";
        return 0;
    }
    switch (config->type) {
        case BOOL_CONFIG:
            return setModuleBoolConfig(config->privdata, config->data.yesno.default_value, err);
        case SDS_CONFIG:
            return setModuleStringConfig(config->privdata, config->data.sds.default_value, err);
        case NUMERIC_CONFIG:
            return setModuleNumericConfig(config->privdata, config->data.numeric.default_value, err);
        case ENUM_CONFIG:
            return setModuleEnumConfig(config->privdata, config->data.enumd.default_value, err);
        default:
            serverPanic("Config type of module config is not allowed.");
    }
    return 0;
}

static void restoreBackupConfig(standardConfig **set_configs, sds *old_values, int count, apply_fn *apply_fns, list *module_configs) {
    int i;
    const char *errstr = "unknown error";
    /* Set all backup values */
    for (i = 0; i < count; i++) {
        if (!performInterfaceSet(set_configs[i], old_values[i], &errstr))
            serverLog(LL_WARNING, "Failed restoring failed CONFIG SET command. Error setting %s to '%s': %s",
                      set_configs[i]->name, old_values[i], errstr);
    }
    /* Apply backup */
    if (apply_fns) {
        for (i = 0; i < count && apply_fns[i] != NULL; i++) {
            if (!apply_fns[i](&errstr))
                serverLog(LL_WARNING, "Failed applying restored failed CONFIG SET command: %s", errstr);
        }
    }
    if (module_configs) {
        if (!moduleConfigApplyConfig(module_configs, &errstr, NULL))
            serverLog(LL_WARNING, "Failed applying restored failed CONFIG SET command: %s", errstr);
    }
}

/*-----------------------------------------------------------------------------
 * CONFIG SET implementation
 *----------------------------------------------------------------------------*/

void configSetCommand(client *c) {
    const char *errstr = NULL;
    const char *invalid_arg_name = NULL;
    const char *err_arg_name = NULL;
    standardConfig **set_configs; /* TODO: make this a dict for better performance */
    list *module_configs_apply;
    const char **config_names;
    sds *new_values;
    sds *old_values = NULL;
    apply_fn *apply_fns; /* TODO: make this a set for better performance */
    int config_count, i, j;
    int invalid_args = 0, deny_loading_error = 0;
    int *config_map_fns;

    /* Make sure we have an even number of arguments: conf-val pairs */
    if (c->argc & 1) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    config_count = (c->argc - 2) / 2;

    module_configs_apply = listCreate();
    set_configs = zcalloc(sizeof(standardConfig*)*config_count);
    config_names = zcalloc(sizeof(char*)*config_count);
    new_values = zmalloc(sizeof(sds*)*config_count);
    old_values = zcalloc(sizeof(sds*)*config_count);
    apply_fns = zcalloc(sizeof(apply_fn)*config_count);
    config_map_fns = zmalloc(sizeof(int)*config_count);

    /* Find all relevant configs */
    for (i = 0; i < config_count; i++) {
        standardConfig *config = lookupConfig(c->argv[2+i*2]->ptr);
        /* Fail if we couldn't find this config */
        if (!config) {
            if (!invalid_args) {
                invalid_arg_name = c->argv[2+i*2]->ptr;
                invalid_args = 1;
            }
            continue;
        }

        /* Note: it's important we run over ALL passed configs and check if we need to call `redactClientCommandArgument()`.
         * This is in order to avoid anyone using this command for a log/slowlog/monitor/etc. displaying sensitive info.
         * So even if we encounter an error we still continue running over the remaining arguments. */
        if (config->flags & SENSITIVE_CONFIG) {
            redactClientCommandArgument(c,2+i*2+1);
        }

        /* We continue to make sure we redact all the configs */ 
        if (invalid_args) continue;

        if (config->flags & IMMUTABLE_CONFIG ||
            (config->flags & PROTECTED_CONFIG && !allowProtectedAction(server.enable_protected_configs, c)))
        {
            /* Note: we don't abort the loop since we still want to handle redacting sensitive configs (above) */
            errstr = (config->flags & IMMUTABLE_CONFIG) ? "can't set immutable config" : "can't set protected config";
            err_arg_name = c->argv[2+i*2]->ptr;
            invalid_args = 1;
            continue;
        }

        if (server.loading && config->flags & DENY_LOADING_CONFIG) {
            /* Note: we don't abort the loop since we still want to handle redacting sensitive configs (above) */
            deny_loading_error = 1;
            invalid_args = 1;
            continue;
        }

        /* If this config appears twice then fail */
        for (j = 0; j < i; j++) {
            if (set_configs[j] == config) {
                /* Note: we don't abort the loop since we still want to handle redacting sensitive configs (above) */
                errstr = "duplicate parameter";
                err_arg_name = c->argv[2+i*2]->ptr;
                invalid_args = 1;
                break;
            }
        }
        set_configs[i] = config;
        config_names[i] = config->name;
        new_values[i] = c->argv[2+i*2+1]->ptr;
    }
    
    if (invalid_args) goto err;

    /* Backup old values before setting new ones */
    for (i = 0; i < config_count; i++)
        old_values[i] = set_configs[i]->interface.get(set_configs[i]);

    /* Set all new values (don't apply yet) */
    for (i = 0; i < config_count; i++) {
        int res = performInterfaceSet(set_configs[i], new_values[i], &errstr);
        if (!res) {
            restoreBackupConfig(set_configs, old_values, i+1, NULL, NULL);
            err_arg_name = set_configs[i]->name;
            goto err;
        } else if (res == 1) {
            /* A new value was set, if this config has an apply function then store it for execution later */
            if (set_configs[i]->flags & MODULE_CONFIG) {
                addModuleConfigApply(module_configs_apply, set_configs[i]->privdata);
            } else if (set_configs[i]->interface.apply) {
                /* Check if this apply function is already stored */
                int exists = 0;
                for (j = 0; apply_fns[j] != NULL && j <= i; j++) {
                    if (apply_fns[j] == set_configs[i]->interface.apply) {
                        exists = 1;
                        break;
                    }
                }
                /* Apply function not stored, store it */
                if (!exists) {
                    apply_fns[j] = set_configs[i]->interface.apply;
                    config_map_fns[j] = i;
                }
            }
        }
    }

    /* Apply all configs after being set */
    for (i = 0; i < config_count && apply_fns[i] != NULL; i++) {
        if (!apply_fns[i](&errstr)) {
            serverLog(LL_WARNING, "Failed applying new configuration. Possibly related to new %s setting. Restoring previous settings.", set_configs[config_map_fns[i]]->name);
            restoreBackupConfig(set_configs, old_values, config_count, apply_fns, NULL);
            err_arg_name = set_configs[config_map_fns[i]]->name;
            goto err;
        }
    }
    /* Apply all module configs that were set. */
    if (!moduleConfigApplyConfig(module_configs_apply, &errstr, &err_arg_name)) {
        serverLogRaw(LL_WARNING, "Failed applying new module configuration. Restoring previous settings.");
        restoreBackupConfig(set_configs, old_values, config_count, apply_fns, module_configs_apply);
        goto err;
    }

    RedisModuleConfigChangeV1 cc = {.num_changes = config_count, .config_names = config_names};
    moduleFireServerEvent(REDISMODULE_EVENT_CONFIG, REDISMODULE_SUBEVENT_CONFIG_CHANGE, &cc);
    addReply(c,shared.ok);
    goto end;

err:
    if (deny_loading_error) {
        /* We give the loading error precedence because it may be handled by clients differently, unlike a plain -ERR. */
        addReplyErrorObject(c,shared.loadingerr);
    } else if (invalid_arg_name) {
        addReplyErrorFormat(c,"Unknown option or number of arguments for CONFIG SET - '%s'", invalid_arg_name);
    } else if (errstr) {
        addReplyErrorFormat(c,"CONFIG SET failed (possibly related to argument '%s') - %s", err_arg_name, errstr);
    } else {
        addReplyErrorFormat(c,"CONFIG SET failed (possibly related to argument '%s')", err_arg_name);
    }
end:
    zfree(set_configs);
    zfree(config_names);
    zfree(new_values);
    for (i = 0; i < config_count; i++)
        sdsfree(old_values[i]);
    zfree(old_values);
    zfree(apply_fns);
    zfree(config_map_fns);
    listRelease(module_configs_apply);
}

/*-----------------------------------------------------------------------------
 * CONFIG GET implementation
 *----------------------------------------------------------------------------*/

void configGetCommand(client *c) {
    int i;
    dictEntry *de;
    dictIterator *di;
    /* Create a dictionary to store the matched configs */
    dict *matches = dictCreate(&externalStringType);
    for (i = 0; i < c->argc - 2; i++) {
        robj *o = c->argv[2+i];
        sds name = o->ptr;

        /* If the string doesn't contain glob patterns, just directly
         * look up the key in the dictionary. */
        if (!strpbrk(name, "[*?")) {
            if (dictFind(matches, name)) continue;
            standardConfig *config = lookupConfig(name);

            if (config) {
                dictAdd(matches, name, config);
            }
            continue;
        }

        /* Otherwise, do a match against all items in the dictionary. */
        di = dictGetIterator(configs);
        
        while ((de = dictNext(di)) != NULL) {
            standardConfig *config = dictGetVal(de);
            /* Note that hidden configs require an exact match (not a pattern) */
            if (config->flags & HIDDEN_CONFIG) continue;
            if (dictFind(matches, config->name)) continue;
            if (stringmatch(name, dictGetKey(de), 1)) {
                dictAdd(matches, dictGetKey(de), config);
            }
        }
        dictReleaseIterator(di);
    }
    
    di = dictGetIterator(matches);
    addReplyMapLen(c, dictSize(matches));
    while ((de = dictNext(di)) != NULL) {
        standardConfig *config = (standardConfig *) dictGetVal(de);
        addReplyBulkCString(c, dictGetKey(de));
        addReplyBulkSds(c, config->interface.get(config));
    }
    dictReleaseIterator(di);
    dictRelease(matches);
}

/*-----------------------------------------------------------------------------
 * CONFIG REWRITE implementation
 *----------------------------------------------------------------------------*/

#define REDIS_CONFIG_REWRITE_SIGNATURE "# Generated by CONFIG REWRITE"

/* We use the following dictionary type to store where a configuration
 * option is mentioned in the old configuration file, so it's
 * like "maxmemory" -> list of line numbers (first line is zero). */
void dictListDestructor(dict *d, void *val);

/* Sentinel config rewriting is implemented inside sentinel.c by
 * rewriteConfigSentinelOption(). */
void rewriteConfigSentinelOption(struct rewriteConfigState *state);

dictType optionToLineDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictListDestructor,         /* val destructor */
    NULL                        /* allow to expand */
};

dictType optionSetDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* The config rewrite state. */
struct rewriteConfigState {
    dict *option_to_line; /* Option -> list of config file lines map */
    dict *rewritten;      /* Dictionary of already processed options */
    int numlines;         /* Number of lines in current config */
    sds *lines;           /* Current lines as an array of sds strings */
    int needs_signature;  /* True if we need to append the rewrite
                             signature. */
    int force_write;      /* True if we want all keywords to be force
                             written. Currently only used for testing
                             and debug information. */
};

/* Free the configuration rewrite state. */
void rewriteConfigReleaseState(struct rewriteConfigState *state) {
    sdsfreesplitres(state->lines,state->numlines);
    dictRelease(state->option_to_line);
    dictRelease(state->rewritten);
    zfree(state);
}

/* Create the configuration rewrite state */
struct rewriteConfigState *rewriteConfigCreateState(void) {
    struct rewriteConfigState *state = zmalloc(sizeof(*state));
    state->option_to_line = dictCreate(&optionToLineDictType);
    state->rewritten = dictCreate(&optionSetDictType);
    state->numlines = 0;
    state->lines = NULL;
    state->needs_signature = 1;
    state->force_write = 0;
    return state;
}

/* Append the new line to the current configuration state. */
void rewriteConfigAppendLine(struct rewriteConfigState *state, sds line) {
    state->lines = zrealloc(state->lines, sizeof(char*) * (state->numlines+1));
    state->lines[state->numlines++] = line;
}

/* Populate the option -> list of line numbers map. */
void rewriteConfigAddLineNumberToOption(struct rewriteConfigState *state, sds option, int linenum) {
    list *l = dictFetchValue(state->option_to_line,option);

    if (l == NULL) {
        l = listCreate();
        dictAdd(state->option_to_line,sdsdup(option),l);
    }
    listAddNodeTail(l,(void*)(long)linenum);
}

/* Add the specified option to the set of processed options.
 * This is useful as only unused lines of processed options will be blanked
 * in the config file, while options the rewrite process does not understand
 * remain untouched. */
void rewriteConfigMarkAsProcessed(struct rewriteConfigState *state, const char *option) {
    sds opt = sdsnew(option);

    if (dictAdd(state->rewritten,opt,NULL) != DICT_OK) sdsfree(opt);
}

/* Read the old file, split it into lines to populate a newly created
 * config rewrite state, and return it to the caller.
 *
 * If it is impossible to read the old file, NULL is returned.
 * If the old file does not exist at all, an empty state is returned. */
struct rewriteConfigState *rewriteConfigReadOldFile(char *path) {
    FILE *fp = fopen(path,"r");
    if (fp == NULL && errno != ENOENT) return NULL;

    struct redis_stat sb;
    if (fp && redis_fstat(fileno(fp),&sb) == -1) return NULL;

    int linenum = -1;
    struct rewriteConfigState *state = rewriteConfigCreateState();

    if (fp == NULL || sb.st_size == 0) return state;

    /* Load the file content */
    sds config = sdsnewlen(SDS_NOINIT,sb.st_size);
    if (fread(config,1,sb.st_size,fp) == 0) {
        sdsfree(config);
        rewriteConfigReleaseState(state);
        fclose(fp);
        return NULL;
    }

    int i, totlines;
    sds *lines = sdssplitlen(config,sdslen(config),"\n",1,&totlines);

    /* Read the old content line by line, populate the state. */
    for (i = 0; i < totlines; i++) {
        int argc;
        sds *argv;
        sds line = sdstrim(lines[i],"\r\n\t ");
        lines[i] = NULL;

        linenum++; /* Zero based, so we init at -1 */

        /* Handle comments and empty lines. */
        if (line[0] == '#' || line[0] == '\0') {
            if (state->needs_signature && !strcmp(line,REDIS_CONFIG_REWRITE_SIGNATURE))
                state->needs_signature = 0;
            rewriteConfigAppendLine(state,line);
            continue;
        }

        /* Not a comment, split into arguments. */
        argv = sdssplitargs(line,&argc);

        if (argv == NULL ||
            (!lookupConfig(argv[0]) &&
             /* The following is a list of config features that are only supported in
              * config file parsing and are not recognized by lookupConfig */
             strcasecmp(argv[0],"include") &&
             strcasecmp(argv[0],"rename-command") &&
             strcasecmp(argv[0],"user") &&
             strcasecmp(argv[0],"loadmodule") &&
             strcasecmp(argv[0],"sentinel")))
        {
            /* The line is either unparsable for some reason, for
             * instance it may have unbalanced quotes, may contain a
             * config that doesn't exist anymore, for instance a module that got
             * unloaded. Load it as a comment. */
            sds aux = sdsnew("# ??? ");
            aux = sdscatsds(aux,line);
            if (argv) sdsfreesplitres(argv, argc);
            sdsfree(line);
            rewriteConfigAppendLine(state,aux);
            continue;
        }

        sdstolower(argv[0]); /* We only want lowercase config directives. */

        /* Now we populate the state according to the content of this line.
         * Append the line and populate the option -> line numbers map. */
        rewriteConfigAppendLine(state,line);

        /* If this is a alias config, replace it with the original name. */
        standardConfig *s_conf = lookupConfig(argv[0]);
        if (s_conf && s_conf->flags & ALIAS_CONFIG) {
            sdsfree(argv[0]);
            argv[0] = sdsnew(s_conf->alias);
        }

        /* If this is sentinel config, we use sentinel "sentinel <config>" as option 
            to avoid messing up the sequence. */
        if (server.sentinel_mode && argc > 1 && !strcasecmp(argv[0],"sentinel")) {
            sds sentinelOption = sdsempty();
            sentinelOption = sdscatfmt(sentinelOption,"%S %S",argv[0],argv[1]);
            rewriteConfigAddLineNumberToOption(state,sentinelOption,linenum);
            sdsfree(sentinelOption);
        } else {
            rewriteConfigAddLineNumberToOption(state,argv[0],linenum);
        }
        sdsfreesplitres(argv,argc);
    }
    fclose(fp);
    sdsfreesplitres(lines,totlines);
    sdsfree(config);
    return state;
}

/* Rewrite the specified configuration option with the new "line".
 * It progressively uses lines of the file that were already used for the same
 * configuration option in the old version of the file, removing that line from
 * the map of options -> line numbers.
 *
 * If there are lines associated with a given configuration option and
 * "force" is non-zero, the line is appended to the configuration file.
 * Usually "force" is true when an option has not its default value, so it
 * must be rewritten even if not present previously.
 *
 * The first time a line is appended into a configuration file, a comment
 * is added to show that starting from that point the config file was generated
 * by CONFIG REWRITE.
 *
 * "line" is either used, or freed, so the caller does not need to free it
 * in any way. */
int rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force) {
    sds o = sdsnew(option);
    list *l = dictFetchValue(state->option_to_line,o);

    rewriteConfigMarkAsProcessed(state,option);

    if (!l && !force && !state->force_write) {
        /* Option not used previously, and we are not forced to use it. */
        sdsfree(line);
        sdsfree(o);
        return 0;
    }

    if (l) {
        listNode *ln = listFirst(l);
        int linenum = (long) ln->value;

        /* There are still lines in the old configuration file we can reuse
         * for this option. Replace the line with the new one. */
        listDelNode(l,ln);
        if (listLength(l) == 0) dictDelete(state->option_to_line,o);
        sdsfree(state->lines[linenum]);
        state->lines[linenum] = line;
    } else {
        /* Append a new line. */
        if (state->needs_signature) {
            rewriteConfigAppendLine(state,
                sdsnew(REDIS_CONFIG_REWRITE_SIGNATURE));
            state->needs_signature = 0;
        }
        rewriteConfigAppendLine(state,line);
    }
    sdsfree(o);
    return 1;
}

/* Write the long long 'bytes' value as a string in a way that is parsable
 * inside redis.conf. If possible uses the GB, MB, KB notation. */
int rewriteConfigFormatMemory(char *buf, size_t len, long long bytes) {
    int gb = 1024*1024*1024;
    int mb = 1024*1024;
    int kb = 1024;

    if (bytes && (bytes % gb) == 0) {
        return snprintf(buf,len,"%lldgb",bytes/gb);
    } else if (bytes && (bytes % mb) == 0) {
        return snprintf(buf,len,"%lldmb",bytes/mb);
    } else if (bytes && (bytes % kb) == 0) {
        return snprintf(buf,len,"%lldkb",bytes/kb);
    } else {
        return snprintf(buf,len,"%lld",bytes);
    }
}

/* Rewrite a simple "option-name <bytes>" configuration option. */
void rewriteConfigBytesOption(struct rewriteConfigState *state, const char *option, long long value, long long defvalue) {
    char buf[64];
    int force = value != defvalue;
    sds line;

    rewriteConfigFormatMemory(buf,sizeof(buf),value);
    line = sdscatprintf(sdsempty(),"%s %s",option,buf);
    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a simple "option-name n%" configuration option. */
void rewriteConfigPercentOption(struct rewriteConfigState *state, const char *option, long long value, long long defvalue) {
    int force = value != defvalue;
    sds line = sdscatprintf(sdsempty(),"%s %lld%%",option,value);

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a yes/no option. */
void rewriteConfigYesNoOption(struct rewriteConfigState *state, const char *option, int value, int defvalue) {
    int force = value != defvalue;
    sds line = sdscatprintf(sdsempty(),"%s %s",option,
        value ? "yes" : "no");

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a string option. */
void rewriteConfigStringOption(struct rewriteConfigState *state, const char *option, char *value, const char *defvalue) {
    int force = 1;
    sds line;

    /* String options set to NULL need to be not present at all in the
     * configuration file to be set to NULL again at the next reboot. */
    if (value == NULL) {
        rewriteConfigMarkAsProcessed(state,option);
        return;
    }

    /* Set force to zero if the value is set to its default. */
    if (defvalue && strcmp(value,defvalue) == 0) force = 0;

    line = sdsnew(option);
    line = sdscatlen(line, " ", 1);
    line = sdscatrepr(line, value, strlen(value));

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite a SDS string option. */
void rewriteConfigSdsOption(struct rewriteConfigState *state, const char *option, sds value, const char *defvalue) {
    int force = 1;
    sds line;

    /* If there is no value set, we don't want the SDS option
     * to be present in the configuration at all. */
    if (value == NULL) {
        rewriteConfigMarkAsProcessed(state, option);
        return;
    }

    /* Set force to zero if the value is set to its default. */
    if (defvalue && strcmp(value, defvalue) == 0) force = 0;

    line = sdsnew(option);
    line = sdscatlen(line, " ", 1);
    line = sdscatrepr(line, value, sdslen(value));

    rewriteConfigRewriteLine(state, option, line, force);
}

/* Rewrite a numerical (long long range) option. */
void rewriteConfigNumericalOption(struct rewriteConfigState *state, const char *option, long long value, long long defvalue) {
    int force = value != defvalue;
    sds line = sdscatprintf(sdsempty(),"%s %lld",option,value);

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite an octal option. */
void rewriteConfigOctalOption(struct rewriteConfigState *state, const char *option, long long value, long long defvalue) {
    int force = value != defvalue;
    sds line = sdscatprintf(sdsempty(),"%s %llo",option,value);

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite an enumeration option. It takes as usually state and option name,
 * and in addition the enumeration array and the default value for the
 * option. */
void rewriteConfigEnumOption(struct rewriteConfigState *state, const char *option, int value, standardConfig *config) {
    int multiarg = config->flags & MULTI_ARG_CONFIG;
    sds names = configEnumGetName(config->data.enumd.enum_value,value,multiarg);
    sds line = sdscatfmt(sdsempty(),"%s %s",option,names);
    sdsfree(names);
    int force = value != config->data.enumd.default_value;

    rewriteConfigRewriteLine(state,option,line,force);
}

/* Rewrite the save option. */
void rewriteConfigSaveOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    int j;
    sds line;

    /* In Sentinel mode we don't need to rewrite the save parameters */
    if (server.sentinel_mode) {
        rewriteConfigMarkAsProcessed(state,name);
        return;
    }

    /* Rewrite save parameters, or an empty 'save ""' line to avoid the
     * defaults from being used.
     */
    if (!server.saveparamslen) {
        rewriteConfigRewriteLine(state,name,sdsnew("save \"\""),1);
    } else {
        for (j = 0; j < server.saveparamslen; j++) {
            line = sdscatprintf(sdsempty(),"save %ld %d",
                (long) server.saveparams[j].seconds, server.saveparams[j].changes);
            rewriteConfigRewriteLine(state,name,line,1);
        }
    }

    /* Mark "save" as processed in case server.saveparamslen is zero. */
    rewriteConfigMarkAsProcessed(state,name);
}

/* Rewrite the user option. */
void rewriteConfigUserOption(struct rewriteConfigState *state) {
    /* If there is a user file defined we just mark this configuration
     * directive as processed, so that all the lines containing users
     * inside the config file gets discarded. */
    if (server.acl_filename[0] != '\0') {
        rewriteConfigMarkAsProcessed(state,"user");
        return;
    }

    /* Otherwise scan the list of users and rewrite every line. Note that
     * in case the list here is empty, the effect will just be to comment
     * all the users directive inside the config file. */
    raxIterator ri;
    raxStart(&ri,Users);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        user *u = ri.data;
        sds line = sdsnew("user ");
        line = sdscatsds(line,u->name);
        line = sdscatlen(line," ",1);
        robj *descr = ACLDescribeUser(u);
        line = sdscatsds(line,descr->ptr);
        decrRefCount(descr);
        rewriteConfigRewriteLine(state,"user",line,1);
    }
    raxStop(&ri);

    /* Mark "user" as processed in case there are no defined users. */
    rewriteConfigMarkAsProcessed(state,"user");
}

/* Rewrite the dir option, always using absolute paths.*/
void rewriteConfigDirOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    char cwd[1024];

    if (getcwd(cwd,sizeof(cwd)) == NULL) {
        rewriteConfigMarkAsProcessed(state,name);
        return; /* no rewrite on error. */
    }
    rewriteConfigStringOption(state,name,cwd,NULL);
}

/* Rewrite the slaveof option. */
void rewriteConfigReplicaOfOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    sds line;

    /* If this is a master, we want all the slaveof config options
     * in the file to be removed. Note that if this is a cluster instance
     * we don't want a slaveof directive inside redis.conf. */
    if (server.cluster_enabled || server.masterhost == NULL) {
        rewriteConfigMarkAsProcessed(state, name);
        return;
    }
    line = sdscatprintf(sdsempty(),"%s %s %d", name,
        server.masterhost, server.masterport);
    rewriteConfigRewriteLine(state,name,line,1);
}

/* Rewrite the notify-keyspace-events option. */
void rewriteConfigNotifyKeyspaceEventsOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    int force = server.notify_keyspace_events != 0;
    sds line, flags;

    flags = keyspaceEventsFlagsToString(server.notify_keyspace_events);
    line = sdsnew(name);
    line = sdscatlen(line, " ", 1);
    line = sdscatrepr(line, flags, sdslen(flags));
    sdsfree(flags);
    rewriteConfigRewriteLine(state,name,line,force);
}

/* Rewrite the client-output-buffer-limit option. */
void rewriteConfigClientOutputBufferLimitOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    int j;
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++) {
        int force = (server.client_obuf_limits[j].hard_limit_bytes !=
                    clientBufferLimitsDefaults[j].hard_limit_bytes) ||
                    (server.client_obuf_limits[j].soft_limit_bytes !=
                    clientBufferLimitsDefaults[j].soft_limit_bytes) ||
                    (server.client_obuf_limits[j].soft_limit_seconds !=
                    clientBufferLimitsDefaults[j].soft_limit_seconds);
        sds line;
        char hard[64], soft[64];

        rewriteConfigFormatMemory(hard,sizeof(hard),
                server.client_obuf_limits[j].hard_limit_bytes);
        rewriteConfigFormatMemory(soft,sizeof(soft),
                server.client_obuf_limits[j].soft_limit_bytes);

        char *typename = getClientTypeName(j);
        if (!strcmp(typename,"slave")) typename = "replica";
        line = sdscatprintf(sdsempty(),"%s %s %s %s %ld",
                name, typename, hard, soft,
                (long) server.client_obuf_limits[j].soft_limit_seconds);
        rewriteConfigRewriteLine(state,name,line,force);
    }
}

/* Rewrite the oom-score-adj-values option. */
void rewriteConfigOOMScoreAdjValuesOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    int force = 0;
    int j;
    sds line;

    line = sdsnew(name);
    line = sdscatlen(line, " ", 1);
    for (j = 0; j < CONFIG_OOM_COUNT; j++) {
        if (server.oom_score_adj_values[j] != configOOMScoreAdjValuesDefaults[j])
            force = 1;

        line = sdscatprintf(line, "%d", server.oom_score_adj_values[j]);
        if (j+1 != CONFIG_OOM_COUNT)
            line = sdscatlen(line, " ", 1);
    }
    rewriteConfigRewriteLine(state,name,line,force);
}

/* Rewrite the bind option. */
void rewriteConfigBindOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    int force = 1;
    sds line, addresses;
    int is_default = 0;

    /* Compare server.bindaddr with CONFIG_DEFAULT_BINDADDR */
    if (server.bindaddr_count == CONFIG_DEFAULT_BINDADDR_COUNT) {
        is_default = 1;
        char *default_bindaddr[CONFIG_DEFAULT_BINDADDR_COUNT] = CONFIG_DEFAULT_BINDADDR;
        for (int j = 0; j < CONFIG_DEFAULT_BINDADDR_COUNT; j++) {
            if (strcmp(server.bindaddr[j], default_bindaddr[j]) != 0) {
                is_default = 0;
                break;
            }
        }
    }

    if (is_default) {
        rewriteConfigMarkAsProcessed(state,name);
        return;
    }

    /* Rewrite as bind <addr1> <addr2> ... <addrN> */
    if (server.bindaddr_count > 0)
        addresses = sdsjoin(server.bindaddr,server.bindaddr_count," ");
    else
        addresses = sdsnew("\"\"");
    line = sdsnew(name);
    line = sdscatlen(line, " ", 1);
    line = sdscatsds(line, addresses);
    sdsfree(addresses);

    rewriteConfigRewriteLine(state,name,line,force);
}

/* Rewrite the loadmodule option. */
void rewriteConfigLoadmoduleOption(struct rewriteConfigState *state) {
    sds line;

    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        line = sdsnew("loadmodule ");
        line = sdscatsds(line, module->loadmod->path);
        for (int i = 0; i < module->loadmod->argc; i++) {
            line = sdscatlen(line, " ", 1);
            line = sdscatsds(line, module->loadmod->argv[i]->ptr);
        }
        rewriteConfigRewriteLine(state,"loadmodule",line,1);
    }
    dictReleaseIterator(di);
    /* Mark "loadmodule" as processed in case modules is empty. */
    rewriteConfigMarkAsProcessed(state,"loadmodule");
}

/* Glue together the configuration lines in the current configuration
 * rewrite state into a single string, stripping multiple empty lines. */
sds rewriteConfigGetContentFromState(struct rewriteConfigState *state) {
    sds content = sdsempty();
    int j, was_empty = 0;

    for (j = 0; j < state->numlines; j++) {
        /* Every cluster of empty lines is turned into a single empty line. */
        if (sdslen(state->lines[j]) == 0) {
            if (was_empty) continue;
            was_empty = 1;
        } else {
            was_empty = 0;
        }
        content = sdscatsds(content,state->lines[j]);
        content = sdscatlen(content,"\n",1);
    }
    return content;
}

/* At the end of the rewrite process the state contains the remaining
 * map between "option name" => "lines in the original config file".
 * Lines used by the rewrite process were removed by the function
 * rewriteConfigRewriteLine(), all the other lines are "orphaned" and
 * should be replaced by empty lines.
 *
 * This function does just this, iterating all the option names and
 * blanking all the lines still associated. */
void rewriteConfigRemoveOrphaned(struct rewriteConfigState *state) {
    dictIterator *di = dictGetIterator(state->option_to_line);
    dictEntry *de;

    while((de = dictNext(di)) != NULL) {
        list *l = dictGetVal(de);
        sds option = dictGetKey(de);

        /* Don't blank lines about options the rewrite process
         * don't understand. */
        if (dictFind(state->rewritten,option) == NULL) {
            serverLog(LL_DEBUG,"Not rewritten option: %s", option);
            continue;
        }

        while(listLength(l)) {
            listNode *ln = listFirst(l);
            int linenum = (long) ln->value;

            sdsfree(state->lines[linenum]);
            state->lines[linenum] = sdsempty();
            listDelNode(l,ln);
        }
    }
    dictReleaseIterator(di);
}

/* This function returns a string representation of all the config options
 * marked with DEBUG_CONFIG, which can be used to help with debugging. */
sds getConfigDebugInfo(void) {
    struct rewriteConfigState *state = rewriteConfigCreateState();
    state->force_write = 1; /* Force the output */
    state->needs_signature = 0; /* Omit the rewrite signature */

    /* Iterate the configs and "rewrite" the ones that have 
     * the debug flag. */
    dictIterator *di = dictGetIterator(configs);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        standardConfig *config = dictGetVal(de);
        if (!(config->flags & DEBUG_CONFIG)) continue;
        config->interface.rewrite(config, config->name, state);
    }
    dictReleaseIterator(di);
    sds info = rewriteConfigGetContentFromState(state);
    rewriteConfigReleaseState(state);
    return info;
}

/* This function replaces the old configuration file with the new content
 * in an atomic manner.
 *
 * The function returns 0 on success, otherwise -1 is returned and errno
 * is set accordingly. */
int rewriteConfigOverwriteFile(char *configfile, sds content) {
    int fd = -1;
    int retval = -1;
    char tmp_conffile[PATH_MAX];
    const char *tmp_suffix = ".XXXXXX";
    size_t offset = 0;
    ssize_t written_bytes = 0;
    int old_errno;

    int tmp_path_len = snprintf(tmp_conffile, sizeof(tmp_conffile), "%s%s", configfile, tmp_suffix);
    if (tmp_path_len <= 0 || (unsigned int)tmp_path_len >= sizeof(tmp_conffile)) {
        serverLog(LL_WARNING, "Config file full path is too long");
        errno = ENAMETOOLONG;
        return retval;
    }

#if defined(_GNU_SOURCE) && !defined(__HAIKU__)
    fd = mkostemp(tmp_conffile, O_CLOEXEC);
#else
    /* There's a theoretical chance here to leak the FD if a module thread forks & execv in the middle */
    fd = mkstemp(tmp_conffile);
#endif

    if (fd == -1) {
        serverLog(LL_WARNING, "Could not create tmp config file (%s)", strerror(errno));
        return retval;
    }

    while (offset < sdslen(content)) {
         written_bytes = write(fd, content + offset, sdslen(content) - offset);
         if (written_bytes <= 0) {
             if (errno == EINTR) continue; /* FD is blocking, no other retryable errors */
             serverLog(LL_WARNING, "Failed after writing (%zd) bytes to tmp config file (%s)", offset, strerror(errno));
             goto cleanup;
         }
         offset+=written_bytes;
    }

    if (fsync(fd))
        serverLog(LL_WARNING, "Could not sync tmp config file to disk (%s)", strerror(errno));
    else if (fchmod(fd, 0644 & ~server.umask) == -1)
        serverLog(LL_WARNING, "Could not chmod config file (%s)", strerror(errno));
    else if (rename(tmp_conffile, configfile) == -1)
        serverLog(LL_WARNING, "Could not rename tmp config file (%s)", strerror(errno));
    else if (fsyncFileDir(configfile) == -1)
        serverLog(LL_WARNING, "Could not sync config file dir (%s)", strerror(errno));
    else {
        retval = 0;
        serverLog(LL_DEBUG, "Rewritten config file (%s) successfully", configfile);
    }

cleanup:
    old_errno = errno;
    close(fd);
    if (retval) unlink(tmp_conffile);
    errno = old_errno;
    return retval;
}

/* Rewrite the configuration file at "path".
 * If the configuration file already exists, we try at best to retain comments
 * and overall structure.
 *
 * Configuration parameters that are at their default value, unless already
 * explicitly included in the old configuration file, are not rewritten.
 * The force_write flag overrides this behavior and forces everything to be
 * written. This is currently only used for testing purposes.
 *
 * On error -1 is returned and errno is set accordingly, otherwise 0. */
int rewriteConfig(char *path, int force_write) {
    struct rewriteConfigState *state;
    sds newcontent;
    int retval;

    /* Step 1: read the old config into our rewrite state. */
    if ((state = rewriteConfigReadOldFile(path)) == NULL) return -1;
    if (force_write) state->force_write = 1;

    /* Step 2: rewrite every single option, replacing or appending it inside
     * the rewrite state. */

    /* Iterate the configs that are standard */
    dictIterator *di = dictGetIterator(configs);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        standardConfig *config = dictGetVal(de);
        /* Only rewrite the primary names */
        if (config->flags & ALIAS_CONFIG) continue;
        if (config->interface.rewrite) config->interface.rewrite(config, dictGetKey(de), state);
    }
    dictReleaseIterator(di);

    rewriteConfigUserOption(state);
    rewriteConfigLoadmoduleOption(state);

    /* Rewrite Sentinel config if in Sentinel mode. */
    if (server.sentinel_mode) rewriteConfigSentinelOption(state);

    /* Step 3: remove all the orphaned lines in the old file, that is, lines
     * that were used by a config option and are no longer used, like in case
     * of multiple "save" options or duplicated options. */
    rewriteConfigRemoveOrphaned(state);

    /* Step 4: generate a new configuration file from the modified state
     * and write it into the original file. */
    newcontent = rewriteConfigGetContentFromState(state);
    retval = rewriteConfigOverwriteFile(server.configfile,newcontent);

    sdsfree(newcontent);
    rewriteConfigReleaseState(state);
    return retval;
}

/*-----------------------------------------------------------------------------
 * Configs that fit one of the major types and require no special handling
 *----------------------------------------------------------------------------*/
#define LOADBUF_SIZE 256
static char loadbuf[LOADBUF_SIZE];

#define embedCommonConfig(config_name, config_alias, config_flags) \
    .name = (config_name), \
    .alias = (config_alias), \
    .flags = (config_flags),

#define embedConfigInterface(initfn, setfn, getfn, rewritefn, applyfn) .interface = { \
    .init = (initfn), \
    .set = (setfn), \
    .get = (getfn), \
    .rewrite = (rewritefn), \
    .apply = (applyfn) \
},

/* What follows is the generic config types that are supported. To add a new
 * config with one of these types, add it to the standardConfig table with
 * the creation macro for each type.
 *
 * Each type contains the following:
 * * A function defining how to load this type on startup.
 * * A function defining how to update this type on CONFIG SET.
 * * A function defining how to serialize this type on CONFIG SET.
 * * A function defining how to rewrite this type on CONFIG REWRITE.
 * * A Macro defining how to create this type.
 */

/* Bool Configs */
static void boolConfigInit(standardConfig *config) {
    *config->data.yesno.config = config->data.yesno.default_value;
}

static int boolConfigSet(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(argc);
    int yn = yesnotoi(argv[0]);
    if (yn == -1) {
        *err = "argument must be 'yes' or 'no'";
        return 0;
    }
    if (config->data.yesno.is_valid_fn && !config->data.yesno.is_valid_fn(yn, err))
        return 0;
    int prev = config->flags & MODULE_CONFIG ? getModuleBoolConfig(config->privdata) : *(config->data.yesno.config);
    if (prev != yn) {
        if (config->flags & MODULE_CONFIG) {
            return setModuleBoolConfig(config->privdata, yn, err);
        }
        *(config->data.yesno.config) = yn;
        return 1;
    }
    return (config->flags & VOLATILE_CONFIG) ? 1 : 2;
}

static sds boolConfigGet(standardConfig *config) {
    if (config->flags & MODULE_CONFIG) {
        return sdsnew(getModuleBoolConfig(config->privdata) ? "yes" : "no");
    }
    return sdsnew(*config->data.yesno.config ? "yes" : "no");
}

static void boolConfigRewrite(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    int val = config->flags & MODULE_CONFIG ? getModuleBoolConfig(config->privdata) : *(config->data.yesno.config);
    rewriteConfigYesNoOption(state, name, val, config->data.yesno.default_value);
}

#define createBoolConfig(name, alias, flags, config_addr, default, is_valid, apply) { \
    embedCommonConfig(name, alias, flags) \
    embedConfigInterface(boolConfigInit, boolConfigSet, boolConfigGet, boolConfigRewrite, apply) \
    .type = BOOL_CONFIG, \
    .data.yesno = { \
        .config = &(config_addr), \
        .default_value = (default), \
        .is_valid_fn = (is_valid), \
    } \
}

/* String Configs */
static void stringConfigInit(standardConfig *config) {
    *config->data.string.config = (config->data.string.convert_empty_to_null && !config->data.string.default_value) ? NULL : zstrdup(config->data.string.default_value);
}

static int stringConfigSet(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(argc);
    if (config->data.string.is_valid_fn && !config->data.string.is_valid_fn(argv[0], err))
        return 0;
    char *prev = *config->data.string.config;
    char *new = (config->data.string.convert_empty_to_null && !argv[0][0]) ? NULL : argv[0];
    if (new != prev && (new == NULL || prev == NULL || strcmp(prev, new))) {
        *config->data.string.config = new != NULL ? zstrdup(new) : NULL;
        zfree(prev);
        return 1;
    }
    return (config->flags & VOLATILE_CONFIG) ? 1 : 2;
}

static sds stringConfigGet(standardConfig *config) {
    return sdsnew(*config->data.string.config ? *config->data.string.config : "");
}

static void stringConfigRewrite(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    rewriteConfigStringOption(state, name,*(config->data.string.config), config->data.string.default_value);
}

/* SDS Configs */
static void sdsConfigInit(standardConfig *config) {
    *config->data.sds.config = (config->data.sds.convert_empty_to_null && !config->data.sds.default_value) ? NULL : sdsnew(config->data.sds.default_value);
}

static int sdsConfigSet(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(argc);
    if (config->data.sds.is_valid_fn && !config->data.sds.is_valid_fn(argv[0], err))
        return 0;

    sds prev = config->flags & MODULE_CONFIG ? getModuleStringConfig(config->privdata) : *config->data.sds.config;
    sds new = (config->data.string.convert_empty_to_null && (sdslen(argv[0]) == 0)) ? NULL : argv[0];

    /* if prev and new configuration are not equal, set the new one */
    if (new != prev && (new == NULL || prev == NULL || sdscmp(prev, new))) {
        /* If MODULE_CONFIG flag is set, then free temporary prev getModuleStringConfig returned.
         * Otherwise, free the actual previous config value Redis held (Same action, different reasons) */
        sdsfree(prev);

        if (config->flags & MODULE_CONFIG) {
            return setModuleStringConfig(config->privdata, new, err);
        }
        *config->data.sds.config = new != NULL ? sdsdup(new) : NULL;
        return 1;
    }
    if (config->flags & MODULE_CONFIG && prev) sdsfree(prev);
    return (config->flags & VOLATILE_CONFIG) ? 1 : 2;
}

static sds sdsConfigGet(standardConfig *config) {
    sds val = config->flags & MODULE_CONFIG ? getModuleStringConfig(config->privdata) : *config->data.sds.config;
    if (val) {
        if (config->flags & MODULE_CONFIG) return val;
        return sdsdup(val);
    } else {
        return sdsnew("");
    }
}

static void sdsConfigRewrite(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    sds val = config->flags & MODULE_CONFIG ? getModuleStringConfig(config->privdata) : *config->data.sds.config;
    rewriteConfigSdsOption(state, name, val, config->data.sds.default_value);
    if ((val) && (config->flags & MODULE_CONFIG)) sdsfree(val);
}


#define ALLOW_EMPTY_STRING 0
#define EMPTY_STRING_IS_NULL 1

#define createStringConfig(name, alias, flags, empty_to_null, config_addr, default, is_valid, apply) { \
    embedCommonConfig(name, alias, flags) \
    embedConfigInterface(stringConfigInit, stringConfigSet, stringConfigGet, stringConfigRewrite, apply) \
    .type = STRING_CONFIG, \
    .data.string = { \
        .config = &(config_addr), \
        .default_value = (default), \
        .is_valid_fn = (is_valid), \
        .convert_empty_to_null = (empty_to_null), \
    } \
}

#define createSDSConfig(name, alias, flags, empty_to_null, config_addr, default, is_valid, apply) { \
    embedCommonConfig(name, alias, flags) \
    embedConfigInterface(sdsConfigInit, sdsConfigSet, sdsConfigGet, sdsConfigRewrite, apply) \
    .type = SDS_CONFIG, \
    .data.sds = { \
        .config = &(config_addr), \
        .default_value = (default), \
        .is_valid_fn = (is_valid), \
        .convert_empty_to_null = (empty_to_null), \
    } \
}

/* Enum configs */
static void enumConfigInit(standardConfig *config) {
    *config->data.enumd.config = config->data.enumd.default_value;
}

static int enumConfigSet(standardConfig *config, sds *argv, int argc, const char **err) {
    int enumval;
    int bitflags = !!(config->flags & MULTI_ARG_CONFIG);
    enumval = configEnumGetValue(config->data.enumd.enum_value, argv, argc, bitflags);

    if (enumval == INT_MIN) {
        sds enumerr = sdsnew("argument(s) must be one of the following: ");
        configEnum *enumNode = config->data.enumd.enum_value;
        while(enumNode->name != NULL) {
            enumerr = sdscatlen(enumerr, enumNode->name,
                                strlen(enumNode->name));
            enumerr = sdscatlen(enumerr, ", ", 2);
            enumNode++;
        }
        sdsrange(enumerr,0,-3); /* Remove final ", ". */

        redis_strlcpy(loadbuf, enumerr, LOADBUF_SIZE);

        sdsfree(enumerr);
        *err = loadbuf;
        return 0;
    }
    if (config->data.enumd.is_valid_fn && !config->data.enumd.is_valid_fn(enumval, err))
        return 0;
    int prev = config->flags & MODULE_CONFIG ? getModuleEnumConfig(config->privdata) : *(config->data.enumd.config);
    if (prev != enumval) {
        if (config->flags & MODULE_CONFIG)
            return setModuleEnumConfig(config->privdata, enumval, err);
        *(config->data.enumd.config) = enumval;
        return 1;
    }
    return (config->flags & VOLATILE_CONFIG) ? 1 : 2;
}

static sds enumConfigGet(standardConfig *config) {
    int val = config->flags & MODULE_CONFIG ? getModuleEnumConfig(config->privdata) : *(config->data.enumd.config);
    int bitflags = !!(config->flags & MULTI_ARG_CONFIG);
    return configEnumGetName(config->data.enumd.enum_value,val,bitflags);
}

static void enumConfigRewrite(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    int val = config->flags & MODULE_CONFIG ? getModuleEnumConfig(config->privdata) : *(config->data.enumd.config);
    rewriteConfigEnumOption(state, name, val, config);
}

#define createEnumConfig(name, alias, flags, enum, config_addr, default, is_valid, apply) { \
    embedCommonConfig(name, alias, flags) \
    embedConfigInterface(enumConfigInit, enumConfigSet, enumConfigGet, enumConfigRewrite, apply) \
    .type = ENUM_CONFIG, \
    .data.enumd = { \
        .config = &(config_addr), \
        .default_value = (default), \
        .is_valid_fn = (is_valid), \
        .enum_value = (enum), \
    } \
}

/* Gets a 'long long val' and sets it into the union, using a macro to get
 * compile time type check. */
int setNumericType(standardConfig *config, long long val, const char **err) {
    if (config->data.numeric.numeric_type == NUMERIC_TYPE_INT) {
        *(config->data.numeric.config.i) = (int) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_UINT) {
        *(config->data.numeric.config.ui) = (unsigned int) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_LONG) {
        *(config->data.numeric.config.l) = (long) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_ULONG) {
        *(config->data.numeric.config.ul) = (unsigned long) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_LONG_LONG) {
        if (config->flags & MODULE_CONFIG)
            return setModuleNumericConfig(config->privdata, val, err);
        else *(config->data.numeric.config.ll) = (long long) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_ULONG_LONG) {
        *(config->data.numeric.config.ull) = (unsigned long long) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_SIZE_T) {
        *(config->data.numeric.config.st) = (size_t) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_SSIZE_T) {
        *(config->data.numeric.config.sst) = (ssize_t) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_OFF_T) {
        *(config->data.numeric.config.ot) = (off_t) val;
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_TIME_T) {
        *(config->data.numeric.config.tt) = (time_t) val;
    }
    return 1;
}

/* Gets a 'long long val' and sets it with the value from the union, using a
 * macro to get compile time type check. */
#define GET_NUMERIC_TYPE(val) \
    if (config->data.numeric.numeric_type == NUMERIC_TYPE_INT) { \
        val = *(config->data.numeric.config.i); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_UINT) { \
        val = *(config->data.numeric.config.ui); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_LONG) { \
        val = *(config->data.numeric.config.l); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_ULONG) { \
        val = *(config->data.numeric.config.ul); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_LONG_LONG) { \
        if (config->flags & MODULE_CONFIG) val = getModuleNumericConfig(config->privdata); \
        else val = *(config->data.numeric.config.ll); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_ULONG_LONG) { \
        val = *(config->data.numeric.config.ull); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_SIZE_T) { \
        val = *(config->data.numeric.config.st); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_SSIZE_T) { \
        val = *(config->data.numeric.config.sst); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_OFF_T) { \
        val = *(config->data.numeric.config.ot); \
    } else if (config->data.numeric.numeric_type == NUMERIC_TYPE_TIME_T) { \
        val = *(config->data.numeric.config.tt); \
    }

/* Numeric configs */
static void numericConfigInit(standardConfig *config) {
    setNumericType(config, config->data.numeric.default_value, NULL);
}

static int numericBoundaryCheck(standardConfig *config, long long ll, const char **err) {
    if (config->data.numeric.numeric_type == NUMERIC_TYPE_ULONG_LONG ||
        config->data.numeric.numeric_type == NUMERIC_TYPE_UINT ||
        config->data.numeric.numeric_type == NUMERIC_TYPE_SIZE_T) {
        /* Boundary check for unsigned types */
        unsigned long long ull = ll;
        unsigned long long upper_bound = config->data.numeric.upper_bound;
        unsigned long long lower_bound = config->data.numeric.lower_bound;
        if (ull > upper_bound || ull < lower_bound) {
            if (config->data.numeric.flags & OCTAL_CONFIG) {
                snprintf(loadbuf, LOADBUF_SIZE,
                    "argument must be between %llo and %llo inclusive",
                    lower_bound,
                    upper_bound);
            } else {
                snprintf(loadbuf, LOADBUF_SIZE,
                    "argument must be between %llu and %llu inclusive",
                    lower_bound,
                    upper_bound);
            }
            *err = loadbuf;
            return 0;
        }
    } else {
        /* Boundary check for percentages */
        if (config->data.numeric.flags & PERCENT_CONFIG && ll < 0) {
            if (ll < config->data.numeric.lower_bound) {
                snprintf(loadbuf, LOADBUF_SIZE,
                         "percentage argument must be less or equal to %lld",
                         -config->data.numeric.lower_bound);
                *err = loadbuf;
                return 0;
            }
        }
        /* Boundary check for signed types */
        else if (ll > config->data.numeric.upper_bound || ll < config->data.numeric.lower_bound) {
            snprintf(loadbuf, LOADBUF_SIZE,
                "argument must be between %lld and %lld inclusive",
                config->data.numeric.lower_bound,
                config->data.numeric.upper_bound);
            *err = loadbuf;
            return 0;
        }
    }
    return 1;
}

static int numericParseString(standardConfig *config, sds value, const char **err, long long *res) {
    /* First try to parse as memory */
    if (config->data.numeric.flags & MEMORY_CONFIG) {
        int memerr;
        *res = memtoull(value, &memerr);
        if (!memerr)
            return 1;
    }

    /* Attempt to parse as percent */
    if (config->data.numeric.flags & PERCENT_CONFIG &&
        sdslen(value) > 1 && value[sdslen(value)-1] == '%' &&
        string2ll(value, sdslen(value)-1, res) &&
        *res >= 0) {
            /* We store percentage as negative value */
            *res = -*res;
            return 1;
    }

    /* Attempt to parse as an octal number */
    if (config->data.numeric.flags & OCTAL_CONFIG) {
        char *endptr;
        errno = 0;
        *res = strtoll(value, &endptr, 8);
        if (errno == 0 && *endptr == '\0')
            return 1; /* No overflow or invalid characters */
    }

    /* Attempt a simple number (no special flags set) */
    if (!config->data.numeric.flags && string2ll(value, sdslen(value), res))
        return 1;

    /* Select appropriate error string */
    if (config->data.numeric.flags & MEMORY_CONFIG &&
        config->data.numeric.flags & PERCENT_CONFIG)
        *err = "argument must be a memory or percent value" ;
    else if (config->data.numeric.flags & MEMORY_CONFIG)
        *err = "argument must be a memory value";
    else if (config->data.numeric.flags & OCTAL_CONFIG)
        *err = "argument couldn't be parsed as an octal number";
    else
        *err = "argument couldn't be parsed into an integer";
    return 0;
}

static int numericConfigSet(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(argc);
    long long ll, prev = 0;

    if (!numericParseString(config, argv[0], err, &ll))
        return 0;

    if (!numericBoundaryCheck(config, ll, err))
        return 0;

    if (config->data.numeric.is_valid_fn && !config->data.numeric.is_valid_fn(ll, err))
        return 0;

    GET_NUMERIC_TYPE(prev)
    if (prev != ll) {
        return setNumericType(config, ll, err);
    }

    return (config->flags & VOLATILE_CONFIG) ? 1 : 2;
}

static sds numericConfigGet(standardConfig *config) {
    char buf[128];

    long long value = 0;
    GET_NUMERIC_TYPE(value)

    if (config->data.numeric.flags & PERCENT_CONFIG && value < 0) {
        int len = ll2string(buf, sizeof(buf), -value);
        buf[len] = '%';
        buf[len+1] = '\0';
    }
    else if (config->data.numeric.flags & MEMORY_CONFIG) {
        ull2string(buf, sizeof(buf), value);
    } else if (config->data.numeric.flags & OCTAL_CONFIG) {
        snprintf(buf, sizeof(buf), "%llo", value);
    } else {
        ll2string(buf, sizeof(buf), value);
    }
    return sdsnew(buf);
}

static void numericConfigRewrite(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    long long value = 0;

    GET_NUMERIC_TYPE(value)

    if (config->data.numeric.flags & PERCENT_CONFIG && value < 0) {
        rewriteConfigPercentOption(state, name, -value, config->data.numeric.default_value);
    } else if (config->data.numeric.flags & MEMORY_CONFIG) {
        rewriteConfigBytesOption(state, name, value, config->data.numeric.default_value);
    } else if (config->data.numeric.flags & OCTAL_CONFIG) {
        rewriteConfigOctalOption(state, name, value, config->data.numeric.default_value);
    } else {
        rewriteConfigNumericalOption(state, name, value, config->data.numeric.default_value);
    }
}

#define embedCommonNumericalConfig(name, alias, _flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) { \
    embedCommonConfig(name, alias, _flags) \
    embedConfigInterface(numericConfigInit, numericConfigSet, numericConfigGet, numericConfigRewrite, apply) \
    .type = NUMERIC_CONFIG, \
    .data.numeric = { \
        .lower_bound = (lower), \
        .upper_bound = (upper), \
        .default_value = (default), \
        .is_valid_fn = (is_valid), \
        .flags = (num_conf_flags),

#define createIntConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_INT, \
        .config.i = &(config_addr) \
    } \
}

#define createUIntConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_UINT, \
        .config.ui = &(config_addr) \
    } \
}

#define createLongConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_LONG, \
        .config.l = &(config_addr) \
    } \
}

#define createULongConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_ULONG, \
        .config.ul = &(config_addr) \
    } \
}

#define createLongLongConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_LONG_LONG, \
        .config.ll = &(config_addr) \
    } \
}

#define createULongLongConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_ULONG_LONG, \
        .config.ull = &(config_addr) \
    } \
}

#define createSizeTConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_SIZE_T, \
        .config.st = &(config_addr) \
    } \
}

#define createSSizeTConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_SSIZE_T, \
        .config.sst = &(config_addr) \
    } \
}

#define createTimeTConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_TIME_T, \
        .config.tt = &(config_addr) \
    } \
}

#define createOffTConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
    embedCommonNumericalConfig(name, alias, flags, lower, upper, config_addr, default, num_conf_flags, is_valid, apply) \
        .numeric_type = NUMERIC_TYPE_OFF_T, \
        .config.ot = &(config_addr) \
    } \
}

#define createSpecialConfig(name, alias, modifiable, setfn, getfn, rewritefn, applyfn) { \
    .type = SPECIAL_CONFIG, \
    embedCommonConfig(name, alias, modifiable) \
    embedConfigInterface(NULL, setfn, getfn, rewritefn, applyfn) \
}

static int isValidActiveDefrag(int val, const char **err) {
#ifndef HAVE_DEFRAG
    if (val) {
        *err = "Active defragmentation cannot be enabled: it "
               "requires a Redis server compiled with a modified Jemalloc "
               "like the one shipped by default with the Redis source "
               "distribution";
        return 0;
    }
#else
    UNUSED(val);
    UNUSED(err);
#endif
    return 1;
}

static int isValidDBfilename(char *val, const char **err) {
    if (!pathIsBaseName(val)) {
        *err = "dbfilename can't be a path, just a filename";
        return 0;
    }
    return 1;
}

static int isValidAOFfilename(char *val, const char **err) {
    if (!strcmp(val, "")) {
        *err = "appendfilename can't be empty";
        return 0;
    }
    if (!pathIsBaseName(val)) {
        *err = "appendfilename can't be a path, just a filename";
        return 0;
    }
    return 1;
}

static int isValidAOFdirname(char *val, const char **err) {
    if (!strcmp(val, "")) {
        *err = "appenddirname can't be empty";
        return 0;
    }
    if (!pathIsBaseName(val)) {
        *err = "appenddirname can't be a path, just a dirname";
        return 0;
    }
    return 1;
}

static int isValidShutdownOnSigFlags(int val, const char **err) {
    /* Individual arguments are validated by createEnumConfig logic.
     * We just need to ensure valid combinations here. */
    if (val & SHUTDOWN_NOSAVE && val & SHUTDOWN_SAVE) {
        *err = "shutdown options SAVE and NOSAVE can't be used simultaneously";
        return 0;
    }
    return 1;
}

static int isValidAnnouncedHostname(char *val, const char **err) {
    if (strlen(val) >= NET_HOST_STR_LEN) {
        *err = "Hostnames must be less than "
            STRINGIFY(NET_HOST_STR_LEN) " characters";
        return 0;
    }

    int i = 0;
    char c;
    while ((c = val[i])) {
        /* We just validate the character set to make sure that everything
         * is parsed and handled correctly. */
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || (c == '-') || (c == '.')))
        {
            *err = "Hostnames may only contain alphanumeric characters, "
                "hyphens or dots";
            return 0;
        }
        c = val[i++];
    }
    return 1;
}

/* Validate specified string is a valid proc-title-template */
static int isValidProcTitleTemplate(char *val, const char **err) {
    if (!validateProcTitleTemplate(val)) {
        *err = "template format is invalid or contains unknown variables";
        return 0;
    }
    return 1;
}

static int updateLocaleCollate(const char **err) {
    const char *s = setlocale(LC_COLLATE, server.locale_collate);
    if (s == NULL) {
        *err = "Invalid locale name";
        return 0;
    }
    return 1;
}

static int updateProcTitleTemplate(const char **err) {
    if (redisSetProcTitle(NULL) == C_ERR) {
        *err = "failed to set process title";
        return 0;
    }
    return 1;
}

static int updateHZ(const char **err) {
    UNUSED(err);
    /* Hz is more a hint from the user, so we accept values out of range
     * but cap them to reasonable values. */
    if (server.config_hz < CONFIG_MIN_HZ) server.config_hz = CONFIG_MIN_HZ;
    if (server.config_hz > CONFIG_MAX_HZ) server.config_hz = CONFIG_MAX_HZ;
    server.hz = server.config_hz;
    return 1;
}

static int updatePort(const char **err) {
    connListener *listener = listenerByType(CONN_TYPE_SOCKET);

    serverAssert(listener != NULL);
    listener->bindaddr = server.bindaddr;
    listener->bindaddr_count = server.bindaddr_count;
    listener->port = server.port;
    listener->ct = connectionByType(CONN_TYPE_SOCKET);
    if (changeListener(listener) == C_ERR) {
        *err = "Unable to listen on this port. Check server logs.";
        return 0;
    }

    return 1;
}

static int updateJemallocBgThread(const char **err) {
    UNUSED(err);
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    return 1;
}

static int updateReplBacklogSize(const char **err) {
    UNUSED(err);
    resizeReplicationBacklog();
    return 1;
}

static int updateMaxmemory(const char **err) {
    UNUSED(err);
    if (server.maxmemory) {
        size_t used = zmalloc_used_memory()-freeMemoryGetNotCountedMemory();
        if (server.maxmemory < used) {
            serverLog(LL_WARNING,"WARNING: the new maxmemory value set via CONFIG SET (%llu) is smaller than the current memory usage (%zu). This will result in key eviction and/or the inability to accept new write commands depending on the maxmemory-policy.", server.maxmemory, used);
        }
        startEvictionTimeProc();
    }
    return 1;
}

static int updateGoodSlaves(const char **err) {
    UNUSED(err);
    refreshGoodSlavesCount();
    return 1;
}

static int updateWatchdogPeriod(const char **err) {
    UNUSED(err);
    applyWatchdogPeriod();
    return 1;
}

static int updateAppendonly(const char **err) {
    if (!server.aof_enabled && server.aof_state != AOF_OFF) {
        stopAppendOnly();
    } else if (server.aof_enabled && server.aof_state == AOF_OFF) {
        if (startAppendOnly() == C_ERR) {
            *err = "Unable to turn on AOF. Check server logs.";
            return 0;
        }
    }
    return 1;
}

static int updateAofAutoGCEnabled(const char **err) {
    UNUSED(err);
    if (!server.aof_disable_auto_gc) {
        aofDelHistoryFiles();
    }

    return 1;
}

static int updateSighandlerEnabled(const char **err) {
    UNUSED(err);
    if (server.crashlog_enabled)
        setupSignalHandlers();
    else
        removeSignalHandlers();
    return 1;
}

static int updateMaxclients(const char **err) {
    unsigned int new_maxclients = server.maxclients;
    adjustOpenFilesLimit();
    if (server.maxclients != new_maxclients) {
        static char msg[128];
        snprintf(msg, sizeof(msg), "The operating system is not able to handle the specified number of clients, try with %d", server.maxclients);
        *err = msg;
        return 0;
    }
    if ((unsigned int) aeGetSetSize(server.el) <
        server.maxclients + CONFIG_FDSET_INCR)
    {
        if (aeResizeSetSize(server.el,
            server.maxclients + CONFIG_FDSET_INCR) == AE_ERR)
        {
            *err = "The event loop API used by Redis is not able to handle the specified number of clients";
            return 0;
        }
    }
    return 1;
}

static int updateOOMScoreAdj(const char **err) {
    if (setOOMScoreAdj(-1) == C_ERR) {
        *err = "Failed to set current oom_score_adj. Check server logs.";
        return 0;
    }

    return 1;
}

int updateRequirePass(const char **err) {
    UNUSED(err);
    /* The old "requirepass" directive just translates to setting
     * a password to the default user. The only thing we do
     * additionally is to remember the cleartext password in this
     * case, for backward compatibility with Redis <= 5. */
    ACLUpdateDefaultUserPassword(server.requirepass);
    return 1;
}

int updateAppendFsync(const char **err) {
    UNUSED(err);
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* Wait for all bio jobs related to AOF to drain before proceeding. This prevents a race
         * between updates to `fsynced_reploff_pending` done in the main thread and those done on the
         * worker thread. */
        bioDrainWorker(BIO_AOF_FSYNC);
    }
    return 1;
}

/* applyBind affects both TCP and TLS (if enabled) together */
static int applyBind(const char **err) {
    connListener *tcp_listener = listenerByType(CONN_TYPE_SOCKET);
    connListener *tls_listener = listenerByType(CONN_TYPE_TLS);

    serverAssert(tcp_listener != NULL);
    tcp_listener->bindaddr = server.bindaddr;
    tcp_listener->bindaddr_count = server.bindaddr_count;
    tcp_listener->port = server.port;
    tcp_listener->ct = connectionByType(CONN_TYPE_SOCKET);
    if (changeListener(tcp_listener) == C_ERR) {
        *err = "Failed to bind to specified addresses.";
        if (tls_listener)
            closeListener(tls_listener); /* failed with TLS together */
        return 0;
    }

    if (server.tls_port != 0) {
        serverAssert(tls_listener != NULL);
        tls_listener->bindaddr = server.bindaddr;
        tls_listener->bindaddr_count = server.bindaddr_count;
        tls_listener->port = server.tls_port;
        tls_listener->ct = connectionByType(CONN_TYPE_TLS);
        if (changeListener(tls_listener) == C_ERR) {
            *err = "Failed to bind to specified addresses.";
            closeListener(tcp_listener); /* failed with TCP together */
            return 0;
        }
    }

    return 1;
}

int updateClusterFlags(const char **err) {
    UNUSED(err);
    clusterUpdateMyselfFlags();
    return 1;
}

static int updateClusterAnnouncedPort(const char **err) {
    UNUSED(err);
    clusterUpdateMyselfAnnouncedPorts();
    return 1;
}

static int updateClusterIp(const char **err) {
    UNUSED(err);
    clusterUpdateMyselfIp();
    return 1;
}

int updateClusterHostname(const char **err) {
    UNUSED(err);
    clusterUpdateMyselfHostname();
    return 1;
}

static int applyTlsCfg(const char **err) {
    UNUSED(err);

    /* If TLS is enabled, try to configure OpenSSL. */
    if ((server.tls_port || server.tls_replication || server.tls_cluster)
         && connTypeConfigure(connectionTypeTls(), &server.tls_ctx_config, 1) == C_ERR) {
        *err = "Unable to update TLS configuration. Check server logs.";
        return 0;
    }
    return 1;
}

static int applyTLSPort(const char **err) {
    /* Configure TLS in case it wasn't enabled */
    if (connTypeConfigure(connectionTypeTls(), &server.tls_ctx_config, 0) == C_ERR) {
        *err = "Unable to update TLS configuration. Check server logs.";
        return 0;
    }

    connListener *listener = listenerByType(CONN_TYPE_TLS);
    serverAssert(listener != NULL);
    listener->bindaddr = server.bindaddr;
    listener->bindaddr_count = server.bindaddr_count;
    listener->port = server.tls_port;
    listener->ct = connectionByType(CONN_TYPE_TLS);
    if (changeListener(listener) == C_ERR) {
        *err = "Unable to listen on this port. Check server logs.";
        return 0;
    }

    return 1;
}

static int setConfigDirOption(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(config);
    if (argc != 1) {
        *err = "wrong number of arguments";
        return 0;
    }
    if (chdir(argv[0]) == -1) {
        *err = strerror(errno);
        return 0;
    }
    return 1;
}

static sds getConfigDirOption(standardConfig *config) {
    UNUSED(config);
    char buf[1024];

    if (getcwd(buf,sizeof(buf)) == NULL)
        buf[0] = '\0';

    return sdsnew(buf);
}

static int setConfigSaveOption(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(config);
    int j;

    /* Special case: treat single arg "" as zero args indicating empty save configuration */
    if (argc == 1 && !strcasecmp(argv[0],"")) {
        resetServerSaveParams();
        argc = 0;
    }

    /* Perform sanity check before setting the new config:
    * - Even number of args
    * - Seconds >= 1, changes >= 0 */
    if (argc & 1) {
        *err = "Invalid save parameters";
        return 0;
    }
    for (j = 0; j < argc; j++) {
        char *eptr;
        long val;

        val = strtoll(argv[j], &eptr, 10);
        if (eptr[0] != '\0' ||
            ((j & 1) == 0 && val < 1) ||
            ((j & 1) == 1 && val < 0)) {
            *err = "Invalid save parameters";
            return 0;
        }
    }
    /* Finally set the new config */
    if (!reading_config_file) {
        resetServerSaveParams();
    } else {
        /* We don't reset save params before loading, because if they're not part
         * of the file the defaults should be used.
         */
        static int save_loaded = 0;
        if (!save_loaded) {
            save_loaded = 1;
            resetServerSaveParams();
        }
    }

    for (j = 0; j < argc; j += 2) {
        time_t seconds;
        int changes;

        seconds = strtoll(argv[j],NULL,10);
        changes = strtoll(argv[j+1],NULL,10);
        appendServerSaveParams(seconds, changes);
    }

    return 1;
}

static sds getConfigSaveOption(standardConfig *config) {
    UNUSED(config);
    sds buf = sdsempty();
    int j;

    for (j = 0; j < server.saveparamslen; j++) {
        buf = sdscatprintf(buf,"%jd %d",
                           (intmax_t)server.saveparams[j].seconds,
                           server.saveparams[j].changes);
        if (j != server.saveparamslen-1)
            buf = sdscatlen(buf," ",1);
    }

    return buf;
}

static int setConfigClientOutputBufferLimitOption(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(config);
    return updateClientOutputBufferLimit(argv, argc, err);
}

static sds getConfigClientOutputBufferLimitOption(standardConfig *config) {
    UNUSED(config);
    sds buf = sdsempty();
    int j;
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++) {
        buf = sdscatprintf(buf,"%s %llu %llu %ld",
                           getClientTypeName(j),
                           server.client_obuf_limits[j].hard_limit_bytes,
                           server.client_obuf_limits[j].soft_limit_bytes,
                           (long) server.client_obuf_limits[j].soft_limit_seconds);
        if (j != CLIENT_TYPE_OBUF_COUNT-1)
            buf = sdscatlen(buf," ",1);
    }
    return buf;
}

/* Parse an array of CONFIG_OOM_COUNT sds strings, validate and populate
 * server.oom_score_adj_values if valid.
 */
static int setConfigOOMScoreAdjValuesOption(standardConfig *config, sds *argv, int argc, const char **err) {
    int i;
    int values[CONFIG_OOM_COUNT];
    int change = 0;
    UNUSED(config);

    if (argc != CONFIG_OOM_COUNT) {
        *err = "wrong number of arguments";
        return 0;
    }

    for (i = 0; i < CONFIG_OOM_COUNT; i++) {
        char *eptr;
        long long val = strtoll(argv[i], &eptr, 10);

        if (*eptr != '\0' || val < -2000 || val > 2000) {
            if (err) *err = "Invalid oom-score-adj-values, elements must be between -2000 and 2000.";
            return 0;
        }

        values[i] = val;
    }

    /* Verify that the values make sense. If they don't omit a warning but
     * keep the configuration, which may still be valid for privileged processes.
     */

    if (values[CONFIG_OOM_REPLICA] < values[CONFIG_OOM_MASTER] ||
        values[CONFIG_OOM_BGCHILD] < values[CONFIG_OOM_REPLICA])
    {
        serverLog(LL_WARNING,
                  "The oom-score-adj-values configuration may not work for non-privileged processes! "
                  "Please consult the documentation.");
    }

    for (i = 0; i < CONFIG_OOM_COUNT; i++) {
        if (server.oom_score_adj_values[i] != values[i]) {
            server.oom_score_adj_values[i] = values[i];
            change = 1;
        }
    }

    return change ? 1 : 2;
}

static sds getConfigOOMScoreAdjValuesOption(standardConfig *config) {
    UNUSED(config);
    sds buf = sdsempty();
    int j;

    for (j = 0; j < CONFIG_OOM_COUNT; j++) {
        buf = sdscatprintf(buf,"%d", server.oom_score_adj_values[j]);
        if (j != CONFIG_OOM_COUNT-1)
            buf = sdscatlen(buf," ",1);
    }

    return buf;
}

static int setConfigNotifyKeyspaceEventsOption(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(config);
    if (argc != 1) {
        *err = "wrong number of arguments";
        return 0;
    }
    int flags = keyspaceEventsStringToFlags(argv[0]);
    if (flags == -1) {
        *err = "Invalid event class character. Use 'Ag$lshzxeKEtmdn'.";
        return 0;
    }
    server.notify_keyspace_events = flags;
    return 1;
}

static sds getConfigNotifyKeyspaceEventsOption(standardConfig *config) {
    UNUSED(config);
    return keyspaceEventsFlagsToString(server.notify_keyspace_events);
}

static int setConfigBindOption(standardConfig *config, sds* argv, int argc, const char **err) {
    UNUSED(config);
    int j;

    if (argc > CONFIG_BINDADDR_MAX) {
        *err = "Too many bind addresses specified.";
        return 0;
    }

    /* A single empty argument is treated as a zero bindaddr count */
    if (argc == 1 && sdslen(argv[0]) == 0) argc = 0;

    /* Free old bind addresses */
    for (j = 0; j < server.bindaddr_count; j++) {
        zfree(server.bindaddr[j]);
    }
    for (j = 0; j < argc; j++)
        server.bindaddr[j] = zstrdup(argv[j]);
    server.bindaddr_count = argc;

    return 1;
}

static int setConfigReplicaOfOption(standardConfig *config, sds* argv, int argc, const char **err) {
    UNUSED(config);

    if (argc != 2) {
        *err = "wrong number of arguments";
        return 0;
    }

    sdsfree(server.masterhost);
    server.masterhost = NULL;
    if (!strcasecmp(argv[0], "no") && !strcasecmp(argv[1], "one")) {
        return 1;
    }
    char *ptr;
    server.masterport = strtol(argv[1], &ptr, 10);
    if (server.masterport < 0 || server.masterport > 65535 || *ptr != '\0') {
        *err = "Invalid master port";
        return 0;
    }
    server.masterhost = sdsnew(argv[0]);
    server.repl_state = REPL_STATE_CONNECT;
    return 1;
}

static sds getConfigBindOption(standardConfig *config) {
    UNUSED(config);
    return sdsjoin(server.bindaddr,server.bindaddr_count," ");
}

static sds getConfigReplicaOfOption(standardConfig *config) {
    UNUSED(config);
    char buf[256];
    if (server.masterhost)
        snprintf(buf,sizeof(buf),"%s %d",
                 server.masterhost, server.masterport);
    else
        buf[0] = '\0';
    return sdsnew(buf);
}

int allowProtectedAction(int config, client *c) {
    return (config == PROTECTED_ACTION_ALLOWED_YES) ||
           (config == PROTECTED_ACTION_ALLOWED_LOCAL && (connIsLocal(c->conn) == 1));
}


static int setConfigLatencyTrackingInfoPercentilesOutputOption(standardConfig *config, sds *argv, int argc, const char **err) {
    UNUSED(config);
    zfree(server.latency_tracking_info_percentiles);
    server.latency_tracking_info_percentiles = NULL;
    server.latency_tracking_info_percentiles_len = argc;

    /* Special case: treat single arg "" as zero args indicating empty percentile configuration */
    if (argc == 1 && sdslen(argv[0]) == 0)
        server.latency_tracking_info_percentiles_len = 0;
    else
        server.latency_tracking_info_percentiles = zmalloc(sizeof(double)*argc);

    for (int j = 0; j < server.latency_tracking_info_percentiles_len; j++) {
        double percentile;
        if (!string2d(argv[j], sdslen(argv[j]), &percentile)) {
            *err = "Invalid latency-tracking-info-percentiles parameters";
            goto configerr;
        }
        if (percentile > 100.0 || percentile < 0.0) {
            *err = "latency-tracking-info-percentiles parameters should sit between [0.0,100.0]";
            goto configerr;
        }
        server.latency_tracking_info_percentiles[j] = percentile;
    }

    return 1;
configerr:
    zfree(server.latency_tracking_info_percentiles);
    server.latency_tracking_info_percentiles = NULL;
    server.latency_tracking_info_percentiles_len = 0;
    return 0;
}

static sds getConfigLatencyTrackingInfoPercentilesOutputOption(standardConfig *config) {
    UNUSED(config);
    sds buf = sdsempty();
    for (int j = 0; j < server.latency_tracking_info_percentiles_len; j++) {
        char fbuf[128];
        size_t len = snprintf(fbuf, sizeof(fbuf), "%f", server.latency_tracking_info_percentiles[j]);
        len = trimDoubleString(fbuf, len);
        buf = sdscatlen(buf, fbuf, len);
        if (j != server.latency_tracking_info_percentiles_len-1)
            buf = sdscatlen(buf," ",1);
    }
    return buf;
}

/* Rewrite the latency-tracking-info-percentiles option. */
void rewriteConfigLatencyTrackingInfoPercentilesOutputOption(standardConfig *config, const char *name, struct rewriteConfigState *state) {
    UNUSED(config);
    sds line = sdsnew(name);
    /* Rewrite latency-tracking-info-percentiles parameters,
     * or an empty 'latency-tracking-info-percentiles ""' line to avoid the
     * defaults from being used.
     */
    if (!server.latency_tracking_info_percentiles_len) {
        line = sdscat(line," \"\"");
    } else {
        for (int j = 0; j < server.latency_tracking_info_percentiles_len; j++) {
            char fbuf[128];
            size_t len = snprintf(fbuf, sizeof(fbuf), " %f", server.latency_tracking_info_percentiles[j]);
            len = trimDoubleString(fbuf, len);
            line = sdscatlen(line, fbuf, len);
        }
    }
    rewriteConfigRewriteLine(state,name,line,1);
}

static int applyClientMaxMemoryUsage(const char **err) {
    UNUSED(err);
    listIter li;
    listNode *ln;

    /* server.client_mem_usage_buckets is an indication that the previous config
     * was non-zero, in which case we can exit and no apply is needed. */
    if(server.maxmemory_clients !=0 && server.client_mem_usage_buckets)
        return 1;
    if (server.maxmemory_clients != 0)
        initServerClientMemUsageBuckets();

    /* When client eviction is enabled update memory buckets for all clients.
     * When disabled, clear that data structure. */
    listRewind(server.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (server.maxmemory_clients == 0) {
            /* Remove client from memory usage bucket. */
            removeClientFromMemUsageBucket(c, 0);
        } else {
            /* Update each client(s) memory usage and add to appropriate bucket. */
            updateClientMemUsageAndBucket(c);
        }
    }

    if (server.maxmemory_clients == 0)
        freeServerClientMemUsageBuckets();
    return 1;
}

standardConfig static_configs[] = {
    /* Bool configs */
    createBoolConfig("rdbchecksum", NULL, IMMUTABLE_CONFIG, server.rdb_checksum, 1, NULL, NULL),
    createBoolConfig("daemonize", NULL, IMMUTABLE_CONFIG, server.daemonize, 0, NULL, NULL),
    createBoolConfig("io-threads-do-reads", NULL, DEBUG_CONFIG | IMMUTABLE_CONFIG, server.io_threads_do_reads, 0,NULL, NULL), /* Read + parse from threads? */
    createBoolConfig("always-show-logo", NULL, IMMUTABLE_CONFIG, server.always_show_logo, 0, NULL, NULL),
    createBoolConfig("protected-mode", NULL, MODIFIABLE_CONFIG, server.protected_mode, 1, NULL, NULL),
    createBoolConfig("rdbcompression", NULL, MODIFIABLE_CONFIG, server.rdb_compression, 1, NULL, NULL),
    createBoolConfig("rdb-del-sync-files", NULL, MODIFIABLE_CONFIG, server.rdb_del_sync_files, 0, NULL, NULL),
    createBoolConfig("activerehashing", NULL, MODIFIABLE_CONFIG, server.activerehashing, 1, NULL, NULL),
    createBoolConfig("stop-writes-on-bgsave-error", NULL, MODIFIABLE_CONFIG, server.stop_writes_on_bgsave_err, 1, NULL, NULL),
    createBoolConfig("set-proc-title", NULL, IMMUTABLE_CONFIG, server.set_proc_title, 1, NULL, NULL), /* Should setproctitle be used? */
    createBoolConfig("dynamic-hz", NULL, MODIFIABLE_CONFIG, server.dynamic_hz, 1, NULL, NULL), /* Adapt hz to # of clients.*/
    createBoolConfig("lazyfree-lazy-eviction", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_eviction, 0, NULL, NULL),
    createBoolConfig("lazyfree-lazy-expire", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_expire, 0, NULL, NULL),
    createBoolConfig("lazyfree-lazy-server-del", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_server_del, 0, NULL, NULL),
    createBoolConfig("lazyfree-lazy-user-del", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_user_del , 0, NULL, NULL),
    createBoolConfig("lazyfree-lazy-user-flush", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.lazyfree_lazy_user_flush , 0, NULL, NULL),
    createBoolConfig("repl-disable-tcp-nodelay", NULL, MODIFIABLE_CONFIG, server.repl_disable_tcp_nodelay, 0, NULL, NULL),
    createBoolConfig("repl-diskless-sync", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.repl_diskless_sync, 1, NULL, NULL),
    createBoolConfig("aof-rewrite-incremental-fsync", NULL, MODIFIABLE_CONFIG, server.aof_rewrite_incremental_fsync, 1, NULL, NULL),
    createBoolConfig("no-appendfsync-on-rewrite", NULL, MODIFIABLE_CONFIG, server.aof_no_fsync_on_rewrite, 0, NULL, NULL),
    createBoolConfig("cluster-require-full-coverage", NULL, MODIFIABLE_CONFIG, server.cluster_require_full_coverage, 1, NULL, NULL),
    createBoolConfig("rdb-save-incremental-fsync", NULL, MODIFIABLE_CONFIG, server.rdb_save_incremental_fsync, 1, NULL, NULL),
    createBoolConfig("aof-load-truncated", NULL, MODIFIABLE_CONFIG, server.aof_load_truncated, 1, NULL, NULL),
    createBoolConfig("aof-use-rdb-preamble", NULL, MODIFIABLE_CONFIG, server.aof_use_rdb_preamble, 1, NULL, NULL),
    createBoolConfig("aof-timestamp-enabled", NULL, MODIFIABLE_CONFIG, server.aof_timestamp_enabled, 0, NULL, NULL),
    createBoolConfig("cluster-replica-no-failover", "cluster-slave-no-failover", MODIFIABLE_CONFIG, server.cluster_slave_no_failover, 0, NULL, updateClusterFlags), /* Failover by default. */
    createBoolConfig("replica-lazy-flush", "slave-lazy-flush", MODIFIABLE_CONFIG, server.repl_slave_lazy_flush, 0, NULL, NULL),
    createBoolConfig("replica-serve-stale-data", "slave-serve-stale-data", MODIFIABLE_CONFIG, server.repl_serve_stale_data, 1, NULL, NULL),
    createBoolConfig("replica-read-only", "slave-read-only", DEBUG_CONFIG | MODIFIABLE_CONFIG, server.repl_slave_ro, 1, NULL, NULL),
    createBoolConfig("replica-ignore-maxmemory", "slave-ignore-maxmemory", MODIFIABLE_CONFIG, server.repl_slave_ignore_maxmemory, 1, NULL, NULL),
    createBoolConfig("jemalloc-bg-thread", NULL, MODIFIABLE_CONFIG, server.jemalloc_bg_thread, 1, NULL, updateJemallocBgThread),
    createBoolConfig("activedefrag", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, server.active_defrag_enabled, 0, isValidActiveDefrag, NULL),
    createBoolConfig("syslog-enabled", NULL, IMMUTABLE_CONFIG, server.syslog_enabled, 0, NULL, NULL),
    createBoolConfig("cluster-enabled", NULL, IMMUTABLE_CONFIG, server.cluster_enabled, 0, NULL, NULL),
    createBoolConfig("appendonly", NULL, MODIFIABLE_CONFIG | DENY_LOADING_CONFIG, server.aof_enabled, 0, NULL, updateAppendonly),
    createBoolConfig("cluster-allow-reads-when-down", NULL, MODIFIABLE_CONFIG, server.cluster_allow_reads_when_down, 0, NULL, NULL),
    createBoolConfig("cluster-allow-pubsubshard-when-down", NULL, MODIFIABLE_CONFIG, server.cluster_allow_pubsubshard_when_down, 1, NULL, NULL),
    createBoolConfig("crash-log-enabled", NULL, MODIFIABLE_CONFIG, server.crashlog_enabled, 1, NULL, updateSighandlerEnabled),
    createBoolConfig("crash-memcheck-enabled", NULL, MODIFIABLE_CONFIG, server.memcheck_enabled, 1, NULL, NULL),
    createBoolConfig("use-exit-on-panic", NULL, MODIFIABLE_CONFIG | HIDDEN_CONFIG, server.use_exit_on_panic, 0, NULL, NULL),
    createBoolConfig("disable-thp", NULL, IMMUTABLE_CONFIG, server.disable_thp, 1, NULL, NULL),
    createBoolConfig("cluster-allow-replica-migration", NULL, MODIFIABLE_CONFIG, server.cluster_allow_replica_migration, 1, NULL, NULL),
    createBoolConfig("replica-announced", NULL, MODIFIABLE_CONFIG, server.replica_announced, 1, NULL, NULL),
    createBoolConfig("latency-tracking", NULL, MODIFIABLE_CONFIG, server.latency_tracking_enabled, 1, NULL, NULL),
    createBoolConfig("aof-disable-auto-gc", NULL, MODIFIABLE_CONFIG, server.aof_disable_auto_gc, 0, NULL, updateAofAutoGCEnabled),
    createBoolConfig("replica-ignore-disk-write-errors", NULL, MODIFIABLE_CONFIG, server.repl_ignore_disk_write_error, 0, NULL, NULL),

    /* String Configs */
    createStringConfig("aclfile", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.acl_filename, "", NULL, NULL),
    createStringConfig("unixsocket", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.unixsocket, NULL, NULL, NULL),
    createStringConfig("pidfile", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.pidfile, NULL, NULL, NULL),
    createStringConfig("replica-announce-ip", "slave-announce-ip", MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.slave_announce_ip, NULL, NULL, NULL),
    createStringConfig("masteruser", NULL, MODIFIABLE_CONFIG | SENSITIVE_CONFIG, EMPTY_STRING_IS_NULL, server.masteruser, NULL, NULL, NULL),
    createStringConfig("cluster-announce-ip", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.cluster_announce_ip, NULL, NULL, updateClusterIp),
    createStringConfig("cluster-config-file", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.cluster_configfile, "nodes.conf", NULL, NULL),
    createStringConfig("cluster-announce-hostname", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.cluster_announce_hostname, NULL, isValidAnnouncedHostname, updateClusterHostname),
    createStringConfig("syslog-ident", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.syslog_ident, "redis", NULL, NULL),
    createStringConfig("dbfilename", NULL, MODIFIABLE_CONFIG | PROTECTED_CONFIG, ALLOW_EMPTY_STRING, server.rdb_filename, "dump.rdb", isValidDBfilename, NULL),
    createStringConfig("appendfilename", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.aof_filename, "appendonly.aof", isValidAOFfilename, NULL),
    createStringConfig("appenddirname", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.aof_dirname, "appendonlydir", isValidAOFdirname, NULL),
    createStringConfig("server_cpulist", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.server_cpulist, NULL, NULL, NULL),
    createStringConfig("bio_cpulist", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.bio_cpulist, NULL, NULL, NULL),
    createStringConfig("aof_rewrite_cpulist", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.aof_rewrite_cpulist, NULL, NULL, NULL),
    createStringConfig("bgsave_cpulist", NULL, IMMUTABLE_CONFIG, EMPTY_STRING_IS_NULL, server.bgsave_cpulist, NULL, NULL, NULL),
    createStringConfig("ignore-warnings", NULL, MODIFIABLE_CONFIG, ALLOW_EMPTY_STRING, server.ignore_warnings, "", NULL, NULL),
    createStringConfig("proc-title-template", NULL, MODIFIABLE_CONFIG, ALLOW_EMPTY_STRING, server.proc_title_template, CONFIG_DEFAULT_PROC_TITLE_TEMPLATE, isValidProcTitleTemplate, updateProcTitleTemplate),
    createStringConfig("bind-source-addr", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.bind_source_addr, NULL, NULL, NULL),
    createStringConfig("logfile", NULL, IMMUTABLE_CONFIG, ALLOW_EMPTY_STRING, server.logfile, "", NULL, NULL),
#ifdef LOG_REQ_RES
    createStringConfig("req-res-logfile", NULL, IMMUTABLE_CONFIG | HIDDEN_CONFIG, EMPTY_STRING_IS_NULL, server.req_res_logfile, NULL, NULL, NULL),
#endif
    createStringConfig("locale-collate", NULL, MODIFIABLE_CONFIG, ALLOW_EMPTY_STRING, server.locale_collate, "", NULL, updateLocaleCollate),

    /* SDS Configs */
    createSDSConfig("masterauth", NULL, MODIFIABLE_CONFIG | SENSITIVE_CONFIG, EMPTY_STRING_IS_NULL, server.masterauth, NULL, NULL, NULL),
    createSDSConfig("requirepass", NULL, MODIFIABLE_CONFIG | SENSITIVE_CONFIG, EMPTY_STRING_IS_NULL, server.requirepass, NULL, NULL, updateRequirePass),

    /* Enum Configs */
    createEnumConfig("supervised", NULL, IMMUTABLE_CONFIG, supervised_mode_enum, server.supervised_mode, SUPERVISED_NONE, NULL, NULL),
    createEnumConfig("syslog-facility", NULL, IMMUTABLE_CONFIG, syslog_facility_enum, server.syslog_facility, LOG_LOCAL0, NULL, NULL),
    createEnumConfig("repl-diskless-load", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG | DENY_LOADING_CONFIG, repl_diskless_load_enum, server.repl_diskless_load, REPL_DISKLESS_LOAD_DISABLED, NULL, NULL),
    createEnumConfig("loglevel", NULL, MODIFIABLE_CONFIG, loglevel_enum, server.verbosity, LL_NOTICE, NULL, NULL),
    createEnumConfig("maxmemory-policy", NULL, MODIFIABLE_CONFIG, maxmemory_policy_enum, server.maxmemory_policy, MAXMEMORY_NO_EVICTION, NULL, NULL),
    createEnumConfig("appendfsync", NULL, MODIFIABLE_CONFIG, aof_fsync_enum, server.aof_fsync, AOF_FSYNC_EVERYSEC, NULL, updateAppendFsync),
    createEnumConfig("oom-score-adj", NULL, MODIFIABLE_CONFIG, oom_score_adj_enum, server.oom_score_adj, OOM_SCORE_ADJ_NO, NULL, updateOOMScoreAdj),
    createEnumConfig("acl-pubsub-default", NULL, MODIFIABLE_CONFIG, acl_pubsub_default_enum, server.acl_pubsub_default, 0, NULL, NULL),
    createEnumConfig("sanitize-dump-payload", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, sanitize_dump_payload_enum, server.sanitize_dump_payload, SANITIZE_DUMP_NO, NULL, NULL),
    createEnumConfig("enable-protected-configs", NULL, IMMUTABLE_CONFIG, protected_action_enum, server.enable_protected_configs, PROTECTED_ACTION_ALLOWED_NO, NULL, NULL),
    createEnumConfig("enable-debug-command", NULL, IMMUTABLE_CONFIG, protected_action_enum, server.enable_debug_cmd, PROTECTED_ACTION_ALLOWED_NO, NULL, NULL),
    createEnumConfig("enable-module-command", NULL, IMMUTABLE_CONFIG, protected_action_enum, server.enable_module_cmd, PROTECTED_ACTION_ALLOWED_NO, NULL, NULL),
    createEnumConfig("cluster-preferred-endpoint-type", NULL, MODIFIABLE_CONFIG, cluster_preferred_endpoint_type_enum, server.cluster_preferred_endpoint_type, CLUSTER_ENDPOINT_TYPE_IP, NULL, NULL),
    createEnumConfig("propagation-error-behavior", NULL, MODIFIABLE_CONFIG, propagation_error_behavior_enum, server.propagation_error_behavior, PROPAGATION_ERR_BEHAVIOR_IGNORE, NULL, NULL),
    createEnumConfig("shutdown-on-sigint", NULL, MODIFIABLE_CONFIG | MULTI_ARG_CONFIG, shutdown_on_sig_enum, server.shutdown_on_sigint, 0, isValidShutdownOnSigFlags, NULL),
    createEnumConfig("shutdown-on-sigterm", NULL, MODIFIABLE_CONFIG | MULTI_ARG_CONFIG, shutdown_on_sig_enum, server.shutdown_on_sigterm, 0, isValidShutdownOnSigFlags, NULL),

    /* Integer configs */
    createIntConfig("databases", NULL, IMMUTABLE_CONFIG, 1, INT_MAX, server.dbnum, 16, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.port, 6379, INTEGER_CONFIG, NULL, updatePort), /* TCP port. */
    createIntConfig("io-threads", NULL, DEBUG_CONFIG | IMMUTABLE_CONFIG, 1, 128, server.io_threads_num, 1, INTEGER_CONFIG, NULL, NULL), /* Single threaded by default */
    createIntConfig("auto-aof-rewrite-percentage", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.aof_rewrite_perc, 100, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("cluster-replica-validity-factor", "cluster-slave-validity-factor", MODIFIABLE_CONFIG, 0, INT_MAX, server.cluster_slave_validity_factor, 10, INTEGER_CONFIG, NULL, NULL), /* Slave max data age factor. */
    createIntConfig("list-max-listpack-size", "list-max-ziplist-size", MODIFIABLE_CONFIG, INT_MIN, INT_MAX, server.list_max_listpack_size, -2, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("tcp-keepalive", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.tcpkeepalive, 300, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("cluster-migration-barrier", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.cluster_migration_barrier, 1, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("active-defrag-cycle-min", NULL, MODIFIABLE_CONFIG, 1, 99, server.active_defrag_cycle_min, 1, INTEGER_CONFIG, NULL, NULL), /* Default: 1% CPU min (at lower threshold) */
    createIntConfig("active-defrag-cycle-max", NULL, MODIFIABLE_CONFIG, 1, 99, server.active_defrag_cycle_max, 25, INTEGER_CONFIG, NULL, NULL), /* Default: 25% CPU max (at upper threshold) */
    createIntConfig("active-defrag-threshold-lower", NULL, MODIFIABLE_CONFIG, 0, 1000, server.active_defrag_threshold_lower, 10, INTEGER_CONFIG, NULL, NULL), /* Default: don't defrag when fragmentation is below 10% */
    createIntConfig("active-defrag-threshold-upper", NULL, MODIFIABLE_CONFIG, 0, 1000, server.active_defrag_threshold_upper, 100, INTEGER_CONFIG, NULL, NULL), /* Default: maximum defrag force at 100% fragmentation */
    createIntConfig("lfu-log-factor", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.lfu_log_factor, 10, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("lfu-decay-time", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.lfu_decay_time, 1, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("replica-priority", "slave-priority", MODIFIABLE_CONFIG, 0, INT_MAX, server.slave_priority, 100, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("repl-diskless-sync-delay", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.repl_diskless_sync_delay, 5, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("maxmemory-samples", NULL, MODIFIABLE_CONFIG, 1, INT_MAX, server.maxmemory_samples, 5, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("maxmemory-eviction-tenacity", NULL, MODIFIABLE_CONFIG, 0, 100, server.maxmemory_eviction_tenacity, 10, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("timeout", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.maxidletime, 0, INTEGER_CONFIG, NULL, NULL), /* Default client timeout: infinite */
    createIntConfig("replica-announce-port", "slave-announce-port", MODIFIABLE_CONFIG, 0, 65535, server.slave_announce_port, 0, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("tcp-backlog", NULL, IMMUTABLE_CONFIG, 0, INT_MAX, server.tcp_backlog, 511, INTEGER_CONFIG, NULL, NULL), /* TCP listen backlog. */
    createIntConfig("cluster-port", NULL, IMMUTABLE_CONFIG, 0, 65535, server.cluster_port, 0, INTEGER_CONFIG, NULL, NULL),    
    createIntConfig("cluster-announce-bus-port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.cluster_announce_bus_port, 0, INTEGER_CONFIG, NULL, updateClusterAnnouncedPort), /* Default: Use +10000 offset. */
    createIntConfig("cluster-announce-port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.cluster_announce_port, 0, INTEGER_CONFIG, NULL, updateClusterAnnouncedPort), /* Use server.port */
    createIntConfig("cluster-announce-tls-port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.cluster_announce_tls_port, 0, INTEGER_CONFIG, NULL, updateClusterAnnouncedPort), /* Use server.tls_port */
    createIntConfig("repl-timeout", NULL, MODIFIABLE_CONFIG, 1, INT_MAX, server.repl_timeout, 60, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("repl-ping-replica-period", "repl-ping-slave-period", MODIFIABLE_CONFIG, 1, INT_MAX, server.repl_ping_slave_period, 10, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("list-compress-depth", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, 0, INT_MAX, server.list_compress_depth, 0, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("rdb-key-save-delay", NULL, MODIFIABLE_CONFIG | HIDDEN_CONFIG, INT_MIN, INT_MAX, server.rdb_key_save_delay, 0, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("key-load-delay", NULL, MODIFIABLE_CONFIG | HIDDEN_CONFIG, INT_MIN, INT_MAX, server.key_load_delay, 0, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("active-expire-effort", NULL, MODIFIABLE_CONFIG, 1, 10, server.active_expire_effort, 1, INTEGER_CONFIG, NULL, NULL), /* From 1 to 10. */
    createIntConfig("hz", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.config_hz, CONFIG_DEFAULT_HZ, INTEGER_CONFIG, NULL, updateHZ),
    createIntConfig("min-replicas-to-write", "min-slaves-to-write", MODIFIABLE_CONFIG, 0, INT_MAX, server.repl_min_slaves_to_write, 0, INTEGER_CONFIG, NULL, updateGoodSlaves),
    createIntConfig("min-replicas-max-lag", "min-slaves-max-lag", MODIFIABLE_CONFIG, 0, INT_MAX, server.repl_min_slaves_max_lag, 10, INTEGER_CONFIG, NULL, updateGoodSlaves),
    createIntConfig("watchdog-period", NULL, MODIFIABLE_CONFIG | HIDDEN_CONFIG, 0, INT_MAX, server.watchdog_period, 0, INTEGER_CONFIG, NULL, updateWatchdogPeriod),
    createIntConfig("shutdown-timeout", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.shutdown_timeout, 10, INTEGER_CONFIG, NULL, NULL),
    createIntConfig("repl-diskless-sync-max-replicas", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.repl_diskless_sync_max_replicas, 0, INTEGER_CONFIG, NULL, NULL),

    /* Unsigned int configs */
    createUIntConfig("maxclients", NULL, MODIFIABLE_CONFIG, 1, UINT_MAX, server.maxclients, 10000, INTEGER_CONFIG, NULL, updateMaxclients),
    createUIntConfig("unixsocketperm", NULL, IMMUTABLE_CONFIG, 0, 0777, server.unixsocketperm, 0, OCTAL_CONFIG, NULL, NULL),
    createUIntConfig("socket-mark-id", NULL, IMMUTABLE_CONFIG, 0, UINT_MAX, server.socket_mark_id, 0, INTEGER_CONFIG, NULL, NULL),
#ifdef LOG_REQ_RES
    createUIntConfig("client-default-resp", NULL, IMMUTABLE_CONFIG | HIDDEN_CONFIG, 2, 3, server.client_default_resp, 2, INTEGER_CONFIG, NULL, NULL),
#endif

    /* Unsigned Long configs */
    createULongConfig("active-defrag-max-scan-fields", NULL, MODIFIABLE_CONFIG, 1, LONG_MAX, server.active_defrag_max_scan_fields, 1000, INTEGER_CONFIG, NULL, NULL), /* Default: keys with more than 1000 fields will be processed separately */
    createULongConfig("slowlog-max-len", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.slowlog_max_len, 128, INTEGER_CONFIG, NULL, NULL),
    createULongConfig("acllog-max-len", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.acllog_max_len, 128, INTEGER_CONFIG, NULL, NULL),

    /* Long Long configs */
    createLongLongConfig("busy-reply-threshold", "lua-time-limit", MODIFIABLE_CONFIG, 0, LONG_MAX, server.busy_reply_threshold, 5000, INTEGER_CONFIG, NULL, NULL),/* milliseconds */
    createLongLongConfig("cluster-node-timeout", NULL, MODIFIABLE_CONFIG, 0, LLONG_MAX, server.cluster_node_timeout, 15000, INTEGER_CONFIG, NULL, NULL),
    createLongLongConfig("cluster-ping-interval", NULL, MODIFIABLE_CONFIG | HIDDEN_CONFIG, 0, LLONG_MAX, server.cluster_ping_interval, 0, INTEGER_CONFIG, NULL, NULL),
    createLongLongConfig("slowlog-log-slower-than", NULL, MODIFIABLE_CONFIG, -1, LLONG_MAX, server.slowlog_log_slower_than, 10000, INTEGER_CONFIG, NULL, NULL),
    createLongLongConfig("latency-monitor-threshold", NULL, MODIFIABLE_CONFIG, 0, LLONG_MAX, server.latency_monitor_threshold, 0, INTEGER_CONFIG, NULL, NULL),
    createLongLongConfig("proto-max-bulk-len", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, 1024*1024, LONG_MAX, server.proto_max_bulk_len, 512ll*1024*1024, MEMORY_CONFIG, NULL, NULL), /* Bulk request max size */
    createLongLongConfig("stream-node-max-entries", NULL, MODIFIABLE_CONFIG, 0, LLONG_MAX, server.stream_node_max_entries, 100, INTEGER_CONFIG, NULL, NULL),
    createLongLongConfig("repl-backlog-size", NULL, MODIFIABLE_CONFIG, 1, LLONG_MAX, server.repl_backlog_size, 1024*1024, MEMORY_CONFIG, NULL, updateReplBacklogSize), /* Default: 1mb */

    /* Unsigned Long Long configs */
    createULongLongConfig("maxmemory", NULL, MODIFIABLE_CONFIG, 0, ULLONG_MAX, server.maxmemory, 0, MEMORY_CONFIG, NULL, updateMaxmemory),
    createULongLongConfig("cluster-link-sendbuf-limit", NULL, MODIFIABLE_CONFIG, 0, ULLONG_MAX, server.cluster_link_msg_queue_limit_bytes, 0, MEMORY_CONFIG, NULL, NULL),

    /* Size_t configs */
    createSizeTConfig("hash-max-listpack-entries", "hash-max-ziplist-entries", MODIFIABLE_CONFIG, 0, LONG_MAX, server.hash_max_listpack_entries, 512, INTEGER_CONFIG, NULL, NULL),
    createSizeTConfig("set-max-intset-entries", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.set_max_intset_entries, 512, INTEGER_CONFIG, NULL, NULL),
    createSizeTConfig("set-max-listpack-entries", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.set_max_listpack_entries, 128, INTEGER_CONFIG, NULL, NULL),
    createSizeTConfig("set-max-listpack-value", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.set_max_listpack_value, 64, INTEGER_CONFIG, NULL, NULL),
    createSizeTConfig("zset-max-listpack-entries", "zset-max-ziplist-entries", MODIFIABLE_CONFIG, 0, LONG_MAX, server.zset_max_listpack_entries, 128, INTEGER_CONFIG, NULL, NULL),
    createSizeTConfig("active-defrag-ignore-bytes", NULL, MODIFIABLE_CONFIG, 1, LLONG_MAX, server.active_defrag_ignore_bytes, 100<<20, MEMORY_CONFIG, NULL, NULL), /* Default: don't defrag if frag overhead is below 100mb */
    createSizeTConfig("hash-max-listpack-value", "hash-max-ziplist-value", MODIFIABLE_CONFIG, 0, LONG_MAX, server.hash_max_listpack_value, 64, MEMORY_CONFIG, NULL, NULL),
    createSizeTConfig("stream-node-max-bytes", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.stream_node_max_bytes, 4096, MEMORY_CONFIG, NULL, NULL),
    createSizeTConfig("zset-max-listpack-value", "zset-max-ziplist-value", MODIFIABLE_CONFIG, 0, LONG_MAX, server.zset_max_listpack_value, 64, MEMORY_CONFIG, NULL, NULL),
    createSizeTConfig("hll-sparse-max-bytes", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.hll_sparse_max_bytes, 3000, MEMORY_CONFIG, NULL, NULL),
    createSizeTConfig("tracking-table-max-keys", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.tracking_table_max_keys, 1000000, INTEGER_CONFIG, NULL, NULL), /* Default: 1 million keys max. */
    createSizeTConfig("client-query-buffer-limit", NULL, DEBUG_CONFIG | MODIFIABLE_CONFIG, 1024*1024, LONG_MAX, server.client_max_querybuf_len, 1024*1024*1024, MEMORY_CONFIG, NULL, NULL), /* Default: 1GB max query buffer. */
    createSSizeTConfig("maxmemory-clients", NULL, MODIFIABLE_CONFIG, -100, SSIZE_MAX, server.maxmemory_clients, 0, MEMORY_CONFIG | PERCENT_CONFIG, NULL, applyClientMaxMemoryUsage),

    /* Other configs */
    createTimeTConfig("repl-backlog-ttl", NULL, MODIFIABLE_CONFIG, 0, LONG_MAX, server.repl_backlog_time_limit, 60*60, INTEGER_CONFIG, NULL, NULL), /* Default: 1 hour */
    createOffTConfig("auto-aof-rewrite-min-size", NULL, MODIFIABLE_CONFIG, 0, LLONG_MAX, server.aof_rewrite_min_size, 64*1024*1024, MEMORY_CONFIG, NULL, NULL),
    createOffTConfig("loading-process-events-interval-bytes", NULL, MODIFIABLE_CONFIG | HIDDEN_CONFIG, 1024, INT_MAX, server.loading_process_events_interval_bytes, 1024*1024*2, INTEGER_CONFIG, NULL, NULL),

    createIntConfig("tls-port", NULL, MODIFIABLE_CONFIG, 0, 65535, server.tls_port, 0, INTEGER_CONFIG, NULL, applyTLSPort), /* TCP port. */
    createIntConfig("tls-session-cache-size", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.tls_ctx_config.session_cache_size, 20*1024, INTEGER_CONFIG, NULL, applyTlsCfg),
    createIntConfig("tls-session-cache-timeout", NULL, MODIFIABLE_CONFIG, 0, INT_MAX, server.tls_ctx_config.session_cache_timeout, 300, INTEGER_CONFIG, NULL, applyTlsCfg),
    createBoolConfig("tls-cluster", NULL, MODIFIABLE_CONFIG, server.tls_cluster, 0, NULL, applyTlsCfg),
    createBoolConfig("tls-replication", NULL, MODIFIABLE_CONFIG, server.tls_replication, 0, NULL, applyTlsCfg),
    createEnumConfig("tls-auth-clients", NULL, MODIFIABLE_CONFIG, tls_auth_clients_enum, server.tls_auth_clients, TLS_CLIENT_AUTH_YES, NULL, NULL),
    createBoolConfig("tls-prefer-server-ciphers", NULL, MODIFIABLE_CONFIG, server.tls_ctx_config.prefer_server_ciphers, 0, NULL, applyTlsCfg),
    createBoolConfig("tls-session-caching", NULL, MODIFIABLE_CONFIG, server.tls_ctx_config.session_caching, 1, NULL, applyTlsCfg),
    createStringConfig("tls-cert-file", NULL, VOLATILE_CONFIG | MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.cert_file, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-key-file", NULL, VOLATILE_CONFIG | MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.key_file, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-key-file-pass", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.key_file_pass, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-client-cert-file", NULL, VOLATILE_CONFIG | MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.client_cert_file, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-client-key-file", NULL, VOLATILE_CONFIG | MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.client_key_file, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-client-key-file-pass", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.client_key_file_pass, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-dh-params-file", NULL, VOLATILE_CONFIG | MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.dh_params_file, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-ca-cert-file", NULL, VOLATILE_CONFIG | MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.ca_cert_file, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-ca-cert-dir", NULL, VOLATILE_CONFIG | MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.ca_cert_dir, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-protocols", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.protocols, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-ciphers", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.ciphers, NULL, NULL, applyTlsCfg),
    createStringConfig("tls-ciphersuites", NULL, MODIFIABLE_CONFIG, EMPTY_STRING_IS_NULL, server.tls_ctx_config.ciphersuites, NULL, NULL, applyTlsCfg),

    /* Special configs */
    createSpecialConfig("dir", NULL, MODIFIABLE_CONFIG | PROTECTED_CONFIG | DENY_LOADING_CONFIG, setConfigDirOption, getConfigDirOption, rewriteConfigDirOption, NULL),
    createSpecialConfig("save", NULL, MODIFIABLE_CONFIG | MULTI_ARG_CONFIG, setConfigSaveOption, getConfigSaveOption, rewriteConfigSaveOption, NULL),
    createSpecialConfig("client-output-buffer-limit", NULL, MODIFIABLE_CONFIG | MULTI_ARG_CONFIG, setConfigClientOutputBufferLimitOption, getConfigClientOutputBufferLimitOption, rewriteConfigClientOutputBufferLimitOption, NULL),
    createSpecialConfig("oom-score-adj-values", NULL, MODIFIABLE_CONFIG | MULTI_ARG_CONFIG, setConfigOOMScoreAdjValuesOption, getConfigOOMScoreAdjValuesOption, rewriteConfigOOMScoreAdjValuesOption, updateOOMScoreAdj),
    createSpecialConfig("notify-keyspace-events", NULL, MODIFIABLE_CONFIG, setConfigNotifyKeyspaceEventsOption, getConfigNotifyKeyspaceEventsOption, rewriteConfigNotifyKeyspaceEventsOption, NULL),
    createSpecialConfig("bind", NULL, MODIFIABLE_CONFIG | MULTI_ARG_CONFIG, setConfigBindOption, getConfigBindOption, rewriteConfigBindOption, applyBind),
    createSpecialConfig("replicaof", "slaveof", IMMUTABLE_CONFIG | MULTI_ARG_CONFIG, setConfigReplicaOfOption, getConfigReplicaOfOption, rewriteConfigReplicaOfOption, NULL),
    createSpecialConfig("latency-tracking-info-percentiles", NULL, MODIFIABLE_CONFIG | MULTI_ARG_CONFIG, setConfigLatencyTrackingInfoPercentilesOutputOption, getConfigLatencyTrackingInfoPercentilesOutputOption, rewriteConfigLatencyTrackingInfoPercentilesOutputOption, NULL),

    /* NULL Terminator, this is dropped when we convert to the runtime array. */
    {NULL}
};

/* Create a new config by copying the passed in config. Returns 1 on success
 * or 0 when their was already a config with the same name.. */
int registerConfigValue(const char *name, const standardConfig *config, int alias) {
    standardConfig *new = zmalloc(sizeof(standardConfig));
    memcpy(new, config, sizeof(standardConfig));
    if (alias) {
        new->flags |= ALIAS_CONFIG;
        new->name = config->alias;
        new->alias = config->name;
    }

    return dictAdd(configs, sdsnew(name), new) == DICT_OK;
}

/* Initialize configs to their default values and create and populate the 
 * runtime configuration dictionary. */
void initConfigValues(void) {
    configs = dictCreate(&sdsHashDictType);
    dictExpand(configs, sizeof(static_configs) / sizeof(standardConfig));
    for (standardConfig *config = static_configs; config->name != NULL; config++) {
        if (config->interface.init) config->interface.init(config);
        /* Add the primary config to the dictionary. */
        int ret = registerConfigValue(config->name, config, 0);
        serverAssert(ret);

        /* Aliases are the same as their primary counter parts, but they
         * also have a flag indicating they are the alias. */
        if (config->alias) {
            int ret = registerConfigValue(config->alias, config, ALIAS_CONFIG);
            serverAssert(ret);
        }
    }
}

/* Remove a config by name from the configs dict. */
void removeConfig(sds name) {
    standardConfig *config = lookupConfig(name);
    if (!config) return;
    if (config->flags & MODULE_CONFIG) {
        sdsfree((sds) config->name);
        if (config->type == ENUM_CONFIG) {
            configEnum *enumNode = config->data.enumd.enum_value;
            while(enumNode->name != NULL) {
                zfree(enumNode->name);
                enumNode++;
            }
            zfree(config->data.enumd.enum_value);
        } else if (config->type == SDS_CONFIG) {
            if (config->data.sds.default_value) sdsfree((sds)config->data.sds.default_value);
        }
    }
    dictDelete(configs, name);
}

/*-----------------------------------------------------------------------------
 * Module Config
 *----------------------------------------------------------------------------*/

/* Create a bool/string/enum/numeric standardConfig for a module config in the configs dictionary */
void addModuleBoolConfig(const char *module_name, const char *name, int flags, void *privdata, int default_val) {
    sds config_name = sdscatfmt(sdsempty(), "%s.%s", module_name, name);
    int config_dummy_address;
    standardConfig module_config = createBoolConfig(config_name, NULL, flags | MODULE_CONFIG, config_dummy_address, default_val, NULL, NULL);
    module_config.data.yesno.config = NULL;
    module_config.privdata = privdata;
    registerConfigValue(config_name, &module_config, 0);
}

void addModuleStringConfig(const char *module_name, const char *name, int flags, void *privdata, sds default_val) {
    sds config_name = sdscatfmt(sdsempty(), "%s.%s", module_name, name);
    sds config_dummy_address;
    standardConfig module_config = createSDSConfig(config_name, NULL, flags | MODULE_CONFIG, 0, config_dummy_address, default_val, NULL, NULL);
    module_config.data.sds.config = NULL;
    module_config.privdata = privdata;
    registerConfigValue(config_name, &module_config, 0);
}

void addModuleEnumConfig(const char *module_name, const char *name, int flags, void *privdata, int default_val, configEnum *enum_vals) {
    sds config_name = sdscatfmt(sdsempty(), "%s.%s", module_name, name);
    int config_dummy_address;
    standardConfig module_config = createEnumConfig(config_name, NULL, flags | MODULE_CONFIG, enum_vals, config_dummy_address, default_val, NULL, NULL);
    module_config.data.enumd.config = NULL;
    module_config.privdata = privdata;
    registerConfigValue(config_name, &module_config, 0);
}

void addModuleNumericConfig(const char *module_name, const char *name, int flags, void *privdata, long long default_val, int conf_flags, long long lower, long long upper) {
    sds config_name = sdscatfmt(sdsempty(), "%s.%s", module_name, name);
    long long config_dummy_address;
    standardConfig module_config = createLongLongConfig(config_name, NULL, flags | MODULE_CONFIG, lower, upper, config_dummy_address, default_val, conf_flags, NULL, NULL);
    module_config.data.numeric.config.ll = NULL;
    module_config.privdata = privdata;
    registerConfigValue(config_name, &module_config, 0);
}

/*-----------------------------------------------------------------------------
 * CONFIG HELP
 *----------------------------------------------------------------------------*/

void configHelpCommand(client *c) {
    const char *help[] = {
"GET <pattern>",
"    Return parameters matching the glob-like <pattern> and their values.",
"SET <directive> <value>",
"    Set the configuration <directive> to <value>.",
"RESETSTAT",
"    Reset statistics reported by the INFO command.",
"REWRITE",
"    Rewrite the configuration file.",
NULL
    };

    addReplyHelp(c, help);
}

/*-----------------------------------------------------------------------------
 * CONFIG RESETSTAT
 *----------------------------------------------------------------------------*/

void configResetStatCommand(client *c) {
    resetServerStats();
    resetCommandTableStats(server.commands);
    resetErrorTableStats();
    addReply(c,shared.ok);
}

/*-----------------------------------------------------------------------------
 * CONFIG REWRITE
 *----------------------------------------------------------------------------*/

void configRewriteCommand(client *c) {
    if (server.configfile == NULL) {
        addReplyError(c,"The server is running without a config file");
        return;
    }
    if (rewriteConfig(server.configfile, 0) == -1) {
        /* save errno in case of being tainted. */
        int err = errno;
        serverLog(LL_WARNING,"CONFIG REWRITE failed: %s", strerror(err));
        addReplyErrorFormat(c,"Rewriting config file: %s", strerror(err));
    } else {
        serverLog(LL_NOTICE,"CONFIG REWRITE executed with success.");
        addReply(c,shared.ok);
    }
}
