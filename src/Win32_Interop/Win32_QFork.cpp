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

#include "win32_types.h"
#include "Win32_FDAPI.h"
#include "Win32_Common.h"

#include <Windows.h>
#include <WinNT.h>
#include <errno.h>
#include <stdio.h>
#include <wchar.h>
#include <Psapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <assert.h>

#define QFORK_MAIN_IMPL
#include "Win32_QFork.h"

#include "Win32_QFork_impl.h"
#include "Win32_dlmalloc.h"
#include "Win32_SmartHandle.h"
#include "Win32_Service.h"
#include "Win32_CommandLine.h"
#include "Win32_RedisLog.h"
#include "Win32_StackTrace.h"
#include "Win32_ThreadControl.h"
#include "Win32_EventLog.h"

#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <stdint.h>
#include <exception>
#include <algorithm>
#include <memory>

using namespace std;

#ifndef PAGE_REVERT_TO_FILE_MAP
#define PAGE_REVERT_TO_FILE_MAP 0x80000000  // From Win8.1 SDK
#endif

extern "C" int checkForSentinelMode(int argc, char **argv);
extern "C" void InitTimeFunctions();
extern "C" PORT_LONGLONG memtoll(const char *p, int *err); // forward def from util.h

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

void ThrowLastError(const char* msg) {
    throw system_error(GetLastError(), system_category(), msg);
}
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
        - maps global data from the QForkControlMemoryMap into this process
        - executes the requested operation
        - unmaps all the mm views (discarding any writes)
        - signals the parent when the operation is complete

How the parent invokes the QFork process:
    - protects mapped memory segments with VirtualProtect using PAGE_WRITECOPY (both the allocated portions of DLMAllocMemoryMap and 
      the QForkControlMemoryMap)
    - QForked process is signaled to process command
    - Parent waits (asynchronously) until QForked process signals that operation is complete, then as an atomic operation:
        - signals and waits for the child process to terminate
        - restores protection status on mapped blocks
        - determines which pages have been modified and copies these to a buffer
        - unmaps the view of the heap (discarding COW changes form the view)
        - remaps the view
        - copies the changes back into the view
*/

#ifndef LODWORD
    #define LODWORD(_qw)    ((DWORD)(_qw))
#endif
#ifndef HIDWORD
    #define HIDWORD(_qw)    ((DWORD)((_qw) >> (sizeof(DWORD)*8)))
#endif

const SIZE_T cAllocationGranularity = 1 << 18;      // 256KB per heap block (matches large block allocation threshold of dlmalloc)
const int cMaxBlocks = 1 << 24;                     // 256KB * 16M heap blocks = 4TB. 4TB is the largest memory config Windows supports at present.
const char* cMapFileBaseName = "RedisQFork";
const int cDeadForkWait = 30000;

#ifndef _WIN64
size_t cDefaultmaxHeap32Bit = pow(2, 29);           // 512MB
#endif

enum class BlockState : uint8_t {bsINVALID = 0, bsUNMAPPED = 1, bsMAPPED = 2};

#define REDIS_GLOBALS_MAX_SIZE 10000

struct QForkStartupData {
    BYTE redisGlobals[REDIS_GLOBALS_MAX_SIZE];
    SIZE_T redisGlobalsSize;
    uint32_t dictHashSeed;

    // Filename used by the AOF or RDB save
    char filename[MAX_PATH];

    // The following variables are used by the Socket save (diskless replication)
    int *fds;
    int numfds;
    uint64_t *clientids;
    HANDLE pipe_write_handle;
    LPVOID protocolInfo;
};

struct QForkControl {
    HANDLE heapMemoryMapFile;
    HANDLE heapMemoryMap;
    // Number of available blocks in the heapBlockMap, its value
    // is determined at run-time based on the max heap size.
    int availableBlocksInHeap;
    BlockState heapBlockMap[cMaxBlocks];
    LPVOID heapStart;

    OperationType typeOfOperation;
    HANDLE operationComplete;
    HANDLE operationFailed;

    // Shared data pointers to be passed to the forked process
    QForkStartupData forkData;
    BYTE DLMallocGlobalState[1000];
    size_t DLMallocGlobalStateSize;
};

QForkControl* g_pQForkControl;
HANDLE g_hQForkControlFileMap;
HANDLE g_hForkedProcess = 0;
DWORD g_SystemAllocationGranularity;
BOOL g_IsChildProcess;
BOOL g_SentinelMode;
BOOL g_PersistenceDisabled;
BOOL g_BypassMemoryMapOnAlloc;

bool ReportSpecialSystemErrors(int error) {
    switch (error) {
        case ERROR_COMMITMENT_LIMIT: {
            redisLog(
                REDIS_WARNING,
                "\n"
                "The Windows version of Redis allocates a memory mapped heap for sharing with\n"
                "the forked process used for persistence operations. In order to share this\n"
                "memory, Windows allocates from the system paging file a portion equal to the\n"
                "size of the Redis heap. At this time there is insufficient contiguous free\n"
                "space available in the system paging file for this operation (Windows error \n"
                "0x5AF). To work around this you may either increase the size of the system\n"
                "paging file, or decrease the size of the Redis heap with the --maxheap flag.\n"
                "Sometimes a reboot will defragment the system paging file sufficiently for \n"
                "this operation to complete successfully.\n"
                "\n"
                "Please see the documentation included with the binary distributions for more \n"
                "details on the --maxheap flag.\n"
                "\n"
                "Redis can not continue. Exiting."
                );
            RedisEventLog().LogError("Failed to allocate the memory mapped file.");
            return true;
        }

        case ERROR_DISK_FULL: {
            redisLog(
                REDIS_WARNING,
                "\n"
                "The Windows version of Redis allocates a large memory mapped file for sharing\n" 
                "the heap with the forked process used in persistence operations. This file\n" 
                "will be created in the current working directory or the directory specified by\n"
                "the 'heapdir' directive in the .conf file. Windows is reporting that there is \n"
                "insufficient disk space available for this file (Windows error 0x70).\n"
                "\n" 
                "You may fix this problem by either reducing the size of the Redis heap with\n"
                "the --maxheap flag, or by moving the heap file to a local drive with sufficient\n"
                "space."
                "\n"
                "Please see the documentation included with the binary distributions for more \n"
                "details on the --maxheap and --heapdir flags.\n"
                "\n"
                "Redis can not continue. Exiting."
                );
            RedisEventLog().LogError("Disk full error while allocating the memory mapped file.");
            return true;
        }
    
        default:
            return false;
    }
}

