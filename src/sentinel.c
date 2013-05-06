/* Redis Sentinel implementation
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

#include "redis.h"
#include "hiredis.h"
#include "async.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>

extern char **environ;

#define REDIS_SENTINEL_PORT 26379

/* ======================== Sentinel global state =========================== */

typedef long long mstime_t; /* millisecond time type. */

/* Address object, used to describe an ip:port pair. */
typedef struct sentinelAddr {
    char *ip;
    int port;
} sentinelAddr;

/* A Sentinel Redis Instance object is monitoring. */
#define SRI_MASTER  (1<<0)
#define SRI_SLAVE   (1<<1)
#define SRI_SENTINEL (1<<2)
#define SRI_DISCONNECTED (1<<3)
#define SRI_S_DOWN (1<<4)   /* Subjectively down (no quorum). */
#define SRI_O_DOWN (1<<5)   /* Objectively down (quorum reached). */
#define SRI_MASTER_DOWN (1<<6) /* A Sentinel with this flag set thinks that
                                   its master is down. */
/* SRI_CAN_FAILOVER when set in an SRI_MASTER instance means that we are
 * allowed to perform the failover for this master.
 * When set in a SRI_SENTINEL instance means that sentinel is allowed to
 * perform the failover on its master. */
#define SRI_CAN_FAILOVER (1<<7)
#define SRI_FAILOVER_IN_PROGRESS (1<<8) /* Failover is in progress for
                                           this master. */
#define SRI_I_AM_THE_LEADER (1<<9)     /* We are the leader for this master. */
#define SRI_PROMOTED (1<<10)            /* Slave selected for promotion. */
#define SRI_RECONF_SENT (1<<11)     /* SLAVEOF <newmaster> sent. */
#define SRI_RECONF_INPROG (1<<12)   /* Slave synchronization in progress. */
#define SRI_RECONF_DONE (1<<13)     /* Slave synchronized with new master. */
#define SRI_FORCE_FAILOVER (1<<14)  /* Force failover with master up. */
#define SRI_SCRIPT_KILL_SENT (1<<15) /* SCRIPT KILL already sent on -BUSY */

#define SENTINEL_INFO_PERIOD 10000
#define SENTINEL_PING_PERIOD 1000
#define SENTINEL_ASK_PERIOD 1000
#define SENTINEL_PUBLISH_PERIOD 5000
#define SENTINEL_DOWN_AFTER_PERIOD 30000
#define SENTINEL_HELLO_CHANNEL "__sentinel__:hello"
#define SENTINEL_TILT_TRIGGER 2000
#define SENTINEL_TILT_PERIOD (SENTINEL_PING_PERIOD*30)
#define SENTINEL_DEFAULT_SLAVE_PRIORITY 100
#define SENTINEL_PROMOTION_RETRY_PERIOD 30000
#define SENTINEL_SLAVE_RECONF_RETRY_PERIOD 10000
#define SENTINEL_DEFAULT_PARALLEL_SYNCS 1
#define SENTINEL_MIN_LINK_RECONNECT_PERIOD 15000
#define SENTINEL_DEFAULT_FAILOVER_TIMEOUT (60*15*1000)
#define SENTINEL_MAX_PENDING_COMMANDS 100
#define SENTINEL_EXTENDED_SDOWN_MULTIPLIER 10

/* How many milliseconds is an information valid? This applies for instance
 * to the reply to SENTINEL IS-MASTER-DOWN-BY-ADDR replies. */
#define SENTINEL_INFO_VALIDITY_TIME 5000
#define SENTINEL_FAILOVER_FIXED_DELAY 5000
#define SENTINEL_FAILOVER_MAX_RANDOM_DELAY 10000

/* Failover machine different states. */
#define SENTINEL_FAILOVER_STATE_NONE 0  /* No failover in progress. */
#define SENTINEL_FAILOVER_STATE_WAIT_START 1  /* Wait for failover_start_time*/ 
#define SENTINEL_FAILOVER_STATE_SELECT_SLAVE 2 /* Select slave to promote */
#define SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE 3 /* Slave -> Master */
#define SENTINEL_FAILOVER_STATE_WAIT_PROMOTION 4 /* Wait slave to change role */
#define SENTINEL_FAILOVER_STATE_RECONF_SLAVES 5 /* SLAVEOF newmaster */
#define SENTINEL_FAILOVER_STATE_WAIT_NEXT_SLAVE 6 /* wait replication */
#define SENTINEL_FAILOVER_STATE_ALERT_CLIENTS 7 /* Run user script. */
#define SENTINEL_FAILOVER_STATE_WAIT_ALERT_SCRIPT 8 /* Wait script exec. */
#define SENTINEL_FAILOVER_STATE_DETECT_END 9 /* Check for failover end. */
#define SENTINEL_FAILOVER_STATE_UPDATE_CONFIG 10 /* Monitor promoted slave. */

#define SENTINEL_MASTER_LINK_STATUS_UP 0
#define SENTINEL_MASTER_LINK_STATUS_DOWN 1

/* Generic flags that can be used with different functions. */
#define SENTINEL_NO_FLAGS 0
#define SENTINEL_GENERATE_EVENT 1
#define SENTINEL_LEADER 2
#define SENTINEL_OBSERVER 4

/* Script execution flags and limits. */
#define SENTINEL_SCRIPT_NONE 0
#define SENTINEL_SCRIPT_RUNNING 1
#define SENTINEL_SCRIPT_MAX_QUEUE 256
#define SENTINEL_SCRIPT_MAX_RUNNING 16
#define SENTINEL_SCRIPT_MAX_RUNTIME 60000 /* 60 seconds max exec time. */
#define SENTINEL_SCRIPT_MAX_RETRY 10
#define SENTINEL_SCRIPT_RETRY_DELAY 30000 /* 30 seconds between retries. */

typedef struct sentinelRedisInstance {
    int flags;      /* See SRI_... defines */
    char *name;     /* Master name from the point of view of this sentinel. */
    char *runid;    /* run ID of this instance. */
    sentinelAddr *addr; /* Master host. */
    redisAsyncContext *cc; /* Hiredis context for commands. */
    redisAsyncContext *pc; /* Hiredis context for Pub / Sub. */
    int pending_commands;   /* Number of commands sent waiting for a reply. */
    mstime_t cc_conn_time; /* cc connection time. */
    mstime_t pc_conn_time; /* pc connection time. */
    mstime_t pc_last_activity; /* Last time we received any message. */
    mstime_t last_avail_time; /* Last time the instance replied to ping with
                                 a reply we consider valid. */
    mstime_t last_pong_time;  /* Last time the instance replied to ping,
                                 whatever the reply was. That's used to check
                                 if the link is idle and must be reconnected. */
    mstime_t last_pub_time;   /* Last time we sent hello via Pub/Sub. */
    mstime_t last_hello_time; /* Only used if SRI_SENTINEL is set. Last time
                                 we received an hello from this Sentinel
                                 via Pub/Sub. */
    mstime_t last_master_down_reply_time; /* Time of last reply to
                                             SENTINEL is-master-down command. */
    mstime_t s_down_since_time; /* Subjectively down since time. */
    mstime_t o_down_since_time; /* Objectively down since time. */
    mstime_t down_after_period; /* Consider it down after that period. */
    mstime_t info_refresh;  /* Time at which we received INFO output from it. */

    /* Master specific. */
    dict *sentinels;    /* Other sentinels monitoring the same master. */
    dict *slaves;       /* Slaves for this master instance. */
    int quorum;         /* Number of sentinels that need to agree on failure. */
    int parallel_syncs; /* How many slaves to reconfigure at same time. */
    char *auth_pass;    /* Password to use for AUTH against master & slaves. */

    /* Slave specific. */
    mstime_t master_link_down_time; /* Slave replication link down time. */
    int slave_priority; /* Slave priority according to its INFO output. */
    mstime_t slave_reconf_sent_time; /* Time at which we sent SLAVE OF <new> */
    struct sentinelRedisInstance *master; /* Master instance if SRI_SLAVE is set. */
    char *slave_master_host;    /* Master host as reported by INFO */
    int slave_master_port;      /* Master port as reported by INFO */
    int slave_master_link_status; /* Master link status as reported by INFO */
    /* Failover */
    char *leader;       /* If this is a master instance, this is the runid of
                           the Sentinel that should perform the failover. If
                           this is a Sentinel, this is the runid of the Sentinel
                           that this other Sentinel is voting as leader.
                           This field is valid only if SRI_MASTER_DOWN is
                           set on the Sentinel instance. */
    int failover_state; /* See SENTINEL_FAILOVER_STATE_* defines. */
    mstime_t failover_state_change_time;
    mstime_t failover_start_time;   /* When to start to failover if leader. */
    mstime_t failover_timeout;      /* Max time to refresh failover state. */
    struct sentinelRedisInstance *promoted_slave; /* Promoted slave instance. */
    /* Scripts executed to notify admin or reconfigure clients: when they
     * are set to NULL no script is executed. */
    char *notification_script;
    char *client_reconfig_script;
} sentinelRedisInstance;

/* Main state. */
struct sentinelState {
    dict *masters;      /* Dictionary of master sentinelRedisInstances.
                           Key is the instance name, value is the
                           sentinelRedisInstance structure pointer. */
    int tilt;           /* Are we in TILT mode? */
    int running_scripts;    /* Number of scripts in execution right now. */
    mstime_t tilt_start_time;   /* When TITL started. */
    mstime_t previous_time;     /* Time last time we ran the time handler. */
    list *scripts_queue;    /* Queue of user scripts to execute. */
} sentinel;

/* A script execution job. */
typedef struct sentinelScriptJob {
    int flags;              /* Script job flags: SENTINEL_SCRIPT_* */
    int retry_num;          /* Number of times we tried to execute it. */
    char **argv;            /* Arguments to call the script. */
    mstime_t start_time;    /* Script execution time if the script is running,
                               otherwise 0 if we are allowed to retry the
                               execution at any time. If the script is not
                               running and it's not 0, it means: do not run
                               before the specified time. */
    pid_t pid;              /* Script execution pid. */
} sentinelScriptJob;

/* ======================= hiredis ae.c adapters =============================
 * Note: this implementation is taken from hiredis/adapters/ae.h, however
 * we have our modified copy for Sentinel in order to use our allocator
 * and to have full control over how the adapter works. */

typedef struct redisAeEvents {
    redisAsyncContext *context;
    aeEventLoop *loop;
    int fd;
    int reading, writing;
} redisAeEvents;

static void redisAeReadEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el); ((void)fd); ((void)mask);

    redisAeEvents *e = (redisAeEvents*)privdata;
    redisAsyncHandleRead(e->context);
}

static void redisAeWriteEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el); ((void)fd); ((void)mask);

    redisAeEvents *e = (redisAeEvents*)privdata;
    redisAsyncHandleWrite(e->context);
}

static void redisAeAddRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (!e->reading) {
        e->reading = 1;
        aeCreateFileEvent(loop,e->fd,AE_READABLE,redisAeReadEvent,e);
    }
}

static void redisAeDelRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (e->reading) {
        e->reading = 0;
        aeDeleteFileEvent(loop,e->fd,AE_READABLE);
    }
}

static void redisAeAddWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (!e->writing) {
        e->writing = 1;
        aeCreateFileEvent(loop,e->fd,AE_WRITABLE,redisAeWriteEvent,e);
    }
}

static void redisAeDelWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (e->writing) {
        e->writing = 0;
        aeDeleteFileEvent(loop,e->fd,AE_WRITABLE);
    }
}

static void redisAeCleanup(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    redisAeDelRead(privdata);
    redisAeDelWrite(privdata);
    zfree(e);
}

static int redisAeAttach(aeEventLoop *loop, redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisAeEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    e = (redisAeEvents*)zmalloc(sizeof(*e));
    e->context = ac;
    e->loop = loop;
    e->fd = c->fd;
    e->reading = e->writing = 0;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = redisAeAddRead;
    ac->ev.delRead = redisAeDelRead;
    ac->ev.addWrite = redisAeAddWrite;
    ac->ev.delWrite = redisAeDelWrite;
    ac->ev.cleanup = redisAeCleanup;
    ac->ev.data = e;

    return REDIS_OK;
}

/* ============================= Prototypes ================================= */

void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status);
void sentinelDisconnectCallback(const redisAsyncContext *c, int status);
void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata);
sentinelRedisInstance *sentinelGetMasterByName(char *name);
char *sentinelGetSubjectiveLeader(sentinelRedisInstance *master);
char *sentinelGetObjectiveLeader(sentinelRedisInstance *master);
int yesnotoi(char *s);
void sentinelDisconnectInstanceFromContext(const redisAsyncContext *c);
void sentinelKillLink(sentinelRedisInstance *ri, redisAsyncContext *c);
const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri);
void sentinelAbortFailover(sentinelRedisInstance *ri);
void sentinelEvent(int level, char *type, sentinelRedisInstance *ri, const char *fmt, ...);
sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master);
void sentinelScheduleScriptExecution(char *path, ...);
void sentinelStartFailover(sentinelRedisInstance *master, int state);
void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata);

/* ========================= Dictionary types =============================== */

unsigned int dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
void releaseSentinelRedisInstance(sentinelRedisInstance *ri);

void dictInstancesValDestructor (void *privdata, void *obj) {
    releaseSentinelRedisInstance(obj);
}

/* Instance name (sds) -> instance (sentinelRedisInstance pointer)
 *
 * also used for: sentinelRedisInstance->sentinels dictionary that maps
 * sentinels ip:port to last seen time in Pub/Sub hello message. */
dictType instancesDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    dictInstancesValDestructor /* val destructor */
};

/* Instance runid (sds) -> votes (long casted to void*)
 *
 * This is useful into sentinelGetObjectiveLeader() function in order to
 * count the votes and understand who is the leader. */
dictType leaderVotesDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

/* =========================== Initialization =============================== */

void sentinelCommand(redisClient *c);
void sentinelInfoCommand(redisClient *c);

