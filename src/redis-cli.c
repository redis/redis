/* Redis CLI (command line interface)
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

#include "fmacros.h"
#include "version.h"

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

#include <hiredis.h>
#include <sds.h> /* use sds.h from hiredis, so that only one set of sds functions will be present in the binary */
#include "zmalloc.h"
#include "linenoise.h"
#include "help.h"
#include "anet.h"
#include "ae.h"

#define UNUSED(V) ((void) V)

#define OUTPUT_STANDARD 0
#define OUTPUT_RAW 1
#define OUTPUT_CSV 2
#define REDIS_CLI_KEEPALIVE_INTERVAL 15 /* seconds */
#define REDIS_CLI_DEFAULT_PIPE_TIMEOUT 30 /* seconds */
#define REDIS_CLI_HISTFILE_ENV "REDISCLI_HISTFILE"
#define REDIS_CLI_HISTFILE_DEFAULT ".rediscli_history"
#define REDIS_CLI_RCFILE_ENV "REDISCLI_RCFILE"
#define REDIS_CLI_RCFILE_DEFAULT ".redisclirc"

/* --latency-dist palettes. */
int spectrum_palette_color_size = 19;
int spectrum_palette_color[] = {0,233,234,235,237,239,241,243,245,247,144,143,142,184,226,214,208,202,196};

int spectrum_palette_mono_size = 13;
int spectrum_palette_mono[] = {0,233,234,235,237,239,241,243,245,247,249,251,253};

/* The actual palette in use. */
int *spectrum_palette;
int spectrum_palette_size;

static redisContext *context;
static struct config {
    char *hostip;
    int hostport;
    char *hostsocket;
    long repeat;
    long interval;
    int dbnum;
    int interactive;
    int shutdown;
    int monitor_mode;
    int pubsub_mode;
    int latency_mode;
    int latency_dist_mode;
    int latency_history;
    int lru_test_mode;
    long long lru_test_sample_size;
    int cluster_mode;
    int cluster_reissue_command;
    int slave_mode;
    int pipe_mode;
    int pipe_timeout;
    int getrdb_mode;
    int stat_mode;
    int scan_mode;
    int intrinsic_latency_mode;
    int intrinsic_latency_duration;
    char *pattern;
    char *rdb_filename;
    int bigkeys;
    int stdinarg; /* get last arg from stdin. (-x option) */
    char *auth;
    int output; /* output mode, see OUTPUT_* defines */
    sds mb_delim;
    char prompt[128];
    char *eval;
    int eval_ldb;
    int eval_ldb_sync;  /* Ask for synchronous mode of the Lua debugger. */
    int eval_ldb_end;   /* Lua debugging session ended. */
    int enable_ldb_on_eval; /* Handle manual SCRIPT DEBUG + EVAL commands. */
    int last_cmd_type;
} config;

/* User preferences. */
static struct pref {
    int hints;
} pref;

static volatile sig_atomic_t force_cancel_loop = 0;
static void usage(void);
static void slaveMode(void);
char *redisGitSHA1(void);
char *redisGitDirty(void);
static int cliConnect(int force);

/*------------------------------------------------------------------------------
 * Utility functions
 *--------------------------------------------------------------------------- */

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
    int len;

    if (config.eval_ldb) return;
    if (config.hostsocket != NULL)
        len = snprintf(config.prompt,sizeof(config.prompt),"redis %s",
                       config.hostsocket);
    else
        len = anetFormatAddr(config.prompt, sizeof(config.prompt),
                           config.hostip, config.hostport);
    /* Add [dbnum] if needed */
    if (config.dbnum != 0)
        len += snprintf(config.prompt+len,sizeof(config.prompt)-len,"[%d]",
            config.dbnum);
    snprintf(config.prompt+len,sizeof(config.prompt)-len,"> ");
}

/* Return the name of the dotfile for the specified 'dotfilename'.
 * Normally it just concatenates user $HOME to the file specified
 * in 'dotfilename'. However if the environment varialbe 'envoverride'
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
    struct commandHelp *org;
} helpEntry;

static helpEntry *helpEntries;
static int helpEntriesLen;

static sds cliVersion(void) {
    sds version;
    version = sdscatprintf(sdsempty(), "%s", REDIS_VERSION);

    /* Add git commit and working tree status when available */
    if (strtoll(redisGitSHA1(),NULL,16)) {
        version = sdscatprintf(version, " (git:%s", redisGitSHA1());
        if (strtoll(redisGitDirty(),NULL,10))
            version = sdscatprintf(version, "-dirty");
        version = sdscat(version, ")");
    }
    return version;
}

static void cliInitHelp(void) {
    int commandslen = sizeof(commandHelp)/sizeof(struct commandHelp);
    int groupslen = sizeof(commandGroups)/sizeof(char*);
    int i, len, pos = 0;
    helpEntry tmp;

    helpEntriesLen = len = commandslen+groupslen;
    helpEntries = zmalloc(sizeof(helpEntry)*len);

    for (i = 0; i < groupslen; i++) {
        tmp.argc = 1;
        tmp.argv = zmalloc(sizeof(sds));
        tmp.argv[0] = sdscatprintf(sdsempty(),"@%s",commandGroups[i]);
        tmp.full = tmp.argv[0];
        tmp.type = CLI_HELP_GROUP;
        tmp.org = NULL;
        helpEntries[pos++] = tmp;
    }

    for (i = 0; i < commandslen; i++) {
        tmp.argv = sdssplitargs(commandHelp[i].name,&tmp.argc);
        tmp.full = sdsnew(commandHelp[i].name);
        tmp.type = CLI_HELP_COMMAND;
        tmp.org = &commandHelp[i];
        helpEntries[pos++] = tmp;
    }
}

/* cliInitHelp() setups the helpEntries array with the command and group
 * names from the help.h file. However the Redis instance we are connecting
 * to may support more commands, so this function integrates the previous
 * entries with additional entries obtained using the COMMAND command
 * available in recent versions of Redis. */
static void cliIntegrateHelp(void) {
    if (cliConnect(0) == REDIS_ERR) return;

    redisReply *reply = redisCommand(context, "COMMAND");
    if(reply == NULL || reply->type != REDIS_REPLY_ARRAY) return;

    /* Scan the array reported by COMMAND and fill only the entries that
     * don't already match what we have. */
    for (size_t j = 0; j < reply->elements; j++) {
        redisReply *entry = reply->element[j];
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

        struct commandHelp *ch = zmalloc(sizeof(*ch));
        ch->name = new->argv[0];
        ch->params = sdsempty();
        int args = llabs(entry->element[1]->integer);
        if (entry->element[3]->integer == 1) {
            ch->params = sdscat(ch->params,"key ");
            args--;
        }
        while(args--) ch->params = sdscat(ch->params,"arg ");
        if (entry->element[1]->integer < 0)
            ch->params = sdscat(ch->params,"...options...");
        ch->summary = "Help not available";
        ch->group = 0;
        ch->since = "not known";
        new->org = ch;
    }
    freeReplyObject(reply);
}

