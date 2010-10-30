/* Redis CLI (command line interface)
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#include "anet.h"
#include "sds.h"
#include "adlist.h"
#include "zmalloc.h"
#include "linenoise.h"

#define REDIS_NOTUSED(V) ((void) V)

static struct config {
    char *hostip;
    int hostport;
    long repeat;
    int dbnum;
    int interactive;
    int shutdown;
    int monitor_mode;
    int pubsub_mode;
    int raw_output; /* output mode per command */
    int tty; /* flag for default output format */
    int stdinarg; /* get last arg from stdin. (-x option) */
    char mb_sep;
    char *auth;
    char *historyfile;
} config;

static int cliReadReply(int fd, sds *response);
static void usage();

/* Connect to the client. If force is not zero the connection is performed
 * even if there is already a connected socket. */
static int cliConnect(int force) {
    char err[ANET_ERR_LEN];
    static int fd = ANET_ERR;

    if (fd == ANET_ERR || force) {
        if (force) close(fd);
        fd = anetTcpConnect(err,config.hostip,config.hostport);
        if (fd == ANET_ERR) {
            fprintf(stderr, "Could not connect to Redis at %s:%d: %s", config.hostip, config.hostport, err);
            return -1;
        }
        anetTcpNoDelay(NULL,fd);
    }
    return fd;
}

static sds cliReadLine(int fd) {
    sds line = sdsempty();

    while(1) {
        char c;
        ssize_t ret;

        ret = read(fd,&c,1);
        if (ret <= 0) {
            sdsfree(line);
            return NULL;
        } else if ((ret == 0) || (c == '\n')) {
            break;
        } else {
            line = sdscatlen(line,&c,1);
        }
    }
    return sdstrim(line,"\r\n");
}

static int cliReadSingleLineReply(int fd, int quiet, sds *response) {
    sds reply = cliReadLine(fd);
    *response = sdscat(*response, reply);

    if (reply == NULL) return 1;
    if (!quiet)
        printf("%s", reply);
    sdsfree(reply);
    return 0;
}

static void printStringRepr(char *s, int len) {
    printf("\"");
    while(len--) {
        switch(*s) {
        case '\\':
        case '"':
            printf("\\%c",*s);
            break;
        case '\n': printf("\\n"); break;
        case '\r': printf("\\r"); break;
        case '\t': printf("\\t"); break;
        case '\a': printf("\\a"); break;
        case '\b': printf("\\b"); break;
        default:
            if (isprint(*s))
                printf("%c",*s);
            else
                printf("\\x%02x",(unsigned char)*s);
            break;
        }
        s++;
    }
    printf("\"");
}

static int cliReadBulkReply(int fd, sds *response) {
    sds replylen = cliReadLine(fd);

    char *reply, crlf[2];
    int bulklen;

    if (replylen == NULL) return 1;
    bulklen = atoi(replylen);
    if (bulklen == -1) {
        sdsfree(replylen);
        printf("(nil)\n");
        return 0;
    }
    reply = zmalloc(bulklen);
    anetRead(fd,reply,bulklen);
    anetRead(fd,crlf,2);
    if (config.raw_output || !config.tty) {
        if (bulklen && fwrite(reply,bulklen,1,stdout) == 0) {
            zfree(reply);
            return 1;
        }
    } else {
        /* If you are producing output for the standard output we want
         * a more interesting output with quoted characters and so forth */
        printStringRepr(reply,bulklen);
        *response = sdscat(*response, reply);
    }
    zfree(reply);
    return 0;
}

static int cliReadMultiBulkReply(int fd, sds *response) {
    sds replylen = cliReadLine(fd);
    int elements, c = 1;
    int retval = 0;

    if (replylen == NULL) return 1;
    elements = atoi(replylen);
    if (elements == -1) {
        sdsfree(replylen);
        printf("(nil)\n");
        return 0;
    }
    if (elements == 0) {
        printf("(empty list or set)\n");
    }
    while(elements--) {
        if (config.tty) printf("%d. ", c);
        if (cliReadReply(fd, response)) retval = 1;
        if (elements) {
            printf("%c",config.mb_sep);
            *response = sdscat(*response, " ");
        }
        c++;
    }
    return retval;
}

