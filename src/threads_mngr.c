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
#include "server.h"

#include <signal.h>
#include <time.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/syscall.h>

#define IN_PROGRESS 1
static const clock_t RUN_ON_THREADS_TIMEOUT = 2;

/*================================= Globals ================================= */

static run_on_thread_cb g_callback = NULL;
static volatile size_t g_tids_len = 0;
static redisAtomic size_t g_num_threads_done = 0;

static sem_t wait_for_threads_sem;

/* This flag is set while ThreadsManager_runOnThreads is running */
static redisAtomic int g_in_progress = 0;

static pthread_rwlock_t globals_rw_lock = PTHREAD_RWLOCK_INITIALIZER;
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
    g_callback = callback;

    /* Set g_tids_len */
    g_tids_len = tids_len;

    /* Initialize a semaphore that we will be waiting on for the threads
    use pshared = 0 to indicate the semaphore is shared between the process's threads (and not between processes),
    and value = 0 as the initial semaphore value. */
    sem_init(&wait_for_threads_sem, 0, 0);

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

    /* If the lock is already locked for write, we are running cleanups, no reason to proceed. */
    if(0 != pthread_rwlock_tryrdlock(&globals_rw_lock)) {
        serverLog(LL_WARNING, "threads_mngr: ThreadsManager_cleanups() is in progress, can't invoke signal handler.");
        return;
    }

    if (g_callback) {
        g_callback();
        size_t curr_done_count;
        atomicIncrGet(g_num_threads_done, curr_done_count, 1);

        /* last thread shuts down the light */
        if (curr_done_count == g_tids_len) {
            sem_post(&wait_for_threads_sem);
        }
    }

    pthread_rwlock_unlock(&globals_rw_lock);
}

static void wait_threads(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    /* calculate relative time until timeout */
    ts.tv_sec += RUN_ON_THREADS_TIMEOUT;

    int status = 0;

    /* lock the semaphore until the semaphore value rises above zero or a signal
    handler interrupts the call. In the later case continue to wait. */
    while ((status = sem_timedwait(&wait_for_threads_sem, &ts)) == -1 && errno == EINTR) {
        serverLog(LL_WARNING, "threads_mngr: waiting for threads' output was interrupted by signal. Continue waiting.");
        continue;
    }
}

static void ThreadsManager_cleanups(void) {
    pthread_rwlock_wrlock(&globals_rw_lock);

    g_callback = NULL;
    g_tids_len = 0;
    g_num_threads_done = 0;
    sem_destroy(&wait_for_threads_sem);

    /* Lastly, turn off g_in_progress */
    atomicSet(g_in_progress, 0);
    pthread_rwlock_unlock(&globals_rw_lock);

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
