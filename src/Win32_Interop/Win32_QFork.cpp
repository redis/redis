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
#include <errno.h>
#include <stdio.h>
#include <wchar.h>
#include <Psapi.h>

#define QFORK_MAIN_IMPL
#include "Win32_QFork.h"

#include "Win32_QFork_impl.h"
#include "Win32_dlmalloc.h"
#include "Win32_SmartHandle.h"
#include <vector>
#include <iostream>
using namespace std;

//#define DEBUG_WITH_PROCMON
#ifdef DEBUG_WITH_PROCMON
#define FILE_DEVICE_PROCMON_LOG 0x00009535
#define IOCTL_EXTERNAL_LOG_DEBUGOUT (ULONG) CTL_CODE( FILE_DEVICE_PROCMON_LOG, 0x81, METHOD_BUFFERED, FILE_WRITE_ACCESS )

HANDLE hProcMonDevice = INVALID_HANDLE_VALUE;
BOOL WriteToProcmon (wstring message)
{
    if (hProcMonDevice != INVALID_HANDLE_VALUE) {
        DWORD nb = 0;
        return DeviceIoControl(
            hProcMonDevice, 
            IOCTL_EXTERNAL_LOG_DEBUGOUT,
            (LPVOID)(message.c_str()),
            (DWORD)(message.length() * sizeof(wchar_t)),
            NULL,
            0,
            &nb,
            NULL);
    } else {
        return FALSE;
    }
}
#endif

/*
Redis is an in memory DB. We need to share the redis database with a quasi-forked process so that we can do the RDB and AOF saves 
without halting the main redis process, or crashing due to code that was never designed to be thread safe. Essentially we need to
replicate the COW behavior of fork() on Windows, but we don't actually need a complete fork() implementation. A complete fork() 
implementation would require subsystem level support to make happen. The following is required to make this quasi-fork scheme work:

DLMalloc (http://g.oswego.edu/dl/html/malloc.html):
    - replaces malloc/realloc/free, either by manual patching of the zmalloc code in Redis or by patching the CRT routines at link time
    - partitions space into segments that it allocates from (currently configured as 64MB chunks)
    - we map/unmap these chunks as requested into a memory map (unmapping allows the system to decide how to reduce the physical memory 
      pressure on system)

DLMallocMemoryMap:
   - An uncomitted memory map whose size is the total physical memory on the system less some memory for the rest of the system so that 
     we avoid excessive swapping.
   - This is reserved high in VM space so that it can be mapped at a specific address in the child qforked process (ASLR must be 
     disabled for these processes)
   - This must be mapped in exactly the same virtual memory space in both forker and forkee.

QForkConrolMemoryMap:
   - contains a map of the allocated segments in the DLMallocMemoryMap
   - contains handles for inter-process synchronization
   - contains pointers to some of the global data in the parent process if mapped into DLMallocMemoryMap, and a copy of any other 
     required global data

QFork process:
    - a copy of the parent process with a command line specifying QFork behavior
    - when a COW operation is requested via an event signal
        - opens the DLMAllocMemoryMap with PAGE_WRITECOPY
        - reserve space for DLMAllocMemoryMap at the memory location specified in ControlMemoryMap
        - locks the DLMalloc segments as specified in QForkConrolMemoryMap 
        - maps global data from the QForkConrolMEmoryMap into this process
        - executes the requested operation
        - unmaps all the mm views (discarding any writes)
        - signals the parent when the operation is complete

How the parent invokes the QFork process:
    - protects mapped memory segments with VirtualProtect using PAGE_WRITECOPY (both the allocated portions of DLMAllocMemoryMap and 
      the QForkConrolMemoryMap)
    - QForked process is signaled to process command
    - Parent waits (asynchronously) until QForked process signals that operation is complete, then as an atomic operation:
        - signals and waits for the forked process to terminate
        - resotres protection status on mapped blocks
        - determines which pages have been modified and copies these to a buffer
        - unmaps the view of the heap (discarding COW changes form the view)
        - remaps the view
        - copies the changes back into the view
*/

#ifndef LODWORD
    #define LODWORD(_qw)    ((DWORD)(_qw))
#endif
#ifndef HIDWORD
    #define HIDWORD(_qw)    ((DWORD)(((_qw) >> (sizeof(DWORD)*8)) & DWORD(~0)))
#endif