struct redisCommand sentinelcmds[] = {
    {"ping",pingCommand,1,"",0,NULL,0,0,0,0,0},
    {"sentinel",sentinelCommand,-2,"",0,NULL,0,0,0,0,0},
    {"subscribe",subscribeCommand,-2,"",0,NULL,0,0,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,"",0,NULL,0,0,0,0,0},
    {"psubscribe",psubscribeCommand,-2,"",0,NULL,0,0,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,"",0,NULL,0,0,0,0,0},
    {"info",sentinelInfoCommand,-1,"",0,NULL,0,0,0,0,0}
};

/* This function overwrites a few normal Redis config default with Sentinel
 * specific defaults. */
void initSentinelConfig(void) {
    server.port = REDIS_SENTINEL_PORT;
}

/* Perform the Sentinel mode initialization. */
void initSentinel(void) {
    int j;

    /* Remove usual Redis commands from the command table, then just add
     * the SENTINEL command. */
    dictEmpty(server.commands);
    for (j = 0; j < sizeof(sentinelcmds)/sizeof(sentinelcmds[0]); j++) {
        int retval;
        struct redisCommand *cmd = sentinelcmds+j;

        retval = dictAdd(server.commands, sdsnew(cmd->name), cmd);
        redisAssert(retval == DICT_OK);
    }

    /* Initialize various data structures. */
    sentinel.masters = dictCreate(&instancesDictType,NULL);
    sentinel.tilt = 0;
    sentinel.tilt_start_time = mstime();
    sentinel.previous_time = mstime();
    sentinel.running_scripts = 0;
    sentinel.scripts_queue = listCreate();
}

/* ============================== sentinelAddr ============================== */

/* Create a sentinelAddr object and return it on success.
 * On error NULL is returned and errno is set to:
 *  ENOENT: Can't resolve the hostname.
 *  EINVAL: Invalid port number.
 */
sentinelAddr *createSentinelAddr(char *hostname, int port) {
    char buf[32];
    sentinelAddr *sa;

    if (port <= 0 || port > 65535) {
        errno = EINVAL;
        return NULL;
    }
    if (anetResolve(NULL,hostname,buf) == ANET_ERR) {
        errno = ENOENT;
        return NULL;
    }
    sa = zmalloc(sizeof(*sa));
    sa->ip = sdsnew(buf);
    sa->port = port;
    return sa;
}

/* Free a Sentinel address. Can't fail. */
void releaseSentinelAddr(sentinelAddr *sa) {
    sdsfree(sa->ip);
    zfree(sa);
}

/* =========================== Events notification ========================== */

/* Send an event to log, pub/sub, user notification script.
 * 
 * 'level' is the log level for logging. Only REDIS_WARNING events will trigger
 * the execution of the user notification script.
 *
 * 'type' is the message type, also used as a pub/sub channel name.
 *
 * 'ri', is the redis instance target of this event if applicable, and is
 * used to obtain the path of the notification script to execute.
 *
 * The remaining arguments are printf-alike.
 * If the format specifier starts with the two characters "%@" then ri is
 * not NULL, and the message is prefixed with an instance identifier in the
 * following format:
 *
 *  <instance type> <instance name> <ip> <port>
 *
 *  If the instance type is not master, than the additional string is
 *  added to specify the originating master:
 *
 *  @ <master name> <master ip> <master port>
 *
 *  Any other specifier after "%@" is processed by printf itself.
 */
void sentinelEvent(int level, char *type, sentinelRedisInstance *ri,
                   const char *fmt, ...) {
    va_list ap;
    char msg[REDIS_MAX_LOGMSG_LEN];
    robj *channel, *payload;

    /* Handle %@ */
    if (fmt[0] == '%' && fmt[1] == '@') {
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                         NULL : ri->master;

        if (master) {
            snprintf(msg, sizeof(msg), "%s %s %s %d @ %s %s %d",
                sentinelRedisInstanceTypeStr(ri),
                ri->name, ri->addr->ip, ri->addr->port,
                master->name, master->addr->ip, master->addr->port);
        } else {
            snprintf(msg, sizeof(msg), "%s %s %s %d",
                sentinelRedisInstanceTypeStr(ri),
                ri->name, ri->addr->ip, ri->addr->port);
        }
        fmt += 2;
    } else {
        msg[0] = '\0';
    }

    /* Use vsprintf for the rest of the formatting if any. */
    if (fmt[0] != '\0') {
        va_start(ap, fmt);
        vsnprintf(msg+strlen(msg), sizeof(msg)-strlen(msg), fmt, ap);
        va_end(ap);
    }

    /* Log the message if the log level allows it to be logged. */
    if (level >= server.verbosity)
        redisLog(level,"%s %s",type,msg);

    /* Publish the message via Pub/Sub if it's not a debugging one. */
    if (level != REDIS_DEBUG) {
        channel = createStringObject(type,strlen(type));
        payload = createStringObject(msg,strlen(msg));
        pubsubPublishMessage(channel,payload);
        decrRefCount(channel);
        decrRefCount(payload);
    }

    /* Call the notification script if applicable. */
    if (level == REDIS_WARNING && ri != NULL) {
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                         ri : ri->master;
        if (master->notification_script) {
            sentinelScheduleScriptExecution(master->notification_script,
                type,msg,NULL);
        }
    }
}

/* ============================ script execution ============================ */

/* Release a script job structure and all the associated data. */
void sentinelReleaseScriptJob(sentinelScriptJob *sj) {
    int j = 0;

    while(sj->argv[j]) sdsfree(sj->argv[j++]);
    zfree(sj->argv);
    zfree(sj);
}

#define SENTINEL_SCRIPT_MAX_ARGS 16
void sentinelScheduleScriptExecution(char *path, ...) {
    va_list ap;
    char *argv[SENTINEL_SCRIPT_MAX_ARGS+1];
    int argc = 1;
    sentinelScriptJob *sj;

    va_start(ap, path);
    while(argc < SENTINEL_SCRIPT_MAX_ARGS) {
        argv[argc] = va_arg(ap,char*);
        if (!argv[argc]) break;
        argv[argc] = sdsnew(argv[argc]); /* Copy the string. */
        argc++;
    }
    va_end(ap);
    argv[0] = sdsnew(path);
    
    sj = zmalloc(sizeof(*sj));
    sj->flags = SENTINEL_SCRIPT_NONE;
    sj->retry_num = 0;
    sj->argv = zmalloc(sizeof(char*)*(argc+1));
    sj->start_time = 0;
    sj->pid = 0;
    memcpy(sj->argv,argv,sizeof(char*)*(argc+1));

    listAddNodeTail(sentinel.scripts_queue,sj);

    /* Remove the oldest non running script if we already hit the limit. */
    if (listLength(sentinel.scripts_queue) > SENTINEL_SCRIPT_MAX_QUEUE) {
        listNode *ln;
        listIter li;

        listRewind(sentinel.scripts_queue,&li);
        while ((ln = listNext(&li)) != NULL) {
            sj = ln->value;

            if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;
            /* The first node is the oldest as we add on tail. */
            listDelNode(sentinel.scripts_queue,ln);
            sentinelReleaseScriptJob(sj);
            break;
        }
        redisAssert(listLength(sentinel.scripts_queue) <=
                    SENTINEL_SCRIPT_MAX_QUEUE);
    }
}

/* Lookup a script in the scripts queue via pid, and returns the list node
 * (so that we can easily remove it from the queue if needed). */
listNode *sentinelGetScriptListNodeByPid(pid_t pid) {
    listNode *ln;
    listIter li;

    listRewind(sentinel.scripts_queue,&li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;

        if ((sj->flags & SENTINEL_SCRIPT_RUNNING) && sj->pid == pid)
            return ln;
    }
    return NULL;
}

/* Run pending scripts if we are not already at max number of running
 * scripts. */
void sentinelRunPendingScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    /* Find jobs that are not running and run them, from the top to the
     * tail of the queue, so we run older jobs first. */
    listRewind(sentinel.scripts_queue,&li);
    while (sentinel.running_scripts < SENTINEL_SCRIPT_MAX_RUNNING &&
           (ln = listNext(&li)) != NULL)
    {
        sentinelScriptJob *sj = ln->value;
        pid_t pid;

        /* Skip if already running. */
        if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;

        /* Skip if it's a retry, but not enough time has elapsed. */
        if (sj->start_time && sj->start_time > now) continue;

        sj->flags |= SENTINEL_SCRIPT_RUNNING;
        sj->start_time = mstime();
        sj->retry_num++;
        pid = fork();

        if (pid == -1) {
            /* Parent (fork error).
             * We report fork errors as signal 99, in order to unify the
             * reporting with other kind of errors. */
            sentinelEvent(REDIS_WARNING,"-script-error",NULL,
                          "%s %d %d", sj->argv[0], 99, 0);
            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
        } else if (pid == 0) {
            /* Child */
            execve(sj->argv[0],sj->argv,environ);
            /* If we are here an error occurred. */
            _exit(2); /* Don't retry execution. */
        } else {
            sentinel.running_scripts++;
            sj->pid = pid;
            sentinelEvent(REDIS_DEBUG,"+script-child",NULL,"%ld",(long)pid);
        }
    }
}

/* How much to delay the execution of a script that we need to retry after
 * an error?
 *
 * We double the retry delay for every further retry we do. So for instance
 * if RETRY_DELAY is set to 30 seconds and the max number of retries is 10
 * starting from the second attempt to execute the script the delays are:
 * 30 sec, 60 sec, 2 min, 4 min, 8 min, 16 min, 32 min, 64 min, 128 min. */
mstime_t sentinelScriptRetryDelay(int retry_num) {
    mstime_t delay = SENTINEL_SCRIPT_RETRY_DELAY;

    while (retry_num-- > 1) delay *= 2;
    return delay;
}

/* Check for scripts that terminated, and remove them from the queue if the
 * script terminated successfully. If instead the script was terminated by
 * a signal, or returned exit code "1", it is scheduled to run again if
 * the max number of retries did not already elapsed. */
void sentinelCollectTerminatedScripts(void) {
    int statloc;
    pid_t pid;

    while ((pid = wait3(&statloc,WNOHANG,NULL)) > 0) {
        int exitcode = WEXITSTATUS(statloc);
        int bysignal = 0;
        listNode *ln;
        sentinelScriptJob *sj;

        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);
        sentinelEvent(REDIS_DEBUG,"-script-child",NULL,"%ld %d %d",
            (long)pid, exitcode, bysignal);
        
        ln = sentinelGetScriptListNodeByPid(pid);
        if (ln == NULL) {
            redisLog(REDIS_WARNING,"wait3() returned a pid (%ld) we can't find in our scripts execution queue!", (long)pid);
            continue;
        }
        sj = ln->value;

        /* If the script was terminated by a signal or returns an
         * exit code of "1" (that means: please retry), we reschedule it
         * if the max number of retries is not already reached. */
        if ((bysignal || exitcode == 1) &&
            sj->retry_num != SENTINEL_SCRIPT_MAX_RETRY)
        {
            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
            sj->start_time = mstime() +
                             sentinelScriptRetryDelay(sj->retry_num);
        } else {
            /* Otherwise let's remove the script, but log the event if the
             * execution did not terminated in the best of the ways. */
            if (bysignal || exitcode != 0) {
                sentinelEvent(REDIS_WARNING,"-script-error",NULL,
                              "%s %d %d", sj->argv[0], bysignal, exitcode);
            }
            listDelNode(sentinel.scripts_queue,ln);
            sentinelReleaseScriptJob(sj);
            sentinel.running_scripts--;
        }
    }
}

/* Kill scripts in timeout, they'll be collected by the
 * sentinelCollectTerminatedScripts() function. */
void sentinelKillTimedoutScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    listRewind(sentinel.scripts_queue,&li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;

        if (sj->flags & SENTINEL_SCRIPT_RUNNING &&
            (now - sj->start_time) > SENTINEL_SCRIPT_MAX_RUNTIME)
        {
            sentinelEvent(REDIS_WARNING,"-script-timeout",NULL,"%s %ld",
                sj->argv[0], (long)sj->pid);
            kill(sj->pid,SIGKILL);
        }
    }
}

/* Implements SENTINEL PENDING-SCRIPTS command. */
void sentinelPendingScriptsCommand(redisClient *c) {
    listNode *ln;
    listIter li;

    addReplyMultiBulkLen(c,listLength(sentinel.scripts_queue));
    listRewind(sentinel.scripts_queue,&li);
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;
        int j = 0;

        addReplyMultiBulkLen(c,10);

        addReplyBulkCString(c,"argv");
        while (sj->argv[j]) j++;
        addReplyMultiBulkLen(c,j);
        j = 0;
        while (sj->argv[j]) addReplyBulkCString(c,sj->argv[j++]);

        addReplyBulkCString(c,"flags");
        addReplyBulkCString(c,
            (sj->flags & SENTINEL_SCRIPT_RUNNING) ? "running" : "scheduled");

        addReplyBulkCString(c,"pid");
        addReplyBulkLongLong(c,sj->pid);

        if (sj->flags & SENTINEL_SCRIPT_RUNNING) {
            addReplyBulkCString(c,"run-time");
            addReplyBulkLongLong(c,mstime() - sj->start_time);
        } else {
            mstime_t delay = sj->start_time ? (sj->start_time-mstime()) : 0;
            if (delay < 0) delay = 0;
            addReplyBulkCString(c,"run-delay");
            addReplyBulkLongLong(c,delay);
        }

        addReplyBulkCString(c,"retry-num");
        addReplyBulkLongLong(c,sj->retry_num);
    }
}

/* This function calls, if any, the client reconfiguration script with the
 * following parameters:
 *
 * <master-name> <role> <state> <from-ip> <from-port> <to-ip> <to-port>
 *
 * It is called every time a failover starts, ends, or is aborted.
 *
 * <state> is "start", "end" or "abort".
 * <role> is either "leader" or "observer".
 *
 * from/to fields are respectively master -> promoted slave addresses for
 * "start" and "end", or the reverse (promoted slave -> master) in case of
 * "abort".
 */
void sentinelCallClientReconfScript(sentinelRedisInstance *master, int role, char *state, sentinelAddr *from, sentinelAddr *to) {
    char fromport[32], toport[32];

    if (master->client_reconfig_script == NULL) return;
    ll2string(fromport,sizeof(fromport),from->port);
    ll2string(toport,sizeof(toport),to->port);
    sentinelScheduleScriptExecution(master->client_reconfig_script,
        master->name,
        (role == SENTINEL_LEADER) ? "leader" : "observer",
        state, from->ip, fromport, to->ip, toport, NULL);
}

