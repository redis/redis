/*
 * Copyright (c) 2022, Redis
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
#include "syscheck.h"
#include "sds.h"
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
static sds read_sysfs_line(char *path) {
    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    sds res = sdsnew(buf);
    res = sdstrim(res, " \n");
    return res;
}

static sds clocksource_warning_msg(void) {
    sds avail = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/available_clocksource");
    sds curr = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    sds msg = sdscatprintf(sdsempty(),
        "Slow system clocksource detected. This can result in degraded performance. "
        "Consider changing the system's clocksource. "
        "Current clocksource: %s. Available clocksources: %s. "
        "For example: run the command 'echo tsc > /sys/devices/system/clocksource/clocksource0/current_clocksource' as root. "
        "To permanently change the system's clocksource you'll need to set the 'clocksource=' kernel command line parameter.",
        curr ? curr : "", avail ? avail : "");
    sdsfree(avail);
    sdsfree(curr);
    return msg;
}

static int check_clocksource(sds *error_msg) {
    unsigned long test_time_us, system_hz;
    struct timespec ts;
    unsigned long long start_us;
    struct rusage ru_start, ru_end;

    system_hz = sysconf(_SC_CLK_TCK);

    if (getrusage(RUSAGE_SELF, &ru_start) != 0)
        return 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }
    start_us = (ts.tv_sec * 1000000 + ts.tv_nsec / 1000);

    /* clock_gettime() busy loop of 5 times system tick (for a system_hz of 100 this is 50ms)
     * Using system_hz is required to ensure accurate measurements from getrusage().
     * If our clocksource is configured correctly (vdso) this will result in no system calls.
     * If our clocksource is inefficient it'll waste most of the busy loop in the kernel. */
    test_time_us = 5 * 1000000 / system_hz;
    while (1) {
        unsigned long long d;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
            return 0;
        d = (ts.tv_sec * 1000000 + ts.tv_nsec / 1000) - start_us;
        if (d >= test_time_us) break;
    }
    if (getrusage(RUSAGE_SELF, &ru_end) != 0)
        return 0;

    long long stime_us = (ru_end.ru_stime.tv_sec * 1000000 + ru_end.ru_stime.tv_usec) - (ru_start.ru_stime.tv_sec * 1000000 + ru_start.ru_stime.tv_usec);
    long long utime_us = (ru_end.ru_utime.tv_sec * 1000000 + ru_end.ru_utime.tv_usec) - (ru_start.ru_utime.tv_sec * 1000000 + ru_start.ru_utime.tv_usec);

    /* If more than 10% of the process time was in system calls we probably have an inefficient clocksource, print a warning */
    if (stime_us * 10 > stime_us + utime_us) {
        *error_msg = clocksource_warning_msg();
        return -1;
    } else {
        return 1;
    }
}

int check_xen(sds *error_msg) {
    sds curr = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    int res = 1;
    if (curr == NULL) {
        res = 0;
    } else if (strcmp(curr, "xen") == 0) {
        *error_msg = clocksource_warning_msg();
        res = -1;
    }
    sdsfree(curr);
    return res;
}

int check_overcommit(sds *error_msg) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (atoi(buf)) {
        *error_msg = sdsnew("WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
        return -1;
    } else {
        return 1;
    }
}

int check_thp_enabled(sds *error_msg) {
    char buf[1024];

    FILE *fp = fopen("/sys/kernel/mm/transparent_hugepage/enabled","r");
    if (!fp) return 0;
    if (fgets(buf,sizeof(buf),fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (strstr(buf,"[always]") != NULL) {
        *error_msg = sdsnew("You have Transparent Huge Pages (THP) support enabled in your kernel. This will create latency and memory usage issues with Redis. To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, and add it to your /etc/rc.local in order to retain the setting after a reboot. Redis must be restarted after THP is disabled (set to 'madvise' or 'never').");
        return -1;
    } else {
        return 1;
    }
}
#endif

typedef struct {
    const char *name;
    int (*check_fn)(sds*);
} check;

check checks[] = {
#ifdef __linux__
    {.name = "clocksource", .check_fn = check_clocksource},
    {.name = "xen", .check_fn = check_xen},
    {.name = "overcommit", .check_fn = check_overcommit},
    {.name = "THP", .check_fn = check_thp_enabled},
#endif
    {.name = NULL, .check_fn = NULL}
};

void syscheck(void) {
    check *cur_check = checks;
    int exit_code = 0;
    sds err_msg;
    while (cur_check->check_fn) {
        int res = cur_check->check_fn(&err_msg);
        printf("[%s]...", cur_check->name);
        if (res == 0) {
            printf("skipped\n");
        } else if (res == 1) {
            printf("OK\n");
        } else {
            printf("WARNING:\n");
            printf("%s\n", err_msg);
            sdsfree(err_msg);
            exit_code = 1;
        }
        cur_check++;
    }

    exit(exit_code);
}
