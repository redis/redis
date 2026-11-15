/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __SLOWLOG_H__
#define __SLOWLOG_H__

/* This structure defines an entry inside the slow log list */
typedef struct slowlogEntry {
    robj **argv;
    int argc;
    long long id;       /* Unique entry identifier. */
    long long duration; /* Time spent by the query, in microseconds. */
    time_t time;        /* Unix time at which the query was executed. */
    sds cname;          /* Client name. */
    sds peerid;         /* Client network address. */
    int cmd_argc;       /* Total argument count of the command. */
    long long cpu_duration; /* Upper-bound estimate of CPU time (usec); 0 if not measured. */
} slowlogEntry;

/* CPU accounting for one command about to be logged (see slowlogMeasureCpu()). */
typedef struct slowlogCpu {
    ustime_t measured;  /* Execution time (c->duration). */
    ustime_t cpu;       /* Stored CPU time (main thread: measured - off_min). 0 if not sampled. */
    ustime_t off_min;   /* Off-CPU time that must be inside the command. */
    ustime_t off_max;   /* At most this much off-CPU time. */
    long nvcsw, nivcsw; /* Context switches over the window. */
    int sampled;
} slowlogCpu;

/* Exported API */
void slowlogInit(void);
void slowlogMeasureCpu(client *c, const cpuSample *start, slowlogCpu *m);
void slowlogReportOffCpu(const slowlogCpu *m);
int slowlogWouldLog(long long duration);
int slowlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long duration,
                              long long cpu_duration);

#endif /* __SLOWLOG_H__ */