const SIZE_T cAllocationGranularity = 1 << 26;                   // 64MB per dlmalloc heap block 
const int cMaxBlocks = 1 << 16;                                  // 64KB*64K sections = 4TB. 4TB is the largest memory config Windows supports at present.
const SIZE_T cSystemReserve = 3 * 1024i64 * 1024i64 * 1024i64;   // Reserve left for Windows to operate on when we are heavily loaded.
const wchar_t* cMapFileBaseName = L"RedisQFork";
const char* qforkFlag = "--QFork";
const int cDeadForkWait = 30000;

typedef enum BlockState {
    bsINVALID = 0,
    bsUNMAPPED = 1,   
    bsMAPPED = 2
}BlockState;

struct QForkControl {
    HANDLE heapMemoryMapFile;
    HANDLE heapMemoryMap;
    int availableBlocksInHeap;                 // number of blocks in blockMap (dynamically determined at run time)
    SIZE_T heapBlockSize;           
    BlockState heapBlockMap[cMaxBlocks];
    LPVOID heapStart;

    OperationType typeOfOperation;
    HANDLE forkedProcessReady;
    HANDLE startOperation;
    HANDLE operationComplete;
    HANDLE operationFailed;
    HANDLE terminateForkedProcess;

    // global data pointers to be passed to the forked process
    QForkBeginInfo globalData;
    BYTE DLMallocGlobalState[1000];
    size_t DLMallocGlobalStateSize;
};

QForkControl* g_pQForkControl;
HANDLE g_hQForkControlFileMap;
HANDLE g_hForkedProcess;

BOOL QForkSlaveInit(HANDLE QForkConrolMemoryMapHandle, DWORD ParentProcessID) {
    try {
        SmartHandle shParent( 
            OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, TRUE, ParentProcessID),
            string("Could not open parent process"));

        SmartHandle shMMFile(shParent, QForkConrolMemoryMapHandle);
        SmartFileView<QForkControl> sfvMasterQForkControl( 
            shMMFile, 
            FILE_MAP_COPY, 
            string("Could not map view of QForkControl in slave. Is system swap file large enough?"));
        g_pQForkControl = sfvMasterQForkControl;

        // duplicate handles and stuff into control structure (master protected by PAGE_WRITECOPY)
        SmartHandle dupHeapFileHandle(shParent, sfvMasterQForkControl->heapMemoryMapFile);
        g_pQForkControl->heapMemoryMapFile = dupHeapFileHandle;
        SmartHandle dupForkedProcessReady(shParent,sfvMasterQForkControl->forkedProcessReady);
        g_pQForkControl->forkedProcessReady = dupForkedProcessReady;
        SmartHandle dupStartOperation(shParent,sfvMasterQForkControl->startOperation);
        g_pQForkControl->startOperation = dupStartOperation;
        SmartHandle dupOperationComplete(shParent,sfvMasterQForkControl->operationComplete);
        g_pQForkControl->operationComplete = dupOperationComplete;
        SmartHandle dupOperationFailed(shParent,sfvMasterQForkControl->operationFailed);
        g_pQForkControl->operationFailed = dupOperationFailed;
        SmartHandle dupTerminateProcess(shParent,sfvMasterQForkControl->terminateForkedProcess);
        g_pQForkControl->terminateForkedProcess = dupTerminateProcess;

       // create section handle on MM file
       SIZE_T mmSize = g_pQForkControl->availableBlocksInHeap * cAllocationGranularity;
       SmartFileMapHandle sfmhMapFile(
           g_pQForkControl->heapMemoryMapFile, 
           PAGE_WRITECOPY, 
           HIDWORD(mmSize), LODWORD(mmSize),
           string("QForkSlaveInit: Could not open file mapping object in slave"));
       g_pQForkControl->heapMemoryMap = sfmhMapFile;

        SmartFileView<byte> sfvHeap(
            g_pQForkControl->heapMemoryMap,
            FILE_MAP_COPY,
            0, 0, 0,
            g_pQForkControl->heapStart,
            string("QForkSlaveInit: Could not map heap in forked process. Is system swap file large enough?"));

        // setup DLMalloc global data
        if( SetDLMallocGlobalState(g_pQForkControl->DLMallocGlobalStateSize, g_pQForkControl->DLMallocGlobalState) != 0) {
            throw std::runtime_error("QForkSlaveInit: DLMalloc global state copy failed.");
        }

        // signal parent that we are ready
        SetEvent(g_pQForkControl->forkedProcessReady);

        // wait for parent to signal operation start
        WaitForSingleObject(g_pQForkControl->startOperation, INFINITE);

        // copy redis globals into fork process
        SetupGlobals(g_pQForkControl->globalData.globalData, g_pQForkControl->globalData.globalDataSize, g_pQForkControl->globalData.dictHashSeed);

        // execute requiested operation
        if (g_pQForkControl->typeOfOperation == OperationType::otRDB) {
            do_rdbSave(g_pQForkControl->globalData.filename);
        } else if (g_pQForkControl->typeOfOperation == OperationType::otAOF) {
            do_aofSave(g_pQForkControl->globalData.filename);
        } else {
            throw runtime_error("unexpected operation type");
        }

        // let parent know weare done
        SetEvent(g_pQForkControl->operationComplete);

        // parent will notify us when to quit
        WaitForSingleObject(g_pQForkControl->terminateForkedProcess, INFINITE);

        g_pQForkControl = NULL;
        return TRUE;
    }
    catch(std::system_error syserr) {
        g_pQForkControl = NULL;
        if(g_pQForkControl != NULL) {
            if(g_pQForkControl->operationFailed != NULL) {
                SetEvent(g_pQForkControl->operationFailed);
            }
        }
        return FALSE;
    }
    catch(std::runtime_error runerr) {
        g_pQForkControl = NULL;
        SetEvent(g_pQForkControl->operationFailed);
        return FALSE;
    }
    return FALSE;
}


