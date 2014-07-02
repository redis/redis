/* The latency monitor allows to easily observe the sources of latency
 * in a Redis instance using the LATENCY command. Different latency
 * sources are monitored, like disk I/O, execution of commands, fork
 * system call, and so forth.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2014, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* Dictionary type for latency events. Key/Val destructors are set to NULL
 * since we never delete latency time series at runtime. */
int dictStringKeyCompare(void *privdata, const void *key1, const void *key2) {
    return strcmp(key1,key2) == 0;
}

unsigned int dictStringHash(const void *key) {
    return dictGenHashFunction(key, strlen(key));
}

dictType latencyTimeSeriesDictType = {
    dictStringHash,             /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictStringKeyCompare,       /* key compare */
    NULL,                       /* key destructor */
    NULL                        /* val destructor */
};

/* ---------------------------- Latency API --------------------------------- */

/* Latency monitor initialization. We just need to create the dictionary
 * of time series, each time serie is craeted on demand in order to avoid
 * having a fixed list to maintain. */
void latencyMonitorInit(void) {
    server.latency_events = dictCreate(&latencyTimeSeriesDictType,NULL);
}

/* Add the specified sample to the specified time series "event".
 * This function is usually called via latencyAddSampleIfNeeded(), that
 * is a macro that only adds the sample if the latency is higher than
 * server.latency_monitor_threshold. */
void latencyAddSample(char *event, mstime_t latency) {
    struct latencyTimeSeries *ts = dictFetchValue(server.latency_events,event);
    time_t now = time(NULL);
    int prev;

    /* Create the time series if it does not exist. */
    if (ts == NULL) {
        ts = zmalloc(sizeof(*ts));
        ts->idx = 0;
        ts->max = 0;
        memset(ts->samples,0,sizeof(ts->samples));
        dictAdd(server.latency_events,zstrdup(event),ts);
    }

    /* If the previous sample is in the same second, we update our old sample
     * if this latency is > of the old one, or just return. */
    prev = (ts->idx + LATENCY_TS_LEN - 1) % LATENCY_TS_LEN;
    if (ts->samples[prev].time == now) {
        if (latency > ts->samples[prev].latency)
            ts->samples[prev].latency = latency;
        return;
    }

    ts->samples[ts->idx].time = time(NULL);
    ts->samples[ts->idx].latency = latency;
    if (latency > ts->max) ts->max = latency;

    ts->idx++;
    if (ts->idx == LATENCY_TS_LEN) ts->idx = 0;
}

/* ---------------------- Latency command implementation -------------------- */

/* latencyCommand() helper to produce a time-delay reply for all the samples
 * in memory for the specified time series. */
void latencyCommandReplyWithSamples(redisClient *c, struct latencyTimeSeries *ts) {
    void *replylen = addDeferredMultiBulkLength(c);
    int samples = 0, j;

    for (j = 0; j < LATENCY_TS_LEN; j++) {
        int i = (ts->idx + j) % LATENCY_TS_LEN;

        if (ts->samples[i].time == 0) continue;
        addReplyMultiBulkLen(c,2);
        addReplyLongLong(c,ts->samples[i].time);
        addReplyLongLong(c,ts->samples[i].latency);
        samples++;
    }
    setDeferredMultiBulkLength(c,replylen,samples);
}

/* latencyCommand() helper to produce the reply for the LATEST subcommand,
 * listing the last latency sample for every event type registered so far. */
void latencyCommandReplyWithLatestEvents(redisClient *c) {
    dictIterator *di;
    dictEntry *de;

    addReplyMultiBulkLen(c,dictSize(server.latency_events));
    di = dictGetIterator(server.latency_events);
    while((de = dictNext(di)) != NULL) {
        char *event = dictGetKey(de);
        struct latencyTimeSeries *ts = dictGetVal(de);
        int last = (ts->idx + LATENCY_TS_LEN - 1) % LATENCY_TS_LEN;

        addReplyMultiBulkLen(c,4);
        addReplyBulkCString(c,event);
        addReplyLongLong(c,ts->samples[last].time);
        addReplyLongLong(c,ts->samples[last].latency);
        addReplyLongLong(c,ts->max);
    }
    dictReleaseIterator(di);
}

