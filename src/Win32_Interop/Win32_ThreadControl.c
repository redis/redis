/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <Windows.h>
#include <assert.h>


volatile LONG g_NumWorkerThreads = 0;

// Safe mode means the threads are not touching the heap, or they are suspended because of an explicit suspension request
// Threads in safe mode because they are not touching the heap will block if trying to transition to unsafe mode while a suspension is requested
volatile LONG g_NumWorkerThreadsInSafeMode = 0;

volatile LONG g_SuspensionRequested = 0;
HANDLE g_hResumeFromSuspension;

CRITICAL_SECTION g_ThreadControlMutex;


void InitThreadControl() {    
    InitializeCriticalSection(&g_ThreadControlMutex);
    g_hResumeFromSuspension = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (!g_hResumeFromSuspension) {
        exit(GetLastError());
    }
}

void IncrementWorkerThreadCount() {
    EnterCriticalSection(&g_ThreadControlMutex);
    g_NumWorkerThreads++;
    LeaveCriticalSection(&g_ThreadControlMutex);
}

void DecrementWorkerThreadCount() {
    EnterCriticalSection(&g_ThreadControlMutex);
    g_NumWorkerThreads--;
    LeaveCriticalSection(&g_ThreadControlMutex);
}


// Returns TRUE if threads are already in safe mode or suspended
BOOL SuspensionCompleted() { 
    BOOL result;
    EnterCriticalSection(&g_ThreadControlMutex);
    result = (g_NumWorkerThreadsInSafeMode == g_NumWorkerThreads);
    LeaveCriticalSection(&g_ThreadControlMutex);
    return result;
}

// This is meant to be called from the main thread only. 
void RequestSuspension() {
    if (!g_SuspensionRequested) {
        if (!ResetEvent(g_hResumeFromSuspension)) {
            exit(GetLastError());
        }
        _InterlockedOr(&g_SuspensionRequested, 1);
    }
}

void ResumeFromSuspension() {
    // This is meant to be called from the main thread only. 
    assert(g_SuspensionRequested && SuspensionCompleted());

    _InterlockedAnd(&g_SuspensionRequested, 0);
    if (!SetEvent(g_hResumeFromSuspension)) {
        exit(GetLastError());
    }
}

void WorkerThread_EnterSafeMode() {
    EnterCriticalSection(&g_ThreadControlMutex);
    g_NumWorkerThreadsInSafeMode++;
    LeaveCriticalSection(&g_ThreadControlMutex);
}

void WorkerThread_ExitSafeMode() {
    for(;;) {
        EnterCriticalSection(&g_ThreadControlMutex);
        if (g_SuspensionRequested) {
            LeaveCriticalSection(&g_ThreadControlMutex);
            if (WaitForSingleObject(g_hResumeFromSuspension, INFINITE) != WAIT_OBJECT_0) {
                exit(GetLastError());
            }
            continue;
        } else {
            g_NumWorkerThreadsInSafeMode--;
            LeaveCriticalSection(&g_ThreadControlMutex);
            break;
        }
    }
}