/* QForkChildRun is the core of the QFork implementation for the child process,
 * it initializes all the required data structures and runs the background
 * command (i.e. RDB save, AOF save or SOCKET save).
 * Returns 0 if succeeds, -1 otherwise */
int QForkChildRun() {
    int returnValue = -1;
    try {
        // Child command line looks like: --QFork [QForkControlMemoryMap handle] [parent process id]
        HANDLE QForkControlMemoryMapHandle = (HANDLE) strtoul(g_argMap[cQFork].at(0).at(0).c_str(), NULL, 10);
        DWORD PPID = strtoul(g_argMap[cQFork].at(0).at(1).c_str(), NULL, 10);

        SmartHandle shParent( 
            OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, TRUE, PPID),
            string("Could not open parent process"));

        SmartHandle shMMFile(shParent, QForkControlMemoryMapHandle);
        SmartFileView<QForkControl> sfvParentQForkControl(
            shMMFile, 
            FILE_MAP_COPY, 
            string("Could not map view of QForkControl in child. Is system swap file large enough?"));
        g_pQForkControl = sfvParentQForkControl;

        // Duplicate handles and stuff into control structure (parent protected by PAGE_WRITECOPY)
        SmartHandle dupHeapFileHandle(shParent, sfvParentQForkControl->heapMemoryMapFile);
        g_pQForkControl->heapMemoryMapFile = dupHeapFileHandle;
        SmartHandle dupOperationComplete(shParent, sfvParentQForkControl->operationComplete);
        g_pQForkControl->operationComplete = dupOperationComplete;
        SmartHandle dupOperationFailed(shParent, sfvParentQForkControl->operationFailed);
        g_pQForkControl->operationFailed = dupOperationFailed;

       // Create section handle on MM file
       SIZE_T mmSize = g_pQForkControl->availableBlocksInHeap * cAllocationGranularity;
       SmartFileMapHandle sfmhMapFile(
           g_pQForkControl->heapMemoryMapFile, 
           PAGE_WRITECOPY, 
#ifdef _WIN64           
           HIDWORD(mmSize),
#else      
           0,
#endif
           LODWORD(mmSize),
           string("Could not open file mapping object in child"));
       g_pQForkControl->heapMemoryMap = sfmhMapFile;


       // The key to mapping a heap larger than physical memory is to not map it
       // all at once, deal with the unmapped pages with VectoredHeapMapper.
       SmartFileView<byte> sfvHeap(
            g_pQForkControl->heapMemoryMap,
            FILE_MAP_COPY,
            0,
            0,
            cAllocationGranularity, // Only map a portion of the heap.
            g_pQForkControl->heapStart,
            string("Could not map heap in forked process. Is system swap file large enough?"));

        // Setup DLMalloc global data
        if (SetDLMallocGlobalState(g_pQForkControl->DLMallocGlobalStateSize,
                                   g_pQForkControl->DLMallocGlobalState) != 0) {
            throw runtime_error("DLMalloc global state copy failed.");
        }

        // Copy redis globals into fork process
        SetupRedisGlobals(g_pQForkControl->forkData.redisGlobals,
                          g_pQForkControl->forkData.redisGlobalsSize,
                          g_pQForkControl->forkData.dictHashSeed);

        // Execute requested operation
        if (g_pQForkControl->typeOfOperation == OperationType::otRDB) {
            returnValue = do_rdbSave(g_pQForkControl->forkData.filename);
        } else if (g_pQForkControl->typeOfOperation == OperationType::otAOF) {
            returnValue = do_aofSave(g_pQForkControl->forkData.filename);
        } else if (g_pQForkControl->typeOfOperation == OperationType::otSocket) {
            LPWSAPROTOCOL_INFO lpProtocolInfo = (LPWSAPROTOCOL_INFO) g_pQForkControl->forkData.protocolInfo;
            int pipe_write_fd = fdapi_open_osfhandle((intptr_t)g_pQForkControl->forkData.pipe_write_handle, _O_APPEND);
            int* fds = (int*) malloc(sizeof(int) * g_pQForkControl->forkData.numfds);
            for (int i = 0; i < g_pQForkControl->forkData.numfds; i++) {
                fds[i] = FDAPI_WSASocket(FROM_PROTOCOL_INFO,
                                         FROM_PROTOCOL_INFO,
                                         FROM_PROTOCOL_INFO,
                                         &lpProtocolInfo[i],
                                         0,
                                         WSA_FLAG_OVERLAPPED);
            }

            returnValue = do_socketSave(fds,
                                        g_pQForkControl->forkData.numfds,
                                        g_pQForkControl->forkData.clientids,
                                        pipe_write_fd);
            // After the socket replication has finished, close the duplicated sockets.
            // Failing to close the sockets properly will produce a socket read error
            // on both the parent process and the slave.
            for (int i = 0; i < g_pQForkControl->forkData.numfds; i++) {
                FDAPI_CloseDuplicatedSocket(fds[i]);
            }
            free(fds);
        } else {
            throw runtime_error("unexpected operation type");
        }

        // Let parent know we are done
        SetEvent(g_pQForkControl->operationComplete);

        g_pQForkControl = NULL;
        return returnValue;
    }
    catch(system_error syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            RedisEventLog().LogError("QForkChildRun: system error. " + string(syserr.what()));
            redisLog(REDIS_WARNING, "QForkChildRun: system error. ErrCode: 0x%08x, ErrMsg: %s\n", syserr.code().value(), syserr.what());
        }
    }
    catch(runtime_error runerr) {
        RedisEventLog().LogError("QForkChildRun: runtime error. " + string(runerr.what()));
        redisLog(REDIS_WARNING, "QForkChildRun: runtime error. ErrMsg: %s\n", runerr.what());
    }
    
    if (g_pQForkControl != NULL) {
        if (g_pQForkControl->operationFailed != NULL) {
            SetEvent(g_pQForkControl->operationFailed);
        }
        g_pQForkControl = NULL;
    }
    return -1;
}

