/* Slowlog implements a system that is able to remember the latest N
 * queries that took more than M microseconds to execute.
 *
 * The execution time to reach to be logged in the slow log is set
 * using the 'slowlog-log-slower-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * The slow queries log is actually not "logged" in the Redis log file
 * but is accessible thanks to the SLOWLOG command.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */


#include "server.h"
#include "slowlog.h"
#include <sys/resource.h>

/* Create a new slowlog entry.
 * Incrementing the ref count of all the objects retained is up to
 * this function. */
slowlogEntry *slowlogCreateEntry(client *c, robj **argv, int argc, long long duration,
                                  long long cpu_duration) {
    slowlogEntry *se = zmalloc(sizeof(*se));
    int j, slargc = argc;

    if (slargc > server.slowlog_max_argc) slargc = server.slowlog_max_argc;
    se->argc = slargc;
    se->cmd_argc = argc;
    se->argv = zmalloc(sizeof(robj*)*slargc);
    for (j = 0; j < slargc; j++) {
        /* Logging too many arguments is a useless memory waste, so we stop
         * at server.slowlog_max_argc, but use the last argument to specify
         * how many remaining arguments there were in the original command. */
        if (slargc != argc && j == slargc-1) {
            se->argv[j] = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),"... (%d more arguments)",
                argc-slargc+1));
        } else {
            /* Trim too long strings as well... */
            if (argv[j]->type == OBJ_STRING &&
                sdsEncodedObject(argv[j]) &&
                sdslen(argv[j]->ptr) > server.slowlog_max_string_len)
            {
                sds s = sdsnewlen(argv[j]->ptr, server.slowlog_max_string_len);

                s = sdscatprintf(s,"... (%lu more bytes)",
                    (unsigned long)
                    sdslen(argv[j]->ptr) - server.slowlog_max_string_len);
                se->argv[j] = createObject(OBJ_STRING,s);
            } else if (argv[j]->refcount == OBJ_SHARED_REFCOUNT) {
                se->argv[j] = argv[j];
            } else {
                /* Here we need to duplicate the string objects composing the
                 * argument vector of the command, because those may otherwise
                 * end shared with string objects stored into keys. Having
                 * shared objects between any part of Redis, and the data
                 * structure holding the data, is a problem: FLUSHALL ASYNC
                 * may release the shared string object and create a race. */
                se->argv[j] = dupStringObject(argv[j]);
            }
        }
    }
    se->time = time(NULL);
    se->duration = duration;
    se->cpu_duration = cpu_duration;
    se->id = server.slowlog_entry_id++;
    se->peerid = sdsnew(getClientPeerId(c));
    se->cname = c->name ? sdsnew(c->name->ptr) : sdsempty();
    return se;
}

/* Free a slow log entry. The argument is void so that the prototype of this
 * function matches the one of the 'free' method of adlist.c.
 *
 * This function will take care to release all the retained object. */
void slowlogFreeEntry(void *septr) {
    slowlogEntry *se = septr;
    int j;

    for (j = 0; j < se->argc; j++)
        decrRefCount(se->argv[j]);
    zfree(se->argv);
    sdsfree(se->peerid);
    sdsfree(se->cname);
    zfree(se);
}

/* Initialize the slow log. This function should be called a single time
 * at server startup. */
void slowlogInit(void) {
    server.slowlog = listCreate();
    server.slowlog_entry_id = 0;
    listSetFreeMethod(server.slowlog,slowlogFreeEntry);
}

/* Returns 1 if a command that took 'duration' microseconds would be logged. */
int slowlogWouldLog(long long duration) {
    if (server.slowlog_log_slower_than < 0 || server.slowlog_max_len == 0) return 0;
    return duration >= server.slowlog_log_slower_than;
}

/* Push a new entry into the slow log.
 * This function will make sure to trim the slow log accordingly to the
 * configured max length.
 * Returns 1 if an entry was added, 0 otherwise. */
int slowlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long duration,
                              long long cpu_duration) {
    if (slowlogWouldLog(duration)) {
        listAddNodeHead(server.slowlog,
                        slowlogCreateEntry(c,argv,argc,duration,cpu_duration));

        /* Remove old entries if needed. */
        while (listLength(server.slowlog) > server.slowlog_max_len)
            listDelNode(server.slowlog,listLast(server.slowlog));
        return 1;
    }
    return 0;
}

/* ----------------------------- Off-CPU accounting -------------------------- */

/* The thread running call(): the main thread, or a module thread via RM_Call. */
#ifdef RUSAGE_THREAD
#define COMMAND_RUSAGE_WHO RUSAGE_THREAD
#else
#define COMMAND_RUSAGE_WHO RUSAGE_SELF
#endif