BOOL QForkMasterInit() {
    // allocate file map for qfork control so it can be passed to the forked process
    g_hQForkControlFileMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0, sizeof(QForkControl),
        NULL);
    if (g_hQForkControlFileMap == NULL) {
        errno = EBADF;
        goto err;
    }

    g_pQForkControl = (QForkControl*)MapViewOfFile(
        g_hQForkControlFileMap, 
        FILE_MAP_ALL_ACCESS,
        0, 0,
        0);
    if (g_pQForkControl == NULL) {
        errno = ENOMEM;
        goto err;
    }

    // This must be called only once per process! Calling it more times than that will not recreate existing 
    // section, and dlmalloc will ultimately fail with an access violation. Once is good.
    if (dlmallopt(M_GRANULARITY, cAllocationGranularity) == 0) {
        errno = ENOMEM;
        goto err;
    }
    g_pQForkControl->heapBlockSize = cAllocationGranularity;

    // determine the number of blocks we can allocate
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&ms);
    SIZE_T maxPhysicalMapping = ms.ullTotalPhys - cSystemReserve;
    g_pQForkControl->availableBlocksInHeap = (int)(maxPhysicalMapping / cAllocationGranularity);
    if (g_pQForkControl->availableBlocksInHeap <= 0) {
        errno = ENOMEM;
        goto err;
    }

    wchar_t heapMemoryMapPath[MAX_PATH];
    swprintf_s( 
        heapMemoryMapPath, 
        MAX_PATH, 
        L"%s_%d.dat", 
        cMapFileBaseName, 
        GetCurrentProcessId());

    g_pQForkControl->heapMemoryMapFile = 
        CreateFileW( 
            heapMemoryMapPath,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL| FILE_FLAG_DELETE_ON_CLOSE,
            NULL );
    if (g_pQForkControl->heapMemoryMapFile == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        goto err;
    }

    SIZE_T mmSize = g_pQForkControl->availableBlocksInHeap * cAllocationGranularity;
    g_pQForkControl->heapMemoryMap = 
        CreateFileMappingW( 
            g_pQForkControl->heapMemoryMapFile,
            NULL,
            PAGE_READWRITE,
            HIDWORD(mmSize),
            LODWORD(mmSize),
            NULL);
    if (g_pQForkControl->heapMemoryMap == NULL) {
        errno = EBADF;
        goto err;
    }
            
    // Find a place in the virtual memory space where we can reserve space for our allocations that is likely
    // to be available in the forked process.  (If this ever fails in the forked process, we will have to launch
    // the forked process and negotiate for a shared memory address here.)
    LPVOID pHigh = VirtualAllocEx( 
        GetCurrentProcess(),
        NULL,
        mmSize,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, 
        PAGE_READWRITE);
    if (pHigh == NULL) {
        DWORD err = GetLastError();
        errno = ENOMEM;
        goto err;
    }
    if (VirtualFree(pHigh, 0, MEM_RELEASE) == FALSE) {
        DWORD err = GetLastError();
        errno = ENOMEM;
        goto err;
    }

    g_pQForkControl->heapStart = 
        MapViewOfFileEx(
            g_pQForkControl->heapMemoryMap,
            FILE_MAP_ALL_ACCESS,
            0,0,                            
            0,  
            pHigh);
    if (g_pQForkControl->heapStart == NULL) {
        DWORD err = GetLastError();
        errno = ENOMEM;
        goto err;
    }

    for (int n = 0; n < cMaxBlocks; n++) {
        g_pQForkControl->heapBlockMap[n] = 
            ((n < g_pQForkControl->availableBlocksInHeap) ?
            BlockState::bsUNMAPPED : BlockState::bsINVALID);
    }

    g_pQForkControl->typeOfOperation = OperationType::otINVALID;
    g_pQForkControl->forkedProcessReady = CreateEvent(NULL,TRUE,FALSE,NULL);
    if (g_pQForkControl->forkedProcessReady == NULL) {
        errno = EBADF;
        goto err;
    }
    g_pQForkControl->startOperation = CreateEvent(NULL,TRUE,FALSE,NULL);
    if (g_pQForkControl->startOperation == NULL) {
        errno = EBADF;
        goto err;
    }
    g_pQForkControl->operationComplete = CreateEvent(NULL,TRUE,FALSE,NULL);
    if (g_pQForkControl->operationComplete == NULL) {
        errno = EBADF;
        goto err;
    }
    g_pQForkControl->operationFailed = CreateEvent(NULL,TRUE,FALSE,NULL);
    if (g_pQForkControl->operationFailed == NULL) {
        errno = EBADF;
        goto err;
    }
    g_pQForkControl->terminateForkedProcess = CreateEvent(NULL,TRUE,FALSE,NULL);
    if (g_pQForkControl->terminateForkedProcess == NULL) {
        errno = EBADF;
        goto err;
    }

    return TRUE;