/* ========================== sentinelRedisInstance ========================= */

/* Create a redis instance, the following fields must be populated by the
 * caller if needed:
 * runid: set to NULL but will be populated once INFO output is received.
 * info_refresh: is set to 0 to mean that we never received INFO so far.
 *
 * If SRI_MASTER is set into initial flags the instance is added to
 * sentinel.masters table.
 *
 * if SRI_SLAVE or SRI_SENTINEL is set then 'master' must be not NULL and the
 * instance is added into master->slaves or master->sentinels table.
 *
 * If the instance is a slave or sentinel, the name parameter is ignored and
 * is created automatically as hostname:port.
 *
 * The function fails if hostname can't be resolved or port is out of range.
 * When this happens NULL is returned and errno is set accordingly to the
 * createSentinelAddr() function.
 *
 * The function may also fail and return NULL with errno set to EBUSY if
 * a master or slave with the same name already exists. */
sentinelRedisInstance *createSentinelRedisInstance(char *name, int flags, char *hostname, int port, int quorum, sentinelRedisInstance *master) {
    sentinelRedisInstance *ri;
    sentinelAddr *addr;
    dict *table = NULL;
    char slavename[128], *sdsname;

    redisAssert(flags & (SRI_MASTER|SRI_SLAVE|SRI_SENTINEL));
    redisAssert((flags & SRI_MASTER) || master != NULL);

    /* Check address validity. */
    addr = createSentinelAddr(hostname,port);
    if (addr == NULL) return NULL;

    /* For slaves and sentinel we use ip:port as name. */
    if (flags & (SRI_SLAVE|SRI_SENTINEL)) {
        snprintf(slavename,sizeof(slavename),"%s:%d",hostname,port);
        name = slavename;
    }

    /* Make sure the entry is not duplicated. This may happen when the same
     * name for a master is used multiple times inside the configuration or
     * if we try to add multiple times a slave or sentinel with same ip/port
     * to a master. */
    if (flags & SRI_MASTER) table = sentinel.masters;
    else if (flags & SRI_SLAVE) table = master->slaves;
    else if (flags & SRI_SENTINEL) table = master->sentinels;
    sdsname = sdsnew(name);
    if (dictFind(table,sdsname)) {
        sdsfree(sdsname);
        errno = EBUSY;
        return NULL;
    }

    /* Create the instance object. */
    ri = zmalloc(sizeof(*ri));
    /* Note that all the instances are started in the disconnected state,
     * the event loop will take care of connecting them. */
    ri->flags = flags | SRI_DISCONNECTED;
    ri->name = sdsname;
    ri->runid = NULL;
    ri->addr = addr;
    ri->cc = NULL;
    ri->pc = NULL;
    ri->pending_commands = 0;
    ri->cc_conn_time = 0;
    ri->pc_conn_time = 0;
    ri->pc_last_activity = 0;
    ri->last_avail_time = mstime();
    ri->last_pong_time = mstime();
    ri->last_pub_time = mstime();
    ri->last_hello_time = mstime();
    ri->last_master_down_reply_time = mstime();
    ri->s_down_since_time = 0;
    ri->o_down_since_time = 0;
    ri->down_after_period = master ? master->down_after_period :
                            SENTINEL_DOWN_AFTER_PERIOD;
    ri->master_link_down_time = 0;
    ri->auth_pass = NULL;
    ri->slave_priority = SENTINEL_DEFAULT_SLAVE_PRIORITY;
    ri->slave_reconf_sent_time = 0;
    ri->slave_master_host = NULL;
    ri->slave_master_port = 0;
    ri->slave_master_link_status = SENTINEL_MASTER_LINK_STATUS_DOWN;
    ri->sentinels = dictCreate(&instancesDictType,NULL);
    ri->quorum = quorum;
    ri->parallel_syncs = SENTINEL_DEFAULT_PARALLEL_SYNCS;
    ri->master = master;
    ri->slaves = dictCreate(&instancesDictType,NULL);
    ri->info_refresh = 0;

    /* Failover state. */
    ri->leader = NULL;
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0;
    ri->failover_timeout = SENTINEL_DEFAULT_FAILOVER_TIMEOUT;
    ri->promoted_slave = NULL;
    ri->notification_script = NULL;
    ri->client_reconfig_script = NULL;

    /* Add into the right table. */
    dictAdd(table, ri->name, ri);
    return ri;
}

/* Release this instance and all its slaves, sentinels, hiredis connections.
 * This function also takes care of unlinking the instance from the main
 * masters table (if it is a master) or from its master sentinels/slaves table
 * if it is a slave or sentinel. */
void releaseSentinelRedisInstance(sentinelRedisInstance *ri) {
    /* Release all its slaves or sentinels if any. */
    dictRelease(ri->sentinels);
    dictRelease(ri->slaves);

    /* Release hiredis connections. */
    if (ri->cc) sentinelKillLink(ri,ri->cc);
    if (ri->pc) sentinelKillLink(ri,ri->pc);

    /* Free other resources. */
    sdsfree(ri->name);
    sdsfree(ri->runid);
    sdsfree(ri->notification_script);
    sdsfree(ri->client_reconfig_script);
    sdsfree(ri->slave_master_host);
    sdsfree(ri->leader);
    sdsfree(ri->auth_pass);
    releaseSentinelAddr(ri->addr);

    /* Clear state into the master if needed. */
    if ((ri->flags & SRI_SLAVE) && (ri->flags & SRI_PROMOTED) && ri->master)
        ri->master->promoted_slave = NULL;

    zfree(ri);
}

/* Lookup a slave in a master Redis instance, by ip and port. */
sentinelRedisInstance *sentinelRedisInstanceLookupSlave(
                sentinelRedisInstance *ri, char *ip, int port)
{
    sds key;
    sentinelRedisInstance *slave;
  
    redisAssert(ri->flags & SRI_MASTER);
    key = sdscatprintf(sdsempty(),"%s:%d",ip,port);
    slave = dictFetchValue(ri->slaves,key);
    sdsfree(key);
    return slave;
}

/* Return the name of the type of the instance as a string. */
const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri) {
    if (ri->flags & SRI_MASTER) return "master";
    else if (ri->flags & SRI_SLAVE) return "slave";
    else if (ri->flags & SRI_SENTINEL) return "sentinel";
    else return "unknown";
}

/* This function removes all the instances found in the dictionary of instances
 * 'd', having either:
 * 
 * 1) The same ip/port as specified.
 * 2) The same runid.
 *
 * "1" and "2" don't need to verify at the same time, just one is enough.
 * If "runid" is NULL it is not checked.
 * Similarly if "ip" is NULL it is not checked.
 *
 * This function is useful because every time we add a new Sentinel into
 * a master's Sentinels dictionary, we want to be very sure about not
 * having duplicated instances for any reason. This is so important because
 * we use those other sentinels in order to run our quorum protocol to
 * understand if it's time to proceed with the fail over.
 *
 * Making sure no duplication is possible we greatly improve the robustness
 * of the quorum (otherwise we may end counting the same instance multiple
 * times for some reason).
 *
 * The function returns the number of Sentinels removed. */
int removeMatchingSentinelsFromMaster(sentinelRedisInstance *master, char *ip, int port, char *runid) {
    dictIterator *di;
    dictEntry *de;
    int removed = 0;

    di = dictGetSafeIterator(master->sentinels);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if ((ri->runid && runid && strcmp(ri->runid,runid) == 0) ||
            (ip && strcmp(ri->addr->ip,ip) == 0 && port == ri->addr->port))
        {
            dictDelete(master->sentinels,ri->name);
            removed++;
        }
    }
    dictReleaseIterator(di);
    return removed;
}

/* Search an instance with the same runid, ip and port into a dictionary
 * of instances. Return NULL if not found, otherwise return the instance
 * pointer.
 *
 * runid or ip can be NULL. In such a case the search is performed only
 * by the non-NULL field. */
sentinelRedisInstance *getSentinelRedisInstanceByAddrAndRunID(dict *instances, char *ip, int port, char *runid) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *instance = NULL;

    redisAssert(ip || runid);   /* User must pass at least one search param. */
    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (runid && !ri->runid) continue;
        if ((runid == NULL || strcmp(ri->runid, runid) == 0) &&
            (ip == NULL || (strcmp(ri->addr->ip, ip) == 0 &&
                            ri->addr->port == port)))
        {
            instance = ri;
            break;
        }
    }
    dictReleaseIterator(di);
    return instance;
}

/* Simple master lookup by name */
sentinelRedisInstance *sentinelGetMasterByName(char *name) {
    sentinelRedisInstance *ri;
    sds sdsname = sdsnew(name);

    ri = dictFetchValue(sentinel.masters,sdsname);
    sdsfree(sdsname);
    return ri;
}

/* Add the specified flags to all the instances in the specified dictionary. */
void sentinelAddFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        ri->flags |= flags;
    }
    dictReleaseIterator(di);
}

/* Remove the specified flags to all the instances in the specified
 * dictionary. */
void sentinelDelFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        ri->flags &= ~flags;
    }
    dictReleaseIterator(di);
}

/* Reset the state of a monitored master:
 * 1) Remove all slaves.
 * 2) Remove all sentinels.
 * 3) Remove most of the flags resulting from runtime operations.
 * 4) Reset timers to their default value.
 * 5) In the process of doing this undo the failover if in progress.
 * 6) Disconnect the connections with the master (will reconnect automatically).
 */
void sentinelResetMaster(sentinelRedisInstance *ri, int flags) {
    redisAssert(ri->flags & SRI_MASTER);
    dictRelease(ri->slaves);
    dictRelease(ri->sentinels);
    ri->slaves = dictCreate(&instancesDictType,NULL);
    ri->sentinels = dictCreate(&instancesDictType,NULL);
    if (ri->cc) sentinelKillLink(ri,ri->cc);
    if (ri->pc) sentinelKillLink(ri,ri->pc);
    ri->flags &= SRI_MASTER|SRI_CAN_FAILOVER|SRI_DISCONNECTED;
    if (ri->leader) {
        sdsfree(ri->leader);
        ri->leader = NULL;
    }
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0;
    ri->promoted_slave = NULL;
    sdsfree(ri->runid);
    sdsfree(ri->slave_master_host);
    ri->runid = NULL;
    ri->slave_master_host = NULL;
    ri->last_avail_time = mstime();
    ri->last_pong_time = mstime();
    if (flags & SENTINEL_GENERATE_EVENT)
        sentinelEvent(REDIS_WARNING,"+reset-master",ri,"%@");
}

/* Call sentinelResetMaster() on every master with a name matching the specified
 * pattern. */
int sentinelResetMastersByPattern(char *pattern, int flags) {
    dictIterator *di;
    dictEntry *de;
    int reset = 0;

    di = dictGetIterator(sentinel.masters);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (ri->name) {
            if (stringmatch(pattern,ri->name,0)) {
                sentinelResetMaster(ri,flags);
                reset++;
            }
        }
    }
    dictReleaseIterator(di);
    return reset;
}

/* Reset the specified master with sentinelResetMaster(), and also change
 * the ip:port address, but take the name of the instance unmodified.
 *
 * This is used to handle the +switch-master and +redirect-to-master events.
 *
 * The function returns REDIS_ERR if the address can't be resolved for some
 * reason. Otherwise REDIS_OK is returned.
 *
 * TODO: make this reset so that original sentinels are re-added with
 * same ip / port / runid.
 */

int sentinelResetMasterAndChangeAddress(sentinelRedisInstance *master, char *ip, int port) {
    sentinelAddr *oldaddr, *newaddr;

    newaddr = createSentinelAddr(ip,port);
    if (newaddr == NULL) return REDIS_ERR;
    sentinelResetMaster(master,SENTINEL_NO_FLAGS);
    oldaddr = master->addr;
    master->addr = newaddr;
    /* Release the old address at the end so we are safe even if the function
     * gets the master->addr->ip and master->addr->port as arguments. */
    releaseSentinelAddr(oldaddr);
    return REDIS_OK;
}

/* ============================ Config handling ============================= */
char *sentinelHandleConfiguration(char **argv, int argc) {
    sentinelRedisInstance *ri;

    if (!strcasecmp(argv[0],"monitor") && argc == 5) {
        /* monitor <name> <host> <port> <quorum> */
        int quorum = atoi(argv[4]);

        if (quorum <= 0) return "Quorum must be 1 or greater.";
        if (createSentinelRedisInstance(argv[1],SRI_MASTER,argv[2],
                                        atoi(argv[3]),quorum,NULL) == NULL)
        {
            switch(errno) {
            case EBUSY: return "Duplicated master name.";
            case ENOENT: return "Can't resolve master instance hostname.";
            case EINVAL: return "Invalid port number";
            }
        }
    } else if (!strcasecmp(argv[0],"down-after-milliseconds") && argc == 3) {
        /* down-after-milliseconds <name> <milliseconds> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->down_after_period = atoi(argv[2]);
        if (ri->down_after_period <= 0)
            return "negative or zero time parameter.";
    } else if (!strcasecmp(argv[0],"failover-timeout") && argc == 3) {
        /* failover-timeout <name> <milliseconds> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->failover_timeout = atoi(argv[2]);
        if (ri->failover_timeout <= 0)
            return "negative or zero time parameter.";
    } else if (!strcasecmp(argv[0],"can-failover") && argc == 3) {
        /* can-failover <name> <yes/no> */
        int yesno = yesnotoi(argv[2]);

        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if (yesno == -1) return "Argument must be either yes or no.";
        if (yesno)
            ri->flags |= SRI_CAN_FAILOVER;
        else
            ri->flags &= ~SRI_CAN_FAILOVER;
   } else if (!strcasecmp(argv[0],"parallel-syncs") && argc == 3) {
        /* parallel-syncs <name> <milliseconds> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->parallel_syncs = atoi(argv[2]);
   } else if (!strcasecmp(argv[0],"notification-script") && argc == 3) {
        /* notification-script <name> <path> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if (access(argv[2],X_OK) == -1)
            return "Notification script seems non existing or non executable.";
        ri->notification_script = sdsnew(argv[2]);
   } else if (!strcasecmp(argv[0],"client-reconfig-script") && argc == 3) {
        /* client-reconfig-script <name> <path> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        if (access(argv[2],X_OK) == -1)
            return "Client reconfiguration script seems non existing or "
                   "non executable.";
        ri->client_reconfig_script = sdsnew(argv[2]);
   } else if (!strcasecmp(argv[0],"auth-pass") && argc == 3) {
        /* auth-pass <name> <password> */
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        ri->auth_pass = sdsnew(argv[2]);
    } else {
        return "Unrecognized sentinel configuration statement.";
    }
    return NULL;
}