/* The default location for the memory mapped file is the local app data. */
string GetLocalAppDataFolder() {
    char localAppDataPath[MAX_PATH];
    HRESULT hr;
    if (S_OK != (hr = SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, localAppDataPath))) {
        throw system_error(hr, system_category(), "SHGetFolderPathA failed");
    }
    char redisAppDataPath[MAX_PATH];
    if (NULL == PathCombineA(redisAppDataPath, localAppDataPath, "Redis")) {
        throw runtime_error("PathCombineA failed");
    }

    if (PathIsDirectoryA(redisAppDataPath) == FALSE) {
        if (CreateDirectoryA(redisAppDataPath, NULL) == FALSE) {
            ThrowLastError("CreateDirectoryA failed");
        }
    }

    return redisAppDataPath;
}

/* Get the location where the memory mapped file will be saved. */
string GetHeapDirPath() {
    string heapDirPath;
    if (g_argMap.find(cHeapDir) != g_argMap.end()) {
        heapDirPath = g_argMap[cHeapDir][0][0];
        replace(heapDirPath.begin(), heapDirPath.end(), '/', '\\');

        if (PathIsRelativeA(heapDirPath.c_str())) {
            char cwd[MAX_PATH];
            if (GetCurrentDirectoryA(MAX_PATH, cwd) == 0) {
                ThrowLastError("GetCurrentDirectoryA failed");
            }

            char fullPath[MAX_PATH];
            if (PathCombineA(fullPath, cwd, heapDirPath.c_str()) == NULL) {
                ThrowLastError("PathCombineA failed");
            }
            heapDirPath = fullPath;
        }
    } else {
        heapDirPath = GetLocalAppDataFolder();
    }

    if (heapDirPath.length() > 0 && heapDirPath.at(heapDirPath.length() - 1) != '\\') {
        heapDirPath = heapDirPath.append("\\");
    }

    return heapDirPath;
}

/* In the case of a BSOD or power failure the memory map file is not
 * deleted automatically, make sure all leftover files are removed.
 */
void CleanupHeapDir(string heapDir) {
    try {
        char heapMemoryMapWildCard[MAX_PATH];
        WIN32_FIND_DATAA fd;

        sprintf_s(
            heapMemoryMapWildCard,
            MAX_PATH,
            "%s%s_*.dat",
            heapDir.c_str(),
            cMapFileBaseName);

        HANDLE hFind = FindFirstFileA(heapMemoryMapWildCard, &fd);

        while (hFind != INVALID_HANDLE_VALUE) {
            string filePath;
            filePath.append(heapDir.c_str()).append(fd.cFileName);
            // Failure likely means the file is in use by another redis instance
            DeleteFileA(filePath.c_str());

            if (FALSE == FindNextFileA(hFind, &fd)) {
                FindClose(hFind);
                hFind = INVALID_HANDLE_VALUE;
            }
        }
    }
    catch (...) {}
}