err:
    return FALSE;
}


// QFork API
StartupStatus QForkStartup(int argc, char** argv) {
    bool foundSlaveFlag = false;
    HANDLE QForkConrolMemoryMapHandle = NULL;
    DWORD PPID = 0;
    if ((argc == 3) && (strcmp(argv[0], qforkFlag) == 0)) {
        // slave command line looks like: --QFork [QForkConrolMemoryMap handle] [parent process id]
        foundSlaveFlag = true;
        char* endPtr;
        QForkConrolMemoryMapHandle = (HANDLE)strtoul(argv[1],&endPtr,10);
        char* end = NULL;
        PPID = strtoul(argv[2], &end, 10);
    }

    if (foundSlaveFlag) {
        return QForkSlaveInit( QForkConrolMemoryMapHandle, PPID ) ? StartupStatus::ssSLAVE_EXIT : StartupStatus::ssFAILED;
    } else {
        return QForkMasterInit() ? StartupStatus::ssCONTINUE_AS_MASTER : StartupStatus::ssFAILED;
    }
}

BOOL QForkShutdown() {
    if(g_hForkedProcess != NULL) {
        TerminateProcess(g_hForkedProcess, -1);
        g_hForkedProcess = NULL;
    }

    if( g_pQForkControl != NULL )
    {
        if (g_pQForkControl->forkedProcessReady != NULL) {
            CloseHandle(g_pQForkControl->forkedProcessReady);
            g_pQForkControl->forkedProcessReady = NULL;
        }
        if (g_pQForkControl->startOperation != NULL) {
            CloseHandle(g_pQForkControl->startOperation);
            g_pQForkControl->startOperation = NULL;
        }
        if (g_pQForkControl->operationComplete != NULL) {
            CloseHandle(g_pQForkControl->operationComplete);
            g_pQForkControl->operationComplete = NULL;
        }
        if (g_pQForkControl->operationFailed != NULL) {
            CloseHandle(g_pQForkControl->operationFailed);
            g_pQForkControl->operationFailed = NULL;
        }
        if (g_pQForkControl->terminateForkedProcess != NULL) {
            CloseHandle(g_pQForkControl->terminateForkedProcess);
            g_pQForkControl->terminateForkedProcess = NULL;
        }
        if (g_pQForkControl->heapMemoryMap != NULL) {
            CloseHandle(g_pQForkControl->heapMemoryMap);
            g_pQForkControl->heapMemoryMap = NULL;
        }
        if (g_pQForkControl->heapMemoryMapFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_pQForkControl->heapMemoryMapFile);
            g_pQForkControl->heapMemoryMapFile = INVALID_HANDLE_VALUE;
        }
        if (g_pQForkControl->heapStart != NULL) {
            UnmapViewOfFile(g_pQForkControl->heapStart);
            g_pQForkControl->heapStart = NULL;
        }

        if(g_pQForkControl != NULL) {
            UnmapViewOfFile(g_pQForkControl);
            g_pQForkControl = NULL;
        }
        if (g_hQForkControlFileMap != NULL) {
            CloseHandle(g_hQForkControlFileMap);
            g_hQForkControlFileMap = NULL;
        };
    }

    return TRUE;
}