static int cliReadReply(int fd, sds *response) {
    char type;
    int nread;
    sds status;
    int retval;

    if ((nread = anetRead(fd,&type,1)) <= 0) {
        if (config.shutdown) return 0;
        if (config.interactive &&
            (nread == 0 || (nread == -1 && errno == ECONNRESET)))
        {
            return ECONNRESET;
        } else {
            printf("I/O error while reading from socket: %s",strerror(errno));
            exit(1);
        }
    }

    status = sdsempty();
    switch(type) {
    case '-':
        if (config.tty) printf("(error) ");
        cliReadSingleLineReply(fd,0,&status);
        retval = 1;
        break;
    case '+':
        retval = cliReadSingleLineReply(fd,0,&status);
        break;
    case ':':
        if (config.tty) printf("(integer) ");
        retval = cliReadSingleLineReply(fd,0,response);
        break;
    case '$':
        retval = cliReadBulkReply(fd, response);
        break;
    case '*':
        retval = cliReadMultiBulkReply(fd, response);
        break;
    default:
        printf("protocol error, got '%c' as reply type byte", type);
        retval = 1;
        break;
    }
    sdsfree(status);
    return retval;
}

static int selectDb(int fd) {
    int retval;
    sds cmd;
    char type;

    if (config.dbnum == 0)
        return 0;

    cmd = sdsempty();
    cmd = sdscatprintf(cmd,"SELECT %d\r\n",config.dbnum);
    anetWrite(fd,cmd,sdslen(cmd));
    anetRead(fd,&type,1);
    if (type <= 0 || type != '+') return 1;
    sds response = sdsempty();
    retval = cliReadSingleLineReply(fd,1,&response);
    sdsfree(response);
    if (retval) {
        return retval;
    }
    return 0;
}

static void showInteractiveHelp(void) {
    printf(
    "\n"
    "Welcome to redis-cli " REDIS_VERSION "!\n"
    "Just type any valid Redis command to see a pretty printed output.\n"
    "\n"
    "It is possible to quote strings, like in:\n"
    "  set \"my key\" \"some string \\xff\\n\"\n"
    "\n"
    "You can find a list of valid Redis commands at\n"
    "  http://code.google.com/p/redis/wiki/CommandReference\n"
    "\n"
    "Note: redis-cli supports line editing, use up/down arrows for history."
    "\n\n");
}

static int cliSendCommand(int argc, char **argv, int repeat, sds *response) {
    char *command = argv[0];
    int fd, j, retval = 0;
    sds cmd;

    config.raw_output = !strcasecmp(command,"info");
    if (!strcasecmp(command,"help")) {
        showInteractiveHelp();
        return 0;
    }
    if (!strcasecmp(command,"shutdown")) config.shutdown = 1;
    if (!strcasecmp(command,"monitor")) config.monitor_mode = 1;
    if (!strcasecmp(command,"subscribe") ||
        !strcasecmp(command,"psubscribe")) config.pubsub_mode = 1;
    if ((fd = cliConnect(0)) == -1) return 1;

    /* Select db number */
    retval = selectDb(fd);
    if (retval) {
        fprintf(stderr,"Error setting DB num\n");
        return 1;
    }

    /* Build the command to send */
    cmd = sdscatprintf(sdsempty(),"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        cmd = sdscatprintf(cmd,"$%lu\r\n",
            (unsigned long)sdslen(argv[j]));
        cmd = sdscatlen(cmd,argv[j],sdslen(argv[j]));
        cmd = sdscatlen(cmd,"\r\n",2);
    }

    while(repeat--) {
        anetWrite(fd,cmd,sdslen(cmd));
        while (config.monitor_mode) {
            if (cliReadSingleLineReply(fd,0,response)) exit(1);
            printf("\n");
        }

        if (config.pubsub_mode) {
            printf("Reading messages... (press Ctrl-c to quit)\n");
            while (1) {
                cliReadReply(fd, response);
                printf("\n\n");
            }
        }

        retval = cliReadReply(fd, response);
        if (!config.raw_output && config.tty) printf("\n");
        if (retval) return retval;
    }
    return 0;
}

