/* Redis benchmark utility.
 *
 * Copyright (c) 2006-2009, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>
#include <assert.h>

#include "ae.h"
#include "anet.h"
#include "sds.h"
#include "adlist.h"
#include "zmalloc.h"

#define REPLY_INT 0
#define REPLY_RETCODE 1
#define REPLY_BULK 2
#define REPLY_MBULK 3

#define CLIENT_CONNECTING 0
#define CLIENT_SENDQUERY 1
#define CLIENT_READREPLY 2

#define MAX_LATENCY 5000

#define REDIS_NOTUSED(V) ((void) V)

static struct config {
    int debug;
    int numclients;
    int requests;
    int liveclients;
    int donerequests;
    int keysize;
    int datasize;
    int randomkeys;
    int randomkeys_keyspacelen;
    aeEventLoop *el;
    char *hostip;
    int hostport;
    int keepalive;
    long long start;
    long long totlatency;
    int *latency;
    list *clients;
    int quiet;
    int loop;
    int idlemode;
} config;

typedef struct _client {
    int state;
    int fd;
    sds obuf;
    sds ibuf;
    int mbulk;          /* Number of elements in an mbulk reply */
    int readlen;        /* readlen == -1 means read a single line */
    int totreceived;
    unsigned int written;        /* bytes of 'obuf' already written */
    int replytype;
    long long start;    /* start time in milliseconds */
} *client;

/* Prototypes */
static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void createMissingClients(client c);

/* Implementation */
static long long mstime(void) {
    struct timeval tv;
    long long mst;

    gettimeofday(&tv, NULL);
    mst = ((long)tv.tv_sec)*1000;
    mst += tv.tv_usec/1000;
    return mst;
}

static void freeClient(client c) {
    listNode *ln;

    aeDeleteFileEvent(config.el,c->fd,AE_WRITABLE);
    aeDeleteFileEvent(config.el,c->fd,AE_READABLE);
    sdsfree(c->ibuf);
    sdsfree(c->obuf);
    close(c->fd);
    zfree(c);
    config.liveclients--;
    ln = listSearchKey(config.clients,c);
    assert(ln != NULL);
    listDelNode(config.clients,ln);
}

static void freeAllClients(void) {
    listNode *ln = config.clients->head, *next;

    while(ln) {
        next = ln->next;
        freeClient(ln->value);
        ln = next;
    }
}

static void resetClient(client c) {
    aeDeleteFileEvent(config.el,c->fd,AE_WRITABLE);
    aeDeleteFileEvent(config.el,c->fd,AE_READABLE);
    aeCreateFileEvent(config.el,c->fd, AE_WRITABLE,writeHandler,c);
    sdsfree(c->ibuf);
    c->ibuf = sdsempty();
    c->readlen = (c->replytype == REPLY_BULK ||
                  c->replytype == REPLY_MBULK) ? -1 : 0;
    c->mbulk = -1;
    c->written = 0;
    c->totreceived = 0;
    c->state = CLIENT_SENDQUERY;
    c->start = mstime();
    createMissingClients(c);
}

static void randomizeClientKey(client c) {
    char *p;
    char buf[32];
    long r;

    p = strstr(c->obuf, "_rand");
    if (!p) return;
    p += 5;
    r = random() % config.randomkeys_keyspacelen;
    sprintf(buf,"%ld",r);
    memcpy(p,buf,strlen(buf));
}

static void prepareClientForReply(client c, int type) {
    if (type == REPLY_BULK) {
        c->replytype = REPLY_BULK;
        c->readlen = -1;
    } else if (type == REPLY_MBULK) {
        c->replytype = REPLY_MBULK;
        c->readlen = -1;
        c->mbulk = -1;
    } else {
        c->replytype = type;
        c->readlen = 0;
    }
}

static void clientDone(client c) {
    static int last_tot_received = 1;

    long long latency;
    config.donerequests ++;
    latency = mstime() - c->start;
    if (latency > MAX_LATENCY) latency = MAX_LATENCY;
    config.latency[latency]++;

    if (config.debug && last_tot_received != c->totreceived) {
        printf("Tot bytes received: %d\n", c->totreceived);
        last_tot_received = c->totreceived;
    }
    if (config.donerequests == config.requests) {
        freeClient(c);
        aeStop(config.el);
        return;
    }
    if (config.keepalive) {
        resetClient(c);
        if (config.randomkeys) randomizeClientKey(c);
    } else {
        config.liveclients--;
        createMissingClients(c);
        config.liveclients++;
        freeClient(c);
    }
}