BOOL BeginForkOperation(OperationType type, char* fileName, LPVOID globalData, int sizeOfGlobalData, DWORD* childPID, uint32_t dictHashSeed) {
    try {
        // copy operation data
        g_pQForkControl->typeOfOperation = type;
        strcpy_s(g_pQForkControl->globalData.filename, fileName);
        if (sizeOfGlobalData > MAX_GLOBAL_DATA) {
            throw std::runtime_error("Global state too large.");
        }
        memcpy(&(g_pQForkControl->globalData.globalData), globalData, sizeOfGlobalData);
        g_pQForkControl->globalData.globalDataSize = sizeOfGlobalData;
        g_pQForkControl->globalData.dictHashSeed = dictHashSeed;

        GetDLMallocGlobalState(&g_pQForkControl->DLMallocGlobalStateSize, NULL);
        if (g_pQForkControl->DLMallocGlobalStateSize > sizeof(g_pQForkControl->DLMallocGlobalState)) {
            throw std::runtime_error("DLMalloc global state too large.");
        }
        if(GetDLMallocGlobalState(&g_pQForkControl->DLMallocGlobalStateSize, g_pQForkControl->DLMallocGlobalState) != 0) {
            throw std::runtime_error("DLMalloc global state copy failed.");
        }

        // protect both the heap and the fork control map from propagating local changes 
        DWORD oldProtect = 0;
        if (VirtualProtect(g_pQForkControl, sizeof(QForkControl), PAGE_WRITECOPY, &oldProtect) == FALSE) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "BeginForkOperation: VirtualProtect failed");
        }
        if (VirtualProtect( 
            g_pQForkControl->heapStart, 
            g_pQForkControl->availableBlocksInHeap * g_pQForkControl->heapBlockSize, 
            PAGE_WRITECOPY, 
            &oldProtect) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "BeginForkOperation: VirtualProtect failed");
        }

        // ensure events are in the correst state
        if (ResetEvent(g_pQForkControl->operationComplete) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(), 
                "BeginForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->operationFailed) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(), 
                "BeginForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->startOperation) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "BeginForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->forkedProcessReady) == FALSE) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "BeginForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->terminateForkedProcess) == FALSE) {
            throw std::system_error(
                GetLastError(), 
                system_category(),
                "BeginForkOperation: ResetEvent() failed.");
        }

        // Launch the "forked" process
        char fileName[MAX_PATH];
        if (0 == GetModuleFileNameA(NULL, fileName, MAX_PATH)) {
            throw system_error(
                GetLastError(),
                system_category(),
                "Failed to get module name.");
        }

        STARTUPINFOA si;
        memset(&si,0, sizeof(STARTUPINFOA));
        si.cb = sizeof(STARTUPINFOA);
        char arguments[_MAX_PATH];
        memset(arguments,0,_MAX_PATH);
        PROCESS_INFORMATION pi;
        sprintf_s(arguments, _MAX_PATH, "%s %ld %ld", qforkFlag, g_hQForkControlFileMap, GetCurrentProcessId());
        if (FALSE == CreateProcessA(fileName, arguments, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            throw system_error( 
                GetLastError(),
                system_category(),
                "Problem creating slave process" );
        }
        (*childPID) = pi.dwProcessId;

        // wait for "forked" process to map memory
        if(WaitForSingleObject(g_pQForkControl->forkedProcessReady,10000) != WAIT_OBJECT_0) {
            throw system_error(
                GetLastError(),
                system_category(),
                "Forked Process did not respond in a timely manner.");
        }

        // signal the 2nd process that we want to do some work
        SetEvent(g_pQForkControl->startOperation);

        return TRUE;
    }
    catch(std::system_error syserr) {
        printf("BeginForkOperation: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
    }
    catch(std::runtime_error syserr) {
        printf("BeginForkOperation: runtime error caught. message=%s\n", syserr.what());
    }
    catch(...) {
        printf("BeginForkOperation: other exception caught.\n");
    }
    return FALSE;
}

