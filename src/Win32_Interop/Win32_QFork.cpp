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

QForkControlMemoryMap:
- contains a map of the allocated segments in the DLMallocMemoryMap
- contains handles for inter-process synchronization
- contains pointers to some of the global data in the parent process if mapped into DLMallocMemoryMap, and a copy of any other
required global data

QFork process:
- a copy of the parent process with a command line specifying QFork behavior
- when a COW operation is requested via an event signal
- opens the DLMAllocMemoryMap with PAGE_WRITECOPY
- reserve space for DLMAllocMemoryMap at the memory location specified in ControlMemoryMap
- locks the DLMalloc segments as specified in QForkControlMemoryMap
- maps global data from the QForkControlMEmoryMap into this process
- executes the requested operation
- unmaps all the mm views (discarding any writes)
- signals the parent when the operation is complete

How the parent invokes the QFork process:
- protects mapped memory segments with VirtualProtect using PAGE_WRITECOPY (both the allocated portions of DLMAllocMemoryMap and
the QForkControlMemoryMap)
- QForked process is signaled to process command
- Parent waits (asynchronously) until QForked process signals that operation is complete, then as an atomic operation:
- signals and waits for the forked process to terminate
- resotres protection status on mapped blocks
- determines which pages have been modified and copies these to a buffer
- unmaps the view of the heap (discarding COW changes form the view)
- remaps the view
- copies the changes back into the view
*/

/*
Not specifying the maxmemory flag will result in the default behavior of: new key generation not bounded by heap usage,
and the heap size equal to the size of physical memory.

Redis will respect the maxmemory flag by preventing new key creation when the number of bytes allocated in the heap
exceeds the level specified by the maxmemory flag. This does not account for heap fragmentation or memory usage by
the heap allocator. To allow for this extra space we allow the heap to allocate 10 times the physical memory.

Since the heap is entirely contained in the system paging file, the size of the system paging file needs to be large accordingly.

During forking the system paging file is used for managing virtual memory sharing and the copy on write pages for both
forker and forkee. There must be sufficient system paging space availability for this. By default Windows will dynamically
allocate a system paging file that will expand up to about (3.5 * physical).
*/

#include "win32_types.h"
#include "Win32_FDAPI.h"
#include "Win32_Common.h"
#include "Win32_Assert.h"

#include <Windows.h>
#include <WinNT.h>
#include <errno.h>
#include <Psapi.h>
#include <iostream>

#define QFORK_MAIN_IMPL
#include "Win32_QFork.h"
#include "Win32_QFork_impl.h"
#include "Win32_SmartHandle.h"
#include "Win32_Service.h"
#include "Win32_CommandLine.h"
#include "Win32_RedisLog.h"
#include "Win32_StackTrace.h"
#include "Win32_ThreadControl.h"
#include "Win32_EventLog.h"

#ifdef USE_DLMALLOC
  #include "Win32_dlmalloc.h"
#elif USE_JEMALLOC
  #include <jemalloc/jemalloc.h>
#endif

using namespace std;

//#define DEBUG_WITH_PROCMON
#ifdef DEBUG_WITH_PROCMON
#define FILE_DEVICE_PROCMON_LOG 0x00009535
#define IOCTL_EXTERNAL_LOG_DEBUGOUT (ULONG) CTL_CODE( FILE_DEVICE_PROCMON_LOG, 0x81, METHOD_BUFFERED, FILE_WRITE_ACCESS )

HANDLE hProcMonDevice = INVALID_HANDLE_VALUE;
BOOL WriteToProcmon(wstring message)
{
    if (hProcMonDevice != INVALID_HANDLE_VALUE) {
        DWORD nb = 0;
        return DeviceIoControl(
            hProcMonDevice,
            IOCTL_EXTERNAL_LOG_DEBUGOUT,
            (LPVOID) (message.c_str()),
            (DWORD) (message.length() * sizeof(wchar_t)),
            NULL,
            0,
            &nb,
            NULL);
    } else {
        return FALSE;
    }
}
#endif

#ifndef PAGE_REVERT_TO_FILE_MAP
  #define PAGE_REVERT_TO_FILE_MAP 0x80000000  // From Win8.1 SDK
#endif

#define IFFAILTHROW(a,m) if(!(a)) { throw system_error(GetLastError(), system_category(), m); }

#define MAX_REDIS_DATA_SIZE 10000
struct QForkInfo {
    BYTE redisData[MAX_REDIS_DATA_SIZE];
    size_t redisDataSize;
    uint32_t dictHashSeed;
    char filename[MAX_PATH];
    int *fds;
    int numfds;
    uint64_t *clientids;
    HANDLE pipe_write_handle;
    HANDLE aof_pipe_write_ack_handle;
    HANDLE aof_pipe_read_ack_handle;
    HANDLE aof_pipe_read_data_handle;
    LPVOID protocolInfo;
};

extern "C"
{
    int checkForSentinelMode(int argc, char **argv);
    void InitTimeFunctions();
    PORT_LONGLONG memtoll(const char *p, int *err);     // Forward def from util.h

#ifdef USE_DLMALLOC
    void*(*g_malloc)(size_t) = nullptr;
    void*(*g_calloc)(size_t, size_t) = nullptr;
    void*(*g_realloc)(void*, size_t) = nullptr;
    void(*g_free)(void*) = nullptr;
    size_t(*g_msize)(void*) = nullptr;
#endif
}

#ifdef USE_DLMALLOC
  const size_t cAllocationGranularity = 1 << 18;    // 256KB per heap block (matches large block allocation threshold of dlmalloc)
  #ifdef _WIN64
    const int  cMaxBlocks = 1 << 22;                // 256KB * 4M heap blocks = 1TB
  #else
    const int  cMaxBlocks = 1 << 12;                // 256KB * 4K heap blocks = 1GB
  #endif
#elif USE_JEMALLOC
  const size_t cAllocationGranularity = 1 << 22;    // 4MB per heap block (matches the default allocation threshold of jemalloc)
  #ifdef _WIN64
    const int  cMaxBlocks = 1 << 18;                // 4MB * 256K heap blocks = 1TB
  #else
    const int  cMaxBlocks = 1 << 8;                 // 4MB * 256 heap blocks = 1GB
  #endif
#endif

const int cDeadForkWait = 30000;

enum class BlockState : uint8_t { bsINVALID = 0, bsUNMAPPED = 1, bsMAPPED_IN_USE = 2, bsMAPPED_FREE = 3 };