BOOL QForkParentInit() {
    try {
        // Remove old .dat memory mapped files if present
        string heapDirPath = GetHeapDirPath();
        CleanupHeapDir(heapDirPath);

        // This must be called only once per process. The child process will
        // get this information from the parent through DLMallocGlobalState
        if (dlmallopt(M_GRANULARITY, cAllocationGranularity) == 0) {
            ThrowLastError("DLMalloc failed initializing allocation granularity.");
        }

        /*
        Not specifying the maxmemory or maxheap flags will result in the default behavior of: new key generation not
        bounded by heap usage, and the heap size equal to the size of physical memory.

        Redis will respect the maxmemory flag by preventing new key creation when the number of bytes allocated in the heap
        exceeds the level specified by the maxmemory flag. This does not account for heap fragmentation or memory usage by
        the heap allocator. To allow for this extra space maxheapBytes is implicitly set to (1.5 * maxmemory [rounded up
        to the nearest cAllocationGranularity boundary]). The maxheap flag may be specified along with the maxmemory flag to
        increase the heap further than this.

        If the maxmemory flag is not specified, but the maxheap flag is specified, the heap is sized according to this flag
        (rounded up to the nearest cAllocationGranularity boundary). The heap may be configured larger than physical memory with
        this flag. If maxmemory is sufficiently large enough, the heap will also be made larger than physical memory. This
        has implications for the system swap file size requirement and disk usage as discussed below. Specifying a heap larger
        than physical memory allows Redis to continue operating into virtual memory up to the limit of the heap size specified.

        Since the heap is entirely contained in the memory mapped file we are creating to share with the forked process, the
        size of the memory mapped file will be equal to the size of the heap. There must be sufficient disk space for this file.
        For instance, launching Redis on a server machine with 512GB of RAM and no flags specified for either maxmemory or
        maxheap will result in the allocation of a 512GB memory mapped file. Redis will fail to launch if there is not enough
        space available on the disk where redis is being launched from for this file.

        During forking the system swap file will be used for managing virtual memory sharing and the copy on write pages for both
        forker and forkee. There must be sufficient swap space availability for this. The maximum size of this swap space commit
        is roughly equal to (physical memory + (2 * size of the memory allocated in the redis heap)). For instance, if the heap is nearly
        maxed out on an 8GB machine and the heap has been configured to be twice the size of physical memory, the swap file comittment
        will be (physical + (2 * (2 * physical)) or (5 * physical). By default Windows will dynamically allocate a swap file that will
        expand up to about (3.5 * physical). In this case the forked process will fail with ERROR_COMMITMENT_LIMIT (1455/0x5AF) error.
        The fix for this is to ensure the system swap space is sufficiently large enough to handle this. The reason that the default
        heap size is equal to physical memory is so that Redis will work on a freshly configured OS without requireing reconfiguring
        either Redis or the machine (max comittment of (3 * physical)).
        */

        __int64 maxheapBytes = -1;
        __int64 maxmemoryBytes = -1;

        if (g_argMap.find(cMaxHeap) != g_argMap.end()) {
            maxheapBytes = memtoll(g_argMap[cMaxHeap].at(0).at(0).c_str(), NULL);
        }
        if (g_argMap.find(cMaxMemory) != g_argMap.end()) {
            maxmemoryBytes = memtoll(g_argMap[cMaxMemory].at(0).at(0).c_str(), NULL);
        }
        int64_t maxMemoryPlusHalf = (3 * maxmemoryBytes) / 2;
        if (maxmemoryBytes != -1) {
            if (maxheapBytes < maxMemoryPlusHalf) {
                maxheapBytes = maxMemoryPlusHalf;
            }
        }

        if (maxheapBytes == -1) {
#ifdef _WIN64
            maxheapBytes = Globals::memoryPhysicalTotal * Globals::pageSize;
#else
            maxheapBytes = cDefaultmaxHeap32Bit;
#endif
        }

        // Allocate file map for QForkControl so it can be passed to the
        // child process.
        g_hQForkControlFileMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            sizeof(QForkControl),
            NULL);
        if (g_hQForkControlFileMap == NULL) {
            ThrowLastError("CreateFileMapping failed");
        }

        g_pQForkControl = (QForkControl*)MapViewOfFile(
            g_hQForkControlFileMap, 
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            0);
        if (g_pQForkControl == NULL) {
            ThrowLastError("MapViewOfFile failed");
        }

        // Ensure the number of blocks is a multiple of cAllocationGranularity
        SIZE_T allocationBlocks = (SIZE_T) maxheapBytes / cAllocationGranularity;
        allocationBlocks += ((maxheapBytes % cAllocationGranularity) != 0);

        g_pQForkControl->availableBlocksInHeap = (int) allocationBlocks;
        if (g_pQForkControl->availableBlocksInHeap <= 0) {
            throw runtime_error("Invalid number of heap blocks.");
        }

        // Create the memory mapped file
        char heapMemoryMapPath[MAX_PATH];
        sprintf_s(
            heapMemoryMapPath,
            MAX_PATH,
            "%s%s_%d.dat",
            heapDirPath.c_str(),
            cMapFileBaseName, 
            GetCurrentProcessId());

        g_pQForkControl->heapMemoryMapFile = 
            CreateFileA( 
                heapMemoryMapPath,
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL| FILE_FLAG_DELETE_ON_CLOSE,
                NULL);
        if (g_pQForkControl->heapMemoryMapFile == INVALID_HANDLE_VALUE) {
            ThrowLastError("CreateFile failed");
        }

        SIZE_T mmSize = g_pQForkControl->availableBlocksInHeap * cAllocationGranularity;
        g_pQForkControl->heapMemoryMap = 
            CreateFileMappingW( 
                g_pQForkControl->heapMemoryMapFile,
                NULL,
                PAGE_READWRITE,
#ifdef _WIN64
                HIDWORD(mmSize),
#else
                0,
#endif
                LODWORD(mmSize),
                NULL);
        if (g_pQForkControl->heapMemoryMap == NULL) {
            ThrowLastError("CreateFileMapping failed.");
        }
            
        // Find a place in the virtual memory space where we can reserve space
        // for our allocations that is likely to be available in the forked process.
        // If this ever fails in the forked process, we will have to launch
        // the forked process and negotiate for a shared memory address here.
        LPVOID pHigh = VirtualAllocEx( 
            GetCurrentProcess(),
            NULL,
            mmSize,
            MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, 
            PAGE_READWRITE);
        if (pHigh == NULL) {
            ThrowLastError("VirtualAllocEx failed.");
        }

        if (VirtualFree(pHigh, 0, MEM_RELEASE) == FALSE) {
            ThrowLastError("VirtualFree failed.");
        }

        g_pQForkControl->heapStart = 
            MapViewOfFileEx(
                g_pQForkControl->heapMemoryMap,
                FILE_MAP_ALL_ACCESS,
                0,
                0,
                0,
                pHigh);
        if (g_pQForkControl->heapStart == NULL) {
            ThrowLastError("MapViewOfFileEx failed.");
        }

        for (int n = 0; n < cMaxBlocks; n++) {
            g_pQForkControl->heapBlockMap[n] = 
                ((n < g_pQForkControl->availableBlocksInHeap) ?
                BlockState::bsUNMAPPED : BlockState::bsINVALID);
        }

        g_pQForkControl->typeOfOperation = OperationType::otINVALID;
        g_pQForkControl->operationComplete = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (g_pQForkControl->operationComplete == NULL) {
            ThrowLastError("CreateEvent failed.");
        }

        g_pQForkControl->operationFailed = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (g_pQForkControl->operationFailed == NULL) {
            ThrowLastError("CreateEvent failed.");
        }

        return TRUE;
    }
    catch(system_error syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            RedisEventLog().LogError("QForkParentInit: system error. " + string(syserr.what()));
            redisLog(REDIS_WARNING, "QForkParentInit: system error. ErrCode: 0x%08x, ErrMsg: %s\n", syserr.code().value(), syserr.what());
        }
    }
    catch(runtime_error runerr) {
        RedisEventLog().LogError("QForkParentInit: runtime error. " + string(runerr.what()));
        redisLog(REDIS_WARNING, "QForkParentInit: runtime error. ErrMsg: %s\n", runerr.what());
    }
    catch(exception ex) {
        RedisEventLog().LogError("QForkParentInit: an exception occurred. " + string(ex.what()));
        redisLog(REDIS_WARNING, "QForkParentInit: an exception occurred. ErrMsg: %s\n", ex.what());
    }
    return FALSE;
}