/* Output command help to stdout. */
static void cliOutputCommandHelp(struct commandHelp *help, int group) {
    printf("\r\n  \x1b[1m%s\x1b[0m \x1b[90m%s\x1b[0m\r\n", help->name, help->params);
    printf("  \x1b[33msummary:\x1b[0m %s\r\n", help->summary);
    printf("  \x1b[33msince:\x1b[0m %s\r\n", help->since);
    if (group) {
        printf("  \x1b[33mgroup:\x1b[0m %s\r\n", commandGroups[help->group]);
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
        "To set redis-cli perferences:\n"
        "      \":set hints\" enable online hints\n"
        "      \":set nohints\" disable online hints\n"
        "Set your preferences in ~/.redisclirc\n",
        version
    );
    sdsfree(version);
}

/* Output all command help, filtering by group or command name. */
static void cliOutputHelp(int argc, char **argv) {
    int i, j, len;
    int group = -1;
    helpEntry *entry;
    struct commandHelp *help;

    if (argc == 0) {
        cliOutputGenericHelp();
        return;
    } else if (argc > 0 && argv[0][0] == '@') {
        len = sizeof(commandGroups)/sizeof(char*);
        for (i = 0; i < len; i++) {
            if (strcasecmp(argv[0]+1,commandGroups[i]) == 0) {
                group = i;
                break;
            }
        }
    }

    assert(argc > 0);
    for (i = 0; i < helpEntriesLen; i++) {
        entry = &helpEntries[i];
        if (entry->type != CLI_HELP_COMMAND) continue;

        help = entry->org;
        if (group == -1) {
            /* Compare all arguments */
            if (argc == entry->argc) {
                for (j = 0; j < argc; j++) {
                    if (strcasecmp(argv[j],entry->argv[j]) != 0) break;
                }
                if (j == argc) {
                    cliOutputCommandHelp(help,1);
                }
            }
        } else {
            if (group == help->group) {
                cliOutputCommandHelp(help,0);
            }
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

/* Linenoise hints callback. */
static char *hintsCallback(const char *buf, int *color, int *bold) {
    if (!pref.hints) return NULL;

    int i, argc, buflen = strlen(buf);
    sds *argv = sdssplitargs(buf,&argc);
    int endspace = buflen && isspace(buf[buflen-1]);

    /* Check if the argument list is empty and return ASAP. */
    if (argc == 0) {
        sdsfreesplitres(argv,argc);
        return NULL;
    }

    for (i = 0; i < helpEntriesLen; i++) {
        if (!(helpEntries[i].type & CLI_HELP_COMMAND)) continue;

        if (strcasecmp(argv[0],helpEntries[i].full) == 0)
        {
            *color = 90;
            *bold = 0;
            sds hint = sdsnew(helpEntries[i].org->params);

            /* Remove arguments from the returned hint to show only the
             * ones the user did not yet typed. */
            int toremove = argc-1;
            while(toremove > 0 && sdslen(hint)) {
                if (hint[0] == '[') break;
                if (hint[0] == ' ') toremove--;
                sdsrange(hint,1,-1);
            }

            /* Add an initial space if needed. */
            if (!endspace) {
                sds newhint = sdsnewlen(" ",1);
                newhint = sdscatsds(newhint,hint);
                sdsfree(hint);
                hint = newhint;
            }

            sdsfreesplitres(argv,argc);
            return hint;
        }
    }
    sdsfreesplitres(argv,argc);
    return NULL;
}

static void freeHintsCallback(void *ptr) {
    sdsfree(ptr);
}

/*------------------------------------------------------------------------------
 * Networking / parsing
 *--------------------------------------------------------------------------- */

/* Send AUTH command to the server */
static int cliAuth(void) {
    redisReply *reply;
    if (config.auth == NULL) return REDIS_OK;

    reply = redisCommand(context,"AUTH %s",config.auth);
    if (reply != NULL) {
        freeReplyObject(reply);
        return REDIS_OK;
    }
    return REDIS_ERR;
}

/* Send SELECT dbnum to the server */
static int cliSelect(void) {
    redisReply *reply;
    if (config.dbnum == 0) return REDIS_OK;

    reply = redisCommand(context,"SELECT %d",config.dbnum);
    if (reply != NULL) {
        int result = REDIS_OK;
        if (reply->type == REDIS_REPLY_ERROR) result = REDIS_ERR;
        freeReplyObject(reply);
        return result;
    }
    return REDIS_ERR;
}

/* Connect to the server. If force is not zero the connection is performed
 * even if there is already a connected socket. */
static int cliConnect(int force) {
    if (context == NULL || force) {
        if (context != NULL) {
            redisFree(context);
        }

        if (config.hostsocket == NULL) {
            context = redisConnect(config.hostip,config.hostport);
        } else {
            context = redisConnectUnix(config.hostsocket);
        }

        if (context->err) {
            fprintf(stderr,"Could not connect to Redis at ");
            if (config.hostsocket == NULL)
                fprintf(stderr,"%s:%d: %s\n",config.hostip,config.hostport,context->errstr);
            else
                fprintf(stderr,"%s: %s\n",config.hostsocket,context->errstr);
            redisFree(context);
            context = NULL;
            return REDIS_ERR;
        }

        /* Set aggressive KEEP_ALIVE socket option in the Redis context socket
         * in order to prevent timeouts caused by the execution of long
         * commands. At the same time this improves the detection of real
         * errors. */
        anetKeepAlive(NULL, context->fd, REDIS_CLI_KEEPALIVE_INTERVAL);

        /* Do AUTH and select the right DB. */
        if (cliAuth() != REDIS_OK)
            return REDIS_ERR;
        if (cliSelect() != REDIS_OK)
            return REDIS_ERR;
    }
    return REDIS_OK;
}

static void cliPrintContextError(void) {
    if (context == NULL) return;
    fprintf(stderr,"Error: %s\n",context->errstr);
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
    case REDIS_REPLY_STRING:
        /* If you are producing output for the standard output we want
        * a more interesting output with quoted characters and so forth */
        out = sdscatrepr(out,r->str,r->len);
        out = sdscat(out,"\n");
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"(nil)\n");
    break;
    case REDIS_REPLY_ARRAY:
        if (r->elements == 0) {
            out = sdscat(out,"(empty list or set)\n");
        } else {
            unsigned int i, idxlen = 0;
            char _prefixlen[16];
            char _prefixfmt[16];
            sds _prefix;
            sds tmp;

            /* Calculate chars needed to represent the largest index */
            i = r->elements;
            do {
                idxlen++;
                i /= 10;
            } while(i);

            /* Prefix for nested multi bulks should grow with idxlen+2 spaces */
            memset(_prefixlen,' ',idxlen+2);
            _prefixlen[idxlen+2] = '\0';
            _prefix = sdscat(sdsnew(prefix),_prefixlen);

            /* Setup prefix format for every entry */
            snprintf(_prefixfmt,sizeof(_prefixfmt),"%%s%%%ud) ",idxlen);

            for (i = 0; i < r->elements; i++) {
                /* Don't use the prefix for the first element, as the parent
                 * caller already prepended the index number. */
                out = sdscatprintf(out,_prefixfmt,i == 0 ? "" : prefix,i+1);

                /* Format the multi bulk entry */
                tmp = cliFormatReplyTTY(r->element[i],_prefix);
                out = sdscatlen(out,tmp,sdslen(tmp));
                sdsfree(tmp);
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

int isColorTerm(void) {
    char *t = getenv("TERM");
    return t != NULL && strstr(t,"xterm") != NULL;
}

/* Helpe  function for sdsCatColorizedLdbReply() appending colorize strings
 * to an SDS string. */
sds sdscatcolor(sds o, char *s, size_t len, char *color) {
    if (!isColorTerm()) return sdscatlen(o,s,len);

    int bold = strstr(color,"bold") != NULL;
    int ccode = 37; /* Defaults to white. */
    if (strstr(color,"red")) ccode = 31;
    else if (strstr(color,"red")) ccode = 31;
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
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
        break;
    case REDIS_REPLY_ARRAY:
        for (i = 0; i < r->elements; i++) {
            if (i > 0) out = sdscat(out,config.mb_delim);
            tmp = cliFormatReplyRaw(r->element[i]);
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
    case REDIS_REPLY_STRING:
        out = sdscatrepr(out,r->str,r->len);
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"NIL");
    break;
    case REDIS_REPLY_ARRAY:
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

static int cliReadReply(int output_raw_strings) {
    void *_reply;
    redisReply *reply;
    sds out = NULL;
    int output = 1;

    if (redisGetReply(context,&_reply) != REDIS_OK) {
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

    reply = (redisReply*)_reply;

    config.last_cmd_type = reply->type;

    /* Check if we need to connect to a different node and reissue the
     * request. */
    if (config.cluster_mode && reply->type == REDIS_REPLY_ERROR &&
        (!strncmp(reply->str,"MOVED",5) || !strcmp(reply->str,"ASK")))
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
        sdsfree(config.hostip);
        config.hostip = sdsnew(p+1);
        config.hostport = atoi(s+1);
        if (config.interactive)
            printf("-> Redirected to slot [%d] located at %s:%d\n",
                slot, config.hostip, config.hostport);
        config.cluster_reissue_command = 1;
        cliRefreshPrompt();
    }

    if (output) {
        if (output_raw_strings) {
            out = cliFormatReplyRaw(reply);
        } else {
            if (config.output == OUTPUT_RAW) {
                out = cliFormatReplyRaw(reply);
                out = sdscat(out,"\n");
            } else if (config.output == OUTPUT_STANDARD) {
                out = cliFormatReplyTTY(reply,"");
            } else if (config.output == OUTPUT_CSV) {
                out = cliFormatReplyCSV(reply);
                out = sdscat(out,"\n");
            }
        }
        fwrite(out,sdslen(out),1,stdout);
        sdsfree(out);
    }
    freeReplyObject(reply);
    return REDIS_OK;
}

static int cliSendCommand(int argc, char **argv, int repeat) {
    char *command = argv[0];
    size_t *argvlen;
    int j, output_raw;

    if (!config.eval_ldb && /* In debugging mode, let's pass "help" to Redis. */
        (!strcasecmp(command,"help") || !strcasecmp(command,"?"))) {
        cliOutputHelp(--argc, ++argv);
        return REDIS_OK;
    }

    if (context == NULL) return REDIS_ERR;

    output_raw = 0;
    if (!strcasecmp(command,"info") ||
        (argc >= 2 && !strcasecmp(command,"debug") &&
                      ((!strcasecmp(argv[1],"jemalloc") && !strcasecmp(argv[2],"info")) ||
                       !strcasecmp(argv[1],"htstats"))) ||
        (argc == 2 && !strcasecmp(command,"cluster") &&
                      (!strcasecmp(argv[1],"nodes") ||
                       !strcasecmp(argv[1],"info"))) ||
        (argc == 2 && !strcasecmp(command,"client") &&
                       !strcasecmp(argv[1],"list")) ||
        (argc == 3 && !strcasecmp(command,"latency") &&
                       !strcasecmp(argv[1],"graph")) ||
        (argc == 2 && !strcasecmp(command,"latency") &&
                       !strcasecmp(argv[1],"doctor")))
    {
        output_raw = 1;
    }

    if (!strcasecmp(command,"shutdown")) config.shutdown = 1;
    if (!strcasecmp(command,"monitor")) config.monitor_mode = 1;
    if (!strcasecmp(command,"subscribe") ||
        !strcasecmp(command,"psubscribe")) config.pubsub_mode = 1;
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

    while(repeat--) {
        redisAppendCommandArgv(context,argc,(const char**)argv,argvlen);
        while (config.monitor_mode) {
            if (cliReadReply(output_raw) != REDIS_OK) exit(1);
            fflush(stdout);
        }

        if (config.pubsub_mode) {
            if (config.output != OUTPUT_RAW)
                printf("Reading messages... (press Ctrl-C to quit)\n");
            while (1) {
                if (cliReadReply(output_raw) != REDIS_OK) exit(1);
            }
        }

        if (config.slave_mode) {
            printf("Entering slave output mode...  (press Ctrl-C to quit)\n");
            slaveMode();
            config.slave_mode = 0;
            zfree(argvlen);
            return REDIS_ERR;  /* Error = slaveMode lost connection to master */
        }

        if (cliReadReply(output_raw) != REDIS_OK) {
            zfree(argvlen);
            return REDIS_ERR;
        } else {
            /* Store database number when SELECT was successfully executed. */
            if (!strcasecmp(command,"select") && argc == 2 && config.last_cmd_type != REDIS_REPLY_ERROR) {
                config.dbnum = atoi(argv[1]);
                cliRefreshPrompt();
            } else if (!strcasecmp(command,"auth") && argc == 2) {
                cliSelect();
            }
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
            c = redisConnect(config.hostip,config.hostport);
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
            sdsfree(config.hostip);
            config.hostip = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-h") && lastarg) {
            usage();
        } else if (!strcmp(argv[i],"--help")) {
            usage();
        } else if (!strcmp(argv[i],"-x")) {
            config.stdinarg = 1;
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.hostport = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-s") && !lastarg) {
            config.hostsocket = argv[++i];
        } else if (!strcmp(argv[i],"-r") && !lastarg) {
            config.repeat = strtoll(argv[++i],NULL,10);
        } else if (!strcmp(argv[i],"-i") && !lastarg) {
            double seconds = atof(argv[++i]);
            config.interval = seconds*1000000;
        } else if (!strcmp(argv[i],"-n") && !lastarg) {
            config.dbnum = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-a") && !lastarg) {
            config.auth = argv[++i];
        } else if (!strcmp(argv[i],"--raw")) {
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"--no-raw")) {
            config.output = OUTPUT_STANDARD;
        } else if (!strcmp(argv[i],"--csv")) {
            config.output = OUTPUT_CSV;
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
        } else if (!strcmp(argv[i],"--stat")) {
            config.stat_mode = 1;
        } else if (!strcmp(argv[i],"--scan")) {
            config.scan_mode = 1;
        } else if (!strcmp(argv[i],"--pattern") && !lastarg) {
            config.pattern = argv[++i];
        } else if (!strcmp(argv[i],"--intrinsic-latency") && !lastarg) {
            config.intrinsic_latency_mode = 1;
            config.intrinsic_latency_duration = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--rdb") && !lastarg) {
            config.getrdb_mode = 1;
            config.rdb_filename = argv[++i];
        } else if (!strcmp(argv[i],"--pipe")) {
            config.pipe_mode = 1;
        } else if (!strcmp(argv[i],"--pipe-timeout") && !lastarg) {
            config.pipe_timeout = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--bigkeys")) {
            config.bigkeys = 1;
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
        } else if (!strcmp(argv[i],"-v") || !strcmp(argv[i], "--version")) {
            sds version = cliVersion();
            printf("redis-cli %s\n", version);
            sdsfree(version);
            exit(0);
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

    /* --ldb requires --eval. */
    if (config.eval_ldb && config.eval == NULL) {
        fprintf(stderr,"Options --ldb and --ldb-sync-mode require --eval.\n");
        fprintf(stderr,"Try %s --help for more information.\n", argv[0]);
        exit(1);
    }
    return i;
}

static sds readArgFromStdin(void) {
    char buf[1024];
    sds arg = sdsempty();

    while(1) {
        int nread = read(fileno(stdin),buf,1024);

        if (nread == 0) break;
        else if (nread == -1) {
            perror("Reading from standard input");
            exit(1);
        }
        arg = sdscatlen(arg,buf,nread);
    }
    return arg;
}

static void usage(void) {
    sds version = cliVersion();
    fprintf(stderr,
"redis-cli %s\n"
"\n"
"Usage: redis-cli [OPTIONS] [cmd [arg [arg ...]]]\n"
"  -h <hostname>      Server hostname (default: 127.0.0.1).\n"
"  -p <port>          Server port (default: 6379).\n"
"  -s <socket>        Server socket (overrides hostname and port).\n"
"  -a <password>      Password to use when connecting to the server.\n"
"  -r <repeat>        Execute specified command N times.\n"
"  -i <interval>      When -r is used, waits <interval> seconds per command.\n"
"                     It is possible to specify sub-second times like -i 0.1.\n"
"  -n <db>            Database number.\n"
"  -x                 Read last argument from STDIN.\n"
"  -d <delimiter>     Multi-bulk delimiter in for raw formatting (default: \\n).\n"
"  -c                 Enable cluster mode (follow -ASK and -MOVED redirections).\n"
"  --raw              Use raw formatting for replies (default when STDOUT is\n"
"                     not a tty).\n"
"  --no-raw           Force formatted output even when STDOUT is not a tty.\n"
"  --csv              Output in CSV format.\n"
"  --stat             Print rolling stats about server: mem, clients, ...\n"
"  --latency          Enter a special mode continuously sampling latency.\n"
"  --latency-history  Like --latency but tracking latency changes over time.\n"
"                     Default time interval is 15 sec. Change it using -i.\n"
"  --latency-dist     Shows latency as a spectrum, requires xterm 256 colors.\n"
"                     Default time interval is 1 sec. Change it using -i.\n"
"  --lru-test <keys>  Simulate a cache workload with an 80-20 distribution.\n"
"  --slave            Simulate a slave showing commands received from the master.\n"
"  --rdb <filename>   Transfer an RDB dump from remote server to local file.\n"
"  --pipe             Transfer raw Redis protocol from stdin to server.\n"
"  --pipe-timeout <n> In --pipe mode, abort with error if after sending all data.\n"
"                     no reply is received within <n> seconds.\n"
"                     Default timeout: %d. Use 0 to wait forever.\n"
"  --bigkeys          Sample Redis keys looking for big keys.\n"
"  --scan             List all keys using the SCAN command.\n"
"  --pattern <pat>    Useful with --scan to specify a SCAN pattern.\n"
"  --intrinsic-latency <sec> Run a test to measure intrinsic system latency.\n"
"                     The test will run for the specified amount of seconds.\n"
"  --eval <file>      Send an EVAL command using the Lua script at <file>.\n"
"  --ldb              Used with --eval enable the Redis Lua debugger.\n"
"  --ldb-sync-mode    Like --ldb but uses the synchronous Lua debugger, in\n"
"                     this mode the server is blocked and script changes are\n"
"                     are not rolled back from the server memory.\n"
"  --help             Output this help and exit.\n"
"  --version          Output version and exit.\n"
"\n"
"Examples:\n"
"  cat /etc/passwd | redis-cli -x set mypasswd\n"
"  redis-cli get mypasswd\n"
"  redis-cli -r 100 lpush mylist x\n"
"  redis-cli -r 100 -i 1 info | grep used_memory_human:\n"
"  redis-cli --eval myscript.lua key1 key2 , arg1 arg2 arg3\n"
"  redis-cli --scan --pattern '*:12345*'\n"
"\n"
"  (Note: when using --eval the comma separates KEYS[] from ARGV[] items)\n"
"\n"
"When no command is given, redis-cli starts in interactive mode.\n"
"Type \"help\" in interactive mode for information on available commands\n"
"and settings.\n"
"\n",
        version, REDIS_CLI_DEFAULT_PIPE_TIMEOUT);
    sdsfree(version);
    exit(1);
}

/* Turn the plain C strings into Sds strings */
static char **convertToSds(int count, char** args) {
  int j;
  char **sds = zmalloc(sizeof(char*)*count);

  for(j = 0; j < count; j++)
    sds[j] = sdsnew(args[j]);

  return sds;
}

static int issueCommandRepeat(int argc, char **argv, long repeat) {
    while (1) {
        config.cluster_reissue_command = 0;
        if (cliSendCommand(argc,argv,repeat) != REDIS_OK) {
            cliConnect(1);

            /* If we still cannot send the command print error.
             * We'll try to reconnect the next time. */
            if (cliSendCommand(argc,argv,repeat) != REDIS_OK) {
                cliPrintContextError();
                return REDIS_ERR;
            }
         }
         /* Issue the command again if we got redirected in cluster mode */
         if (config.cluster_mode && config.cluster_reissue_command) {
            cliConnect(1);
         } else {
             break;
        }
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

/* Set the CLI perferences. This function is invoked when an interactive
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
    }
    sdsfree(rcfile);
}

static void repl(void) {
    sds historyfile = NULL;
    int history = 0;
    char *line;
    int argc;
    sds *argv;

    config.interactive = 1;
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(completionCallback);
    linenoiseSetHintsCallback(hintsCallback);
    linenoiseSetFreeHintsCallback(freeHintsCallback);

    /* Only use history and load the rc file when stdin is a tty. */
    if (isatty(fileno(stdin))) {
        historyfile = getDotfilePath(REDIS_CLI_HISTFILE_ENV,REDIS_CLI_HISTFILE_DEFAULT);
        if (historyfile != NULL) {
            history = 1;
            linenoiseHistoryLoad(historyfile);
        }
        cliLoadPreferences();
    }

    cliRefreshPrompt();
    while((line = linenoise(context ? config.prompt : "not connected> ")) != NULL) {
        if (line[0] != '\0') {
            argv = cliSplitArgs(line,&argc);
            if (history) linenoiseHistoryAdd(line);
            if (historyfile) linenoiseHistorySave(historyfile);

            if (argv == NULL) {
                printf("Invalid argument(s)\n");
                linenoiseFree(line);
                continue;
            } else if (argc > 0) {
                if (strcasecmp(argv[0],"quit") == 0 ||
                    strcasecmp(argv[0],"exit") == 0)
                {
                    exit(0);
                } else if (argv[0][0] == ':') {
                    cliSetPreferences(argv,argc,1);
                    continue;
                } else if (strcasecmp(argv[0],"restart") == 0) {
                    if (config.eval) {
                        config.eval_ldb = 1;
                        config.output = OUTPUT_RAW;
                        return; /* Return to evalMode to restart the session. */
                    } else {
                        printf("Use 'restart' only in Lua debugging mode.");
                    }
                } else if (argc == 3 && !strcasecmp(argv[0],"connect")) {
                    sdsfree(config.hostip);
                    config.hostip = sdsnew(argv[1]);
                    config.hostport = atoi(argv[2]);
                    cliRefreshPrompt();
                    cliConnect(1);
                } else if (argc == 1 && !strcasecmp(argv[0],"clear")) {
                    linenoiseClearScreen();
                } else {
                    long long start_time = mstime(), elapsed;
                    int repeat, skipargs = 0;

                    repeat = atoi(argv[0]);
                    if (argc > 1 && repeat) {
                        skipargs = 1;
                    } else {
                        repeat = 1;
                    }

                    issueCommandRepeat(argc-skipargs, argv+skipargs, repeat);

                    /* If our debugging session ended, show the EVAL final
                     * reply. */
                    if (config.eval_ldb_end) {
                        config.eval_ldb_end = 0;
                        cliReadReply(0);
                        printf("\n(Lua debugging session ended%s)\n\n",
                            config.eval_ldb_sync ? "" :
                            " -- dataset changes rolled back");
                    }

                    elapsed = mstime()-start_time;
                    if (elapsed >= 500) {
                        printf("(%.2fs)\n",(double)elapsed/1000);
                    }
                }
            }
            /* Free the argument vector */
            sdsfreesplitres(argv,argc);
        }
        /* linenoise() returns malloc-ed lines like readline() */
        linenoiseFree(line);
    }
    exit(0);
}

static int noninteractive(int argc, char **argv) {
    int retval = 0;
    if (config.stdinarg) {
        argv = zrealloc(argv, (argc+1)*sizeof(char*));
        argv[argc] = readArgFromStdin();
        retval = issueCommand(argc+1, argv);
    } else {
        retval = issueCommand(argc, argv);
    }
    return retval;
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
        int eval_ldb = config.eval_ldb; /* Save it, may be reverteed. */
        retval = issueCommand(argc+3-got_comma, argv2);
        if (eval_ldb) {
            if (!config.eval_ldb) {
                /* If the debugging session ended immediately, there was an
                 * error compiling the script. Show it and don't enter
                 * the REPL at all. */
                printf("Eval debugging session can't start:\n");
                cliReadReply(0);
                break; /* Return to the caller. */
            } else {
                strncpy(config.prompt,"lua debugger> ",sizeof(config.prompt));
                repl();
                /* Restart the session if repl() returned. */
                cliConnect(1);
                printf("\n");
            }
        } else {
            break; /* Return to the caller. */
        }
    }
    return retval;
}

/*------------------------------------------------------------------------------
 * Latency and latency history modes
 *--------------------------------------------------------------------------- */

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
        printf("\x1b[0G\x1b[2Kmin: %lld, max: %lld, avg: %.2f (%lld samples)",
            min, max, avg, count);
        fflush(stdout);
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
 * 'max' value. Last sample max must be 0, to mean that it olds all the
 * samples greater than the previous one, and is also the stop sentinel.
 *
 * "tot' is the total number of samples in the different buckets, so it
 * is the SUM(samples[i].conut) for i to 0 up to the max sample.
 *
 * As a side effect the function sets all the buckets count to 0. */
void showLatencyDistSamples(struct distsamples *samples, long long tot) {
    int j;

     /* We convert samples into a index inside the palette
     * proportional to the percentage a given bucket represents.
     * This way intensity of the different parts of the spectrum
     * don't change relative to the number of requests, which avoids to
     * pollute the visualization with non-latency related info. */
    printf("\033[38;5;0m"); /* Set foreground color to black. */
    for (j = 0; ; j++) {
        int coloridx =
            ceil((float) samples[j].count / tot * (spectrum_palette_size-1));
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

/* Sends SYNC and reads the number of bytes in the payload. Used both by
 * slaveMode() and getRDB(). */
unsigned long long sendSync(int fd) {
    /* To start we need to send the SYNC command and return the payload.
     * The hiredis client lib does not understand this part of the protocol
     * and we don't want to mess with its buffers, so everything is performed
     * using direct low-level I/O. */
    char buf[4096], *p;
    ssize_t nread;

    /* Send the SYNC command. */
    if (write(fd,"SYNC\r\n",6) != 6) {
        fprintf(stderr,"Error writing to master\n");
        exit(1);
    }

    /* Read $<payload>\r\n, making sure to read just up to "\n" */
    p = buf;
    while(1) {
        nread = read(fd,p,1);
        if (nread <= 0) {
            fprintf(stderr,"Error reading bulk length while SYNCing\n");
            exit(1);
        }
        if (*p == '\n' && p != buf) break;
        if (*p != '\n') p++;
    }
    *p = '\0';
    if (buf[0] == '-') {
        printf("SYNC with master failed: %s\n", buf);
        exit(1);
    }
    return strtoull(buf+1,NULL,10);
}

static void slaveMode(void) {
    int fd = context->fd;
    unsigned long long payload = sendSync(fd);
    char buf[1024];
    int original_output = config.output;

    fprintf(stderr,"SYNC with master, discarding %llu "
                   "bytes of bulk transfer...\n", payload);

    /* Discard the payload. */
    while(payload) {
        ssize_t nread;

        nread = read(fd,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"Error reading RDB payload while SYNCing\n");
            exit(1);
        }
        payload -= nread;
    }
    fprintf(stderr,"SYNC done. Logging commands from master.\n");

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
static void getRDB(void) {
    int s = context->fd;
    int fd;
    unsigned long long payload = sendSync(s);
    char buf[4096];

    fprintf(stderr,"SYNC sent to master, writing %llu bytes to '%s'\n",
        payload, config.rdb_filename);

    /* Write to file. */
    if (!strcmp(config.rdb_filename,"-")) {
        fd = STDOUT_FILENO;
    } else {
        fd = open(config.rdb_filename, O_CREAT|O_WRONLY, 0644);
        if (fd == -1) {
            fprintf(stderr, "Error opening '%s': %s\n", config.rdb_filename,
                strerror(errno));
            exit(1);
        }
    }

    while(payload) {
        ssize_t nread, nwritten;

        nread = read(s,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"I/O Error reading RDB payload from socket\n");
            exit(1);
        }
        nwritten = write(fd, buf, nread);
        if (nwritten != nread) {
            fprintf(stderr,"Error writing data to file: %s\n",
                strerror(errno));
            exit(1);
        }
        payload -= nread;
    }
    close(s); /* Close the file descriptor ASAP as fsync() may take time. */
    fsync(fd);
    fprintf(stderr,"Transfer finished with success.\n");
    exit(0);
}

/*------------------------------------------------------------------------------
 * Bulk import (pipe) mode
 *--------------------------------------------------------------------------- */

#define PIPEMODE_WRITE_LOOP_MAX_BYTES (128*1024)
static void pipeMode(void) {
    int fd = context->fd;
    long long errors = 0, replies = 0, obuf_len = 0, obuf_pos = 0;
    char ibuf[1024*16], obuf[1024*16]; /* Input and output buffers */
    char aneterr[ANET_ERR_LEN];
    redisReader *reader = redisReaderCreate();
    redisReply *reply;
    int eof = 0; /* True once we consumed all the standard input. */
    int done = 0;
    char magic[20]; /* Special reply we recognize. */
    time_t last_read_time = time(NULL);

    srand(time(NULL));

    /* Use non blocking I/O. */
    if (anetNonBlock(aneterr,fd) == ANET_ERR) {
        fprintf(stderr, "Can't set the socket in non blocking mode: %s\n",
            aneterr);
        exit(1);
    }

    /* Transfer raw protocol and read replies from the server at the same
     * time. */
    while(!done) {
        int mask = AE_READABLE;

        if (!eof || obuf_len != 0) mask |= AE_WRITABLE;
        mask = aeWait(fd,mask,1000);

        /* Handle the readable state: we can read replies from the server. */
        if (mask & AE_READABLE) {
            ssize_t nread;

            /* Read from socket and feed the hiredis reader. */
            do {
                nread = read(fd,ibuf,sizeof(ibuf));
                if (nread == -1 && errno != EAGAIN && errno != EINTR) {
                    fprintf(stderr, "Error reading from the server: %s\n",
                        strerror(errno));
                    exit(1);
                }
                if (nread > 0) {
                    redisReaderFeed(reader,ibuf,nread);
                    last_read_time = time(NULL);
                }
            } while(nread > 0);

            /* Consume replies. */
            do {
                if (redisReaderGetReply(reader,(void**)&reply) == REDIS_ERR) {
                    fprintf(stderr, "Error reading replies from server\n");
                    exit(1);
                }
                if (reply) {
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
        }

        /* Handle the writable state: we can send protocol to the server. */
        if (mask & AE_WRITABLE) {
            ssize_t loop_nwritten = 0;

            while(1) {
                /* Transfer current buffer to server. */
                if (obuf_len != 0) {
                    ssize_t nwritten = write(fd,obuf+obuf_pos,obuf_len);

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
    redisReaderFree(reader);
    printf("errors: %lld, replies: %lld\n", errors, replies);
    if (errors)
        exit(1);
    else
        exit(0);
}

/*------------------------------------------------------------------------------
 * Find big keys
 *--------------------------------------------------------------------------- */

#define TYPE_STRING 0
#define TYPE_LIST   1
#define TYPE_SET    2
#define TYPE_HASH   3
#define TYPE_ZSET   4
#define TYPE_NONE   5

static redisReply *sendScan(unsigned long long *it) {
    redisReply *reply = redisCommand(context, "SCAN %llu", *it);

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

    if(reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "Couldn't determine DBSIZE!\n");
        exit(1);
    }

    /* Grab the number of keys and free our reply */
    size = reply->integer;
    freeReplyObject(reply);

    return size;
}

static int toIntType(char *key, char *type) {
    if(!strcmp(type, "string")) {
        return TYPE_STRING;
    } else if(!strcmp(type, "list")) {
        return TYPE_LIST;
    } else if(!strcmp(type, "set")) {
        return TYPE_SET;
    } else if(!strcmp(type, "hash")) {
        return TYPE_HASH;
    } else if(!strcmp(type, "zset")) {
        return TYPE_ZSET;
    } else if(!strcmp(type, "none")) {
        return TYPE_NONE;
    } else {
        fprintf(stderr, "Unknown type '%s' for key '%s'\n", type, key);
        exit(1);
    }
}

static void getKeyTypes(redisReply *keys, int *types) {
    redisReply *reply;
    unsigned int i;

    /* Pipeline TYPE commands */
    for(i=0;i<keys->elements;i++) {
        redisAppendCommand(context, "TYPE %s", keys->element[i]->str);
    }

    /* Retrieve types */
    for(i=0;i<keys->elements;i++) {
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            fprintf(stderr, "Error getting type for key '%s' (%d: %s)\n",
                keys->element[i]->str, context->err, context->errstr);
            exit(1);
        } else if(reply->type != REDIS_REPLY_STATUS) {
            fprintf(stderr, "Invalid reply type (%d) for TYPE on key '%s'!\n",
                reply->type, keys->element[i]->str);
            exit(1);
        }

        types[i] = toIntType(keys->element[i]->str, reply->str);
        freeReplyObject(reply);
    }
}

static void getKeySizes(redisReply *keys, int *types,
                        unsigned long long *sizes)
{
    redisReply *reply;
    char *sizecmds[] = {"STRLEN","LLEN","SCARD","HLEN","ZCARD"};
    unsigned int i;

    /* Pipeline size commands */
    for(i=0;i<keys->elements;i++) {
        /* Skip keys that were deleted */
        if(types[i]==TYPE_NONE)
            continue;

        redisAppendCommand(context, "%s %s", sizecmds[types[i]],
            keys->element[i]->str);
    }

    /* Retreive sizes */
    for(i=0;i<keys->elements;i++) {
        /* Skip keys that dissapeared between SCAN and TYPE */
        if(types[i] == TYPE_NONE) {
            sizes[i] = 0;
            continue;
        }

        /* Retreive size */
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            fprintf(stderr, "Error getting size for key '%s' (%d: %s)\n",
                keys->element[i]->str, context->err, context->errstr);
            exit(1);
        } else if(reply->type != REDIS_REPLY_INTEGER) {
            /* Theoretically the key could have been removed and
             * added as a different type between TYPE and SIZE */
            fprintf(stderr,
                "Warning:  %s on '%s' failed (may have changed type)\n",
                 sizecmds[types[i]], keys->element[i]->str);
            sizes[i] = 0;
        } else {
            sizes[i] = reply->integer;
        }

        freeReplyObject(reply);
    }
}

static void findBigKeys(void) {
    unsigned long long biggest[5] = {0}, counts[5] = {0}, totalsize[5] = {0};
    unsigned long long sampled = 0, total_keys, totlen=0, *sizes=NULL, it=0;
    sds maxkeys[5] = {0};
    char *typename[] = {"string","list","set","hash","zset"};
    char *typeunit[] = {"bytes","items","members","fields","members"};
    redisReply *reply, *keys;
    unsigned int arrsize=0, i;
    int type, *types=NULL;
    double pct;

    /* Total keys pre scanning */
    total_keys = getDbSize();

    /* Status message */
    printf("\n# Scanning the entire keyspace to find biggest keys as well as\n");
    printf("# average sizes per key type.  You can use -i 0.1 to sleep 0.1 sec\n");
    printf("# per 100 SCAN commands (not usually needed).\n\n");

    /* New up sds strings to keep track of overall biggest per type */
    for(i=0;i<TYPE_NONE; i++) {
        maxkeys[i] = sdsempty();
        if(!maxkeys[i]) {
            fprintf(stderr, "Failed to allocate memory for largest key names!\n");
            exit(1);
        }
    }

    /* SCAN loop */
    do {
        /* Calculate approximate percentage completion */
        pct = 100 * (double)sampled/total_keys;

        /* Grab some keys and point to the keys array */
        reply = sendScan(&it);
        keys  = reply->element[1];

        /* Reallocate our type and size array if we need to */
        if(keys->elements > arrsize) {
            types = zrealloc(types, sizeof(int)*keys->elements);
            sizes = zrealloc(sizes, sizeof(unsigned long long)*keys->elements);

            if(!types || !sizes) {
                fprintf(stderr, "Failed to allocate storage for keys!\n");
                exit(1);
            }

            arrsize = keys->elements;
        }

        /* Retreive types and then sizes */
        getKeyTypes(keys, types);
        getKeySizes(keys, types, sizes);

        /* Now update our stats */
        for(i=0;i<keys->elements;i++) {
            if((type = types[i]) == TYPE_NONE)
                continue;

            totalsize[type] += sizes[i];
            counts[type]++;
            totlen += keys->element[i]->len;
            sampled++;

            if(biggest[type]<sizes[i]) {
                printf(
                   "[%05.2f%%] Biggest %-6s found so far '%s' with %llu %s\n",
                   pct, typename[type], keys->element[i]->str, sizes[i],
                   typeunit[type]);

                /* Keep track of biggest key name for this type */
                maxkeys[type] = sdscpy(maxkeys[type], keys->element[i]->str);
                if(!maxkeys[type]) {
                    fprintf(stderr, "Failed to allocate memory for key!\n");
                    exit(1);
                }

                /* Keep track of the biggest size for this type */
                biggest[type] = sizes[i];
            }

            /* Update overall progress */
            if(sampled % 1000000 == 0) {
                printf("[%05.2f%%] Sampled %llu keys so far\n", pct, sampled);
            }
        }

        /* Sleep if we've been directed to do so */
        if(sampled && (sampled %100) == 0 && config.interval) {
            usleep(config.interval);
        }

        freeReplyObject(reply);
    } while(it != 0);

    if(types) zfree(types);
    if(sizes) zfree(sizes);

    /* We're done */
    printf("\n-------- summary -------\n\n");

    printf("Sampled %llu keys in the keyspace!\n", sampled);
    printf("Total key length in bytes is %llu (avg len %.2f)\n\n",
       totlen, totlen ? (double)totlen/sampled : 0);

    /* Output the biggest keys we found, for types we did find */
    for(i=0;i<TYPE_NONE;i++) {
        if(sdslen(maxkeys[i])>0) {
            printf("Biggest %6s found '%s' has %llu %s\n", typename[i], maxkeys[i],
               biggest[i], typeunit[i]);
        }
    }

    printf("\n");

    for(i=0;i<TYPE_NONE;i++) {
        printf("%llu %ss with %llu %s (%05.2f%% of keys, avg size %.2f)\n",
           counts[i], typename[i], totalsize[i], typeunit[i],
           sampled ? 100 * (double)counts[i]/sampled : 0,
           counts[i] ? (double)totalsize[i]/counts[i] : 0);
    }

    /* Free sds strings containing max keys */
    for(i=0;i<TYPE_NONE;i++) {
        sdsfree(maxkeys[i]);
    }

    /* Success! */
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
void bytesToHuman(char *s, long long n) {
    double d;

    if (n < 0) {
        *s = '-';
        s++;
        n = -n;
    }
    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lldB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    }
}

static void statMode(void) {
    redisReply *reply;
    long aux, requests = 0;
    int i = 0;

    while(1) {
        char buf[64];
        int j;

        reply = reconnectingRedisCommand(context,"INFO");
        if (reply->type == REDIS_REPLY_ERROR) {
            printf("ERROR: %s\n", reply->str);
            exit(1);
        }

        if ((i++ % 20) == 0) {
            printf(
"------- data ------ --------------------- load -------------------- - child -\n"
"keys       mem      clients blocked requests            connections          \n");
        }

        /* Keys */
        aux = 0;
        for (j = 0; j < 20; j++) {
            long k;

            sprintf(buf,"db%d:keys",j);
            k = getLongInfoField(reply->str,buf);
            if (k == LONG_MIN) continue;
            aux += k;
        }
        sprintf(buf,"%ld",aux);
        printf("%-11s",buf);

        /* Used memory */
        aux = getLongInfoField(reply->str,"used_memory");
        bytesToHuman(buf,aux);
        printf("%-8s",buf);

        /* Clients */
        aux = getLongInfoField(reply->str,"connected_clients");
        sprintf(buf,"%ld",aux);
        printf(" %-8s",buf);

        /* Blocked (BLPOPPING) Clients */
        aux = getLongInfoField(reply->str,"blocked_clients");
        sprintf(buf,"%ld",aux);
        printf("%-8s",buf);

        /* Requets */
        aux = getLongInfoField(reply->str,"total_commands_processed");
        sprintf(buf,"%ld (+%ld)",aux,requests == 0 ? 0 : aux-requests);
        printf("%-19s",buf);
        requests = aux;

        /* Connections */
        aux = getLongInfoField(reply->str,"total_connections_received");
        sprintf(buf,"%ld",aux);
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

    do {
        if (config.pattern)
            reply = redisCommand(context,"SCAN %llu MATCH %s",
                cur,config.pattern);
        else
            reply = redisCommand(context,"SCAN %llu",cur);
        if (reply == NULL) {
            printf("I/O error\n");
            exit(1);
        } else if (reply->type == REDIS_REPLY_ERROR) {
            printf("ERROR: %s\n", reply->str);
            exit(1);
        } else {
            unsigned int j;

            cur = strtoull(reply->element[0]->str,NULL,10);
            for (j = 0; j < reply->element[1]->elements; j++)
                printf("%s\n", reply->element[1]->element[j]->str);
        }
        freeReplyObject(reply);
    } while(cur != 0);

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
    snprintf(buf, buflen, "lru:%lld\n",
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
        while(mstime() - start_cycle < 1000) {
            /* Write cycle. */
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++) {
                LRUTestGenKey(key,sizeof(key));
                redisAppendCommand(context, "SET %s val",key);
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
                            printf("%s\n", reply->str);
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
 * Intrisic latency mode.
 *
 * Measure max latency of a running process that does not result from
 * syscalls. Basically this software should provide an hint about how much
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

static void intrinsicLatencyModeStop(int s) {
    UNUSED(s);
    force_cancel_loop = 1;
}

static void intrinsicLatencyMode(void) {
    long long test_end, run_time, max_latency = 0, runs = 0;

    run_time = config.intrinsic_latency_duration*1000000;
    test_end = ustime() + run_time;
    signal(SIGINT, intrinsicLatencyModeStop);

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

/*------------------------------------------------------------------------------
 * Program main()
 *--------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    int firstarg;

    config.hostip = sdsnew("127.0.0.1");
    config.hostport = 6379;
    config.hostsocket = NULL;
    config.repeat = 1;
    config.interval = 0;
    config.dbnum = 0;
    config.interactive = 0;
    config.shutdown = 0;
    config.monitor_mode = 0;
    config.pubsub_mode = 0;
    config.latency_mode = 0;
    config.latency_dist_mode = 0;
    config.latency_history = 0;
    config.lru_test_mode = 0;
    config.lru_test_sample_size = 0;
    config.cluster_mode = 0;
    config.slave_mode = 0;
    config.getrdb_mode = 0;
    config.stat_mode = 0;
    config.scan_mode = 0;
    config.intrinsic_latency_mode = 0;
    config.pattern = NULL;
    config.rdb_filename = NULL;
    config.pipe_mode = 0;
    config.pipe_timeout = REDIS_CLI_DEFAULT_PIPE_TIMEOUT;
    config.bigkeys = 0;
    config.stdinarg = 0;
    config.auth = NULL;
    config.eval = NULL;
    config.eval_ldb = 0;
    config.eval_ldb_end = 0;
    config.eval_ldb_sync = 0;
    config.enable_ldb_on_eval = 0;
    config.last_cmd_type = -1;

    pref.hints = 1;

    spectrum_palette = spectrum_palette_color;
    spectrum_palette_size = spectrum_palette_color_size;

    if (!isatty(fileno(stdout)) && (getenv("FAKETTY") == NULL))
        config.output = OUTPUT_RAW;
    else
        config.output = OUTPUT_STANDARD;
    config.mb_delim = sdsnew("\n");

    firstarg = parseOptions(argc,argv);
    argc -= firstarg;
    argv += firstarg;

    /* Initialize the help and, if possible, use the COMMAND command in order
     * to retrieve missing entries. */
    cliInitHelp();
    cliIntegrateHelp();

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
        slaveMode();
    }

    /* Get RDB mode. */
    if (config.getrdb_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        getRDB();
    }

    /* Pipe mode */
    if (config.pipe_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        pipeMode();
    }

    /* Find big keys */
    if (config.bigkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findBigKeys();
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

    /* Start interactive mode when no command is provided */
    if (argc == 0 && !config.eval) {
        /* Ignore SIGPIPE in interactive mode to force a reconnect */
        signal(SIGPIPE, SIG_IGN);

        /* Note that in repl mode we don't abort on connection error.
         * A new attempt will be performed for every command send. */
        cliConnect(0);
        repl();
    }

    /* Otherwise, we have some arguments to execute */
    if (cliConnect(0) != REDIS_OK) exit(1);
    if (config.eval) {
        return evalMode(argc,argv);
    } else {
        return noninteractive(argc,convertToSds(argc,argv));
    }
}
