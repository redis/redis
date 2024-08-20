/*
 * Copyright (c) 2021-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#pragma once

#include "fmacros.h"

#include <sys/types.h>
#include <unistd.h>

/** This is an API to invoke callback on a list of threads using a user defined signal handler.
 * NOTE: This is API is only supported only in linux systems. 
 * Calling the functions below on any other system does nothing.
*/

#define THREADS_SIGNAL SIGUSR2

/* Callback signature */
typedef void(*run_on_thread_cb)(void);

/* Register the process to THREADS_SIGNAL */
void ThreadsManager_init(void);

/** @brief Invoke callback by each thread in tids.
 *
 * @param tids  An array of threads that need to invoke callback.
 * @param tids_len The number of threads in @param tids.
 * @param callback A callback to be invoked by each thread in @param tids.
 *
 * NOTES:
 * It is assumed that all the threads don't block or ignore THREADS_SIGNAL.
 *
 * It is safe to include the calling thread in @param tids. However, be aware that subsequent tids will
 * not be signaled until the calling thread returns from the callback invocation.
 * Hence, it is recommended to place the calling thread last in @param tids.
 *
 * The function returns only when @param tids_len threads have returned from @param callback, or when we reached timeout.
 *
 * @return 1 if successful, 0 If ThreadsManager_runOnThreads is already in the middle of execution.
 *
**/

int ThreadsManager_runOnThreads(pid_t *tids, size_t tids_len, run_on_thread_cb callback);
