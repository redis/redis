/*
 * Copyright (c) 2021, Redis Ltd.
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

#include "threads_mngr.h"
/* Anti-warning macro... */
#define UNUSED(V) ((void) V)

#ifdef __linux__
#include "zmalloc.h"
#include "atomicvar.h"

#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <semaphore.h>

#define IN_PROGRESS false
static const clock_t RUN_ON_THREADS_TIMEOUT = 2;

/*================================= Globals ================================= */

static run_on_thread_cb g_callback = NULL;
static volatile size_t g_tids_len = 0;
static void **g_output_array = NULL;
static redisAtomic size_t g_thread_ids = 0;
static redisAtomic volatile size_t g_num_threads_done = 0;

static sem_t wait_for_threads_sem;

/* This flag is set while ThreadsManager_runOnThreads is running */
static redisAtomicFlag g_in_progress = 0;

/*============================ Internal prototypes ========================== */

static void invoke_callback(int sig);
static bool test_and_start(void);
static void wait_threads(void);
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

void **ThreadsManager_runOnThreads(pid_t *tids, size_t tids_len, run_on_thread_cb callback) {
    /* Check if it is safe to start running. If not - return */
    if(test_and_start() == IN_PROGRESS) {
        return NULL;
    }

    /* Update g_callback */
    g_callback = callback;

    /* Set g_tids_len  */
    g_tids_len = tids_len;

    /* Allocate the output buffer */
    g_output_array = zmalloc(sizeof(void*) * tids_len);

    /* Initialize a semaphore that we will be waiting on for the threads
    use pshared = 0 to indicate the semaphore is shared between the process's threads (and not between processes),
    and value = 0 as the initial semaphore value.*/
    sem_init(&wait_for_threads_sem, 0, 0);

    /* Send signal to all the threads in tids */
    pid_t pid = getpid();
    for (size_t i = 0; i < tids_len ; ++i) {
        tgkill(pid, tids[i], THREADS_SIGNAL);
    }

    /* Wait for all the threads to write to the output array, or until timeout is reached */
    wait_threads();

    void **ret = g_output_array;

    /* Cleanups to allow next execution */
    ThreadsManager_cleanups();

    return ret;
}

/*============================ Internal functions implementations ========================== */


static bool test_and_start(void) {
    /* atomicFlagTestSet sets the variable to true and returns true if only if it was already true. */

    bool is_in_progress;
    atomicFlagTestSet(g_in_progress, is_in_progress);

    /* If atomicFlagTestSet returned false, g_in_progress was off. */
    return !is_in_progress;

}

static void invoke_callback(int sig) {
    UNUSED(sig);

    size_t thread_id;
    atomicGetIncr(g_thread_ids, thread_id, 1);
    g_output_array[thread_id] = g_callback();
    size_t curr_done_count;
    atomicIncrGet(g_num_threads_done, curr_done_count, 1);

    /* last thread shuts down the light  */
    if (curr_done_count == g_tids_len) {
        sem_post(&wait_for_threads_sem);
    }
}

static void wait_threads(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    /* calculate relative time until timeout */
    ts.tv_sec += RUN_ON_THREADS_TIMEOUT;

    /* lock the semaphore until the semaphore value rises above zero or a signal
    handler interrupts the call. In the later case continue to wait. */
    while ((sem_timedwait(&wait_for_threads_sem, &ts)) == -1 && errno == EINTR) {
        continue;
    }
}

static void ThreadsManager_cleanups(void) {
    g_callback = NULL;
    g_tids_len = 0;
    g_output_array = NULL;
    atomicSet(g_thread_ids, 0);
    atomicSet(g_num_threads_done, 0);
    sem_destroy(&wait_for_threads_sem);

    /* Lastly, turn off g_in_progress*/
    atomicFlagClear(g_in_progress);
}
#else

void ThreadsManager_init(void) {
    /* DO NOTHING*/
}

void **ThreadsManager_runOnThreads(pid_t *tids, size_t tids_len, run_on_thread_cb callback) {
    /* DO NOTHING*/
    UNUSED(tids);
    UNUSED(tids_len);
    UNUSED(callback);
    return NULL;
}

#endif /* __linux__ */