struct heapBlockInfo {
    HANDLE heapMap;
    BlockState state;
};

struct QForkControl {
    LPVOID heapStart;
    LPVOID heapEnd;
    int maxAvailableBlocks;
    int numMappedBlocks;
    int blockSearchStart;
    heapBlockInfo heapBlockList[cMaxBlocks];

    OperationType typeOfOperation;
    HANDLE operationComplete;
    HANDLE operationFailed;

    // Global data pointers to be passed to the forked process
    QForkInfo globalData;
#ifdef USE_DLMALLOC
    BYTE DLMallocGlobalState[1000];
    size_t DLMallocGlobalStateSize;
#endif
};

QForkControl* g_pQForkControl;
HANDLE g_hQForkControlFileMap;
HANDLE g_hForkedProcess = 0;
int g_ChildExitCode = 0; // For child process
BOOL g_SentinelMode;
BOOL g_PersistenceDisabled;
/* If g_IsForkedProcess || g_PersistenceDisabled || g_SentinelMode is true
 * memory is not allocated from the memory map heap, instead the system heap
 * is used */
BOOL g_BypassMemoryMapOnAlloc;
/* g_HasMemoryMappedHeap is true if g_PersistenceDisabled and g_SentinelMode
 * are both false, so it is true for the parent process and the child process
 * when persistence is available */
BOOL g_HasMemoryMappedHeap;

bool ReportSpecialSystemErrors(int error) {
    switch (error)
    {
        case ERROR_NO_SYSTEM_RESOURCES:
        case ERROR_COMMITMENT_LIMIT:
        {
            redisLog(
                REDIS_WARNING,
                "\n"
                "The Windows version of Redis reserves heap memory from the system paging file\n"
                "for sharing with the forked process used for persistence operations."
                "At this time there is insufficient contiguous free space available in the\n"
                "system paging file. You may increase the size of the system paging file.\n"
                "Sometimes a reboot will defragment the system paging file sufficiently for\n"
                "this operation to complete successfully.\n"
                "\n"
                "Redis can not continue. Exiting."
                );
            RedisEventLog().LogError("Failed to reserves heap memory from the system paging file.");
            return true;
        }
        default:
            return false;
    }
}

#ifdef USE_DLMALLOC
BOOL DLMallocInizialized = false;
void DLMallocInit() {
    // This must be called only once per process
    if (DLMallocInizialized == FALSE) {
        IFFAILTHROW(dlmallopt(M_GRANULARITY, cAllocationGranularity), "DLMalloc failed initializing allocation granularity.");
        DLMallocInizialized = TRUE;
    }
}
#endif

BOOL QForkChildInit(HANDLE QForkControlMemoryMapHandle, DWORD ParentProcessID) {
    SmartHandle shParent;
    SmartHandle shQForkControlHeapMap;
    SmartFileView<QForkControl> sfvParentQForkControl;
    SmartHandle dupOperationComplete;
    SmartHandle dupOperationFailed;

    try {
        shParent.Assign( 
            OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, TRUE, ParentProcessID),
            string("Could not open parent process"));

        shQForkControlHeapMap.Assign(shParent, QForkControlMemoryMapHandle);
        sfvParentQForkControl.Assign(
            shQForkControlHeapMap,
            FILE_MAP_COPY, 
            string("Could not map view of QForkControl in child. Is system swap file large enough?"));
        g_pQForkControl = sfvParentQForkControl;

        // Duplicate handles and stuff into control structure (parent protected by PAGE_WRITECOPY)
        
        dupOperationComplete.Assign(shParent, sfvParentQForkControl->operationComplete);
        g_pQForkControl->operationComplete = dupOperationComplete;
        
        dupOperationFailed.Assign(shParent, sfvParentQForkControl->operationFailed);
        g_pQForkControl->operationFailed = dupOperationFailed;

        vector<SmartHandle> dupHeapHandle(g_pQForkControl->numMappedBlocks);
        vector<SmartFileView<byte>> sfvHeap(g_pQForkControl->numMappedBlocks);
        for (int i = 0; i < g_pQForkControl->numMappedBlocks; i++) {
            if (sfvParentQForkControl->heapBlockList[i].state == BlockState::bsMAPPED_IN_USE) {
                dupHeapHandle[i].Assign(shParent, sfvParentQForkControl->heapBlockList[i].heapMap);
                g_pQForkControl->heapBlockList[i].heapMap = dupHeapHandle[i];

                sfvHeap[i].Assign(g_pQForkControl->heapBlockList[i].heapMap,
                    FILE_MAP_COPY,
                    0,
                    0,
                    cAllocationGranularity,
                    (byte*) g_pQForkControl->heapStart + i * cAllocationGranularity,
                    string("QForkChildInit: could not map heap in forked process"));
            } else {
                g_pQForkControl->heapBlockList[i].heapMap = NULL;
                g_pQForkControl->heapBlockList[i].state = BlockState::bsINVALID;
            }
        }

#ifdef USE_DLMALLOC
        // Setup DLMalloc global data
        if (SetDLMallocGlobalState(g_pQForkControl->DLMallocGlobalStateSize, g_pQForkControl->DLMallocGlobalState) != 0) {
            throw runtime_error("DLMalloc global state copy failed.");
        }
#endif

        // Copy redis globals into fork process
        SetupRedisGlobals(g_pQForkControl->globalData.redisData,
                          g_pQForkControl->globalData.redisDataSize,
                          g_pQForkControl->globalData.dictHashSeed);

        // Execute requested operation
        if (g_pQForkControl->typeOfOperation == OperationType::otRDB) {
            g_ChildExitCode = do_rdbSave(g_pQForkControl->globalData.filename);
        } else if (g_pQForkControl->typeOfOperation == OperationType::otAOF) {
            int aof_pipe_read_ack = FDAPI_open_osfhandle((intptr_t) g_pQForkControl->globalData.aof_pipe_read_ack_handle, _O_APPEND);
            int aof_pipe_read_data = FDAPI_open_osfhandle((intptr_t) g_pQForkControl->globalData.aof_pipe_read_data_handle, _O_APPEND);
            int aof_pipe_write_ack = FDAPI_open_osfhandle((intptr_t) g_pQForkControl->globalData.aof_pipe_write_ack_handle, _O_APPEND);
            g_ChildExitCode = do_aofSave(g_pQForkControl->globalData.filename,
                                         aof_pipe_read_ack,
                                         aof_pipe_read_data,
                                         aof_pipe_write_ack
                                         );
        } else if (g_pQForkControl->typeOfOperation == OperationType::otSocket) {
            LPWSAPROTOCOL_INFO lpProtocolInfo = (LPWSAPROTOCOL_INFO) g_pQForkControl->globalData.protocolInfo;
            int pipe_write_fd = FDAPI_open_osfhandle((intptr_t) g_pQForkControl->globalData.pipe_write_handle, _O_APPEND);
            int* fds = (int*) malloc(sizeof(int) * g_pQForkControl->globalData.numfds);
            for (int i = 0; i < g_pQForkControl->globalData.numfds; i++) {
                fds[i] = FDAPI_WSASocket(FROM_PROTOCOL_INFO,
                                         FROM_PROTOCOL_INFO,
                                         FROM_PROTOCOL_INFO,
                                         &lpProtocolInfo[i],
                                         0,
                                         WSA_FLAG_OVERLAPPED);
            }

            g_ChildExitCode = do_socketSave(fds,
                                            g_pQForkControl->globalData.numfds,
                                            g_pQForkControl->globalData.clientids,
                                            pipe_write_fd);
            // After the socket replication has finished, close the duplicated sockets.
            // Failing to close the sockets properly will produce a socket read error
            // on both the parent process and the slave.
            for (int i = 0; i < g_pQForkControl->globalData.numfds; i++) {
                FDAPI_CloseDuplicatedSocket(fds[i]);
            }
            free(fds);
        } else {
            throw runtime_error("unexpected operation type");
        }

        // Let parent know we are done
        SetEvent(g_pQForkControl->operationComplete);

        g_pQForkControl = NULL;
        return TRUE;
    }
    catch(system_error syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            RedisEventLog().LogError("QForkChildInit: system error. " + string(syserr.what()));
            redisLog(REDIS_WARNING, "QForkChildInit: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
        }
    }
    catch(runtime_error runerr) {
        RedisEventLog().LogError("QForkChildInit: runtime error. " + string(runerr.what()));
        redisLog(REDIS_WARNING, "QForkChildInit: runtime error caught. message=%s\n", runerr.what());
    }
    
    if (g_pQForkControl != NULL) {
        if (g_pQForkControl->operationFailed != NULL) {
            SetEvent(g_pQForkControl->operationFailed);
        }
        g_pQForkControl = NULL;
    }
    return FALSE;
}