/* ====================== hiredis connection handling ======================= */

/* Completely disconnect an hiredis link from an instance. */
void sentinelKillLink(sentinelRedisInstance *ri, redisAsyncContext *c) {
    if (ri->cc == c) {
        ri->cc = NULL;
        ri->pending_commands = 0;
    }
    if (ri->pc == c) ri->pc = NULL;
    c->data = NULL;
    ri->flags |= SRI_DISCONNECTED;
    redisAsyncFree(c);
}

/* This function takes an hiredis context that is in an error condition
 * and make sure to mark the instance as disconnected performing the
 * cleanup needed.
 *
 * Note: we don't free the hiredis context as hiredis will do it for us
 * for async connections. */
void sentinelDisconnectInstanceFromContext(const redisAsyncContext *c) {
    sentinelRedisInstance *ri = c->data;
    int pubsub;

    if (ri == NULL) return; /* The instance no longer exists. */

    pubsub = (ri->pc == c);
    sentinelEvent(REDIS_DEBUG, pubsub ? "-pubsub-link" : "-cmd-link", ri,
        "%@ #%s", c->errstr);
    if (pubsub)
        ri->pc = NULL;
    else
        ri->cc = NULL;
    ri->flags |= SRI_DISCONNECTED;
}

void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        sentinelDisconnectInstanceFromContext(c);
    } else {
        sentinelRedisInstance *ri = c->data;
        int pubsub = (ri->pc == c);

        sentinelEvent(REDIS_DEBUG, pubsub ? "+pubsub-link" : "+cmd-link", ri,
            "%@");
    }
}

void sentinelDisconnectCallback(const redisAsyncContext *c, int status) {
    sentinelDisconnectInstanceFromContext(c);
}

/* Send the AUTH command with the specified master password if needed.
 * Note that for slaves the password set for the master is used.
 *
 * We don't check at all if the command was successfully transmitted
 * to the instance as if it fails Sentinel will detect the instance down,
 * will disconnect and reconnect the link and so forth. */
void sentinelSendAuthIfNeeded(sentinelRedisInstance *ri, redisAsyncContext *c) {
    char *auth_pass = (ri->flags & SRI_MASTER) ? ri->auth_pass :
                                                 ri->master->auth_pass;

    if (auth_pass)
        redisAsyncCommand(c, sentinelDiscardReplyCallback, NULL, "AUTH %s",
            auth_pass);
}

/* Create the async connections for the specified instance if the instance
 * is disconnected. Note that the SRI_DISCONNECTED flag is set even if just
 * one of the two links (commands and pub/sub) is missing. */
void sentinelReconnectInstance(sentinelRedisInstance *ri) {
    if (!(ri->flags & SRI_DISCONNECTED)) return;

    /* Commands connection. */
    if (ri->cc == NULL) {
        ri->cc = redisAsyncConnect(ri->addr->ip,ri->addr->port);
        if (ri->cc->err) {
            sentinelEvent(REDIS_DEBUG,"-cmd-link-reconnection",ri,"%@ #%s",
                ri->cc->errstr);
            sentinelKillLink(ri,ri->cc);
        } else {
            ri->cc_conn_time = mstime();
            ri->cc->data = ri;
            redisAeAttach(server.el,ri->cc);
            redisAsyncSetConnectCallback(ri->cc,
                                            sentinelLinkEstablishedCallback);
            redisAsyncSetDisconnectCallback(ri->cc,
                                            sentinelDisconnectCallback);
            sentinelSendAuthIfNeeded(ri,ri->cc);
        }
    }
    /* Pub / Sub */
    if ((ri->flags & SRI_MASTER) && ri->pc == NULL) {
        ri->pc = redisAsyncConnect(ri->addr->ip,ri->addr->port);
        if (ri->pc->err) {
            sentinelEvent(REDIS_DEBUG,"-pubsub-link-reconnection",ri,"%@ #%s",
                ri->pc->errstr);
            sentinelKillLink(ri,ri->pc);
        } else {
            int retval;

            ri->pc_conn_time = mstime();
            ri->pc->data = ri;
            redisAeAttach(server.el,ri->pc);
            redisAsyncSetConnectCallback(ri->pc,
                                            sentinelLinkEstablishedCallback);
            redisAsyncSetDisconnectCallback(ri->pc,
                                            sentinelDisconnectCallback);
            sentinelSendAuthIfNeeded(ri,ri->pc);
            /* Now we subscribe to the Sentinels "Hello" channel. */
            retval = redisAsyncCommand(ri->pc,
                sentinelReceiveHelloMessages, NULL, "SUBSCRIBE %s",
                    SENTINEL_HELLO_CHANNEL);
            if (retval != REDIS_OK) {
                /* If we can't subscribe, the Pub/Sub connection is useless
                 * and we can simply disconnect it and try again. */
                sentinelKillLink(ri,ri->pc);
                return;
            }
        }
    }
    /* Clear the DISCONNECTED flags only if we have both the connections
     * (or just the commands connection if this is a slave or a
     * sentinel instance). */
    if (ri->cc && (ri->flags & (SRI_SLAVE|SRI_SENTINEL) || ri->pc))
        ri->flags &= ~SRI_DISCONNECTED;
}

/* ======================== Redis instances pinging  ======================== */

/* Process the INFO output from masters. */
void sentinelRefreshInstanceInfo(sentinelRedisInstance *ri, const char *info) {
    sds *lines;
    int numlines, j;
    int role = 0;
    int runid_changed = 0;  /* true if runid changed. */
    int first_runid = 0;    /* true if this is the first runid we receive. */

    /* The following fields must be reset to a given value in the case they
     * are not found at all in the INFO output. */
    ri->master_link_down_time = 0;

    /* Process line by line. */
    lines = sdssplitlen(info,strlen(info),"\r\n",2,&numlines);
    for (j = 0; j < numlines; j++) {
        sentinelRedisInstance *slave;
        sds l = lines[j];

        /* run_id:<40 hex chars>*/
        if (sdslen(l) >= 47 && !memcmp(l,"run_id:",7)) {
            if (ri->runid == NULL) {
                ri->runid = sdsnewlen(l+7,40);
                first_runid = 1;
            } else {
                if (strncmp(ri->runid,l+7,40) != 0) {
                    runid_changed = 1;
                    sentinelEvent(REDIS_NOTICE,"+reboot",ri,"%@");
                    sdsfree(ri->runid);
                    ri->runid = sdsnewlen(l+7,40);
                }
            }
        }

        /* slave0:<ip>,<port>,<state> */
        if ((ri->flags & SRI_MASTER) &&
            sdslen(l) >= 7 &&
            !memcmp(l,"slave",5) && isdigit(l[5]))
        {
            char *ip, *port, *end;

            ip = strchr(l,':'); if (!ip) continue;
            ip++; /* Now ip points to start of ip address. */
            port = strchr(ip,','); if (!port) continue;
            *port = '\0'; /* nul term for easy access. */
            port++; /* Now port points to start of port number. */
            end = strchr(port,','); if (!end) continue;
            *end = '\0'; /* nul term for easy access. */

            /* Check if we already have this slave into our table,
             * otherwise add it. */
            if (sentinelRedisInstanceLookupSlave(ri,ip,atoi(port)) == NULL) {
                if ((slave = createSentinelRedisInstance(NULL,SRI_SLAVE,ip,
                            atoi(port), ri->quorum,ri)) != NULL)
                {
                    sentinelEvent(REDIS_NOTICE,"+slave",slave,"%@");
                }
            }
        }

        /* master_link_down_since_seconds:<seconds> */
        if (sdslen(l) >= 32 &&
            !memcmp(l,"master_link_down_since_seconds",30))
        {
            ri->master_link_down_time = strtoll(l+31,NULL,10)*1000;
        }

        /* role:<role> */
        if (!memcmp(l,"role:master",11)) role = SRI_MASTER;
        else if (!memcmp(l,"role:slave",10)) role = SRI_SLAVE;

        if (role == SRI_SLAVE) {
            /* master_host:<host> */
            if (sdslen(l) >= 12 && !memcmp(l,"master_host:",12)) {
                sdsfree(ri->slave_master_host);
                ri->slave_master_host = sdsnew(l+12);
            }

            /* master_port:<port> */
            if (sdslen(l) >= 12 && !memcmp(l,"master_port:",12))
                ri->slave_master_port = atoi(l+12);
            
            /* master_link_status:<status> */
            if (sdslen(l) >= 19 && !memcmp(l,"master_link_status:",19)) {
                ri->slave_master_link_status =
                    (strcasecmp(l+19,"up") == 0) ?
                    SENTINEL_MASTER_LINK_STATUS_UP :
                    SENTINEL_MASTER_LINK_STATUS_DOWN;
            }

            /* slave_priority:<priority> */
            if (sdslen(l) >= 15 && !memcmp(l,"slave_priority:",15))
                ri->slave_priority = atoi(l+15);
        }
    }
    ri->info_refresh = mstime();
    sdsfreesplitres(lines,numlines);

    /* ---------------------------- Acting half ----------------------------- */
    if (sentinel.tilt) return;

    /* Act if a master turned into a slave. */
    if ((ri->flags & SRI_MASTER) && role == SRI_SLAVE) {
        if ((first_runid || runid_changed) && ri->slave_master_host) {
            /* If it is the first time we receive INFO from it, but it's
             * a slave while it was configured as a master, we want to monitor
             * its master instead. */
            sentinelEvent(REDIS_WARNING,"+redirect-to-master",ri,
                "%s %s %d %s %d",
                ri->name, ri->addr->ip, ri->addr->port,
                ri->slave_master_host, ri->slave_master_port);
            sentinelResetMasterAndChangeAddress(ri,ri->slave_master_host,
                                                   ri->slave_master_port);
            return;
        }
    }

    /* Act if a slave turned into a master. */
    if ((ri->flags & SRI_SLAVE) && role == SRI_MASTER) {
        if (!(ri->master->flags & SRI_FAILOVER_IN_PROGRESS) &&
            (runid_changed || first_runid))
        {
            /* If a slave turned into master but:
             *
             * 1) Failover not in progress.
             * 2) RunID hs changed, or its the first time we see an INFO output.
             * 
             * We assume this is a reboot with a wrong configuration.
             * Log the event and remove the slave. */
            int retval;

            sentinelEvent(REDIS_WARNING,"-slave-restart-as-master",ri,"%@ #removing it from the attached slaves");
            retval = dictDelete(ri->master->slaves,ri->name);
            redisAssert(retval == REDIS_OK);
            return;
        } else if (ri->flags & SRI_PROMOTED) {
            /* If this is a promoted slave we can change state to the
             * failover state machine. */
            if ((ri->master->flags & SRI_FAILOVER_IN_PROGRESS) &&
                (ri->master->flags & SRI_I_AM_THE_LEADER) &&
                (ri->master->failover_state ==
                    SENTINEL_FAILOVER_STATE_WAIT_PROMOTION))
            {
                ri->master->failover_state = SENTINEL_FAILOVER_STATE_RECONF_SLAVES;
                ri->master->failover_state_change_time = mstime();
                sentinelEvent(REDIS_WARNING,"+promoted-slave",ri,"%@");
                sentinelEvent(REDIS_WARNING,"+failover-state-reconf-slaves",
                    ri->master,"%@");
                sentinelCallClientReconfScript(ri->master,SENTINEL_LEADER,
                    "start",ri->master->addr,ri->addr);
            }
        } else if (!(ri->master->flags & SRI_FAILOVER_IN_PROGRESS) ||
                    ((ri->master->flags & SRI_FAILOVER_IN_PROGRESS) &&
                     (ri->master->flags & SRI_I_AM_THE_LEADER) &&
                     ri->master->failover_state ==
                     SENTINEL_FAILOVER_STATE_WAIT_START))
        {
            /* No failover in progress? Then it is the start of a failover
             * and we are an observer.
             *
             * We also do that if we are a leader doing a failover, in wait
             * start, but well, somebody else started before us. */

            if (ri->master->flags & SRI_FAILOVER_IN_PROGRESS) {
                sentinelEvent(REDIS_WARNING,"-failover-abort-race",
                                ri->master, "%@");
                sentinelAbortFailover(ri->master);
            }

            ri->master->flags |= SRI_FAILOVER_IN_PROGRESS;
            sentinelEvent(REDIS_WARNING,"+failover-detected",ri->master,"%@");
            ri->master->failover_state = SENTINEL_FAILOVER_STATE_DETECT_END;
            ri->master->failover_state_change_time = mstime();
            ri->master->promoted_slave = ri;
            ri->flags |= SRI_PROMOTED;
            sentinelCallClientReconfScript(ri->master,SENTINEL_OBSERVER,
                "start", ri->master->addr,ri->addr);
            /* We are an observer, so we can only assume that the leader
             * is reconfiguring the slave instances. For this reason we
             * set all the instances as RECONF_SENT waiting for progresses
             * on this side. */
            sentinelAddFlagsToDictOfRedisInstances(ri->master->slaves,
                SRI_RECONF_SENT);
        }
    }

    /* Detect if the slave that is in the process of being reconfigured
     * changed state. */
    if ((ri->flags & SRI_SLAVE) && role == SRI_SLAVE &&
        (ri->flags & (SRI_RECONF_SENT|SRI_RECONF_INPROG)))
    {
        /* SRI_RECONF_SENT -> SRI_RECONF_INPROG. */
        if ((ri->flags & SRI_RECONF_SENT) &&
            ri->slave_master_host &&
            strcmp(ri->slave_master_host,
                    ri->master->promoted_slave->addr->ip) == 0 &&
            ri->slave_master_port == ri->master->promoted_slave->addr->port)
        {
            ri->flags &= ~SRI_RECONF_SENT;
            ri->flags |= SRI_RECONF_INPROG;
            sentinelEvent(REDIS_NOTICE,"+slave-reconf-inprog",ri,"%@");
        }

        /* SRI_RECONF_INPROG -> SRI_RECONF_DONE */
        if ((ri->flags & SRI_RECONF_INPROG) &&
            ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP)
        {
            ri->flags &= ~SRI_RECONF_INPROG;
            ri->flags |= SRI_RECONF_DONE;
            sentinelEvent(REDIS_NOTICE,"+slave-reconf-done",ri,"%@");
            /* If we are moving forward (a new slave is now configured)
             * we update the change_time as we are conceptually passing
             * to the next slave. */
            ri->failover_state_change_time = mstime();
        }
    }
}

void sentinelInfoReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    if (r->type == REDIS_REPLY_STRING) {
        sentinelRefreshInstanceInfo(ri,r->str);
    }
}

/* Just discard the reply. We use this when we are not monitoring the return
 * value of the command but its effects directly. */
void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;

    if (ri) ri->pending_commands--;
}

void sentinelPingReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    if (r->type == REDIS_REPLY_STATUS ||
        r->type == REDIS_REPLY_ERROR) {
        /* Update the "instance available" field only if this is an
         * acceptable reply. */
        if (strncmp(r->str,"PONG",4) == 0 ||
            strncmp(r->str,"LOADING",7) == 0 ||
            strncmp(r->str,"MASTERDOWN",10) == 0)
        {
            ri->last_avail_time = mstime();
        } else {
            /* Send a SCRIPT KILL command if the instance appears to be
             * down because of a busy script. */
            if (strncmp(r->str,"BUSY",4) == 0 &&
                (ri->flags & SRI_S_DOWN) &&
                !(ri->flags & SRI_SCRIPT_KILL_SENT))
            {
                redisAsyncCommand(ri->cc,
                    sentinelDiscardReplyCallback, NULL, "SCRIPT KILL");
                ri->flags |= SRI_SCRIPT_KILL_SENT;
            }
        }
    }
    ri->last_pong_time = mstime();
}

/* This is called when we get the reply about the PUBLISH command we send
 * to the master to advertise this sentinel. */
void sentinelPublishReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    /* Only update pub_time if we actually published our message. Otherwise
     * we'll retry against in 100 milliseconds. */
    if (r->type != REDIS_REPLY_ERROR)
        ri->last_pub_time = mstime();
}

/* This is our Pub/Sub callback for the Hello channel. It's useful in order
 * to discover other sentinels attached at the same master. */
void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (!reply || !ri) return;
    r = reply;

    /* Update the last activity in the pubsub channel. Note that since we
     * receive our messages as well this timestamp can be used to detect
     * if the link is probably disconnected even if it seems otherwise. */
    ri->pc_last_activity = mstime();
   
    /* Sanity check in the reply we expect, so that the code that follows
     * can avoid to check for details. */
    if (r->type != REDIS_REPLY_ARRAY ||
        r->elements != 3 ||
        r->element[0]->type != REDIS_REPLY_STRING ||
        r->element[1]->type != REDIS_REPLY_STRING ||
        r->element[2]->type != REDIS_REPLY_STRING ||
        strcmp(r->element[0]->str,"message") != 0) return;

    /* We are not interested in meeting ourselves */
    if (strstr(r->element[2]->str,server.runid) != NULL) return;

    {
        int numtokens, port, removed, canfailover;
        char **token = sdssplitlen(r->element[2]->str,
                                   r->element[2]->len,
                                   ":",1,&numtokens);
        sentinelRedisInstance *sentinel;

        if (numtokens == 4) {
            /* First, try to see if we already have this sentinel. */
            port = atoi(token[1]);
            canfailover = atoi(token[3]);
            sentinel = getSentinelRedisInstanceByAddrAndRunID(
                            ri->sentinels,token[0],port,token[2]);

            if (!sentinel) {
                /* If not, remove all the sentinels that have the same runid
                 * OR the same ip/port, because it's either a restart or a
                 * network topology change. */
                removed = removeMatchingSentinelsFromMaster(ri,token[0],port,
                                token[2]);
                if (removed) {
                    sentinelEvent(REDIS_NOTICE,"-dup-sentinel",ri,
                        "%@ #duplicate of %s:%d or %s",
                        token[0],port,token[2]);
                }

                /* Add the new sentinel. */
                sentinel = createSentinelRedisInstance(NULL,SRI_SENTINEL,
                                token[0],port,ri->quorum,ri);
                if (sentinel) {
                    sentinelEvent(REDIS_NOTICE,"+sentinel",sentinel,"%@");
                    /* The runid is NULL after a new instance creation and
                     * for Sentinels we don't have a later chance to fill it,
                     * so do it now. */
                    sentinel->runid = sdsnew(token[2]);
                }
            }

            /* Update the state of the Sentinel. */
            if (sentinel) {
                sentinel->last_hello_time = mstime();
                if (canfailover)
                    sentinel->flags |= SRI_CAN_FAILOVER;
                else
                    sentinel->flags &= ~SRI_CAN_FAILOVER;
            }
        }
        sdsfreesplitres(token,numtokens);
    }
}

void sentinelPingInstance(sentinelRedisInstance *ri) {
    mstime_t now = mstime();
    mstime_t info_period;
    int retval;

    /* Return ASAP if we have already a PING or INFO already pending, or
     * in the case the instance is not properly connected. */
    if (ri->flags & SRI_DISCONNECTED) return;

    /* For INFO, PING, PUBLISH that are not critical commands to send we
     * also have a limit of SENTINEL_MAX_PENDING_COMMANDS. We don't
     * want to use a lot of memory just because a link is not working
     * properly (note that anyway there is a redundant protection about this,
     * that is, the link will be disconnected and reconnected if a long
     * timeout condition is detected. */
    if (ri->pending_commands >= SENTINEL_MAX_PENDING_COMMANDS) return;

    /* If this is a slave of a master in O_DOWN condition we start sending
     * it INFO every second, instead of the usual SENTINEL_INFO_PERIOD
     * period. In this state we want to closely monitor slaves in case they
     * are turned into masters by another Sentinel, or by the sysadmin. */
    if ((ri->flags & SRI_SLAVE) &&
        (ri->master->flags & (SRI_O_DOWN|SRI_FAILOVER_IN_PROGRESS))) {
        info_period = 1000;
    } else {
        info_period = SENTINEL_INFO_PERIOD;
    }

    if ((ri->flags & SRI_SENTINEL) == 0 &&
        (ri->info_refresh == 0 ||
        (now - ri->info_refresh) > info_period))
    {
        /* Send INFO to masters and slaves, not sentinels. */
        retval = redisAsyncCommand(ri->cc,
            sentinelInfoReplyCallback, NULL, "INFO");
        if (retval != REDIS_OK) return;
        ri->pending_commands++;
    } else if ((now - ri->last_pong_time) > SENTINEL_PING_PERIOD) {
        /* Send PING to all the three kinds of instances. */
        retval = redisAsyncCommand(ri->cc,
            sentinelPingReplyCallback, NULL, "PING");
        if (retval != REDIS_OK) return;
        ri->pending_commands++;
    } else if ((ri->flags & SRI_MASTER) &&
               (now - ri->last_pub_time) > SENTINEL_PUBLISH_PERIOD)
    {
        /* PUBLISH hello messages only to masters. */
        struct sockaddr_in sa;
        socklen_t salen = sizeof(sa);

        if (getsockname(ri->cc->c.fd,(struct sockaddr*)&sa,&salen) != -1) {
            char myaddr[128];

            snprintf(myaddr,sizeof(myaddr),"%s:%d:%s:%d",
                inet_ntoa(sa.sin_addr), server.port, server.runid,
                (ri->flags & SRI_CAN_FAILOVER) != 0);
            retval = redisAsyncCommand(ri->cc,
                sentinelPublishReplyCallback, NULL, "PUBLISH %s %s",
                    SENTINEL_HELLO_CHANNEL,myaddr);
            if (retval != REDIS_OK) return;
            ri->pending_commands++;
        }
    }
}

/* =========================== SENTINEL command ============================= */

const char *sentinelFailoverStateStr(int state) {
    switch(state) {
    case SENTINEL_FAILOVER_STATE_NONE: return "none";
    case SENTINEL_FAILOVER_STATE_WAIT_START: return "wait_start";
    case SENTINEL_FAILOVER_STATE_SELECT_SLAVE: return "select_slave";
    case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE: return "send_slaveof_noone";
    case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION: return "wait_promotion";
    case SENTINEL_FAILOVER_STATE_RECONF_SLAVES: return "reconf_slaves";
    case SENTINEL_FAILOVER_STATE_ALERT_CLIENTS: return "alert_clients";
    case SENTINEL_FAILOVER_STATE_DETECT_END: return "detect_end";
    case SENTINEL_FAILOVER_STATE_UPDATE_CONFIG: return "update_config";
    default: return "unknown";
    }
}

/* Redis instance to Redis protocol representation. */
void addReplySentinelRedisInstance(redisClient *c, sentinelRedisInstance *ri) {
    char *flags = sdsempty();
    void *mbl;
    int fields = 0;

    mbl = addDeferredMultiBulkLength(c);

    addReplyBulkCString(c,"name");
    addReplyBulkCString(c,ri->name);
    fields++;

    addReplyBulkCString(c,"ip");
    addReplyBulkCString(c,ri->addr->ip);
    fields++;

    addReplyBulkCString(c,"port");
    addReplyBulkLongLong(c,ri->addr->port);
    fields++;

    addReplyBulkCString(c,"runid");
    addReplyBulkCString(c,ri->runid ? ri->runid : "");
    fields++;

    addReplyBulkCString(c,"flags");
    if (ri->flags & SRI_S_DOWN) flags = sdscat(flags,"s_down,");
    if (ri->flags & SRI_O_DOWN) flags = sdscat(flags,"o_down,");
    if (ri->flags & SRI_MASTER) flags = sdscat(flags,"master,");
    if (ri->flags & SRI_SLAVE) flags = sdscat(flags,"slave,");
    if (ri->flags & SRI_SENTINEL) flags = sdscat(flags,"sentinel,");
    if (ri->flags & SRI_DISCONNECTED) flags = sdscat(flags,"disconnected,");
    if (ri->flags & SRI_MASTER_DOWN) flags = sdscat(flags,"master_down,");
    if (ri->flags & SRI_FAILOVER_IN_PROGRESS)
        flags = sdscat(flags,"failover_in_progress,");
    if (ri->flags & SRI_I_AM_THE_LEADER)
        flags = sdscat(flags,"i_am_the_leader,");
    if (ri->flags & SRI_PROMOTED) flags = sdscat(flags,"promoted,");
    if (ri->flags & SRI_RECONF_SENT) flags = sdscat(flags,"reconf_sent,");
    if (ri->flags & SRI_RECONF_INPROG) flags = sdscat(flags,"reconf_inprog,");
    if (ri->flags & SRI_RECONF_DONE) flags = sdscat(flags,"reconf_done,");

    if (sdslen(flags) != 0) flags = sdsrange(flags,0,-2); /* remove last "," */
    addReplyBulkCString(c,flags);
    sdsfree(flags);
    fields++;

    addReplyBulkCString(c,"pending-commands");
    addReplyBulkLongLong(c,ri->pending_commands);
    fields++;

    if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
        addReplyBulkCString(c,"failover-state");
        addReplyBulkCString(c,(char*)sentinelFailoverStateStr(ri->failover_state));
        fields++;
    }

    addReplyBulkCString(c,"last-ok-ping-reply");
    addReplyBulkLongLong(c,mstime() - ri->last_avail_time);
    fields++;

    addReplyBulkCString(c,"last-ping-reply");
    addReplyBulkLongLong(c,mstime() - ri->last_pong_time);
    fields++;

    if (ri->flags & SRI_S_DOWN) {
        addReplyBulkCString(c,"s-down-time");
        addReplyBulkLongLong(c,mstime()-ri->s_down_since_time);
        fields++;
    }

    if (ri->flags & SRI_O_DOWN) {
        addReplyBulkCString(c,"o-down-time");
        addReplyBulkLongLong(c,mstime()-ri->o_down_since_time);
        fields++;
    }

    /* Masters and Slaves */
    if (ri->flags & (SRI_MASTER|SRI_SLAVE)) {
        addReplyBulkCString(c,"info-refresh");
        addReplyBulkLongLong(c,mstime() - ri->info_refresh);
        fields++;
    }

    /* Only masters */
    if (ri->flags & SRI_MASTER) {
        addReplyBulkCString(c,"num-slaves");
        addReplyBulkLongLong(c,dictSize(ri->slaves));
        fields++;

        addReplyBulkCString(c,"num-other-sentinels");
        addReplyBulkLongLong(c,dictSize(ri->sentinels));
        fields++;

        addReplyBulkCString(c,"quorum");
        addReplyBulkLongLong(c,ri->quorum);
        fields++;
    }

    /* Only slaves */
    if (ri->flags & SRI_SLAVE) {
        addReplyBulkCString(c,"master-link-down-time");
        addReplyBulkLongLong(c,ri->master_link_down_time);
        fields++;

        addReplyBulkCString(c,"master-link-status");
        addReplyBulkCString(c,
            (ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP) ?
            "ok" : "err");
        fields++;

        addReplyBulkCString(c,"master-host");
        addReplyBulkCString(c,
            ri->slave_master_host ? ri->slave_master_host : "?");
        fields++;

        addReplyBulkCString(c,"master-port");
        addReplyBulkLongLong(c,ri->slave_master_port);
        fields++;

        addReplyBulkCString(c,"slave-priority");
        addReplyBulkLongLong(c,ri->slave_priority);
        fields++;
    }

    /* Only sentinels */
    if (ri->flags & SRI_SENTINEL) {
        addReplyBulkCString(c,"last-hello-message");
        addReplyBulkLongLong(c,mstime() - ri->last_hello_time);
        fields++;

        addReplyBulkCString(c,"can-failover-its-master");
        addReplyBulkLongLong(c,(ri->flags & SRI_CAN_FAILOVER) != 0);
        fields++;

        if (ri->flags & SRI_MASTER_DOWN) {
            addReplyBulkCString(c,"subjective-leader");
            addReplyBulkCString(c,ri->leader ? ri->leader : "?");
            fields++;
        }
    }

    setDeferredMultiBulkLength(c,mbl,fields*2);
}