static void readHandler(aeEventLoop *el, int fd, void *privdata, int mask)
{
    char buf[1024];
    int nread;
    client c = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(fd);
    REDIS_NOTUSED(mask);

    nread = read(c->fd, buf, 1024);
    if (nread == -1) {
        fprintf(stderr, "Reading from socket: %s\n", strerror(errno));
        freeClient(c);
        return;
    }
    if (nread == 0) {
        fprintf(stderr, "EOF from client\n");
        freeClient(c);
        return;
    }
    c->totreceived += nread;
    c->ibuf = sdscatlen(c->ibuf,buf,nread);

processdata:
    /* Are we waiting for the first line of the command of for  sdf 
     * count in bulk or multi bulk operations? */
    if (c->replytype == REPLY_INT ||
        c->replytype == REPLY_RETCODE ||
        (c->replytype == REPLY_BULK && c->readlen == -1) ||
        (c->replytype == REPLY_MBULK && c->readlen == -1) ||
        (c->replytype == REPLY_MBULK && c->mbulk == -1)) {
        char *p;

        /* Check if the first line is complete. This is only true if
         * there is a newline inside the buffer. */
        if ((p = strchr(c->ibuf,'\n')) != NULL) {
            if (c->replytype == REPLY_BULK ||
                (c->replytype == REPLY_MBULK && c->mbulk != -1))
            {
                /* Read the count of a bulk reply (being it a single bulk or
                 * a multi bulk reply). "$<count>" for the protocol spec. */
                *p = '\0';
                *(p-1) = '\0';
                c->readlen = atoi(c->ibuf+1)+2;
                // printf("BULK ATOI: %s\n", c->ibuf+1);
                /* Handle null bulk reply "$-1" */
                if (c->readlen-2 == -1) {
                    clientDone(c);
                    return;
                }
                /* Leave all the rest in the input buffer */
                c->ibuf = sdsrange(c->ibuf,(p-c->ibuf)+1,-1);
                /* fall through to reach the point where the code will try
                 * to check if the bulk reply is complete. */
            } else if (c->replytype == REPLY_MBULK && c->mbulk == -1) {
                /* Read the count of a multi bulk reply. That is, how many
                 * bulk replies we have to read next. "*<count>" protocol. */
                *p = '\0';
                *(p-1) = '\0';
                c->mbulk = atoi(c->ibuf+1);
                /* Handle null bulk reply "*-1" */
                if (c->mbulk == -1) {
                    clientDone(c);
                    return;
                }
                // printf("%p) %d elements list\n", c, c->mbulk);
                /* Leave all the rest in the input buffer */
                c->ibuf = sdsrange(c->ibuf,(p-c->ibuf)+1,-1);
                goto processdata;
            } else {
                c->ibuf = sdstrim(c->ibuf,"\r\n");
                clientDone(c);
                return;
            }
        }
    }
    /* bulk read, did we read everything? */
    if (((c->replytype == REPLY_MBULK && c->mbulk != -1) || 
         (c->replytype == REPLY_BULK)) && c->readlen != -1 &&
          (unsigned)c->readlen <= sdslen(c->ibuf))
    {
        // printf("BULKSTATUS mbulk:%d readlen:%d sdslen:%d\n",
        //    c->mbulk,c->readlen,sdslen(c->ibuf));
        if (c->replytype == REPLY_BULK) {
            clientDone(c);
        } else if (c->replytype == REPLY_MBULK) {
            // printf("%p) %d (%d)) ",c, c->mbulk, c->readlen);
            // fwrite(c->ibuf,c->readlen,1,stdout);
            // printf("\n");
            if (--c->mbulk == 0) {
                clientDone(c);
            } else {
                c->ibuf = sdsrange(c->ibuf,c->readlen,-1);
                c->readlen = -1;
                goto processdata;
            }
        }
    }
}

static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask)
{
    client c = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(fd);
    REDIS_NOTUSED(mask);

    if (c->state == CLIENT_CONNECTING) {
        c->state = CLIENT_SENDQUERY;
        c->start = mstime();
    }
    if (sdslen(c->obuf) > c->written) {
        void *ptr = c->obuf+c->written;
        int len = sdslen(c->obuf) - c->written;
        int nwritten = write(c->fd, ptr, len);
        if (nwritten == -1) {
            fprintf(stderr, "Writing to socket: %s\n", strerror(errno));
            freeClient(c);
            return;
        }
        c->written += nwritten;
        if (sdslen(c->obuf) == c->written) {
            aeDeleteFileEvent(config.el,c->fd,AE_WRITABLE);
            aeCreateFileEvent(config.el,c->fd,AE_READABLE,readHandler,c);
            c->state = CLIENT_READREPLY;
        }
    }
}