BOOL QForkParentInit() {
    try {
        // Allocate file map for qfork control so it can be passed to the forked process
        g_hQForkControlFileMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0, sizeof(QForkControl),
            NULL);
        IFFAILTHROW(g_hQForkControlFileMap, "QForkMasterInit: CreateFileMapping failed");

        g_pQForkControl = (QForkControl*)MapViewOfFile(
            g_hQForkControlFileMap, 
            FILE_MAP_ALL_ACCESS,
            0, 0,
            0);
        IFFAILTHROW(g_pQForkControl, "QForkMasterInit: MapViewOfFile failed");

        MEMORYSTATUSEX memstatus;
        memstatus.dwLength = sizeof(MEMORYSTATUSEX);
        IFFAILTHROW(GlobalMemoryStatusEx(&memstatus), "QForkMasterInit: cannot get global memory status");

#ifdef _WIN64
        size_t max_heap_allocation = memstatus.ullTotalPhys * 10;
        if (max_heap_allocation > cAllocationGranularity * cMaxBlocks) {
            max_heap_allocation = cAllocationGranularity * cMaxBlocks;
        }
#else
        // On x86 the limit is always cAllocationGranularity * cMaxBlocks
        size_t max_heap_allocation = cAllocationGranularity * cMaxBlocks;
#endif

        // maxAvailableBlocks is guaranteed to be <= cMaxBlocks
        // On x86 maxAvailableBlocks = cMaxBlocks
        g_pQForkControl->maxAvailableBlocks = (int) (max_heap_allocation / cAllocationGranularity);
        g_pQForkControl->blockSearchStart = 0;
        g_pQForkControl->numMappedBlocks = 0;

        // Find a place in the virtual memory space where we can reserve space for
        // our allocations that is likely to be available in the forked process.
        LPVOID pHigh = VirtualAllocEx( 
            GetCurrentProcess(),
            NULL,
            // the +1 is needed since we will align the heap start address
            (g_pQForkControl->maxAvailableBlocks + 1) * cAllocationGranularity,
#ifdef _WIN64
            MEM_RESERVE | MEM_TOP_DOWN,
#else
            MEM_RESERVE,
#endif
            PAGE_READWRITE);
        IFFAILTHROW(pHigh, "QForkMasterInit: VirtualAllocEx failed.");

        IFFAILTHROW(VirtualFree(pHigh, 0, MEM_RELEASE), "QForkMasterInit: VirtualFree failed.");

        // Need to adjust the heap start address to align on allocation granularity offset
        g_pQForkControl->heapStart = (LPVOID) (((uintptr_t) pHigh + cAllocationGranularity) - ((uintptr_t) pHigh % cAllocationGranularity));
        g_pQForkControl->heapEnd = (LPVOID) ((uintptr_t) g_pQForkControl->heapStart + (g_pQForkControl->maxAvailableBlocks + 1) * cAllocationGranularity);

        // Reserve the heap memory that will be mapped on demand in AllocHeapBlock()
        for (int i = 0; i < g_pQForkControl->maxAvailableBlocks; i++) {
            LPVOID addr = (byte*) g_pQForkControl->heapStart + i * cAllocationGranularity;
            IFFAILTHROW(VirtualAlloc(addr, cAllocationGranularity, MEM_RESERVE, PAGE_READWRITE),
                "QForkMasterInit: VirtualAlloc of reserve segment failed");
        }

        for (int i = 0; i < g_pQForkControl->maxAvailableBlocks; i++) {
            g_pQForkControl->heapBlockList[i].state = BlockState::bsUNMAPPED;
            g_pQForkControl->heapBlockList[i].heapMap = NULL;
        }
        for (int i = g_pQForkControl->maxAvailableBlocks; i < cMaxBlocks; i++) {
            g_pQForkControl->heapBlockList[i].state = BlockState::bsINVALID;
        }

        g_pQForkControl->typeOfOperation = OperationType::otINVALID;
        g_pQForkControl->operationComplete = CreateEvent(NULL,TRUE,FALSE,NULL);
        IFFAILTHROW(g_pQForkControl->operationComplete, "QForkMasterInit: CreateEvent failed.");

        g_pQForkControl->operationFailed = CreateEvent(NULL,TRUE,FALSE,NULL);
        IFFAILTHROW(g_pQForkControl->operationFailed, "QForkMasterInit: CreateEvent failed.");

        return TRUE;
    }
    catch(system_error syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            RedisEventLog().LogError("QForkParentInit: system error. " + string(syserr.what()));
            redisLog(REDIS_WARNING, "QForkParentInit: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
        }
    }
    catch(runtime_error runerr) {
        RedisEventLog().LogError("QForkParentInit: runtime error. " + string(runerr.what()));
        redisLog(REDIS_WARNING, "QForkParentInit: runtime error caught. message=%s\n", runerr.what());
    }
    catch (exception ex) {
        RedisEventLog().LogError("QForkParentInit: an exception occurred. " + string(ex.what()));
        redisLog(REDIS_WARNING, "QForkParentInit: other exception caught.\n");
    }
    return FALSE;
}