#define LATENCY_GRAPH_COLS 80
sds latencyCommandGenSparkeline(char *event, struct latencyTimeSeries *ts) {
    int j;
    struct sequence *seq = createSparklineSequence();
    sds graph = sdsempty();
    uint32_t min = 0, max = 0;

    for (j = 0; j < LATENCY_TS_LEN; j++) {
        int i = (ts->idx + j) % LATENCY_TS_LEN;
        int elapsed;
        char *label;
        char buf[64];

        if (ts->samples[i].time == 0) continue;
        /* Update min and max. */
        if (seq->length == 0) {
            min = max = ts->samples[i].latency;
        } else {
            if (ts->samples[i].latency > max) max = ts->samples[i].latency;
            if (ts->samples[i].latency < min) min = ts->samples[i].latency;
        }
        /* Use as label the number of seconds / minutes / hours / days
         * ago the event happened. */
        elapsed = time(NULL) - ts->samples[i].time;
        if (elapsed < 60)
            snprintf(buf,sizeof(buf),"%ds",elapsed);
        else if (elapsed < 3600)
            snprintf(buf,sizeof(buf),"%dm",elapsed/60);
        else if (elapsed < 3600*24)
            snprintf(buf,sizeof(buf),"%dh",elapsed/3600);
        else
            snprintf(buf,sizeof(buf),"%dd",elapsed/(3600*24));
        label = zstrdup(buf);
        sparklineSequenceAddSample(seq,ts->samples[i].latency,label);
    }

    graph = sdscatprintf(graph,
        "%s - high %lu ms, low %lu ms (all time high %lu ms)\n", event,
        (unsigned long) max, (unsigned long) min, (unsigned long) ts->max);
    for (j = 0; j < LATENCY_GRAPH_COLS; j++)
        graph = sdscatlen(graph,"-",1);
    graph = sdscatlen(graph,"\n",1);
    graph = sparklineRender(graph,seq,LATENCY_GRAPH_COLS,4,SPARKLINE_FILL);
    freeSparklineSequence(seq);
    return graph;
}

/* LATENCY command implementations.
 *
 * LATENCY SAMPLES: return time-latency samples for the specified event.
 * LATENCY LATEST: return the latest latency for all the events classes.
 * LATENCY DOCTOR: returns an human readable analysis of instance latency.
 * LATENCY GRAPH: provide an ASCII graph of the latency of the specified event.
 */
void latencyCommand(redisClient *c) {
    struct latencyTimeSeries *ts;

    if (!strcasecmp(c->argv[1]->ptr,"samples") && c->argc == 3) {
        /* LATENCY SAMPLES <event> */
        ts = dictFetchValue(server.latency_events,c->argv[2]->ptr);
        if (ts == NULL) goto nodataerr;
        latencyCommandReplyWithSamples(c,ts);
    } else if (!strcasecmp(c->argv[1]->ptr,"graph") && c->argc == 3) {
        /* LATENCY GRAPH <event> */
        sds graph;
        dictEntry *de;
        char *event;

        de = dictFind(server.latency_events,c->argv[2]->ptr);
        if (de == NULL) goto nodataerr;
        ts = dictGetVal(de);
        event = dictGetKey(de);

        graph = latencyCommandGenSparkeline(event,ts);
        addReplyBulkCString(c,graph);
        sdsfree(graph);
    } else if (!strcasecmp(c->argv[1]->ptr,"latest") && c->argc == 2) {
        /* LATENCY LATEST */
        latencyCommandReplyWithLatestEvents(c);
    } else {
        addReply(c,shared.syntaxerr);
    }
    return;

nodataerr:
    /* Common error when the user asks for an event we have no latency
     * information about. */
    addReplyErrorFormat(c,
        "No samples available for event '%s'", c->argv[2]->ptr);
}

