/*
 * Copyright (c) 2016-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */
#include "fmacros.h"
#include "config.h"
#include "syscheck.h"
#include "sds.h"
#include "anet.h"

#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#ifdef __linux__
#include <sys/mman.h>
#endif


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

/* Verify our clocksource implementation doesn't go through a system call (uses vdso).
 * Going through a system call to check the time degrades Redis performance. */
static int checkClocksource(sds *error_msg) {
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
        sds avail = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/available_clocksource");
        sds curr = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/current_clocksource");
        *error_msg = sdscatprintf(sdsempty(),
           "Slow system clocksource detected. This can result in degraded performance. "
           "Consider changing the system's clocksource. "
           "Current clocksource: %s. Available clocksources: %s. "
           "For example: run the command 'echo tsc > /sys/devices/system/clocksource/clocksource0/current_clocksource' as root. "
           "To permanently change the system's clocksource you'll need to set the 'clocksource=' kernel command line parameter.",
           curr ? curr : "", avail ? avail : "");
        sdsfree(avail);
        sdsfree(curr);
        return -1;
    } else {
        return 1;
    }
}

/* Verify we're not using the `xen` clocksource. The xen hypervisor's default clocksource is slow and affects
 * Redis's performance. This has been measured on ec2 xen based instances. ec2 recommends using the non-default
 * tsc clock source for these instances. */
int checkXenClocksource(sds *error_msg) {
    sds curr = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    int res = 1;
    if (curr == NULL) {
        res = 0;
    } else if (strcmp(curr, "xen") == 0) {
        *error_msg = sdsnew(
            "Your system is configured to use the 'xen' clocksource which might lead to degraded performance. "
            "Check the result of the [slow-clocksource] system check: run 'redis-server --check-system' to check if "
            "the system's clocksource isn't degrading performance.");
        res = -1;
    }
    sdsfree(curr);
    return res;
}

/* Verify overcommit is enabled.
 * When overcommit memory is disabled Linux will kill the forked child of a background save
 * if we don't have enough free memory to satisfy double the current memory usage even though
 * the forked child uses copy-on-write to reduce its actual memory usage. */
int checkOvercommit(sds *error_msg) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return 0;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (strtol(buf, NULL, 10) != 1) {
        *error_msg = sdsnew(
            "Memory overcommit must be enabled! Without it, a background save or replication may fail under low memory condition. "
#if defined(USE_JEMALLOC)
            "Being disabled, it can also cause failures without low memory condition, see https://github.com/jemalloc/jemalloc/issues/1328. "
#endif
            "To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the "
            "command 'sysctl vm.overcommit_memory=1' for this to take effect.");
        return -1;
    } else {
        return 1;
    }
}

/* Make sure transparent huge pages aren't always enabled. When they are this can cause copy-on-write logic
 * to consume much more memory and reduce performance during forks. */