StartupStatus QForkStartup() {
    PERFORMANCE_INFORMATION perfinfo;
    perfinfo.cb = sizeof(PERFORMANCE_INFORMATION);
    if (FALSE == GetPerformanceInfo(&perfinfo, sizeof(PERFORMANCE_INFORMATION))) {
        redisLog(REDIS_WARNING, "GetPerformanceInfo failed.\n");
        redisLog(REDIS_WARNING, "Failing startup.\n");
        return StartupStatus::ssFAILED;
    }
    Globals::pageSize = perfinfo.PageSize;

    if (g_IsForkedProcess) {
        // Child command line looks like: --QFork [QForkControlMemoryMap handle] [parent pid]
        HANDLE QForkControlMemoryMapHandle = (HANDLE) strtoull(g_argMap[cQFork].at(0).at(0).c_str(), NULL, 10);
        DWORD PPID = strtoul(g_argMap[cQFork].at(0).at(1).c_str(), NULL, 10);
        return QForkChildInit(QForkControlMemoryMapHandle, PPID) ? StartupStatus::ssCHILD_EXIT : StartupStatus::ssFAILED;
    } else {
        return QForkParentInit() ? StartupStatus::ssCONTINUE_AS_PARENT : StartupStatus::ssFAILED;
    }
}

void CloseEventHandle(HANDLE * phandle){
    if (*phandle != NULL) {
        CloseHandle(*phandle);
        *phandle = NULL;
    }
}

BOOL QForkShutdown() {
    if (g_hForkedProcess != NULL) {
        TerminateProcess(g_hForkedProcess, -1);
        CloseHandle(g_hForkedProcess);
        g_hForkedProcess = NULL;
    }

    if (g_pQForkControl != NULL)
    {
        CloseEventHandle(&g_pQForkControl->operationComplete);
        CloseEventHandle(&g_pQForkControl->operationFailed);

        for (int i = 0; i < g_pQForkControl->numMappedBlocks; i++) {
            if (g_pQForkControl->heapBlockList[i].state != BlockState::bsUNMAPPED) {
                CloseEventHandle(&g_pQForkControl->heapBlockList[i].heapMap);
            }
        }

        if (g_pQForkControl->heapStart != NULL) {
            UnmapViewOfFile(g_pQForkControl->heapStart);
            g_pQForkControl->heapStart = NULL;
        }

        if (g_pQForkControl != NULL) {
            UnmapViewOfFile(g_pQForkControl);
            g_pQForkControl = NULL;
        }
        
        CloseEventHandle(&g_hQForkControlFileMap);
    }

    return TRUE;
}

void CopyForkOperationData(OperationType type, LPVOID redisData, int redisDataSize, uint32_t dictHashSeed) {
    // Copy operation data
    g_pQForkControl->typeOfOperation = type;
    if (redisDataSize > MAX_REDIS_DATA_SIZE) {
        throw runtime_error("Global redis data too large.");
    }
    memcpy(&(g_pQForkControl->globalData.redisData), redisData, redisDataSize);
    g_pQForkControl->globalData.redisDataSize = redisDataSize;
    g_pQForkControl->globalData.dictHashSeed = dictHashSeed;

#ifdef USE_DLMALLOC
    GetDLMallocGlobalState(&g_pQForkControl->DLMallocGlobalStateSize, NULL);
    if (g_pQForkControl->DLMallocGlobalStateSize > sizeof(g_pQForkControl->DLMallocGlobalState)) {
        throw runtime_error("DLMalloc global state too large.");
    }
    if(GetDLMallocGlobalState(&g_pQForkControl->DLMallocGlobalStateSize, g_pQForkControl->DLMallocGlobalState) != 0) {
        throw runtime_error("DLMalloc global state copy failed.");
    }
#endif

    // Protect the qfork control map from propagating local changes
    DWORD oldProtect = 0;
    IFFAILTHROW(VirtualProtect(g_pQForkControl, sizeof(QForkControl), PAGE_WRITECOPY, &oldProtect),
                "CopyForkOperationData: VirtualProtect failed for QForkControl");

    // Protect the heap map from propagating local changes
    for (int i = 0; i < g_pQForkControl->numMappedBlocks; i++) {
        if (g_pQForkControl->heapBlockList[i].state == BlockState::bsMAPPED_IN_USE) {
            oldProtect = 0;
            VirtualProtect((byte*) g_pQForkControl->heapStart + i * cAllocationGranularity,
                           cAllocationGranularity,
                           PAGE_WRITECOPY,
                           &oldProtect);
        }
    }
}