OperationStatus GetForkOperationStatus() {
    if (WaitForSingleObject(g_pQForkControl->operationComplete, 0) == WAIT_OBJECT_0) {
        return OperationStatus::osCOMPLETE;
    }

    if (WaitForSingleObject(g_pQForkControl->operationFailed, 0) == WAIT_OBJECT_0) {
        return OperationStatus::osFAILED;
    }

    if (WaitForSingleObject(g_pQForkControl->forkedProcessReady, 0) == WAIT_OBJECT_0) {
        return OperationStatus::osINPROGRESS;
    }
    
    return OperationStatus::osUNSTARTED;
}

BOOL AbortForkOperation()
{
    try {
        if( g_hForkedProcess != 0 )
        {
            if (TerminateProcess(g_hForkedProcess, 1) == FALSE) {
                throw std::system_error(
                    GetLastError(),
                    system_category(),
                    "EndForkOperation: Killing forked process failed.");
            }
            g_hForkedProcess = 0;
        }

        return EndForkOperation();
    }
    catch(std::system_error syserr) {
        printf("0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state, then another fork operation is not possible. 
        exit(1);
    }
    catch( ... ) {
        printf("Some other exception caught in EndForkOperation().\n");
        exit(1);
    }
    return FALSE;
}


BOOL EndForkOperation() {
    try {
        SetEvent(g_pQForkControl->terminateForkedProcess);
        if( g_hForkedProcess != 0 )
        {
            if (WaitForSingleObject(g_hForkedProcess, cDeadForkWait) == WAIT_TIMEOUT) {
                if (TerminateProcess(g_hForkedProcess, 1) == FALSE) {
                    throw std::system_error(
                        GetLastError(),
                        system_category(),
                        "EndForkOperation: Killing forked process failed.");
                }
            }
            g_hForkedProcess = 0;
        }

        if (ResetEvent(g_pQForkControl->operationComplete) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(), 
                "EndForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->operationFailed) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(), 
                "EndForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->startOperation) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "EndForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->forkedProcessReady) == FALSE) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "EndForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->terminateForkedProcess) == FALSE) {
            throw std::system_error(
                GetLastError(), 
                system_category(),
                "EndForkOperation: ResetEvent() failed.");
        }

        // restore protection constants on shared memory blocks 
        DWORD oldProtect = 0;
        if (VirtualProtect(g_pQForkControl, sizeof(QForkControl), PAGE_READWRITE, &oldProtect) == FALSE) {
            throw std::system_error(
                GetLastError(), 
                system_category(),
                "EndForkOperation: VirtualProtect failed.");
        }
        if (VirtualProtect( 
            g_pQForkControl->heapStart, 
            g_pQForkControl->availableBlocksInHeap * g_pQForkControl->heapBlockSize, 
            PAGE_READWRITE, 
            &oldProtect) == FALSE ) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "EndForkOperation: VirtualProtect failed.");
        }

        //
        // What can be done to unify COW pages back into the section?
        //
        // 1. find the modified pages
        // 2. copy the modified pages into a buffer
        // 3. close the section map (discarding local changes)
        // 4. reopen the section map
        // 5. copy modified pages over reopened section map
        //
        // This assumes that the forked process is reasonably quick, such that this copy is not a huge burden.
        //
        typedef vector<INT_PTR> COWList;
        typedef COWList::iterator COWListIterator;
        COWList cowList;
        HANDLE hProcess = GetCurrentProcess();
        const size_t pageSize = 4096;
        size_t mmSize = g_pQForkControl->availableBlocksInHeap * g_pQForkControl->heapBlockSize;
        int pages = (int)(mmSize / pageSize);
        PSAPI_WORKING_SET_EX_INFORMATION* pwsi =
            new PSAPI_WORKING_SET_EX_INFORMATION[pages];
        if (pwsi == NULL) {
            throw new system_error(
                GetLastError(),
                system_category(),
                "pwsi == NULL");
        }
        memset(pwsi, 0, sizeof(PSAPI_WORKING_SET_EX_INFORMATION) * pages);
        int virtualLockFailures = 0;
        for (int page = 0; page < pages; page++) {
            pwsi[page].VirtualAddress = (BYTE*)g_pQForkControl->heapStart + page * pageSize;
        }
                
        if (QueryWorkingSetEx( 
                hProcess, 
                pwsi, 
                sizeof(PSAPI_WORKING_SET_EX_INFORMATION) * pages) == FALSE) {
            throw system_error( 
                GetLastError(),
                system_category(),
                "QueryWorkingSet failure");
        }

        for (int page = 0; page < pages; page++) {
            if (pwsi[page].VirtualAttributes.Valid == 1) {
                // A 0 share count indicates a COW page
                if (pwsi[page].VirtualAttributes.ShareCount == 0) {
                    cowList.push_back(page);
                }
            }
        }