int checkTHPEnabled(sds *error_msg) {
    char buf[1024];

    FILE *fp = fopen("/sys/kernel/mm/transparent_hugepage/enabled","r");
    if (!fp) return 0;
    if (fgets(buf,sizeof(buf),fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (strstr(buf,"[always]") != NULL) {
        *error_msg = sdsnew(
            "You have Transparent Huge Pages (THP) support enabled in your kernel. "
            "This will create latency and memory usage issues with Redis. "
            "To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, "
            "and add it to your /etc/rc.local in order to retain the setting after a reboot. "
            "Redis must be restarted after THP is disabled (set to 'madvise' or 'never').");
        return -1;
    } else {
        return 1;
    }
}

#ifdef __arm64__
/* Get size in kilobytes of the Shared_Dirty pages of the calling process for the
 * memory map corresponding to the provided address, or -1 on error. */
static int smapsGetSharedDirty(unsigned long addr) {
    int ret, in_mapping = 0, val = -1;
    unsigned long from, to;
    char buf[64];
    FILE *f;

    f = fopen("/proc/self/smaps", "r");
    if (!f) return -1;

    while (1) {
        if (!fgets(buf, sizeof(buf), f))
            break;

        ret = sscanf(buf, "%lx-%lx", &from, &to);
        if (ret == 2)
            in_mapping = from <= addr && addr < to;

        if (in_mapping && !memcmp(buf, "Shared_Dirty:", 13)) {
            sscanf(buf, "%*s %d", &val);
            /* If parsing fails, we remain with val == -1 */
            break;
        }
    }

    fclose(f);
    return val;
}

/* Older arm64 Linux kernels have a bug that could lead to data corruption
 * during background save in certain scenarios. This function checks if the
 * kernel is affected.
 * The bug was fixed in commit ff1712f953e27f0b0718762ec17d0adb15c9fd0b
 * titled: "arm64: pgtable: Ensure dirty bit is preserved across pte_wrprotect()"
 */
int checkLinuxMadvFreeForkBug(sds *error_msg) {
    int ret, pipefd[2] = { -1, -1 };
    pid_t pid;
    char *p = NULL, *q;
    int res = 1;
    long page_size = sysconf(_SC_PAGESIZE);
    long map_size = 3 * page_size;

    /* Create a memory map that's in our full control (not one used by the allocator). */
    p = mmap(NULL, map_size, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        return 0;
    }

    q = p + page_size;

    /* Split the memory map in 3 pages by setting their protection as RO|RW|RO to prevent
     * Linux from merging this memory map with adjacent VMAs. */
    ret = mprotect(q, page_size, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        res = 0;
        goto exit;
    }

    /* Write to the page once to make it resident */
    *(volatile char*)q = 0;

    /* Tell the kernel that this page is free to be reclaimed. */
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
    ret = madvise(q, page_size, MADV_FREE);
    if (ret < 0) {
        /* MADV_FREE is not available on older kernels that are presumably
         * not affected. */
        if (errno == EINVAL) goto exit;

        res = 0;
        goto exit;
    }

    /* Write to the page after being marked for freeing, this is supposed to take
     * ownership of that page again. */
    *(volatile char*)q = 0;

    /* Create a pipe for the child to return the info to the parent. */
    ret = anetPipe(pipefd, 0, 0);
    if (ret < 0) {
        res = 0;
        goto exit;
    }

    /* Fork the process. */
    pid = fork();
    if (pid < 0) {
        res = 0;
        goto exit;
    } else if (!pid) {
        /* Child: check if the page is marked as dirty, page_size in kb.
         * A value of 0 means the kernel is affected by the bug. */
        ret = smapsGetSharedDirty((unsigned long) q);
        if (!ret)
            res = -1;
        else if (ret == -1)     /* Failed to read */
            res = 0;

        ret = write(pipefd[1], &res, sizeof(res)); /* Assume success, ignore return value*/
        exit(0);
    } else {
        /* Read the result from the child. */
        ret = read(pipefd[0], &res, sizeof(res));
        if (ret < 0) {
            res = 0;
        }

        /* Reap the child pid. */
        waitpid(pid, NULL, 0);
    }

exit:
    /* Cleanup */
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);
    if (p != NULL) munmap(p, map_size);

    if (res == -1)
        *error_msg = sdsnew(
            "Your kernel has a bug that could lead to data corruption during background save. "
            "Please upgrade to the latest stable kernel.");

    return res;
}
#endif /* __arm64__ */
#endif /* __linux__ */

/*
 * Standard system check interface:
 * Each check has a name `name` and a functions pointer `check_fn`.
 * `check_fn` should return:
 *   -1 in case the check fails.
 *   1 in case the check passes.
 *   0 in case the check could not be completed (usually because of some unexpected failed system call).
 *   When (and only when) the check fails and -1 is returned and error description is places in a new sds pointer to by
 *   the single `sds*` argument to `check_fn`. This message should be freed by the caller via `sdsfree()`.
 */
typedef struct {
    const char *name;
    int (*check_fn)(sds*);
} check;

check checks[] = {
#ifdef __linux__
    {.name = "slow-clocksource", .check_fn = checkClocksource},
    {.name = "xen-clocksource", .check_fn = checkXenClocksource},
    {.name = "overcommit", .check_fn = checkOvercommit},
    {.name = "THP", .check_fn = checkTHPEnabled},
#ifdef __arm64__
    {.name = "madvise-free-fork-bug", .check_fn = checkLinuxMadvFreeForkBug},
#endif
#endif
    {.name = NULL, .check_fn = NULL}
};

/* Performs various system checks, returns 0 if any check fails, 1 otherwise. */
int syscheck(void) {
    check *cur_check = checks;
    int ret = 1;
    sds err_msg = NULL;
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
            ret = 0;
        }
        cur_check++;
    }

    return ret;
}