void CreateChildProcess(PROCESS_INFORMATION *pi, DWORD dwCreationFlags = 0) {
    // Ensure events are in the correst state
    IFFAILTHROW(ResetEvent(g_pQForkControl->operationComplete),
                           "CreateChildProcess: ResetEvent() failed.");
    IFFAILTHROW(ResetEvent(g_pQForkControl->operationFailed),
                           "CreateChildProcess: ResetEvent() failed.");

    // Launch the "forked" process
    char fileName[MAX_PATH];
    IFFAILTHROW(GetModuleFileNameA(NULL, fileName, MAX_PATH),
                "Failed to get module name.");

    STARTUPINFOA si;
    memset(&si, 0, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    char arguments[_MAX_PATH];
    memset(arguments, 0, _MAX_PATH);

    sprintf_s(arguments,
              _MAX_PATH,
              "\"%s\" --%s %llu %lu --%s \"%s\"",
              fileName,
              cQFork.c_str(),
              (uint64_t) g_hQForkControlFileMap,
              GetCurrentProcessId(),
              cLogfile.c_str(),
              getLogFilename());

    IFFAILTHROW(CreateProcessA(fileName, arguments, NULL, NULL, TRUE, dwCreationFlags, NULL, NULL, &si, pi),
                "Problem creating slave process");
    g_hForkedProcess = pi->hProcess;
}

typedef void (*CHILD_PID_HOOK)(DWORD pid);

pid_t BeginForkOperation(OperationType type,
                         LPVOID redisData,
                         int redisDataSize,
                         uint32_t dictHashSeed)
{
    PROCESS_INFORMATION pi;
    try {
        pi.hProcess = INVALID_HANDLE_VALUE;
        pi.dwProcessId = -1;

        if (type == OperationType::otSocket) {
            CreateChildProcess(&pi, CREATE_SUSPENDED);
            BeginForkOperation_Socket_Duplicate(pi.dwProcessId);
            CopyForkOperationData(type, redisData, redisDataSize, dictHashSeed);
            ResumeThread(pi.hThread);
        } else {
            CopyForkOperationData(type, redisData, redisDataSize, dictHashSeed);
            CreateChildProcess(&pi, 0);
        }

        CloseHandle(pi.hThread);

        return pi.dwProcessId;
    }
    catch(system_error syserr) {
        redisLog(REDIS_WARNING, "BeginForkOperation: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
    }
    catch(runtime_error runerr) {
        redisLog(REDIS_WARNING, "BeginForkOperation: runtime error caught. message=%s\n", runerr.what());
    }
    catch(...) {
        redisLog(REDIS_WARNING, "BeginForkOperation: other exception caught.\n");
    }
    if (pi.hProcess != INVALID_HANDLE_VALUE) {
        TerminateProcess(pi.hProcess, 1);
    }
    return -1;
}

pid_t BeginForkOperation_Rdb(char *filename,
                             LPVOID redisData,
                             int redisDataSize,
                             uint32_t dictHashSeed)
{
    strcpy_s(g_pQForkControl->globalData.filename, filename);
    return BeginForkOperation(otRDB, redisData, redisDataSize, dictHashSeed);
}

pid_t BeginForkOperation_Aof(int aof_pipe_write_ack_to_parent,
                             int aof_pipe_read_ack_from_parent,
                             int aof_pipe_read_data_from_parent,
                             char *filename,
                             LPVOID redisData,
                             int redisDataSize,
                             uint32_t dictHashSeed)
{
    HANDLE aof_pipe_write_ack_handle = (HANDLE) FDAPI_get_osfhandle(aof_pipe_write_ack_to_parent);
    HANDLE aof_pipe_read_ack_handle  = (HANDLE) FDAPI_get_osfhandle(aof_pipe_read_ack_from_parent);
    HANDLE aof_pipe_read_data_handle = (HANDLE) FDAPI_get_osfhandle(aof_pipe_read_data_from_parent);

    // The handle is already inheritable so there is no need to duplicate it
    g_pQForkControl->globalData.aof_pipe_write_ack_handle = (aof_pipe_write_ack_handle);
    g_pQForkControl->globalData.aof_pipe_read_ack_handle = (aof_pipe_read_ack_handle);
    g_pQForkControl->globalData.aof_pipe_read_data_handle = (aof_pipe_read_data_handle);

    strcpy_s(g_pQForkControl->globalData.filename, filename);
    return BeginForkOperation(otAOF, redisData, redisDataSize, dictHashSeed);
}

void BeginForkOperation_Socket_Duplicate(DWORD dwProcessId) {
#ifdef USE_DLMALLOC
    WSAPROTOCOL_INFO* protocolInfo = (WSAPROTOCOL_INFO*)dlmalloc(sizeof(WSAPROTOCOL_INFO) * g_pQForkControl->globalData.numfds);
#elif USE_JEMALLOC
    WSAPROTOCOL_INFO* protocolInfo = (WSAPROTOCOL_INFO*) je_malloc(sizeof(WSAPROTOCOL_INFO) * g_pQForkControl->globalData.numfds);
#endif
    g_pQForkControl->globalData.protocolInfo = protocolInfo;
    for(int i = 0; i < g_pQForkControl->globalData.numfds; i++) {
        FDAPI_WSADuplicateSocket(g_pQForkControl->globalData.fds[i],
                                 dwProcessId,
                                 &protocolInfo[i]);
    }
}

pid_t BeginForkOperation_Socket(int *fds,
                                int numfds,
                                uint64_t *clientids,
                                int pipe_write_fd,
                                LPVOID redisData,
                                int redisDataSize,
                                uint32_t dictHashSeed)
{
    g_pQForkControl->globalData.fds = fds;
    g_pQForkControl->globalData.numfds = numfds;
    g_pQForkControl->globalData.clientids = clientids;

    HANDLE pipe_write_handle = (HANDLE) FDAPI_get_osfhandle(pipe_write_fd);

    // The handle is already inheritable so there is no need to duplicate it
    g_pQForkControl->globalData.pipe_write_handle = (pipe_write_handle);

    return BeginForkOperation(otSocket, redisData, redisDataSize, dictHashSeed);
}

OperationStatus GetForkOperationStatus() {
    if (WaitForSingleObject(g_pQForkControl->operationComplete, 0) == WAIT_OBJECT_0) {
        return OperationStatus::osCOMPLETE;
    }

    if (WaitForSingleObject(g_pQForkControl->operationFailed, 0) == WAIT_OBJECT_0) {
        return OperationStatus::osFAILED;
    }

    if (g_hForkedProcess) {
        // Verify if the child process is still running
        if (WaitForSingleObject(g_hForkedProcess, 0) == WAIT_OBJECT_0) {
            // The child process is not running, close the handle and report the status
            // setting the operationFailed event
            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
            if (g_pQForkControl->operationFailed != NULL) {
                SetEvent(g_pQForkControl->operationFailed);
            }
            return OperationStatus::osFAILED;
        } else {
            return OperationStatus::osINPROGRESS;
        }
    }
    
    return OperationStatus::osUNSTARTED;
}

BOOL AbortForkOperation() {
    try {
        if( g_hForkedProcess != 0 )
        {
            IFFAILTHROW(TerminateProcess(g_hForkedProcess, 1),
                        "EndForkOperation: Killing forked process failed.");
            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
        }

        return EndForkOperation(NULL);
    }
    catch(system_error syserr) {
        redisLog(REDIS_WARNING, "AbortForkOperation: 0x%08x - %s\n", syserr.code().value(), syserr.what());
        // If we can not properly restore fork state, then another fork operation is not possible. 
        exit(1);
    }
    catch(exception ex) {
        redisLog(REDIS_WARNING, "AbortForkOperation: %s\n", ex.what());
        exit(1);
    }
    return FALSE;
}

void RejoinCOWPages(HANDLE mmHandle, byte* mmStart, size_t mmSize) {
    SmartFileView<byte> copyView(mmHandle, FILE_MAP_WRITE, 0, 0, mmSize,
                                 string("RejoinCOWPages: Could not map COW back-copy view."));

    for (byte* mmAddress = mmStart; mmAddress < mmStart + mmSize; ) {
        MEMORY_BASIC_INFORMATION memInfo;

        IFFAILTHROW(VirtualQuery(mmAddress, &memInfo, sizeof(memInfo)),
                    "RejoinCOWPages: VirtualQuery failure");

        byte* regionEnd = (byte*)memInfo.BaseAddress + memInfo.RegionSize;

        if (memInfo.Protect != PAGE_WRITECOPY) {
            // Copy back only the pages that have been copied on write
            byte* srcEnd = min(regionEnd, mmStart + mmSize);
            memcpy(copyView + (mmAddress - mmStart), mmAddress, srcEnd - mmAddress);
        }
        mmAddress = regionEnd;
    }

    // If the COWs are not discarded, then there is no way of propagating
    // changes into subsequent fork operations.
#if FALSE   
    // This works when using a memory mapped file but it fails when using
    // the system paging file.
    if (WindowsVersion::getInstance().IsAtLeast_6_2()) {
        // Restores all page protections on the view and culls the COW pages.
        DWORD oldProtect;
        IFFAILTHROW(VirtualProtect(mmStart, mmSize, PAGE_READWRITE | PAGE_REVERT_TO_FILE_MAP, &oldProtect),
                    "RejoinCOWPages: COW cull failed");
    } else
#endif
    {
        // Prior to Win8 unmapping the view was the only way to discard the
        // COW pages from the view. Unfortunately this forces the view to be
        // completely flushed to disk, which is a bit inefficient.
        IFFAILTHROW(UnmapViewOfFile(mmStart), "RejoinCOWPages: UnmapViewOfFile failed.");

        // There is a possible race condition here. Something could map into
        // the virtual address space used by the heap at the moment we are
        // discarding local changes. There is nothing to do but report the
        // problem and exit. This problem does not exist with the code above
        // in Win8+ as the view is never unmapped.
        IFFAILTHROW(MapViewOfFileEx(mmHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0, mmStart),
                    "RejoinCOWPages: MapViewOfFileEx failed.");
    }
}

BOOL EndForkOperation(int * pExitCode) {
    try {
        if (g_hForkedProcess != 0) {
            if (WaitForSingleObject(g_hForkedProcess, cDeadForkWait) == WAIT_TIMEOUT) {
                IFFAILTHROW(TerminateProcess(g_hForkedProcess, 1),
                            "EndForkOperation: Killing forked process failed.");
            }

            if (pExitCode != NULL) {
                GetExitCodeProcess(g_hForkedProcess, (DWORD*) pExitCode);
            }

            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
        }

        IFFAILTHROW(ResetEvent(g_pQForkControl->operationComplete),
                    "EndForkOperation: ResetEvent() failed.");
        IFFAILTHROW(ResetEvent(g_pQForkControl->operationFailed),
                    "EndForkOperation: ResetEvent() failed.");

        // Move the heap local changes back into memory mapped views for next fork operation
        for (int i = 0; i < g_pQForkControl->numMappedBlocks; i++) {
            if (g_pQForkControl->heapBlockList[i].state == BlockState::bsMAPPED_IN_USE) {
                RejoinCOWPages(g_pQForkControl->heapBlockList[i].heapMap,
                    (byte*) g_pQForkControl->heapStart + (cAllocationGranularity * i),
                    cAllocationGranularity);
            }
        }

        RejoinCOWPages(g_hQForkControlFileMap, (byte*) g_pQForkControl, sizeof(QForkControl));

        return TRUE;
    }
    catch (system_error syserr) {
        redisLog(REDIS_WARNING, "EndForkOperation: 0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state, then another fork operation is not possible. 
        exit(1);
    }
    catch (exception ex) {
        redisLog(REDIS_WARNING, "EndForkOperation: %s\n", ex.what());
        exit(1);
    }
    return FALSE;
}

HANDLE CreateBlockMap(int blockIndex) {
    try {
        // cAllocationGranularity is guaranteed to be < 2^31
        ASSERT(cAllocationGranularity < (1 << 31));
        HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                        NULL,
                                        PAGE_READWRITE,
                                        0,
                                        cAllocationGranularity,
                                        NULL);
        IFFAILTHROW(map, "PhysicalMapMemory: CreateFileMapping failed");

        LPVOID addr = (byte*) g_pQForkControl->heapStart + blockIndex * cAllocationGranularity;

        // Free the memory that was reserved in QForkParentInit() before mapping it
        IFFAILTHROW(VirtualFree(addr, 0, MEM_RELEASE),
                    "PhysicalMapMemory: VirtualFree failed");

        LPVOID realAddr = MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, 0, addr);
        IFFAILTHROW(realAddr, "PhysicalMapMemory: MapViewOfFileEx failed");

        DWORD old;
        IFFAILTHROW(VirtualProtect(realAddr, cAllocationGranularity, PAGE_READWRITE, &old),
                    "PhysicalMapMemory: VirtualProtect failed");

        return map;
    }
    catch (system_error syserr) {
        redisLog(REDIS_WARNING, "PhysicalMapMemory: system error 0x%08x - %s", syserr.code().value(), syserr.what());
    }
    catch (runtime_error runerr) {
        redisLog(REDIS_WARNING, "PhysicalMapMemory: runtime error - %s", runerr.what());
    }
    catch (...) {
        redisLog(REDIS_WARNING, "PhysicalMapMemory: exception caught");
    }

    return NULL;
}

#ifdef USE_DLMALLOC
/* NOTE: The allocateHigh parameter is ignored in this implementation */
LPVOID AllocHeapBlock(size_t size, BOOL allocateHigh) {
    if (g_BypassMemoryMapOnAlloc) {
        return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    if (size % cAllocationGranularity != 0) {
        errno = EINVAL;
        return NULL;
    }

    int contiguousBlocksToAllocate = (int) (size / cAllocationGranularity);

    int startSearch = g_pQForkControl->blockSearchStart;
    int endSearch = g_pQForkControl->maxAvailableBlocks - contiguousBlocksToAllocate;
    int contiguousBlocksFound = 0;
    int allocationStartIndex = 0;

    for (int startIdx = startSearch; startIdx < endSearch; startIdx++) {
        for (int i = 0; i < contiguousBlocksToAllocate; i++) {
            if (g_pQForkControl->heapBlockList[startIdx + i].state == BlockState::bsUNMAPPED ||
                g_pQForkControl->heapBlockList[startIdx + i].state == BlockState::bsMAPPED_FREE) {
                contiguousBlocksFound++;
            } else {
                contiguousBlocksFound = 0;
                startIdx += i; // restart searching from there
                break;
            }
        }
        if (contiguousBlocksFound == contiguousBlocksToAllocate) {
            allocationStartIndex = startIdx;
            break;
        }
    }

    if (contiguousBlocksFound != contiguousBlocksToAllocate) {
        errno = ENOMEM;
        return NULL;
    }

    ASSERT(allocationStartIndex + contiguousBlocksToAllocate < g_pQForkControl->maxAvailableBlocks);

    for (int i = 0; i < contiguousBlocksToAllocate; i++) {
        int index = allocationStartIndex + i;
        if (g_pQForkControl->heapBlockList[index].state == BlockState::bsUNMAPPED) {
            g_pQForkControl->heapBlockList[index].heapMap = CreateBlockMap(index);
            g_pQForkControl->numMappedBlocks += 1;
        } else {
            // The current block state is bsMAPPED_FREE, therefore it needs to be
            // zeroed (bsUNMAPPED blocks don't need to be zeroed since newly mapped
            // blocked have zeroed memory by default)
            LPVOID ptr = reinterpret_cast<byte*>(g_pQForkControl->heapStart) + (cAllocationGranularity * index);
            ZeroMemory(ptr, cAllocationGranularity);
        }
        g_pQForkControl->heapBlockList[index].state = BlockState::bsMAPPED_IN_USE;
    }

    LPVOID retPtr = reinterpret_cast<byte*>(g_pQForkControl->heapStart) + (cAllocationGranularity * allocationStartIndex);
    if (allocationStartIndex == g_pQForkControl->blockSearchStart) {
        g_pQForkControl->blockSearchStart = allocationStartIndex + contiguousBlocksToAllocate;
    }

    return retPtr;
}

#elif USE_JEMALLOC

LPVOID AllocHeapBlock(LPVOID addr, size_t size, BOOL zero) {
    if (g_BypassMemoryMapOnAlloc) {
        return VirtualAlloc(addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    if (size % cAllocationGranularity != 0) {
        errno = EINVAL;
        return NULL;
    }

    int contiguousBlocksToAllocate = (int) (size / cAllocationGranularity);

    int startSearch = g_pQForkControl->blockSearchStart;
    int endSearch = g_pQForkControl->maxAvailableBlocks - contiguousBlocksToAllocate;
    int contiguousBlocksFound = 0;
    int allocationStartIndex = 0;

    for (int startIdx = startSearch; startIdx < endSearch; startIdx++) {
        for (int i = 0; i < contiguousBlocksToAllocate; i++) {
            if (g_pQForkControl->heapBlockList[startIdx + i].state == BlockState::bsUNMAPPED ||
                g_pQForkControl->heapBlockList[startIdx + i].state == BlockState::bsMAPPED_FREE) {
                contiguousBlocksFound++;
            } else {
                contiguousBlocksFound = 0;
                startIdx += i; // restart searching from there
                break;
            }
        }
        if (contiguousBlocksFound == contiguousBlocksToAllocate) {
            allocationStartIndex = startIdx;
            break;
        }
    }

    if (contiguousBlocksFound != contiguousBlocksToAllocate) {
        errno = ENOMEM;
        return NULL;
    }

    ASSERT(allocationStartIndex + contiguousBlocksToAllocate < g_pQForkControl->maxAvailableBlocks);

    for (int i = 0; i < contiguousBlocksToAllocate; i++) {
        int index = allocationStartIndex + i;
        if (g_pQForkControl->heapBlockList[index].state == BlockState::bsUNMAPPED) {
            g_pQForkControl->heapBlockList[index].heapMap = CreateBlockMap(index);
            g_pQForkControl->numMappedBlocks += 1;
        } else {
            // The current block state is bsMAPPED_FREE, therefore it needs to be
            // zeroed (bsUNMAPPED blocks don't need to be zeroed since newly mapped
            // blocked have zeroed memory by default)
            if (zero) {
                LPVOID ptr = reinterpret_cast<byte*>(g_pQForkControl->heapStart) + (cAllocationGranularity * index);
                ZeroMemory(ptr, cAllocationGranularity);
            }
        }
        g_pQForkControl->heapBlockList[index].state = BlockState::bsMAPPED_IN_USE;
    }

    LPVOID retPtr = reinterpret_cast<byte*>(g_pQForkControl->heapStart) + (cAllocationGranularity * allocationStartIndex);
    if (allocationStartIndex == g_pQForkControl->blockSearchStart) {
        g_pQForkControl->blockSearchStart = allocationStartIndex + contiguousBlocksToAllocate;
    }

    return retPtr;
}
#endif

BOOL FreeHeapBlock(LPVOID addr, size_t size) {
    if (size == 0) {
        return FALSE;
    }

    // If g_HasMemoryMappedHeap is FALSE this can only be a system heap address
    if (!g_HasMemoryMappedHeap) {
        return VirtualFree(addr, 0, MEM_RELEASE);
    }

    // Check if the address belongs to the memory map heap or to the system heap
    BOOL addressInRedisHeap = ((addr >= g_pQForkControl->heapStart) && (addr < g_pQForkControl->heapEnd));

    // g_BypassMemoryMapOnAlloc is true for the forked process, in this case
    // we need to handle the address differently based on the heap that was 
    // used to allocate it.
    if (g_BypassMemoryMapOnAlloc) {
        if (!addressInRedisHeap) {
            return VirtualFree(addr, 0, MEM_RELEASE);
        } else {
            redisLog(REDIS_DEBUG, "FreeHeapBlock: address in memory map heap 0x%p", addr);
        }
    }

    // Check the address alignment and that belongs to the memory map heap
    size_t ptrDiff = reinterpret_cast<byte*>(addr) - reinterpret_cast<byte*>(g_pQForkControl->heapStart);
    if ((ptrDiff % cAllocationGranularity) != 0 || !addressInRedisHeap) {
        return FALSE;
    }

    int blockStartIndex = (int) (ptrDiff / cAllocationGranularity);
    if (blockStartIndex >= g_pQForkControl->numMappedBlocks) {
        return FALSE;
    }

    int contiguousBlocksToFree = (int) (size / cAllocationGranularity);

    for (int i = 0; i < contiguousBlocksToFree; i++) {
        g_pQForkControl->heapBlockList[blockStartIndex + i].state = BlockState::bsMAPPED_FREE;
    }

    // TODO: use a linked list of free blocks

    if (g_pQForkControl->blockSearchStart > blockStartIndex) {
        g_pQForkControl->blockSearchStart = blockStartIndex;
    }

    return TRUE;
}

BOOL PurgePages(LPVOID addr, size_t length) {
    // VirtualAlloc is called for all cases regardless the value of
    // g_BypassMemoryMapOnAlloc and g_HasMemoryMappedHeap
    VirtualAlloc(addr, length, MEM_RESET, PAGE_READWRITE);
    return TRUE;
}

void SetupLogging() {
    bool serviceRun = g_argMap.find(cServiceRun) != g_argMap.end();
    string syslogEnabledValue = (g_argMap.find(cSyslogEnabled) != g_argMap.end() ? g_argMap[cSyslogEnabled].at(0).at(0) : cNo);
    bool syslogEnabled = (syslogEnabledValue.compare(cYes) == 0) || serviceRun;
    string syslogIdent = (g_argMap.find(cSyslogIdent) != g_argMap.end() ? g_argMap[cSyslogIdent].at(0).at(0) : cDefaultSyslogIdent);
    string logFileName = (g_argMap.find(cLogfile) != g_argMap.end() ? g_argMap[cLogfile].at(0).at(0) : cDefaultLogfile);

    RedisEventLog().EnableEventLog(syslogEnabled);
    if (syslogEnabled) {
        RedisEventLog().SetEventLogIdentity(syslogIdent.c_str());
    } else {
        setLogFile(logFileName.c_str());
    }
}

BOOL IsPersistenceDisabled() {
    if (g_argMap.find(cPersistenceAvailable) != g_argMap.end()) {
        return (g_argMap[cPersistenceAvailable].at(0).at(0) == cNo);
    } else {
        return FALSE;
    }
}

BOOL IsForkedProcess() {
    if (g_argMap.find(cQFork) != g_argMap.end()) {
        return TRUE;
    } else {
        return FALSE;
    }
}

void SetupQForkGlobals(int argc, char* argv[]) {
    // To check sentinel mode we use the antirez code to avoid duplicating code
    g_SentinelMode = checkForSentinelMode(argc, argv);

    g_IsForkedProcess = IsForkedProcess();
    g_PersistenceDisabled = IsPersistenceDisabled();

    g_BypassMemoryMapOnAlloc = g_IsForkedProcess || g_PersistenceDisabled || g_SentinelMode;
    g_HasMemoryMappedHeap = !g_PersistenceDisabled && !g_SentinelMode;
}

extern "C"
{
    // The external main() is redefined as redis_main() by Win32_QFork.h.
    // The CRT will call this replacement main() before the previous main()
    // is invoked so that the QFork allocator can be setup prior to anything 
    // Redis will allocate.
    int main(int argc, char* argv[]) {
        try {
            InitTimeFunctions();
            ParseCommandLineArguments(argc, argv);
            SetupQForkGlobals(argc, argv);
            SetupLogging();
            StackTraceInit();
            InitThreadControl();
        }
        catch (system_error syserr) {
            string errMsg = string("System error during startup: ") + syserr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }
        catch (runtime_error runerr) {
            string errMsg = string("System error during startup: ") + runerr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }
        catch (invalid_argument &iaerr) {
            string errMsg = string("Invalid argument during startup: ") + iaerr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }
        catch (exception othererr) {
            string errMsg = string("An exception occurred during startup: ") + othererr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        }

        try {

#ifdef DEBUG_WITH_PROCMON
            hProcMonDevice =
                CreateFile(
                L"\\\\.\\Global\\ProcmonDebugLogger",
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
#endif

            // Service commands do not launch an instance of redis directly
            if (HandleServiceCommands(argc, argv) == TRUE) {
                return 0;
            }

#ifdef USE_DLMALLOC
            DLMallocInit();
            // Setup memory allocation scheme
            if (g_UseSystemHeap == FALSE) {
                g_malloc = dlmalloc;
                g_calloc = dlcalloc;
                g_realloc = dlrealloc;
                g_free = dlfree;
                g_msize = reinterpret_cast<size_t(*)(void*)>(dlmalloc_usable_size);
            } else {
                g_malloc = malloc;
                g_calloc = calloc;
                g_realloc = realloc;
                g_free = free;
                g_msize = _msize;
            }
#elif USE_JEMALLOC
            je_init();
#endif
            if (g_PersistenceDisabled || g_SentinelMode) {
                return redis_main(argc, argv);
            } else {
                StartupStatus status = QForkStartup();
                if (status == ssCONTINUE_AS_PARENT) {
                    int retval = redis_main(argc, argv);
                    QForkShutdown();
                    return retval;
                } else if (status == ssCHILD_EXIT) {
                    // Child is done - clean up and exit
                    QForkShutdown();
                    return g_ChildExitCode;
                } else if (status == ssFAILED) {
                    // Parent or child failed initialization
                    return 1;
                } else {
                    // Unexpected status return
                    return 2;
                }
            }
        }
        catch (system_error syserr) {
            RedisEventLog().LogError(string("Main: system error. ") + syserr.what());
            redisLog(REDIS_WARNING, "main: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
        }
        catch (runtime_error runerr) {
            RedisEventLog().LogError(string("Main: runtime error. ") + runerr.what());
            redisLog(REDIS_WARNING, "main: runtime error caught. message=%s\n", runerr.what());
        }
        catch (exception ex) {
            RedisEventLog().LogError(string("Main: an exception occurred. ") + ex.what());
            redisLog(REDIS_WARNING, "main: other exception caught.\n");
        }
    }
}