static client createClient(void) {
    client c = zmalloc(sizeof(struct _client));
    char err[ANET_ERR_LEN];

    c->fd = anetTcpNonBlockConnect(err,config.hostip,config.hostport);
    if (c->fd == ANET_ERR) {
        zfree(c);
        fprintf(stderr,"Connect: %s\n",err);
        return NULL;
    }
    anetTcpNoDelay(NULL,c->fd);
    c->obuf = sdsempty();
    c->ibuf = sdsempty();
    c->mbulk = -1;
    c->readlen = 0;
    c->written = 0;
    c->totreceived = 0;
    c->state = CLIENT_CONNECTING;
    aeCreateFileEvent(config.el, c->fd, AE_WRITABLE, writeHandler, c);
    config.liveclients++;
    listAddNodeTail(config.clients,c);
    return c;
}

static void createMissingClients(client c) {
    while(config.liveclients < config.numclients) {
        client new = createClient();
        if (!new) continue;
        sdsfree(new->obuf);
        new->obuf = sdsdup(c->obuf);
        if (config.randomkeys) randomizeClientKey(c);
        prepareClientForReply(new,c->replytype);
    }
}

static void showLatencyReport(char *title) {
    int j, seen = 0;
    float perc, reqpersec;

    reqpersec = (float)config.donerequests/((float)config.totlatency/1000);
    if (!config.quiet) {
        printf("====== %s ======\n", title);
        printf("  %d requests completed in %.2f seconds\n", config.donerequests,
            (float)config.totlatency/1000);
        printf("  %d parallel clients\n", config.numclients);
        printf("  %d bytes payload\n", config.datasize);
        printf("  keep alive: %d\n", config.keepalive);
        printf("\n");
        for (j = 0; j <= MAX_LATENCY; j++) {
            if (config.latency[j]) {
                seen += config.latency[j];
                perc = ((float)seen*100)/config.donerequests;
                printf("%.2f%% <= %d milliseconds\n", perc, j);
            }
        }
        printf("%.2f requests per second\n\n", reqpersec);
    } else {
        printf("%s: %.2f requests per second\n", title, reqpersec);
    }
}

static void prepareForBenchmark(void)
{
    memset(config.latency,0,sizeof(int)*(MAX_LATENCY+1));
    config.start = mstime();
    config.donerequests = 0;
}

static void endBenchmark(char *title) {
    config.totlatency = mstime()-config.start;
    showLatencyReport(title);
    freeAllClients();
}

void parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;
        
        if (!strcmp(argv[i],"-c") && !lastarg) {
            config.numclients = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-n") && !lastarg) {
            config.requests = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-k") && !lastarg) {
            config.keepalive = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-h") && !lastarg) {
            char *ip = zmalloc(32);
            if (anetResolve(NULL,argv[i+1],ip) == ANET_ERR) {
                printf("Can't resolve %s\n", argv[i]);
                exit(1);
            }
            config.hostip = ip;
            i++;
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.hostport = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-d") && !lastarg) {
            config.datasize = atoi(argv[i+1]);
            i++;
            if (config.datasize < 1) config.datasize=1;
            if (config.datasize > 1024*1024) config.datasize = 1024*1024;
        } else if (!strcmp(argv[i],"-r") && !lastarg) {
            config.randomkeys = 1;
            config.randomkeys_keyspacelen = atoi(argv[i+1]);
            if (config.randomkeys_keyspacelen < 0)
                config.randomkeys_keyspacelen = 0;
            i++;
        } else if (!strcmp(argv[i],"-q")) {
            config.quiet = 1;
        } else if (!strcmp(argv[i],"-l")) {
            config.loop = 1;
        } else if (!strcmp(argv[i],"-D")) {
            config.debug = 1;
        } else if (!strcmp(argv[i],"-I")) {
            config.idlemode = 1;
        } else {
            printf("Wrong option '%s' or option argument missing\n\n",argv[i]);
            printf("Usage: redis-benchmark [-h <host>] [-p <port>] [-c <clients>] [-n <requests]> [-k <boolean>]\n\n");
            printf(" -h <hostname>      Server hostname (default 127.0.0.1)\n");
            printf(" -p <hostname>      Server port (default 6379)\n");
            printf(" -c <clients>       Number of parallel connections (default 50)\n");
            printf(" -n <requests>      Total number of requests (default 10000)\n");
            printf(" -d <size>          Data size of SET/GET value in bytes (default 2)\n");
            printf(" -k <boolean>       1=keep alive 0=reconnect (default 1)\n");
            printf(" -r <keyspacelen>   Use random keys for SET/GET/INCR\n");
            printf("  Using this option the benchmark will get/set keys\n");
            printf("  in the form mykey_rand000000012456 instead of constant\n");
            printf("  keys, the <keyspacelen> argument determines the max\n");
            printf("  number of values for the random number. For instance\n");
            printf("  if set to 10 only rand000000000000 - rand000000000009\n");
            printf("  range will be allowed.\n");
            printf(" -q                 Quiet. Just show query/sec values\n");
            printf(" -l                 Loop. Run the tests forever\n");
            printf(" -I                 Idle mode. Just open N idle connections and wait.\n");
            printf(" -D                 Debug mode. more verbose.\n");
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    client c;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    config.debug = 0;
    config.numclients = 50;
    config.requests = 10000;
    config.liveclients = 0;
    config.el = aeCreateEventLoop();
    config.keepalive = 1;
    config.donerequests = 0;
    config.datasize = 3;
    config.randomkeys = 0;
    config.randomkeys_keyspacelen = 0;
    config.quiet = 0;
    config.loop = 0;
    config.idlemode = 0;
    config.latency = NULL;
    config.clients = listCreate();
    config.latency = zmalloc(sizeof(int)*(MAX_LATENCY+1));

    config.hostip = "127.0.0.1";
    config.hostport = 6379;

    parseOptions(argc,argv);

    if (config.keepalive == 0) {
        printf("WARNING: keepalive disabled, you probably need 'echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse' for Linux and 'sudo sysctl -w net.inet.tcp.msl=1000' for Mac OS X in order to use a lot of clients/requests\n");
    }

    if (config.idlemode) {
        printf("Creating %d idle connections and waiting forever (Ctrl+C when done)\n", config.numclients);
        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdsempty();
        prepareClientForReply(c,REPLY_RETCODE); /* will never receive it */
        createMissingClients(c);
        aeMain(config.el);
        /* and will wait for every */
    }

    do {
        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscatprintf(c->obuf,"SET foo_rand000000000000 %d\r\n",config.datasize);
        {
            char *data = zmalloc(config.datasize+2);
            memset(data,'x',config.datasize);
            data[config.datasize] = '\r';
            data[config.datasize+1] = '\n';
            c->obuf = sdscatlen(c->obuf,data,config.datasize+2);
        }
        prepareClientForReply(c,REPLY_RETCODE);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("SET");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"GET foo_rand000000000000\r\n");
        prepareClientForReply(c,REPLY_BULK);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("GET");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"INCR counter_rand000000000000\r\n");
        prepareClientForReply(c,REPLY_INT);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("INCR");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"LPUSH mylist 3\r\nbar\r\n");
        prepareClientForReply(c,REPLY_INT);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("LPUSH");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"LPOP mylist\r\n");
        prepareClientForReply(c,REPLY_BULK);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("LPOP");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"PING\r\n");
        prepareClientForReply(c,REPLY_RETCODE);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("PING");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"LPUSH mylist 3\r\nbar\r\n");
        prepareClientForReply(c,REPLY_RETCODE);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("LPUSH (again, in order to bench LRANGE)");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"LRANGE mylist 0 99\r\n");
        prepareClientForReply(c,REPLY_MBULK);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("LRANGE (first 100 elements)");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"LRANGE mylist 0 299\r\n");
        prepareClientForReply(c,REPLY_MBULK);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("LRANGE (first 300 elements)");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"LRANGE mylist 0 449\r\n");
        prepareClientForReply(c,REPLY_MBULK);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("LRANGE (first 450 elements)");

        prepareForBenchmark();
        c = createClient();
        if (!c) exit(1);
        c->obuf = sdscat(c->obuf,"LRANGE mylist 0 599\r\n");
        prepareClientForReply(c,REPLY_MBULK);
        createMissingClients(c);
        aeMain(config.el);
        endBenchmark("LRANGE (first 600 elements)");

        printf("\n");
    } while(config.loop);

    return 0;
}