/* Off-CPU time is reported when it reaches both slowlog-log-slower-than and
 * this floor (user/sys time are truncated to whole microseconds, so a few us
 * of "off-CPU" can be noise).
 * More involuntary than voluntary switches is reported as starvation,
 * otherwise blocked. Run-queue wait after a sleep adds no involuntary
 * switch, so it shows as blocked. */
#define CPU_STARVATION_MIN_OFFCPU_US 100

/* Sample the current thread: wall clock, CPU time, context switches. */
void cpuSampleTake(cpuSample *s) {
    struct rusage ru;
    getrusage(COMMAND_RUSAGE_WHO, &ru);
    s->wall = getMonotonicUs();
    s->cpu_us = (ustime_t)ru.ru_utime.tv_sec*1000000 + ru.ru_utime.tv_usec +
                (ustime_t)ru.ru_stime.tv_sec*1000000 + ru.ru_stime.tv_usec;
    s->nvcsw = ru.ru_nvcsw;
    s->nivcsw = ru.ru_nivcsw;
}

/* Log an off-CPU warning, at most one line per second. The next line printed
 * reports how many were suppressed; the INFO counters count every event. */
static void offCpuLog(const char *fmt, ...) {
    /* Monotonic, not server.mstime: the cached time isn't refreshed after a
     * long command, which would suppress lines that are due. */
    static monotime last_log_us = 0;
    static long long suppressed = 0;
    monotime now_us = getMonotonicUs();
    if (last_log_us && now_us - last_log_us <= 1000000) {
        suppressed++;
        return;
    }
    char msg[LOG_MAX_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (suppressed)
        serverLog(LL_WARNING, "%s (%lld more suppressed)", msg, suppressed);
    else
        serverLog(LL_WARNING, "%s", msg);
    last_log_us = now_us;
    suppressed = 0;
}

/* CPU accounting for a command that is about to be logged.
 * Main thread: off-CPU time is measured over the window since
 * server.cpu_checkpoint (taken when the event loop wakes, moved by each slow
 * command), or since server.cpu_cmd_start for a top-level command, so fast
 * commands pay no syscall. Only the part that must have happened inside this
 * command is charged to it.
 * Module thread: call() measured the command exactly into c->cpu_*.
 * sample_cpu is 0 on the unblock path: c->duration is from an earlier iteration. */
void slowlogMeasureCpu(client *c, int sample_cpu, slowlogCpu *m) {
    memset(m, 0, sizeof(*m));
    m->measured = c->duration; /* Execution time only, without blocked time. */
    m->sampled = sample_cpu;
    if (!sample_cpu) return;

    if (pthread_equal(pthread_self(), server.main_thread_id)) {
        /* A top-level command measures from its own start, so it keeps the
         * off-CPU time of slow commands it called; nested commands measure
         * from the checkpoint, so siblings (MULTI/EXEC, script calls) don't
         * share one wide window. */
        const cpuSample *start = server.execution_nesting == 0 ?
                                 &server.cpu_cmd_start : &server.cpu_checkpoint;
        cpuSample now;
        cpuSampleTake(&now);
        ustime_t window = (ustime_t)(now.wall - start->wall);
        ustime_t gap = window - (now.cpu_us - start->cpu_us);
        if (gap < 0) gap = 0;
        /* Off-CPU outside the command can't exceed the window time outside it. */
        ustime_t outside = window > m->measured ? window - m->measured : 0;
        m->off_max = gap < m->measured ? gap : m->measured;
        m->off_min = gap > outside ? gap - outside : 0;
        if (m->off_min > m->off_max) m->off_min = m->off_max;
        m->cpu = m->measured - m->off_min;
        m->nvcsw = now.nvcsw - start->nvcsw;
        m->nivcsw = now.nivcsw - start->nivcsw;
        server.cpu_checkpoint = now;
    } else {
        m->cpu = c->cpu_duration;
        m->off_max = m->measured > m->cpu ? m->measured - m->cpu : 0;
        m->off_min = m->off_max;
        m->nvcsw = c->cpu_nvcsw;
        m->nivcsw = c->cpu_nivcsw;
    }
}

/* Count and log a logged command whose guaranteed off-CPU time alone made it
 * slow. Decides on off_min, so earlier off-CPU time isn't blamed on it. */
void slowlogReportOffCpu(const slowlogCpu *m) {
    if (!m->sampled || m->off_min < server.slowlog_log_slower_than ||
        m->off_min < CPU_STARVATION_MIN_OFFCPU_US)
        return;
    int starvation = m->nivcsw > m->nvcsw;
    if (starvation)
        server.stat_cpu_starvation_events++;
    else
        server.stat_off_cpu_blocked_events++;
    offCpuLog("Slow command off-CPU (%s): took %lldus, at least %lldus off-CPU "
              "(up to %lldus), %ld involuntary / %ld voluntary context switches",
              starvation ? "starvation" : "blocked", (long long)m->measured,
              (long long)m->off_min, (long long)m->off_max, m->nivcsw, m->nvcsw);
}

/* Called from beforeSleep(), before the AOF flush: report the event-loop cycle
 * if the main thread was off-CPU long enough to make it slow on that alone
 * (starvation between fast commands never reaches the slowlog). Uses
 * server.el_cpu_start, which commands don't move, so all numbers describe the
 * same window. */
void offCpuCheckEventLoopCycle(ustime_t el_duration) {
    if (server.slowlog_log_slower_than < 0 ||
        el_duration < server.slowlog_log_slower_than)
        return;
    cpuSample now;
    cpuSampleTake(&now);
    ustime_t took = (ustime_t)(now.wall - server.el_cpu_start.wall);
    ustime_t off = took - (now.cpu_us - server.el_cpu_start.cpu_us);
    if (off < 0) off = 0;
    if (off < server.slowlog_log_slower_than || off < CPU_STARVATION_MIN_OFFCPU_US)
        return;
    long nvcsw = now.nvcsw - server.el_cpu_start.nvcsw;
    long nivcsw = now.nivcsw - server.el_cpu_start.nivcsw;
    int starvation = nivcsw > nvcsw;
    if (starvation)
        server.stat_eventloop_cpu_starvation_events++;
    else
        server.stat_eventloop_off_cpu_blocked_events++;
    offCpuLog("Event loop cycle off-CPU (%s): took %lldus, %lldus off-CPU, %lld commands, "
              "%ld involuntary / %ld voluntary context switches",
              starvation ? "starvation" : "blocked", (long long)took, (long long)off,
              server.stat_numcommands - server.el_cmd_cnt_start, nivcsw, nvcsw);
}

/* Remove all the entries from the current slow log. */
void slowlogReset(void) {
    while (listLength(server.slowlog) > 0)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* The SLOWLOG command. Implements all the subcommands needed to handle the
 * Redis slow log. */
void slowlogCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"GET [<count>]",
"    Return top <count> entries from the slowlog (default: 10, -1 mean all).",
"    Entries are made of:",
"    id, timestamp, time in microseconds, arguments array, client IP and port,",
"    client name, total argument count, upper-bound estimate of CPU time in",
"    microseconds",
"    (0 when not measured)",
"LEN",
"    Return the length of the slowlog.",
"RESET",
"    Reset the slowlog.",
NULL
        };
        addReplyHelp(c, help);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"reset")) {
        slowlogReset();
        addReply(c,shared.ok);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"len")) {
        addReplyLongLong(c,listLength(server.slowlog));
    } else if ((c->argc == 2 || c->argc == 3) &&
               !strcasecmp(c->argv[1]->ptr,"get"))
    {
        long count = 10;
        listIter li;
        listNode *ln;
        slowlogEntry *se;

        if (c->argc == 3) {
            /* Consume count arg. */
            if (getRangeLongFromObjectOrReply(c, c->argv[2], -1,
                    LONG_MAX, &count, "count should be greater than or equal to -1") != C_OK)
                return;

            if (count == -1) {
                /* We treat -1 as a special value, which means to get all slow logs.
                 * Simply set count to the length of server.slowlog.*/
                count = listLength(server.slowlog);
            }
        }

        if (count > (long)listLength(server.slowlog)) {
            count = listLength(server.slowlog);
        }
        addReplyArrayLen(c, count);
        listRewind(server.slowlog, &li);
        while (count--) {
            int j;

            ln = listNext(&li);
            se = ln->value;
            addReplyArrayLen(c,8);
            addReplyLongLong(c,se->id);
            addReplyLongLong(c,se->time);
            addReplyLongLong(c,se->duration);
            addReplyArrayLen(c,se->argc);
            for (j = 0; j < se->argc; j++)
                addReplyBulk(c,se->argv[j]);
            addReplyBulkCBuffer(c,se->peerid,sdslen(se->peerid));
            addReplyBulkCBuffer(c,se->cname,sdslen(se->cname));
            addReplyLongLong(c,se->cmd_argc);
            addReplyLongLong(c,se->cpu_duration);
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