/* Output a number of instances contained inside a dictionary as
 * Redis protocol. */
void addReplyDictOfRedisInstances(redisClient *c, dict *instances) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    addReplyMultiBulkLen(c,dictSize(instances));
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        addReplySentinelRedisInstance(c,ri);
    }
    dictReleaseIterator(di);
}

/* Lookup the named master into sentinel.masters.
 * If the master is not found reply to the client with an error and returns
 * NULL. */
sentinelRedisInstance *sentinelGetMasterByNameOrReplyError(redisClient *c,
                        robj *name)
{
    sentinelRedisInstance *ri;

    ri = dictFetchValue(sentinel.masters,c->argv[2]->ptr);
    if (!ri) {
        addReplyError(c,"No such master with that name");
        return NULL;
    }
    return ri;
}

void sentinelCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"masters")) {
        /* SENTINEL MASTERS */
        if (c->argc != 2) goto numargserr;

        addReplyDictOfRedisInstances(c,sentinel.masters);
    } else if (!strcasecmp(c->argv[1]->ptr,"slaves")) {
        /* SENTINEL SLAVES <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        addReplyDictOfRedisInstances(c,ri->slaves);
    } else if (!strcasecmp(c->argv[1]->ptr,"sentinels")) {
        /* SENTINEL SENTINELS <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        addReplyDictOfRedisInstances(c,ri->sentinels);
    } else if (!strcasecmp(c->argv[1]->ptr,"is-master-down-by-addr")) {
        /* SENTINEL IS-MASTER-DOWN-BY-ADDR <ip> <port> */
        sentinelRedisInstance *ri;
        char *leader = NULL;
        long port;
        int isdown = 0;

        if (c->argc != 4) goto numargserr;
        if (getLongFromObjectOrReply(c,c->argv[3],&port,NULL) != REDIS_OK)
            return;
        ri = getSentinelRedisInstanceByAddrAndRunID(sentinel.masters,
            c->argv[2]->ptr,port,NULL);

        /* It exists? Is actually a master? Is subjectively down? It's down.
         * Note: if we are in tilt mode we always reply with "0". */
        if (!sentinel.tilt && ri && (ri->flags & SRI_S_DOWN) &&
                                    (ri->flags & SRI_MASTER))
            isdown = 1;
        if (ri) leader = sentinelGetSubjectiveLeader(ri);

        /* Reply with a two-elements multi-bulk reply: down state, leader. */
        addReplyMultiBulkLen(c,2);
        addReply(c, isdown ? shared.cone : shared.czero);
        addReplyBulkCString(c, leader ? leader : "?");
        if (leader) sdsfree(leader);
    } else if (!strcasecmp(c->argv[1]->ptr,"reset")) {
        /* SENTINEL RESET <pattern> */
        if (c->argc != 3) goto numargserr;
        addReplyLongLong(c,sentinelResetMastersByPattern(c->argv[2]->ptr,SENTINEL_GENERATE_EVENT));
    } else if (!strcasecmp(c->argv[1]->ptr,"get-master-addr-by-name")) {
        /* SENTINEL GET-MASTER-ADDR-BY-NAME <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        ri = sentinelGetMasterByName(c->argv[2]->ptr);
        if (ri == NULL) {
            addReply(c,shared.nullmultibulk);
        } else if (ri->info_refresh == 0) {
            addReplySds(c,sdsnew("-IDONTKNOW I have not enough information to reply. Please ask another Sentinel.\r\n"));
        } else {
            sentinelAddr *addr = ri->addr;

            /* If we are in the middle of a failover, and the slave was
             * already successfully switched to master role, we can advertise
             * the new address as slave in order to allow clients to talk
             * with the new master ASAP. */
            if ((ri->flags & SRI_FAILOVER_IN_PROGRESS) &&
                ri->promoted_slave &&
                ri->failover_state >= SENTINEL_FAILOVER_STATE_RECONF_SLAVES)
            {
                addr = ri->promoted_slave->addr;
            }
            addReplyMultiBulkLen(c,2);
            addReplyBulkCString(c,addr->ip);
            addReplyBulkLongLong(c,addr->port);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"failover")) {
        /* SENTINEL FAILOVER <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
            addReplySds(c,sdsnew("-INPROG Failover already in progress\r\n"));
            return;
        }
        if (sentinelSelectSlave(ri) == NULL) {
            addReplySds(c,sdsnew("-NOGOODSLAVE No suitable slave to promote\r\n"));
            return;
        }
        sentinelStartFailover(ri,SENTINEL_FAILOVER_STATE_WAIT_START);
        ri->flags |= SRI_FORCE_FAILOVER;
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"pending-scripts")) {
        /* SENTINEL PENDING-SCRIPTS */

        if (c->argc != 2) goto numargserr;
        sentinelPendingScriptsCommand(c);
    } else {
        addReplyErrorFormat(c,"Unknown sentinel subcommand '%s'",
                               (char*)c->argv[1]->ptr);
    }
    return;

numargserr:
    addReplyErrorFormat(c,"Wrong number of commands for 'sentinel %s'",
                          (char*)c->argv[1]->ptr);
}

void sentinelInfoCommand(redisClient *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";
    sds info = sdsempty();
    int defsections = !strcasecmp(section,"default");
    int sections = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    }

    if (!strcasecmp(section,"server") || defsections) {
        if (sections++) info = sdscat(info,"\r\n");
        sds serversection = genRedisInfoString("server");
        info = sdscatlen(info,serversection,sdslen(serversection));
        sdsfree(serversection);
    }

    if (!strcasecmp(section,"sentinel") || defsections) {
        dictIterator *di;
        dictEntry *de;
        int master_id = 0;

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Sentinel\r\n"
            "sentinel_masters:%lu\r\n"
            "sentinel_tilt:%d\r\n"
            "sentinel_running_scripts:%d\r\n"
            "sentinel_scripts_queue_length:%ld\r\n",
            dictSize(sentinel.masters),
            sentinel.tilt,
            sentinel.running_scripts,
            listLength(sentinel.scripts_queue));

        di = dictGetIterator(sentinel.masters);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            char *status = "ok";

            if (ri->flags & SRI_O_DOWN) status = "odown";
            else if (ri->flags & SRI_S_DOWN) status = "sdown";
            info = sdscatprintf(info,
                "master%d:name=%s,status=%s,address=%s:%d,"
                "slaves=%lu,sentinels=%lu\r\n",
                master_id++, ri->name, status,
                ri->addr->ip, ri->addr->port,
                dictSize(ri->slaves),
                dictSize(ri->sentinels)+1);
        }
        dictReleaseIterator(di);
    }

    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}

/* ===================== SENTINEL availability checks ======================= */

/* Is this instance down from our point of view? */
void sentinelCheckSubjectivelyDown(sentinelRedisInstance *ri) {
    mstime_t elapsed = mstime() - ri->last_avail_time;

    /* Check if we are in need for a reconnection of one of the 
     * links, because we are detecting low activity.
     *
     * 1) Check if the command link seems connected, was connected not less
     *    than SENTINEL_MIN_LINK_RECONNECT_PERIOD, but still we have an
     *    idle time that is greater than down_after_period / 2 seconds. */
    if (ri->cc &&
        (mstime() - ri->cc_conn_time) > SENTINEL_MIN_LINK_RECONNECT_PERIOD &&
        (mstime() - ri->last_pong_time) > (ri->down_after_period/2))
    {
        sentinelKillLink(ri,ri->cc);
    }

    /* 2) Check if the pubsub link seems connected, was connected not less
     *    than SENTINEL_MIN_LINK_RECONNECT_PERIOD, but still we have no
     *    activity in the Pub/Sub channel for more than
     *    SENTINEL_PUBLISH_PERIOD * 3.
     */
    if (ri->pc &&
        (mstime() - ri->pc_conn_time) > SENTINEL_MIN_LINK_RECONNECT_PERIOD &&
        (mstime() - ri->pc_last_activity) > (SENTINEL_PUBLISH_PERIOD*3))
    {
        sentinelKillLink(ri,ri->pc);
    }

    /* Update the subjectively down flag. */
    if (elapsed > ri->down_after_period) {
        /* Is subjectively down */
        if ((ri->flags & SRI_S_DOWN) == 0) {
            sentinelEvent(REDIS_WARNING,"+sdown",ri,"%@");
            ri->s_down_since_time = mstime();
            ri->flags |= SRI_S_DOWN;
        }
    } else {
        /* Is subjectively up */
        if (ri->flags & SRI_S_DOWN) {
            sentinelEvent(REDIS_WARNING,"-sdown",ri,"%@");
            ri->flags &= ~(SRI_S_DOWN|SRI_SCRIPT_KILL_SENT);
        }
    }
}

/* Is this instance down accordingly to the configured quorum? */
void sentinelCheckObjectivelyDown(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int quorum = 0, odown = 0;

    if (master->flags & SRI_S_DOWN) {
        /* Is down for enough sentinels? */
        quorum = 1; /* the current sentinel. */
        /* Count all the other sentinels. */
        di = dictGetIterator(master->sentinels);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);

            if (ri->flags & SRI_MASTER_DOWN) quorum++;
        }
        dictReleaseIterator(di);
        if (quorum >= master->quorum) odown = 1;
    }

    /* Set the flag accordingly to the outcome. */
    if (odown) {
        if ((master->flags & SRI_O_DOWN) == 0) {
            sentinelEvent(REDIS_WARNING,"+odown",master,"%@ #quorum %d/%d",
                quorum, master->quorum);
            master->flags |= SRI_O_DOWN;
            master->o_down_since_time = mstime();
        }
    } else {
        if (master->flags & SRI_O_DOWN) {
            sentinelEvent(REDIS_WARNING,"-odown",master,"%@");
            master->flags &= ~SRI_O_DOWN;
        }
    }
}

/* Receive the SENTINEL is-master-down-by-addr reply, see the
 * sentinelAskMasterStateToOtherSentinels() function for more information. */
void sentinelReceiveIsMasterDownReply(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = c->data;
    redisReply *r;

    if (ri) ri->pending_commands--;
    if (!reply || !ri) return;
    r = reply;

    /* Ignore every error or unexpected reply.
     * Note that if the command returns an error for any reason we'll
     * end clearing the SRI_MASTER_DOWN flag for timeout anyway. */
    if (r->type == REDIS_REPLY_ARRAY && r->elements == 2 &&
        r->element[0]->type == REDIS_REPLY_INTEGER &&
        r->element[1]->type == REDIS_REPLY_STRING)
    {
        ri->last_master_down_reply_time = mstime();
        if (r->element[0]->integer == 1) {
            ri->flags |= SRI_MASTER_DOWN;
        } else {
            ri->flags &= ~SRI_MASTER_DOWN;
        }
        sdsfree(ri->leader);
        ri->leader = sdsnew(r->element[1]->str);
    }
}

/* If we think (subjectively) the master is down, we start sending
 * SENTINEL IS-MASTER-DOWN-BY-ADDR requests to other sentinels
 * in order to get the replies that allow to reach the quorum and
 * possibly also mark the master as objectively down. */
void sentinelAskMasterStateToOtherSentinels(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(master->sentinels);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        mstime_t elapsed = mstime() - ri->last_master_down_reply_time;
        char port[32];
        int retval;

        /* If the master state from other sentinel is too old, we clear it. */
        if (elapsed > SENTINEL_INFO_VALIDITY_TIME) {
            ri->flags &= ~SRI_MASTER_DOWN;
            sdsfree(ri->leader);
            ri->leader = NULL;
        }

        /* Only ask if master is down to other sentinels if:
         *
         * 1) We believe it is down, or there is a failover in progress.
         * 2) Sentinel is connected.
         * 3) We did not received the info within SENTINEL_ASK_PERIOD ms. */
        if ((master->flags & (SRI_S_DOWN|SRI_FAILOVER_IN_PROGRESS)) == 0)
            continue;
        if (ri->flags & SRI_DISCONNECTED) continue;
        if (mstime() - ri->last_master_down_reply_time < SENTINEL_ASK_PERIOD)
            continue;

        /* Ask */
        ll2string(port,sizeof(port),master->addr->port);
        retval = redisAsyncCommand(ri->cc,
                    sentinelReceiveIsMasterDownReply, NULL,
                    "SENTINEL is-master-down-by-addr %s %s",
                    master->addr->ip, port);
        if (retval == REDIS_OK) ri->pending_commands++;
    }
    dictReleaseIterator(di);
}

/* =============================== FAILOVER ================================= */

/* Given a master get the "subjective leader", that is, among all the sentinels
 * with given characteristics, the one with the lexicographically smaller
 * runid. The characteristics required are:
 *
 * 1) Has SRI_CAN_FAILOVER flag.
 * 2) Is not disconnected.
 * 3) Recently answered to our ping (no longer than
 *    SENTINEL_INFO_VALIDITY_TIME milliseconds ago).
 *
 * The function returns a pointer to an sds string representing the runid of the
 * leader sentinel instance (from our point of view). Otherwise NULL is
 * returned if there are no suitable sentinels.
 */

int compareRunID(const void *a, const void *b) {
    char **aptrptr = (char**)a, **bptrptr = (char**)b;
    return strcasecmp(*aptrptr, *bptrptr);
}

char *sentinelGetSubjectiveLeader(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    char **instance =
        zmalloc(sizeof(char*)*(dictSize(master->sentinels)+1));
    int instances = 0;
    char *leader = NULL;

    if (master->flags & SRI_CAN_FAILOVER) {
        /* Add myself if I'm a Sentinel that can failover this master. */
        instance[instances++] = server.runid;
    }

    di = dictGetIterator(master->sentinels);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        mstime_t lag = mstime() - ri->last_avail_time;

        if (lag > SENTINEL_INFO_VALIDITY_TIME ||
            !(ri->flags & SRI_CAN_FAILOVER) ||
            (ri->flags & SRI_DISCONNECTED) ||
            ri->runid == NULL)
            continue;
        instance[instances++] = ri->runid;
    }
    dictReleaseIterator(di);

    /* If we have at least one instance passing our checks, order the array
     * by runid. */
    if (instances) {
        qsort(instance,instances,sizeof(char*),compareRunID);
        leader = sdsnew(instance[0]);
    }
    zfree(instance);
    return leader;
}