#ifdef _DEBUG
//        cout << cowList.size() << " of " << (mmSize / pageSize) << " are modified" << endl;
#endif

        if (cowList.size() > 0) {
            LPBYTE cowBuffer = (LPBYTE)malloc(cowList.size() * pageSize);
            int bufPageIndex = 0;
            for (COWListIterator cli = cowList.begin(); cli != cowList.end(); cli++) {
                memcpy(
                    cowBuffer + (bufPageIndex * pageSize),
                    (BYTE*)g_pQForkControl->heapStart + ((*cli) * pageSize),
                    pageSize);
                bufPageIndex++;
            }

            delete [] pwsi;
            pwsi = NULL;

            // discard local changes
            if (UnmapViewOfFile(g_pQForkControl->heapStart) == FALSE) {
                throw std::system_error(
                    GetLastError(),
                    system_category(),
                    "EndForkOperation: UnmapViewOfFile failed.");
            }
            g_pQForkControl->heapStart = 
                MapViewOfFileEx(
                    g_pQForkControl->heapMemoryMap,
                    FILE_MAP_ALL_ACCESS,
                    0,0,                            
                    0,  
                    g_pQForkControl->heapStart);
            if (g_pQForkControl->heapStart == NULL) {
                throw std::system_error(
                    GetLastError(),
                    system_category(),
                    "EndForkOperation: Remapping ForkControl block failed.");
            }

            // copied back local changes to remapped view
            bufPageIndex = 0;
            for (COWListIterator cli = cowList.begin(); cli != cowList.end(); cli++) {
                memcpy(
                    (BYTE*)g_pQForkControl->heapStart + ((*cli) * pageSize),
                    cowBuffer + (bufPageIndex * pageSize),
                    pageSize);
                bufPageIndex++;
            }
            delete cowBuffer;
            cowBuffer = NULL;
        }

        // now do the same with qfork control
        LPVOID controlCopy = malloc(sizeof(QForkControl));
        if(controlCopy == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "EndForkOperation: allocation failed.");
        }
        memcpy(controlCopy, g_pQForkControl, sizeof(QForkControl));
        if (UnmapViewOfFile(g_pQForkControl) == FALSE) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "EndForkOperation: UnmapViewOfFile failed.");
        }
        g_pQForkControl = (QForkControl*)
            MapViewOfFileEx(
                g_hQForkControlFileMap,
                FILE_MAP_ALL_ACCESS,
                0,0,                            
                0,  
                g_pQForkControl);
        if (g_pQForkControl == NULL) {
            throw std::system_error(
                GetLastError(), 
                system_category(), 
                "EndForkOperation: Remapping ForkControl failed.");
        }
        memcpy(g_pQForkControl, controlCopy,sizeof(QForkControl));
        delete controlCopy;
        controlCopy = NULL;

        return TRUE;
    }
    catch(std::system_error syserr) {
        printf("0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state, then another fork operation is not possible. 
        exit(1);
    }
    catch( ... ) {
        printf("Some other exception caught in EndForkOperation().\n");
        exit(1);
    }
    return FALSE;
}

int blocksMapped = 0;
int totalAllocCalls = 0;
int totalFreeCalls = 0;

