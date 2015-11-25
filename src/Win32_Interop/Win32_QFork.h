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

#pragma once

#include <Windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL g_IsForkedProcess;

typedef enum operationType {
    otINVALID = 0,
    otRDB = 1,
    otAOF = 2,
    otSocket = 3
} OperationType;

typedef enum operationStatus {
    osUNSTARTED = 0,
    osINPROGRESS = 1,
    osCOMPLETE = 2,
    osFAILED = 3
} OperationStatus;

typedef enum startupStatus {
    ssFAILED = 0,                 // Something went wrong, exit program with error.
    ssCONTINUE_AS_PARENT = 1,     // Parent qfork initialization complete, continue as parent instance. Call QForkShutdown when exiting.
    ssCHILD_EXIT = 2              // Child completed operation. Call QForkShutdown and exit.
} StartupStatus;

// For parent process use only
pid_t BeginForkOperation_Rdb(
    char* fileName,
    LPVOID redisData,
    int sizeOfRedisData,
    uint32_t dictHashSeed);

pid_t BeginForkOperation_Aof(
    int aof_pipe_write_ack_to_parent,
    int aof_pipe_read_ack_from_parent,
    int aof_pipe_read_data_from_parent,
    char* fileName,
    LPVOID redisData,
    int sizeOfRedisData,
    uint32_t dictHashSeed);

pid_t BeginForkOperation_Socket(
    int *fds,
    int numfds,
    uint64_t *clientids,
    int pipe_write_fd,
    LPVOID redisData,
    int sizeOfRedisData,
    uint32_t dictHashSeed);

void BeginForkOperation_Socket_Duplicate(DWORD dwProcessId);

OperationStatus GetForkOperationStatus();
BOOL EndForkOperation(int * pExitCode); 
BOOL AbortForkOperation();

#ifdef USE_DLMALLOC
  LPVOID AllocHeapBlock(size_t size, BOOL allocateHigh);
  // for no persistence optimization/feature when using dlmalloc
  extern void*(*g_malloc)(size_t);
  extern void*(*g_calloc)(size_t, size_t);
  extern void*(*g_realloc)(void*, size_t);
  extern void(*g_free)(void*);
  extern size_t(*g_msize)(void*);
#elif USE_JEMALLOC
  LPVOID AllocHeapBlock(LPVOID addr, size_t size, BOOL zero);
  BOOL PurgePages(LPVOID addr, size_t length);
#endif
BOOL FreeHeapBlock(LPVOID addr, size_t size);

#ifndef NO_QFORKIMPL
#ifdef QFORK_MAIN_IMPL
int redis_main(int argc, char** argv);
#else
#define main redis_main
#endif
#endif

#ifdef __cplusplus
}
#endif