struct sentinelLeader {
    char *runid;
    unsigned long votes;
};

/* Helper function for sentinelGetObjectiveLeader, increment the counter
 * relative to the specified runid. */
void sentinelObjectiveLeaderIncr(dict *counters, char *runid) {
    dictEntry *de = dictFind(counters,runid);
    uint64_t oldval;

    if (de) {
        oldval = dictGetUnsignedIntegerVal(de);
        dictSetUnsignedIntegerVal(de,oldval+1);
    } else {
        de = dictAddRaw(counters,runid);
        redisAssert(de != NULL);
        dictSetUnsignedIntegerVal(de,1);
    }
}

/* Scan all the Sentinels attached to this master to check what is the
 * most voted leader among Sentinels. */
char *sentinelGetObjectiveLeader(sentinelRedisInstance *master) {
    dict *counters;
    dictIterator *di;
    dictEntry *de;
    unsigned int voters = 0, voters_quorum;
    char *myvote;
    char *winner = NULL;

    redisAssert(master->flags & (SRI_O_DOWN|SRI_FAILOVER_IN_PROGRESS));
    counters = dictCreate(&leaderVotesDictType,NULL);

    /* Count my vote. */
    myvote = sentinelGetSubjectiveLeader(master);
    if (myvote) {
        sentinelObjectiveLeaderIncr(counters,myvote);
        voters++;
    }

    /* Count other sentinels votes */
    di = dictGetIterator(master->sentinels);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        if (ri->leader == NULL) continue;
        /* If the failover is not already in progress we are only interested
         * in Sentinels that believe the master is down. Otherwise the leader
         * selection is useful for the "failover-takedown" when the original
         * leader fails. In that case we consider all the voters. */
        if (!(master->flags & SRI_FAILOVER_IN_PROGRESS) &&
            !(ri->flags & SRI_MASTER_DOWN)) continue;
        sentinelObjectiveLeaderIncr(counters,ri->leader);
        voters++;
    }
    dictReleaseIterator(di);
    voters_quorum = voters/2+1;

    /* Check what's the winner. For the winner to win, it needs two conditions:
     * 1) Absolute majority between voters (50% + 1).
     * 2) And anyway at least master->quorum votes. */
    {
        uint64_t max_votes = 0; /* Max votes so far. */

        di = dictGetIterator(counters);
        while((de = dictNext(di)) != NULL) {
            uint64_t votes = dictGetUnsignedIntegerVal(de);

            if (max_votes < votes) {
                max_votes = votes;
                winner = dictGetKey(de);
            }
        }
        dictReleaseIterator(di);
        if (winner && (max_votes < voters_quorum || max_votes < master->quorum))
            winner = NULL;
    }
    winner = winner ? sdsnew(winner) : NULL;
    sdsfree(myvote);
    dictRelease(counters);
    return winner;
}

/* Setup the master state to start a failover as a leader.
 *
 * State can be either:
 *
 * SENTINEL_FAILOVER_STATE_WAIT_START: starts a failover from scratch.
 * SENTINEL_FAILOVER_STATE_RECONF_SLAVES: takedown a failed failover.
 */
void sentinelStartFailover(sentinelRedisInstance *master, int state) {
    redisAssert(master->flags & SRI_MASTER);
    redisAssert(state == SENTINEL_FAILOVER_STATE_WAIT_START ||
                state == SENTINEL_FAILOVER_STATE_RECONF_SLAVES);

    master->failover_state = state;
    master->flags |= SRI_FAILOVER_IN_PROGRESS|SRI_I_AM_THE_LEADER;
    sentinelEvent(REDIS_WARNING,"+failover-triggered",master,"%@");

    /* Pick a random delay if it's a fresh failover (WAIT_START), and not
     * a recovery of a failover started by another sentinel. */
    if (master->failover_state == SENTINEL_FAILOVER_STATE_WAIT_START) {
        master->failover_start_time = mstime() +
            SENTINEL_FAILOVER_FIXED_DELAY +
            (rand() % SENTINEL_FAILOVER_MAX_RANDOM_DELAY);
        sentinelEvent(REDIS_WARNING,"+failover-state-wait-start",master,
            "%@ #starting in %lld milliseconds",
            master->failover_start_time-mstime());
    }
    master->failover_state_change_time = mstime();
}

/* This function checks if there are the conditions to start the failover,
 * that is:
 *
 * 1) Enough time has passed since O_DOWN.
 * 2) The master is marked as SRI_CAN_FAILOVER, so we can failover it.
 * 3) We are the objectively leader for this master.
 *
 * If the conditions are met we flag the master as SRI_FAILOVER_IN_PROGRESS
 * and SRI_I_AM_THE_LEADER.
 */
void sentinelStartFailoverIfNeeded(sentinelRedisInstance *master) {
    char *leader;
    int isleader;

    /* We can't failover if the master is not in O_DOWN state or if
     * there is not already a failover in progress (to perform the
     * takedown if the leader died) or if this Sentinel is not allowed
     * to start a failover. */
    if (!(master->flags & SRI_CAN_FAILOVER) ||
        !(master->flags & (SRI_O_DOWN|SRI_FAILOVER_IN_PROGRESS))) return;

    leader = sentinelGetObjectiveLeader(master);
    isleader = leader && strcasecmp(leader,server.runid) == 0;
    sdsfree(leader);

    /* If I'm not the leader, I can't failover for sure. */
    if (!isleader) return;

    /* If the failover is already in progress there are two options... */
    if (master->flags & SRI_FAILOVER_IN_PROGRESS) {
        if (master->flags & SRI_I_AM_THE_LEADER) {
            /* 1) I'm flagged as leader so I already started the failover.
             *    Just return. */
            return;
        } else {
            mstime_t elapsed = mstime() - master->failover_state_change_time;

            /* 2) I'm the new leader, but I'm not flagged as leader in the
             *    master: I did not started the failover, but the original
             *    leader has no longer the leadership.
             *
             *    In this case if the failover appears to be lagging
             *    for at least 25% of the configured failover timeout,
             *    I can assume I can take control. Otherwise
             *    it's better to return and wait more. */
            if (elapsed < (master->failover_timeout/4)) return;
            sentinelEvent(REDIS_WARNING,"+failover-takedown",master,"%@");
            /* We have already an elected slave if we are in
             * FAILOVER_IN_PROGRESS state, that is, the slave that we
             * observed turning into a master. */
            sentinelStartFailover(master,SENTINEL_FAILOVER_STATE_RECONF_SLAVES);
            /* As an observer we flagged all the slaves as RECONF_SENT but
             * now we are in charge of actually sending the reconfiguration
             * command so let's clear this flag for all the instances. */
            sentinelDelFlagsToDictOfRedisInstances(master->slaves,
                SRI_RECONF_SENT);
        }
    } else {
        /* Brand new failover as SRI_FAILOVER_IN_PROGRESS was not set.
         *
         * Do we have a slave to promote? Otherwise don't start a failover
         * at all. */
        if (sentinelSelectSlave(master) == NULL) return;
        sentinelStartFailover(master,SENTINEL_FAILOVER_STATE_WAIT_START);
    }
}

/* Select a suitable slave to promote. The current algorithm only uses
 * the following parameters:
 *
 * 1) None of the following conditions: S_DOWN, O_DOWN, DISCONNECTED.
 * 2) last_avail_time more recent than SENTINEL_INFO_VALIDITY_TIME.
 * 3) info_refresh more recent than SENTINEL_INFO_VALIDITY_TIME.
 * 4) master_link_down_time no more than:
 *     (now - master->s_down_since_time) + (master->down_after_period * 10).
 * 5) Slave priority can't be zero, otherwise the slave is discarded.
 *
 * Among all the slaves matching the above conditions we select the slave
 * with lower slave_priority. If priority is the same we select the slave
 * with lexicographically smaller runid.
 *
 * The function returns the pointer to the selected slave, otherwise
 * NULL if no suitable slave was found.
 */

int compareSlavesForPromotion(const void *a, const void *b) {
    sentinelRedisInstance **sa = (sentinelRedisInstance **)a,
                          **sb = (sentinelRedisInstance **)b;
    char *sa_runid, *sb_runid;

    if ((*sa)->slave_priority != (*sb)->slave_priority)
        return (*sa)->slave_priority - (*sb)->slave_priority;

    /* If priority is the same, select the slave with that has the
     * lexicographically smaller runid. Note that we try to handle runid
     * == NULL as there are old Redis versions that don't publish runid in
     * INFO. A NULL runid is considered bigger than any other runid. */
    sa_runid = (*sa)->runid;
    sb_runid = (*sb)->runid;
    if (sa_runid == NULL && sb_runid == NULL) return 0;
    else if (sa_runid == NULL) return 1;  /* a > b */
    else if (sb_runid == NULL) return -1; /* a < b */
    return strcasecmp(sa_runid, sb_runid);
}

sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master) {
    sentinelRedisInstance **instance =
        zmalloc(sizeof(instance[0])*dictSize(master->slaves));
    sentinelRedisInstance *selected = NULL;
    int instances = 0;
    dictIterator *di;
    dictEntry *de;
    mstime_t max_master_down_time = 0;

    if (master->flags & SRI_S_DOWN)
        max_master_down_time += mstime() - master->s_down_since_time;
    max_master_down_time += master->down_after_period * 10;

    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        mstime_t info_validity_time = mstime()-SENTINEL_INFO_VALIDITY_TIME;

        if (slave->flags & (SRI_S_DOWN|SRI_O_DOWN|SRI_DISCONNECTED)) continue;
        if (slave->last_avail_time < info_validity_time) continue;
        if (slave->slave_priority == 0) continue;

        /* If the master is in SDOWN state we get INFO for slaves every second.
         * Otherwise we get it with the usual period so we need to account for
         * a larger delay. */
        if ((master->flags & SRI_S_DOWN) == 0)
            info_validity_time -= SENTINEL_INFO_PERIOD;
        if (slave->info_refresh < info_validity_time) continue;
        if (slave->master_link_down_time > max_master_down_time) continue;
        instance[instances++] = slave;
    }
    dictReleaseIterator(di);
    if (instances) {
        qsort(instance,instances,sizeof(sentinelRedisInstance*),
            compareSlavesForPromotion);
        selected = instance[0];
    }
    zfree(instance);
    return selected;
}

/* ---------------- Failover state machine implementation ------------------- */
void sentinelFailoverWaitStart(sentinelRedisInstance *ri) {
    /* If we in "wait start" but the master is no longer in ODOWN nor in
     * SDOWN condition we abort the failover. This is important as it
     * prevents a useless failover in a a notable case of netsplit, where
     * the sentinels are split from the redis instances. In this case
     * the failover will not start while there is the split because no
     * good slave can be reached. However when the split is resolved, we
     * can go to waitstart if the slave is back reachable a few milliseconds
     * before the master is. In that case when the master is back online
     * we cancel the failover. */
    if ((ri->flags & (SRI_S_DOWN|SRI_O_DOWN|SRI_FORCE_FAILOVER)) == 0) {
        sentinelEvent(REDIS_WARNING,"-failover-abort-master-is-back",
            ri,"%@");
        sentinelAbortFailover(ri);
        return;
    }

    /* Start the failover going to the next state if enough time has
     * elapsed. */
    if (mstime() >= ri->failover_start_time) {
        ri->failover_state = SENTINEL_FAILOVER_STATE_SELECT_SLAVE;
        ri->failover_state_change_time = mstime();
        sentinelEvent(REDIS_WARNING,"+failover-state-select-slave",ri,"%@");
    }
}

void sentinelFailoverSelectSlave(sentinelRedisInstance *ri) {
    sentinelRedisInstance *slave = sentinelSelectSlave(ri);

    if (slave == NULL) {
        sentinelEvent(REDIS_WARNING,"-failover-abort-no-good-slave",ri,"%@");
        sentinelAbortFailover(ri);
    } else {
        sentinelEvent(REDIS_WARNING,"+selected-slave",slave,"%@");
        slave->flags |= SRI_PROMOTED;
        ri->promoted_slave = slave;
        ri->failover_state = SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE;
        ri->failover_state_change_time = mstime();
        sentinelEvent(REDIS_NOTICE,"+failover-state-send-slaveof-noone",
            slave, "%@");
    }
}

void sentinelFailoverSendSlaveOfNoOne(sentinelRedisInstance *ri) {
    int retval;

    if (ri->promoted_slave->flags & SRI_DISCONNECTED) return;

    /* Send SLAVEOF NO ONE command to turn the slave into a master.
     * We actually register a generic callback for this command as we don't
     * really care about the reply. We check if it worked indirectly observing
     * if INFO returns a different role (master instead of slave). */
    retval = redisAsyncCommand(ri->promoted_slave->cc,
        sentinelDiscardReplyCallback, NULL, "SLAVEOF NO ONE");
    if (retval != REDIS_OK) return;
    ri->promoted_slave->pending_commands++;
    sentinelEvent(REDIS_NOTICE, "+failover-state-wait-promotion",
        ri->promoted_slave,"%@");
    ri->failover_state = SENTINEL_FAILOVER_STATE_WAIT_PROMOTION;
    ri->failover_state_change_time = mstime();
}

/* We actually wait for promotion indirectly checking with INFO when the
 * slave turns into a master. */
void sentinelFailoverWaitPromotion(sentinelRedisInstance *ri) {
    mstime_t elapsed = mstime() - ri->failover_state_change_time;

    if (elapsed >= SENTINEL_PROMOTION_RETRY_PERIOD) {
        sentinelEvent(REDIS_WARNING,"-promotion-timeout",ri->promoted_slave,
            "%@");
        sentinelEvent(REDIS_WARNING,"+failover-state-select-slave",ri,"%@");
        ri->failover_state = SENTINEL_FAILOVER_STATE_SELECT_SLAVE;
        ri->failover_state_change_time = mstime();
        ri->promoted_slave->flags &= ~SRI_PROMOTED;
        ri->promoted_slave = NULL;
    }
}