static int parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;

        if (!strcmp(argv[i],"-h") && !lastarg) {
            char *ip = zmalloc(32);
            if (anetResolve(NULL,argv[i+1],ip) == ANET_ERR) {
                printf("Can't resolve %s\n", argv[i]);
                exit(1);
            }
            config.hostip = ip;
            i++;
        } else if (!strcmp(argv[i],"-h") && lastarg) {
            usage();
        } else if (!strcmp(argv[i],"-x")) {
            config.stdinarg = 1;
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.hostport = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-r") && !lastarg) {
            config.repeat = strtoll(argv[i+1],NULL,10);
            i++;
        } else if (!strcmp(argv[i],"-n") && !lastarg) {
            config.dbnum = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-a") && !lastarg) {
            config.auth = argv[i+1];
            i++;
        } else if (!strcmp(argv[i],"-i")) {
            fprintf(stderr,
"Starting interactive mode using -i is deprecated. Interactive mode is started\n"
"by default when redis-cli is executed without a command to execute.\n"
            );
        } else if (!strcmp(argv[i],"-c")) {
            fprintf(stderr,
"Reading last argument from standard input using -c is deprecated.\n"
"When standard input is connected to a pipe or regular file, it is\n"
"automatically used as last argument.\n"
            );
        } else if (!strcmp(argv[i],"-v")) {
            printf("redis-cli shipped with Redis version %s\n", REDIS_VERSION);
            exit(0);
        } else {
            break;
        }
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