LPVOID AllocHeapBlock(size_t size, BOOL allocateHigh) {
    totalAllocCalls++;
    LPVOID retPtr = (LPVOID)-1;
    if (size % g_pQForkControl->heapBlockSize != 0 ) {
        errno = EINVAL;
        return retPtr;
    }
    int contiguousBlocksToAllocate = (int)(size / g_pQForkControl->heapBlockSize);

    size_t mapped = 0;
    int startIndex = allocateHigh ? g_pQForkControl->availableBlocksInHeap - 1 : contiguousBlocksToAllocate - 1;
    int endIndex = allocateHigh ? 0 : g_pQForkControl->availableBlocksInHeap - contiguousBlocksToAllocate;
    int direction = allocateHigh ? -1 : 1;
    int blockIndex = 0;
    int contiguousBlocksFound = 0;
    for(blockIndex = startIndex; 
        blockIndex != endIndex; 
        blockIndex += direction) {
        for (int n = 0; n < contiguousBlocksToAllocate; n++) {
            if (g_pQForkControl->heapBlockMap[blockIndex + n * direction] == BlockState::bsUNMAPPED) {
                contiguousBlocksFound++;
            }
            else {
                contiguousBlocksFound = 0;
                break;
            }
        }
        if (contiguousBlocksFound == contiguousBlocksToAllocate) {
            break;
        }
    }

    if (contiguousBlocksFound == contiguousBlocksToAllocate) {
        int allocationStart = blockIndex + (allocateHigh ? 1 - contiguousBlocksToAllocate : 0);
        LPVOID blockStart = 
            reinterpret_cast<byte*>(g_pQForkControl->heapStart) + 
            (g_pQForkControl->heapBlockSize * allocationStart);
        for(int n = 0; n < contiguousBlocksToAllocate; n++ ) {
            g_pQForkControl->heapBlockMap[allocationStart+n] = BlockState::bsMAPPED;
            blocksMapped++;
            mapped += g_pQForkControl->heapBlockSize; 
        }
        retPtr = blockStart;
    }
    else {
        errno = ENOMEM;
    }

    return retPtr;
}

BOOL FreeHeapBlock(LPVOID block, size_t size)
{
    totalFreeCalls++;
    if (size == 0) {
        return FALSE;
    }

    INT_PTR ptrDiff = reinterpret_cast<byte*>(block) - reinterpret_cast<byte*>(g_pQForkControl->heapStart);
    if (ptrDiff < 0 || (ptrDiff % g_pQForkControl->heapBlockSize) != 0) {
        return FALSE;
    }

    int blockIndex = (int)(ptrDiff / g_pQForkControl->heapBlockSize);
    if (blockIndex >= g_pQForkControl->availableBlocksInHeap) {
        return FALSE;
    }

    int contiguousBlocksToFree = (int)(size / g_pQForkControl->heapBlockSize);

    if (VirtualUnlock(block, size) == FALSE) {
        DWORD err = GetLastError();
        if (err != ERROR_NOT_LOCKED) {
            return FALSE;
        }
    };
    for (int n = 0; n < contiguousBlocksToFree; n++ ) {
        blocksMapped--;
        g_pQForkControl->heapBlockMap[blockIndex + n] = BlockState::bsUNMAPPED;
    }
    return TRUE;
}


extern "C"
{
    // The external main() is redefined as redis_main() by Win32_QFork.h.
    // The CRT will call this replacement main() before the previous main()
    // is invoked so that the QFork allocator can be setup prior to anything 
    // Redis will allocate.
    int main(int argc, char* argv[]) {
#ifdef DEBUG_WITH_PROCMON
        hProcMonDevice = 
            CreateFile( 
                L"\\\\.\\Global\\ProcmonDebugLogger", 
                GENERIC_READ|GENERIC_WRITE, 
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, 
                NULL, 
                OPEN_EXISTING, 
                FILE_ATTRIBUTE_NORMAL, 
                NULL );
#endif

        StartupStatus status = QForkStartup(argc, argv);
        if (status == ssCONTINUE_AS_MASTER) {
            int retval = redis_main(argc, argv);
            QForkShutdown();
            return retval;
        } else if (status == ssSLAVE_EXIT) {
            // slave is done - clean up and exit
            QForkShutdown();
            return 1;
        } else if (status == ssFAILED) {
            // master or slave failed initialization
            return 1;
        } else {
            // unexpected status return
            return 2;
        }
    }
}