void sentinelFailoverDetectEnd(sentinelRedisInstance *master) {
    int not_reconfigured = 0, timeout = 0;
    dictIterator *di;
    dictEntry *de;
    mstime_t elapsed = mstime() - master->failover_state_change_time;

    /* We can't consider failover finished if the promoted slave is
     * not reachable. */
    if (master->promoted_slave == NULL ||
        master->promoted_slave->flags & SRI_S_DOWN) return;

    /* The failover terminates once all the reachable slaves are properly
     * configured. */
    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        if (slave->flags & (SRI_PROMOTED|SRI_RECONF_DONE)) continue;
        if (slave->flags & SRI_S_DOWN) continue;
        not_reconfigured++;
    }
    dictReleaseIterator(di);

    /* Force end of failover on timeout. */
    if (elapsed > master->failover_timeout) {
        not_reconfigured = 0;
        timeout = 1;
        sentinelEvent(REDIS_WARNING,"+failover-end-for-timeout",master,"%@");
    }

    if (not_reconfigured == 0) {
        int role = (master->flags & SRI_I_AM_THE_LEADER) ? SENTINEL_LEADER :
                                                           SENTINEL_OBSERVER;

        sentinelEvent(REDIS_WARNING,"+failover-end",master,"%@");
        master->failover_state = SENTINEL_FAILOVER_STATE_UPDATE_CONFIG;
        master->failover_state_change_time = mstime();
        sentinelCallClientReconfScript(master,role,"end",master->addr,
            master->promoted_slave->addr);
    }

    /* If I'm the leader it is a good idea to send a best effort SLAVEOF
     * command to all the slaves still not reconfigured to replicate with
     * the new master. */
    if (timeout && (master->flags & SRI_I_AM_THE_LEADER)) {
        dictIterator *di;
        dictEntry *de;
        char master_port[32];

        ll2string(master_port,sizeof(master_port),
            master->promoted_slave->addr->port);

        di = dictGetIterator(master->slaves);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *slave = dictGetVal(de);
            int retval;

            if (slave->flags &
                (SRI_RECONF_DONE|SRI_RECONF_SENT|SRI_DISCONNECTED)) continue;

            retval = redisAsyncCommand(slave->cc,
                sentinelDiscardReplyCallback, NULL, "SLAVEOF %s %s",
                    master->promoted_slave->addr->ip,
                    master_port);
            if (retval == REDIS_OK) {
                sentinelEvent(REDIS_NOTICE,"+slave-reconf-sent-be",slave,"%@");
                slave->flags |= SRI_RECONF_SENT;
            }
        }
        dictReleaseIterator(di);
    }
}

/* Send SLAVE OF <new master address> to all the remaining slaves that
 * still don't appear to have the configuration updated. */
void sentinelFailoverReconfNextSlave(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int in_progress = 0;

    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);

        if (slave->flags & (SRI_RECONF_SENT|SRI_RECONF_INPROG))
            in_progress++;
    }
    dictReleaseIterator(di);

    di = dictGetIterator(master->slaves);
    while(in_progress < master->parallel_syncs &&
          (de = dictNext(di)) != NULL)
    {
        sentinelRedisInstance *slave = dictGetVal(de);
        int retval;
        char master_port[32];

        /* Skip the promoted slave, and already configured slaves. */
        if (slave->flags & (SRI_PROMOTED|SRI_RECONF_DONE)) continue;

        /* Clear the SRI_RECONF_SENT flag if too much time elapsed without
         * the slave moving forward to the next state. */
        if ((slave->flags & SRI_RECONF_SENT) &&
            (mstime() - slave->slave_reconf_sent_time) >
            SENTINEL_SLAVE_RECONF_RETRY_PERIOD)
        {
            sentinelEvent(REDIS_NOTICE,"-slave-reconf-sent-timeout",slave,"%@");
            slave->flags &= ~SRI_RECONF_SENT;
        }

        /* Nothing to do for instances that are disconnected or already
         * in RECONF_SENT state. */
        if (slave->flags & (SRI_DISCONNECTED|SRI_RECONF_SENT|SRI_RECONF_INPROG))
            continue;

        /* Send SLAVEOF <new master>. */
        ll2string(master_port,sizeof(master_port),
            master->promoted_slave->addr->port);
        retval = redisAsyncCommand(slave->cc,
            sentinelDiscardReplyCallback, NULL, "SLAVEOF %s %s",
                master->promoted_slave->addr->ip,
                master_port);
        if (retval == REDIS_OK) {
            slave->flags |= SRI_RECONF_SENT;
            slave->pending_commands++;
            slave->slave_reconf_sent_time = mstime();
            sentinelEvent(REDIS_NOTICE,"+slave-reconf-sent",slave,"%@");
            in_progress++;
        }
    }
    dictReleaseIterator(di);
    sentinelFailoverDetectEnd(master);
}

/* This function is called when the slave is in
 * SENTINEL_FAILOVER_STATE_UPDATE_CONFIG state. In this state we need
 * to remove it from the master table and add the promoted slave instead.
 *
 * If there are no promoted slaves as this instance is unique, we remove
 * and re-add it with the same address to trigger a complete state
 * refresh. */
void sentinelFailoverSwitchToPromotedSlave(sentinelRedisInstance *master) {
    sentinelRedisInstance *ref = master->promoted_slave ?
                                 master->promoted_slave : master;

    sentinelEvent(REDIS_WARNING,"+switch-master",master,"%s %s %d %s %d",
        master->name, master->addr->ip, master->addr->port,
        ref->addr->ip, ref->addr->port);

    sentinelResetMasterAndChangeAddress(master,ref->addr->ip,ref->addr->port);
}

void sentinelFailoverStateMachine(sentinelRedisInstance *ri) {
    redisAssert(ri->flags & SRI_MASTER);

    if (!(ri->flags & SRI_FAILOVER_IN_PROGRESS)) return;

    switch(ri->failover_state) {
        case SENTINEL_FAILOVER_STATE_WAIT_START:
            sentinelFailoverWaitStart(ri);
            break;
        case SENTINEL_FAILOVER_STATE_SELECT_SLAVE:
            sentinelFailoverSelectSlave(ri);
            break;
        case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE:
            sentinelFailoverSendSlaveOfNoOne(ri);
            break;
        case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION:
            sentinelFailoverWaitPromotion(ri);
            break;
        case SENTINEL_FAILOVER_STATE_RECONF_SLAVES:
            sentinelFailoverReconfNextSlave(ri);
            break;
        case SENTINEL_FAILOVER_STATE_DETECT_END:
            sentinelFailoverDetectEnd(ri);
            break;
    }
}

/* Abort a failover in progress with the following steps:
 * 1) If this instance is the leaer send a SLAVEOF command to all the already
 *    reconfigured slaves if any to configure them to replicate with the
 *    original master.
 * 2) For both leaders and observers: clear the failover flags and state in
 *    the master instance.
 * 3) If there is already a promoted slave and we are the leader, and this
 *    slave is not DISCONNECTED, try to reconfigure it to replicate
 *    back to the master as well, sending a best effort SLAVEOF command.
 */
void sentinelAbortFailover(sentinelRedisInstance *ri) {
    char master_port[32];
    dictIterator *di;
    dictEntry *de;
    int sentinel_role;

    redisAssert(ri->flags & SRI_FAILOVER_IN_PROGRESS);
    ll2string(master_port,sizeof(master_port),ri->addr->port);

    /* Clear failover related flags from slaves.
     * Also if we are the leader make sure to send SLAVEOF commands to all the
     * already reconfigured slaves in order to turn them back into slaves of
     * the original master. */
    di = dictGetIterator(ri->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        if ((ri->flags & SRI_I_AM_THE_LEADER) &&
            !(slave->flags & SRI_DISCONNECTED) &&
             (slave->flags & (SRI_PROMOTED|SRI_RECONF_SENT|SRI_RECONF_INPROG|
                              SRI_RECONF_DONE)))
        {
            int retval;

            retval = redisAsyncCommand(slave->cc,
                sentinelDiscardReplyCallback, NULL, "SLAVEOF %s %s",
                    ri->addr->ip,
                    master_port);
            if (retval == REDIS_OK)
                sentinelEvent(REDIS_NOTICE,"-slave-reconf-undo",slave,"%@");
        }
        slave->flags &= ~(SRI_RECONF_SENT|SRI_RECONF_INPROG|SRI_RECONF_DONE);
    }
    dictReleaseIterator(di);

    sentinel_role = (ri->flags & SRI_I_AM_THE_LEADER) ? SENTINEL_LEADER :
                                                        SENTINEL_OBSERVER;
    ri->flags &= ~(SRI_FAILOVER_IN_PROGRESS|SRI_I_AM_THE_LEADER|SRI_FORCE_FAILOVER);
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = mstime();
    if (ri->promoted_slave) {
        sentinelCallClientReconfScript(ri,sentinel_role,"abort",
            ri->promoted_slave->addr,ri->addr);
        ri->promoted_slave->flags &= ~SRI_PROMOTED;
        ri->promoted_slave = NULL;
    }
}

/* The following is called only for master instances and will abort the
 * failover process if:
 *
 * 1) The failover is in progress.
 * 2) We already promoted a slave.
 * 3) The promoted slave is in extended SDOWN condition.
 */
void sentinelAbortFailoverIfNeeded(sentinelRedisInstance *ri) {
    /* Failover is in progress? Do we have a promoted slave? */
    if (!(ri->flags & SRI_FAILOVER_IN_PROGRESS) || !ri->promoted_slave) return;

    /* Is the promoted slave into an extended SDOWN state? */
    if (!(ri->promoted_slave->flags & SRI_S_DOWN) ||
        (mstime() - ri->promoted_slave->s_down_since_time) <
        (ri->down_after_period * SENTINEL_EXTENDED_SDOWN_MULTIPLIER)) return;

    sentinelEvent(REDIS_WARNING,"-failover-abort-x-sdown",ri->promoted_slave,"%@");
    sentinelAbortFailover(ri);
}

/* ======================== SENTINEL timer handler ==========================
 * This is the "main" our Sentinel, being sentinel completely non blocking
 * in design. The function is called every second.
 * -------------------------------------------------------------------------- */

/* Perform scheduled operations for the specified Redis instance. */
void sentinelHandleRedisInstance(sentinelRedisInstance *ri) {
    /* ========== MONITORING HALF ============ */
    /* Every kind of instance */
    sentinelReconnectInstance(ri);
    sentinelPingInstance(ri);

    /* Masters and slaves */
    if (ri->flags & (SRI_MASTER|SRI_SLAVE)) {
        /* Nothing so far. */
    }

    /* Only masters */
    if (ri->flags & SRI_MASTER) {
        sentinelAskMasterStateToOtherSentinels(ri);
    }

    /* ============== ACTING HALF ============= */
    /* We don't proceed with the acting half if we are in TILT mode.
     * TILT happens when we find something odd with the time, like a
     * sudden change in the clock. */
    if (sentinel.tilt) {
        if (mstime()-sentinel.tilt_start_time < SENTINEL_TILT_PERIOD) return;
        sentinel.tilt = 0;
        sentinelEvent(REDIS_WARNING,"-tilt",NULL,"#tilt mode exited");
    }

    /* Every kind of instance */
    sentinelCheckSubjectivelyDown(ri);

    /* Masters and slaves */
    if (ri->flags & (SRI_MASTER|SRI_SLAVE)) {
        /* Nothing so far. */
    }

    /* Only masters */
    if (ri->flags & SRI_MASTER) {
        sentinelCheckObjectivelyDown(ri);
        sentinelStartFailoverIfNeeded(ri);
        sentinelFailoverStateMachine(ri);
        sentinelAbortFailoverIfNeeded(ri);
    }
}

/* Perform scheduled operations for all the instances in the dictionary.
 * Recursively call the function against dictionaries of slaves. */
void sentinelHandleDictOfRedisInstances(dict *instances) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *switch_to_promoted = NULL;

    /* There are a number of things we need to perform against every master. */
    di = dictGetIterator(instances);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        sentinelHandleRedisInstance(ri);
        if (ri->flags & SRI_MASTER) {
            sentinelHandleDictOfRedisInstances(ri->slaves);
            sentinelHandleDictOfRedisInstances(ri->sentinels);
            if (ri->failover_state == SENTINEL_FAILOVER_STATE_UPDATE_CONFIG) {
                switch_to_promoted = ri;
            }
        }
    }
    if (switch_to_promoted)
        sentinelFailoverSwitchToPromotedSlave(switch_to_promoted);
    dictReleaseIterator(di);
}

/* This function checks if we need to enter the TITL mode.
 *
 * The TILT mode is entered if we detect that between two invocations of the
 * timer interrupt, a negative amount of time, or too much time has passed.
 * Note that we expect that more or less just 100 milliseconds will pass
 * if everything is fine. However we'll see a negative number or a
 * difference bigger than SENTINEL_TILT_TRIGGER milliseconds if one of the
 * following conditions happen:
 *
 * 1) The Sentiel process for some time is blocked, for every kind of
 * random reason: the load is huge, the computer was frozen for some time
 * in I/O or alike, the process was stopped by a signal. Everything.
 * 2) The system clock was altered significantly.
 *
 * Under both this conditions we'll see everything as timed out and failing
 * without good reasons. Instead we enter the TILT mode and wait
 * for SENTINEL_TILT_PERIOD to elapse before starting to act again.
 *
 * During TILT time we still collect information, we just do not act. */
void sentinelCheckTiltCondition(void) {
    mstime_t now = mstime();
    mstime_t delta = now - sentinel.previous_time;

    if (delta < 0 || delta > SENTINEL_TILT_TRIGGER) {
        sentinel.tilt = 1;
        sentinel.tilt_start_time = mstime();
        sentinelEvent(REDIS_WARNING,"+tilt",NULL,"#tilt mode entered");
    }
    sentinel.previous_time = mstime();
}

void sentinelTimer(void) {
    sentinelCheckTiltCondition();
    sentinelHandleDictOfRedisInstances(sentinel.masters);
    sentinelRunPendingScripts();
    sentinelCollectTerminatedScripts();
    sentinelKillTimedoutScripts();
}