/* VectoredHeapMapper is used by the child process. The child process doesn't
 * map the memory mapped file pages all at once since it may have a significant
 * performance impact, therefore an exception handler is used to map the pages
 * on demand. */
LONG CALLBACK VectoredHeapMapper(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters == 2) {
        intptr_t failingMemoryAddress = info->ExceptionRecord->ExceptionInformation[1];
        intptr_t heapStart = (intptr_t) g_pQForkControl->heapStart;
        intptr_t heapEnd = heapStart + ((SIZE_T) g_pQForkControl->availableBlocksInHeap * cAllocationGranularity);
        if (failingMemoryAddress >= heapStart && failingMemoryAddress < heapEnd) {
            intptr_t startOfMapping = failingMemoryAddress - failingMemoryAddress % g_SystemAllocationGranularity;
            intptr_t mmfOffset = startOfMapping - heapStart;
            size_t bytesToMap = min((size_t) g_SystemAllocationGranularity, (size_t) (heapEnd - startOfMapping));
            LPVOID pMapped = MapViewOfFileEx(
                g_pQForkControl->heapMemoryMap,
                FILE_MAP_COPY,
#ifdef _WIN64           
                HIDWORD(mmfOffset),
#else      
                0,
#endif
                LODWORD(mmfOffset),
                bytesToMap,
                (LPVOID) startOfMapping);

            if (pMapped != NULL) {
                return EXCEPTION_CONTINUE_EXECUTION;
            } else {
                DWORD err = GetLastError();
                redisLog(REDIS_WARNING, "\n\n=== REDIS BUG REPORT START: Cut & paste starting from here ===");
                redisLog(REDIS_WARNING, "--- FATAL ERROR MAPPING VIEW OF MAP FILE");
                redisLog(REDIS_WARNING, "\t MapViewOfFileEx failed with error 0x%08X.", err);
                redisLog(REDIS_WARNING, "\t startOfMapping 0x%p", startOfMapping);
                redisLog(REDIS_WARNING, "\t heapStart 0x%p", heapStart);
                redisLog(REDIS_WARNING, "\t heapEnd 0x%p", heapEnd);
                redisLog(REDIS_WARNING, "\t failing access location 0x%p", failingMemoryAddress);
                redisLog(REDIS_WARNING, "\t offset into mmf to start mapping 0x%p", mmfOffset);
                redisLog(REDIS_WARNING, "\t start of new mapping 0x%p", startOfMapping);
                redisLog(REDIS_WARNING, "\t bytes to map 0x%p\n", bytesToMap);
                if (err == 0x000005AF) {
                    redisLog(REDIS_WARNING, "The system paging file is too small for this operation to complete.");
                    redisLog(REDIS_WARNING, "See https://github.com/MSOpenTech/redis/wiki/Memory-Configuration");
                    redisLog(REDIS_WARNING, "for more information on configuring the system paging file for Redis.");

                    RedisEventLog().LogError("VectoredHeapMapper: an exception occurred. The system paging file is too small for this operation to complete.");
                }
                redisLog(REDIS_WARNING, "\n=== REDIS BUG REPORT END. Make sure to include from START to END. ===\n\n");

                // Call exit to avoid executing the Unhandled Exceptiont Handler since we don't need a call stack
                exit(1);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int QForkStartup() {
    LPVOID exceptionHandler = AddVectoredExceptionHandler(1, VectoredHeapMapper);
    int returnValue = -1;
    try {
        returnValue = QForkChildRun();
    }
    catch (...) {}
    RemoveVectoredExceptionHandler(exceptionHandler);
    return returnValue;
}

void SmartCloseHandle(HANDLE* ptrHandle) {
    if (*ptrHandle != NULL) {
        CloseHandle(*ptrHandle);
        *ptrHandle = NULL;
    }
}

BOOL QForkShutdown() {
    if(g_hForkedProcess != NULL) {
        TerminateProcess(g_hForkedProcess, -1);
        CloseHandle(g_hForkedProcess);
        g_hForkedProcess = NULL;
    }

    if (g_pQForkControl != NULL) {
        SmartCloseHandle(&g_pQForkControl->operationComplete);
        SmartCloseHandle(&g_pQForkControl->operationFailed);
        SmartCloseHandle(&g_pQForkControl->heapMemoryMap);

        if (g_pQForkControl->heapMemoryMapFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_pQForkControl->heapMemoryMapFile);
            g_pQForkControl->heapMemoryMapFile = INVALID_HANDLE_VALUE;
        }

        if (g_pQForkControl->heapStart != NULL) {
            UnmapViewOfFile(g_pQForkControl->heapStart);
            g_pQForkControl->heapStart = NULL;
        }

        if (g_pQForkControl != NULL) {
            UnmapViewOfFile(g_pQForkControl);
            g_pQForkControl = NULL;
        }

        SmartCloseHandle(&g_hQForkControlFileMap);
    }

    return TRUE;
}

void CopyForkOperationData(OperationType type, LPVOID redisGlobals, int redisGlobalsSize, uint32_t dictHashSeed) {
    // Copy operation data
    g_pQForkControl->typeOfOperation = type;
    if (redisGlobalsSize > REDIS_GLOBALS_MAX_SIZE) {
        throw runtime_error("Global state too large.");
    }
    memcpy(&(g_pQForkControl->forkData.redisGlobals), redisGlobals, redisGlobalsSize);
    g_pQForkControl->forkData.redisGlobalsSize = redisGlobalsSize;
    g_pQForkControl->forkData.dictHashSeed = dictHashSeed;

    GetDLMallocGlobalState(&g_pQForkControl->DLMallocGlobalStateSize, NULL);
    if (g_pQForkControl->DLMallocGlobalStateSize > sizeof(g_pQForkControl->DLMallocGlobalState)) {
        throw runtime_error("DLMalloc global state too large.");
    }
    if (GetDLMallocGlobalState(&g_pQForkControl->DLMallocGlobalStateSize,
                               g_pQForkControl->DLMallocGlobalState) != 0) {
        throw runtime_error("DLMalloc global state copy failed.");
    }

    // Protect both the heap and the fork control map from propagating local changes 
    DWORD oldProtect = 0;
    if (VirtualProtect(g_pQForkControl,
                       sizeof(QForkControl),
                       PAGE_WRITECOPY,
                       &oldProtect) == FALSE) {
        ThrowLastError("BeginForkOperation: VirtualProtect failed for the fork control map");
    }
    if (VirtualProtect(g_pQForkControl->heapStart,
                       g_pQForkControl->availableBlocksInHeap * cAllocationGranularity,
                       PAGE_WRITECOPY,
                       &oldProtect) == FALSE) {
        ThrowLastError("BeginForkOperation: VirtualProtect failed for the heap");
    }
}

void CreateChildProcess(PROCESS_INFORMATION *pi, char* logfile, DWORD dwCreationFlags = 0) {
    // Ensure events are in the correst state
    if (ResetEvent(g_pQForkControl->operationComplete) == FALSE) {
        ThrowLastError("BeginForkOperation: ResetEvent() failed.");
    }

    if (ResetEvent(g_pQForkControl->operationFailed) == FALSE) {
        ThrowLastError("BeginForkOperation: ResetEvent() failed.");
    }

    // Launch the child process
    char fileName[MAX_PATH];
    if (GetModuleFileNameA(NULL, fileName, MAX_PATH) == 0) {
        ThrowLastError("Failed to get module name.");
    }

    STARTUPINFOA si;
    memset(&si,0, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    char arguments[MAX_PATH];
    memset(arguments, 0, MAX_PATH);
    sprintf_s(
        arguments,
        MAX_PATH,
        "\"%s\" --%s %llu %lu --%s \"%s\"",
        fileName,
        cQFork.c_str(),
        (uint64_t)g_hQForkControlFileMap,
        GetCurrentProcessId(),
        cLogfile.c_str(),
        (logfile != NULL && logfile[0] != '\0') ? logfile : "stdout");
    
    if (CreateProcessA(fileName, arguments, NULL, NULL, TRUE,
                       dwCreationFlags, NULL, NULL, &si, pi) == FALSE) {
        ThrowLastError("Problem creating child process.");
    }
    g_hForkedProcess = pi->hProcess;
}

typedef void (*CHILD_PID_HOOK)(DWORD pid);

pid_t BeginForkOperation(
    OperationType type,
    LPVOID redisGlobals,
    int redisGlobalsSize,
    uint32_t dictHashSeed,
    char* logfile,
    CHILD_PID_HOOK pidHook = NULL)
{
    PROCESS_INFORMATION pi;
    try {
        pi.hProcess = INVALID_HANDLE_VALUE;
        pi.dwProcessId = -1;

        // The pidHook is passed only by the Socket-save operation (diskless replication)
        if (pidHook != NULL) {
            CreateChildProcess(&pi, logfile, CREATE_SUSPENDED);
            pidHook(pi.dwProcessId);
            CopyForkOperationData(type, redisGlobals, redisGlobalsSize, dictHashSeed);
            ResumeThread(pi.hThread);
        } else {
            CopyForkOperationData(type, redisGlobals, redisGlobalsSize, dictHashSeed);
            CreateChildProcess(&pi, logfile, 0);
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

pid_t BeginForkOperation_Rdb(
    char *filename,
    LPVOID redisGlobals,
    int redisGlobalsSize,
    uint32_t dictHashSeed,
    char* logfile)
{
    strcpy_s(g_pQForkControl->forkData.filename, filename);
    return BeginForkOperation(otRDB, redisGlobals, redisGlobalsSize, dictHashSeed, logfile);
}

pid_t BeginForkOperation_Aof(
    char *filename,
    LPVOID redisGlobals,
    int redisGlobalsSize,
    uint32_t dictHashSeed,
    char* logfile)
{
    strcpy_s(g_pQForkControl->forkData.filename, filename);
    return BeginForkOperation(otAOF, redisGlobals, redisGlobalsSize, dictHashSeed, logfile);
}

/* Used by diskless replication to duplicate the sockets created by
 * the master to communicate with the slave */
void BeginForkOperation_Socket_PidHook(DWORD dwProcessId) {
    WSAPROTOCOL_INFO* protocolInfo = (WSAPROTOCOL_INFO*)dlmalloc(sizeof(WSAPROTOCOL_INFO) * g_pQForkControl->forkData.numfds);
    g_pQForkControl->forkData.protocolInfo = protocolInfo;
    for(int i = 0; i < g_pQForkControl->forkData.numfds; i++) {
        FDAPI_WSADuplicateSocket(g_pQForkControl->forkData.fds[i], dwProcessId, &protocolInfo[i]);
    }
}

/* Diskless replication: in the Windows implementation is not strictly diskless
 * since the QFork architecture uses a memory mapped file to simulate the unix
 * fork */
pid_t BeginForkOperation_Socket(
    int *fds,
    int numfds,
    uint64_t *clientids,
    int pipe_write_fd,
    LPVOID redisGlobals,
    int redisGlobalsSize,
    uint32_t dictHashSeed,
    char* logfile)
{
    g_pQForkControl->forkData.fds = fds;
    g_pQForkControl->forkData.numfds = numfds;
    g_pQForkControl->forkData.clientids = clientids;

    HANDLE pipe_write_handle = (HANDLE)_get_osfhandle(pipe_write_fd);

    // The handle is already inheritable so there is no need to duplicate it
    g_pQForkControl->forkData.pipe_write_handle = (pipe_write_handle);

    return BeginForkOperation(otSocket,
                              redisGlobals,
                              redisGlobalsSize,
                              dictHashSeed,
                              logfile,
                              BeginForkOperation_Socket_PidHook);
}

/* Called by the parent process serverCron to monitor the child status */
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
            // The child process is not running, close the handle and report
            // the status setting the operationFailed event
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
        if (g_hForkedProcess != 0) {
            if (TerminateProcess(g_hForkedProcess, 1) == FALSE) {
                ThrowLastError("AbortForkOperation: Killing forked process failed.");
            }
            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
        }
        return EndForkOperation(NULL);
    }
    catch (system_error syserr) {
        redisLog(REDIS_WARNING, "AbortForkOperation: 0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state,
        // then another fork operation is not possible.
        exit(1);
    }
    catch (...) {
        redisLog(REDIS_WARNING, "Some other exception caught in EndForkOperation().\n");
        exit(1);
    }
    return FALSE;
}

void RejoinCOWPages(HANDLE mmHandle, byte* mmStart, size_t mmSize, bool useVirtualProtect) {
    SmartFileView<byte> copyView(
        mmHandle,
        FILE_MAP_WRITE,
        0,
        0,
        mmSize,
        string("RejoinCOWPages: Could not map COW back-copy view."));

    for (byte* mmAddress = mmStart; mmAddress < mmStart + mmSize; ) {
        MEMORY_BASIC_INFORMATION memInfo;
        if (VirtualQuery(mmAddress, &memInfo, sizeof(memInfo)) == 0) {
            ThrowLastError("RejoinCOWPages: VirtualQuery failure");
        }

        byte* regionEnd = (byte*)memInfo.BaseAddress + memInfo.RegionSize;

        if (memInfo.Protect != PAGE_WRITECOPY) {
            byte* srcEnd = min(regionEnd, mmStart + mmSize);
            memcpy(copyView + (mmAddress - mmStart), mmAddress, srcEnd - mmAddress);
        }
        mmAddress = regionEnd;
    }

    // If the COWs are not discarded, then there is no way of propagating
    // changes into subsequent fork operations.
    if (useVirtualProtect && WindowsVersion::getInstance().IsAtLeast_6_2()) {
        // Restores all page protections on the view and culls the COW pages.
        DWORD oldProtect;
        if (VirtualProtect(mmStart, mmSize, PAGE_READWRITE | PAGE_REVERT_TO_FILE_MAP,
                           &oldProtect) == FALSE)
            ThrowLastError("RejoinCOWPages: COW cull failed");
    } else {
        // Prior to Windows 8 unmapping the view was the only way to discard
        // the COW pages from the view. Unfortunately this forces the view to
        // be completely flushed to disk, which is a bit inefficient.
        if (UnmapViewOfFile(mmStart) == FALSE) {
            ThrowLastError("RejoinCOWPages: UnmapViewOfFile failed.");
        }
        // There may be a race condition here. Something could map into the
        // virtual address space used by the heap at the moment we are
        // discarding local changes. There is nothing to do but report the
        // problem and exit. This problem does not exist with the code above
        // in Windows 8+ as the view is never unmapped.
        if (MapViewOfFileEx(mmHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0, mmStart) == NULL) {
            ThrowLastError("RejoinCOWPages: MapViewOfFileEx failed.");
        }
    }
}

BOOL EndForkOperation(int * pExitCode) {
    try {
        if (g_hForkedProcess != 0) {
            if (WaitForSingleObject(g_hForkedProcess, cDeadForkWait) == WAIT_TIMEOUT) {
                if (TerminateProcess(g_hForkedProcess, 1) == FALSE) {
                    ThrowLastError("EndForkOperation: Killing forked process failed.");
                }
            }

            if (pExitCode != NULL) {
                GetExitCodeProcess(g_hForkedProcess, (DWORD*) pExitCode);
            }

            CloseHandle(g_hForkedProcess);
            g_hForkedProcess = 0;
        }

        if (ResetEvent(g_pQForkControl->operationComplete) == FALSE) {
            ThrowLastError("EndForkOperation: ResetEvent() failed.");
        }
        if (ResetEvent(g_pQForkControl->operationFailed) == FALSE) {
            ThrowLastError("EndForkOperation: ResetEvent() failed.");
        }

        // Move local changes back into memory mapped views for next fork operation
        // Use the VirtualProtect optimization for the memory mapped file
        RejoinCOWPages(
            g_pQForkControl->heapMemoryMap,
            (byte*) g_pQForkControl->heapStart,
            g_pQForkControl->availableBlocksInHeap * cAllocationGranularity,
            true);

        // g_hQForkControlFileMap uses the system paging file, therefore
        // we can't use the VirtualProtect optimization
        RejoinCOWPages(
            g_hQForkControlFileMap,
            (byte*) g_pQForkControl,
            sizeof(QForkControl),
            false);

        return TRUE;
    }
    catch (system_error syserr) {
        redisLog(REDIS_WARNING, "EndForkOperation: 0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state, then another fork operation is not possible. 
        exit(1);
    }
    catch (...) {
        redisLog(REDIS_WARNING, "Some other exception caught in EndForkOperation().\n");
        exit(1);
    }
    return FALSE;
}

LPVOID AllocHeapBlock(size_t size, BOOL allocateHigh) {
    if (g_BypassMemoryMapOnAlloc) {
        return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    LPVOID retPtr = (LPVOID)NULL;
    if (size % cAllocationGranularity != 0 ) {
        errno = EINVAL;
        return retPtr;
    }
    int contiguousBlocksToAllocate = (int)(size / cAllocationGranularity);

    if (contiguousBlocksToAllocate > g_pQForkControl->availableBlocksInHeap) {
        errno = ENOMEM;
        return retPtr;
    }

    size_t mapped = 0;
    int startIndex = allocateHigh ? g_pQForkControl->availableBlocksInHeap - 1 : 0;
    int endIndex = allocateHigh ?
                   contiguousBlocksToAllocate - 2 : 
                   g_pQForkControl->availableBlocksInHeap - contiguousBlocksToAllocate + 1;
    int direction = allocateHigh ? -1 : 1;
    int blockIndex = 0;
    int contiguousBlocksFound = 0;
    for(blockIndex = startIndex; 
        blockIndex != endIndex; 
        blockIndex += direction) {
        for (int n = 0; n < contiguousBlocksToAllocate; n++) {
            assert((blockIndex + n * direction >= 0) &&
                   (blockIndex + n * direction < g_pQForkControl->availableBlocksInHeap));

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
            (cAllocationGranularity * allocationStart);
        for(int n = 0; n < contiguousBlocksToAllocate; n++ ) {
            g_pQForkControl->heapBlockMap[allocationStart+n] = BlockState::bsMAPPED;
            mapped += cAllocationGranularity; 
        }
        retPtr = blockStart;
    }
    else {
        errno = ENOMEM;
    }

    return retPtr;
}

BOOL FreeHeapBlock(LPVOID block, size_t size) {
    if (size == 0) {
        return FALSE;
    }

    if (g_BypassMemoryMapOnAlloc) {
        return VirtualFree(block, 0, MEM_RELEASE);
    }

    INT_PTR ptrDiff = reinterpret_cast<byte*>(block) - reinterpret_cast<byte*>(g_pQForkControl->heapStart);
    if (ptrDiff < 0 || (ptrDiff % cAllocationGranularity) != 0) {
        return FALSE;
    }

    int blockIndex = (int)(ptrDiff / cAllocationGranularity);
    if (blockIndex >= g_pQForkControl->availableBlocksInHeap) {
        return FALSE;
    }

    int contiguousBlocksToFree = (int)(size / cAllocationGranularity);

    if (VirtualUnlock(block, size) == FALSE) {
        DWORD err = GetLastError();
        if (err != ERROR_NOT_LOCKED) {
            return FALSE;
        }
    }
    for (int n = 0; n < contiguousBlocksToFree; n++ ) {
        g_pQForkControl->heapBlockMap[blockIndex + n] = BlockState::bsUNMAPPED;
    }
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

BOOL IsChildProcess() {
    return (g_argMap.find(cQFork) != g_argMap.end());
}

void SetupQForkGlobals(int argc, char* argv[]) {
    g_SentinelMode = checkForSentinelMode(argc, argv);
    g_IsChildProcess = IsChildProcess();
    g_PersistenceDisabled = IsPersistenceDisabled();
    g_BypassMemoryMapOnAlloc = g_PersistenceDisabled || g_SentinelMode;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_SystemAllocationGranularity = si.dwAllocationGranularity;

    PERFORMANCE_INFORMATION perfinfo;
    perfinfo.cb = sizeof(PERFORMANCE_INFORMATION);
    if (FALSE == GetPerformanceInfo(&perfinfo, sizeof(PERFORMANCE_INFORMATION))) {
        redisLog(REDIS_WARNING, "GetPerformanceInfo failed.\n");
        redisLog(REDIS_WARNING, "Failing startup.\n");
        exit(1);
    }
    Globals::pageSize = perfinfo.PageSize;
    Globals::memoryPhysicalTotal = perfinfo.PhysicalTotal;
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
            SetupLogging();
            StackTraceInit();
            InitThreadControl();
        } catch (system_error syserr) {
            string errMsg = string("System error during startup: ") + syserr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        } catch (runtime_error runerr) {
            string errMsg = string("System error during startup: ") + runerr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        } catch (invalid_argument &iaerr) {
            string errMsg = string("Invalid argument during startup: ") + iaerr.what();
            RedisEventLog().LogError(errMsg);
            cout << errMsg << endl;
            exit(-1);
        } catch (exception othererr) {
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

            SetupQForkGlobals(argc, argv);

            if (g_PersistenceDisabled || g_SentinelMode) {
                // Sentinel mode and Redis with persistence off don't use the
                // QFork architecture, the redis main can be called directly
                return redis_main(argc, argv);
            } else {
                int result = -1;
                if (g_IsChildProcess) {
                    // Initialize and run the child process
                    result = QForkStartup();
                } else {
                    // Initialize the parent data structures
                    if (QForkParentInit()) {
                        // Run the parent process
                        result = redis_main(argc, argv);
                    }
                }
                // Cleanup and exit
                QForkShutdown();
                return result;
            }
        } catch (system_error syserr) {
            RedisEventLog().LogError(string("Main: system error. ") + syserr.what());
            redisLog(REDIS_WARNING, "Main: system error. ErrCode: 0x%08x, ErrMsg: %s\n", syserr.code().value(), syserr.what());
        } catch (runtime_error runerr) {
            RedisEventLog().LogError(string("Main: runtime error. ") + runerr.what());
            redisLog(REDIS_WARNING, "Main: runtime error. ErrMsg: %s\n", runerr.what());
        } catch (exception ex) {
            RedisEventLog().LogError(string("Main: an exception occurred. ") + ex.what());
            redisLog(REDIS_WARNING, "Main: an exception occurred. ErrMsg: %s\n", ex.what());
        }
    }
}