static void usage() {
    fprintf(stderr, "usage: redis-cli [-iv] [-h host] [-p port] [-a authpw] [-r repeat_times] [-n db_num] cmd arg1 arg2 arg3 ... argN\n");
    fprintf(stderr, "usage: echo \"argN\" | redis-cli -x [options] cmd arg1 arg2 ... arg(N-1)\n\n");
    fprintf(stderr, "example: cat /etc/passwd | redis-cli -x set my_passwd\n");
    fprintf(stderr, "example: redis-cli get my_passwd\n");
    fprintf(stderr, "example: redis-cli -r 100 lpush mylist x\n");
    fprintf(stderr, "\nRun in interactive mode: redis-cli -i or just don't pass any command\n");
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

#define LINE_BUFLEN 4096
#define MEMORIZED_BUFLEN 1024
static void repl() {
    int argc, j;
    char *line;
    sds *argv;
    sds *memorized;
    int memorizedpos;
    int memorizing;

    memorizing = 1;
    memorizedpos = 0;
    memorized = zmalloc(sizeof(sds) * MEMORIZED_BUFLEN);
    config.interactive = 1;
    while((line = linenoise("redis> ")) != NULL) {
        if (line[0] != '\0') {
            argv = sdssplitargs(line,&argc);
            linenoiseHistoryAdd(line);
            if (config.historyfile) linenoiseHistorySave(config.historyfile);
            if (argv == NULL) {
                printf("Invalid argument(s)\n");
                continue;
            } else if (argc > 0) {
                if (strcasecmp(argv[0],"quit") == 0 ||
                    strcasecmp(argv[0],"exit") == 0)
                {
                    exit(0);
                } else {
                    int err;
                    sds response = sdsempty();

                    for (j = 0; j < argc; ++j) {
                        if (argv[j][0] == '$') {
                            sds number = sdsrange(argv[j], 1, sdslen(argv[j]) - 1);
                            if (sdsisdigit(number)) {
                                int n = atoi(number);
                                if (n < memorizedpos) {
                                    sds prev = argv[j];
                                    argv[j] = sdscat(sdsempty(), memorized[n]); // copying the sds, argvs will get released
                                    sdsfree(prev);
                                } else {
                                    printf("Warning: $%d was not set (yet). To escape use \\$%d.\n", n, n);
                                }
                            }
                        }
                        if (argv[j][0] == '\\' && argv[j][1] == '$') {
                            sds prev = argv[j];
                            argv[j] = sdsrange(prev, 1, -1);
                        }
                    }

                    if ((err = cliSendCommand(argc, argv, 1, &response)) != 0) {
                        if (err == ECONNRESET) {
                            printf("Reconnecting... ");
                            fflush(stdout);
                            if (cliConnect(1) == -1) exit(1);
                            printf("OK\n");
                            cliSendCommand(argc,argv,1, &response);
                        }
                    }

                    if (memorizing == 0) {
                        sdsfree(response);
                    } else if (sdslen(response) > 0) {
                        memorized[memorizedpos] = response;
                        printf("$%d = '%s'\n", memorizedpos, memorized[memorizedpos]);
                        fflush(stdout);
                        memorizedpos++;
                        if (memorizedpos % MEMORIZED_BUFLEN == 0) {
                            sds* newmemorized = zrealloc(memorized, sizeof(sds) * (memorizedpos + MEMORIZED_BUFLEN) + 1);
                            if (newmemorized == NULL) {
                                printf("Warning: insufficient memory to continue memorizing response");
                                memorizing = 0;
                            } else {
                                memorized = newmemorized;
                            }
                        }
                    }
                }
            }
            /* Free the argument vector */
            for (j = 0; j < argc; j++)
                sdsfree(argv[j]);
            zfree(argv);
        }
        /* linenoise() returns malloc-ed lines like readline() */
        free(line);
    }
    for (j = 0; j < memorizedpos-1; j++)
        sdsfree(memorized[j]);
    zfree(memorized);
    exit(0);
}

static int noninteractive(int argc, char **argv) {
    int retval = 0;
    if (config.stdinarg) {
        argv = zrealloc(argv, (argc+1)*sizeof(char*));
        argv[argc] = readArgFromStdin();
        sds response = sdsempty();
        retval = cliSendCommand(argc+1, argv, config.repeat, &response);
    } else {
        /* stdin is probably a tty, can be tested with S_ISCHR(s.st_mode) */
        sds response = sdsempty();
        retval = cliSendCommand(argc, argv, config.repeat, &response);
    }
    return retval;
}

int main(int argc, char **argv) {
    int firstarg;

    config.hostip = "127.0.0.1";
    config.hostport = 6379;
    config.repeat = 1;
    config.dbnum = 0;
    config.interactive = 0;
    config.shutdown = 0;
    config.monitor_mode = 0;
    config.pubsub_mode = 0;
    config.raw_output = 0;
    config.stdinarg = 0;
    config.auth = NULL;
    config.historyfile = NULL;
    config.tty = isatty(fileno(stdout)) || (getenv("FAKETTY") != NULL);
    config.mb_sep = '\n';

    if (getenv("HOME") != NULL) {
        config.historyfile = malloc(256);
        snprintf(config.historyfile,256,"%s/.rediscli_history",getenv("HOME"));
        linenoiseHistoryLoad(config.historyfile);
    }

    firstarg = parseOptions(argc,argv);
    argc -= firstarg;
    argv += firstarg;

    if (config.auth != NULL) {
        char *authargv[2];
        int dbnum = config.dbnum;

        /* We need to save the real configured database number and set it to
         * zero here, otherwise cliSendCommand() will try to perform the
         * SELECT command before the authentication, and it will fail. */
        config.dbnum = 0;
        authargv[0] = "AUTH";
        authargv[1] = config.auth;
        sds response = sdsempty();
        cliSendCommand(2, convertToSds(2, authargv), 1, &response);
        config.dbnum = dbnum; /* restore the right DB number */
    }

    /* Start interactive mode when no command is provided */
    if (argc == 0) repl();
    /* Otherwise, we have some arguments to execute */
    return noninteractive(argc,convertToSds(argc,argv));
}
