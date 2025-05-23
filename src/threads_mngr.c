/*
 * Copyright (c) 2021-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "threads_mngr.h"
/* Anti-warning macro... */
#define UNUSED(V) ((void) V)

#ifdef __linux__
#include "atomicvar.h"
#include "server.h"

#include <signal.h>
#include <time.h>
#include <sys/syscall.h>

#define IN_PROGRESS 1
static const clock_t RUN_ON_THREADS_TIMEOUT = 2;

/*================================= Globals ================================= */

static redisAtomic run_on_thread_cb g_callback = NULL;
static redisAtomic size_t g_tids_len = 0;
static redisAtomic size_t g_num_threads_done = 0;

/* This flag is set while ThreadsManager_runOnThreads is running */
static redisAtomic int g_in_progress = 0;

/*============================ Internal prototypes ========================== */

static void invoke_callback(int sig);
/* returns 0 if it is safe to start, IN_PROGRESS otherwise. */
static int test_and_start(void);
static void wait_threads(void);
/* Clean up global variable.
Assuming we are under the g_in_progress protection, this is not a thread-safe function */
static void ThreadsManager_cleanups(void);

/*============================ API functions implementations ========================== */

void ThreadsManager_init(void) {
    /* Register signal handler */
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    /* Not setting SA_RESTART flag means that If a signal handler is invoked while a
    system call or library function call is blocked, use the default behavior
    i.e., the call fails with the error EINTR */
    act.sa_flags = 0;
    act.sa_handler = invoke_callback;
    sigaction(SIGUSR2, &act, NULL);
}

__attribute__ ((noinline))
int ThreadsManager_runOnThreads(pid_t *tids, size_t tids_len, run_on_thread_cb callback) {
    /* Check if it is safe to start running. If not - return */
    if(test_and_start() == IN_PROGRESS) {
        return 0;
    }

    /* Update g_callback */
    atomicSet(g_callback, callback);

    /* Set g_tids_len */
    atomicSet(g_tids_len, tids_len);

    /* set g_num_threads_done to 0 To handler the case where in the previous run we reached the timeout
    and called ThreadsManager_cleanups before one or more threads were done and increased
    (the already set to 0) g_num_threads_done */
    atomicSet(g_num_threads_done, 0);

    /* Send signal to all the threads in tids */
    pid_t pid = getpid();
    for (size_t i = 0; i < tids_len ; ++i) {
        syscall(SYS_tgkill, pid, tids[i], THREADS_SIGNAL);
    }

    /* Wait for all the threads to write to the output array, or until timeout is reached */
    wait_threads();

    /* Cleanups to allow next execution */
    ThreadsManager_cleanups();

    return 1;
}

/*============================ Internal functions implementations ========================== */


static int test_and_start(void) {
    /* atomicFlagGetSet sets the variable to 1 and returns the previous value */
    int prev_state;
    atomicFlagGetSet(g_in_progress, prev_state);

    /* If prev_state is 1, g_in_progress was on. */
    return prev_state;
}

__attribute__ ((noinline))
static void invoke_callback(int sig) {
    UNUSED(sig);

    run_on_thread_cb callback;
    atomicGet(g_callback, callback);
    if (callback) {
        callback();
        atomicIncr(g_num_threads_done, 1);
    } else {
        serverLogFromHandler(LL_WARNING, "tid %ld: ThreadsManager g_callback is NULL", syscall(SYS_gettid));
    }
}

static void wait_threads(void) {
    struct timespec timeout_time;
    clock_gettime(CLOCK_REALTIME, &timeout_time);

    /* calculate relative time until timeout */
    timeout_time.tv_sec += RUN_ON_THREADS_TIMEOUT;

    /* Wait until all threads are done to invoke the callback or until we reached the timeout */
    size_t curr_done_count;
    struct timespec curr_time;
    size_t tids_len;

    do {
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 10};
        /* Sleep a bit to yield to other threads. */
        /* usleep isn't listed as signal safe, so we use select instead */
        select(0, NULL, NULL, NULL, &tv);
        atomicGet(g_num_threads_done, curr_done_count);
        clock_gettime(CLOCK_REALTIME, &curr_time);
        atomicGet(g_tids_len, tids_len);
    } while (curr_done_count < tids_len &&
             curr_time.tv_sec <= timeout_time.tv_sec);

    if (curr_time.tv_sec > timeout_time.tv_sec) {
        serverLogRawFromHandler(LL_WARNING, "wait_threads(): waiting threads timed out");
    }

}

static void ThreadsManager_cleanups(void) {
    atomicSet(g_callback, NULL);
    atomicSet(g_tids_len, 0);
    atomicSet(g_num_threads_done, 0);

    /* Lastly, turn off g_in_progress */
    atomicSet(g_in_progress, 0);

}
#else

void ThreadsManager_init(void) {
    /* DO NOTHING */
}

int ThreadsManager_runOnThreads(pid_t *tids, size_t tids_len, run_on_thread_cb callback) {
    /* DO NOTHING */
    UNUSED(tids);
    UNUSED(tids_len);
    UNUSED(callback);
    return 1;
}

#endif /* __linux__ */
