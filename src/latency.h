#ifndef __LATENCY_H
#define __LATENCY_H

#define LATENCY_TS_LEN 160 /* History length for every monitored event. */

/* Representation of a latency sample: the sampling time and the latency
 * observed in milliseconds. */
struct latencySample {
    int32_t time; /* We don't use time_t to force 4 bytes usage everywhere. */
    uint32_t latency; /* Latency in milliseconds. */
};

/* The latency time series for a given event. */
struct latencyTimeSeries {
    int idx; /* Index of the next sample to store. */
    mstime_t max; /* Max latency observed for this event. */
    struct latencySample samples[LATENCY_TS_LEN]; /* Latest history. */
};

void latencyMonitorInit(void);
void latencyAddSample(char *event, mstime_t latency);

/* Latency monitoring macros. */

/* Start monitoring an event. We just set the current time. */
#define latencyStartMonitor(var) if (server.latency_monitor_threshold) { \
    var = mstime(); \
}

/* End monitoring an event, compute the difference with the current time
 * to check the amount of time elapsed. */
#define latencyEndMonitor(var) if (server.latency_monitor_threshold) { \
    var = mstime() - var; \
}

/* Add the sample only if the elapsed time is >= to the configured threshold. */
#define latencyAddSampleIfNeeded(event,var) \
    if (server.latency_monitor_threshold && \\
        var >= server.latency_monitor_threshold) \
          latencyAddSample(event,var);

#endif /* __LATENCY_H */
