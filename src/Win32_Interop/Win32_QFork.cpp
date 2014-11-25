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

#include "Win32_FDAPI.h"

#include <Windows.h>
#include <WinNT.h>
#include <errno.h>
#include <stdio.h>
#include <wchar.h>
#include <Psapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>

#define QFORK_MAIN_IMPL
#include "Win32_QFork.h"

#include "Win32_QFork_impl.h"
#include "Win32_dlmalloc.h"
#include "Win32_SmartHandle.h"
#include "Win32_Service.h"
#include "Win32_CommandLine.h"
#include "..\redisLog.h"

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

const long long cSentinelHeapSize = 30 * 1024 * 1024;
extern "C" int checkForSentinelMode(int argc, char **argv);
extern "C" void InitTimeFunctions();

extern "C"
{
  void*(*g_malloc)(size_t) = nullptr;
  void*(*g_calloc)(size_t, size_t) = nullptr;
  void*(*g_realloc)(void*, size_t) = nullptr;
  void(*g_free)(void*) = nullptr;
  size_t(*g_msize)(void*) = nullptr;
  
  // forward def from util.h. 
	long long memtoll(const char *p, int *err);
}

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

const SIZE_T cAllocationGranularity = 1 << 18;                    // 256KB per heap block (matches large block allocation threshold of dlmalloc)
const int cMaxBlocks = 1 << 24;                                   // 256KB * 16M heap blocks = 4TB. 4TB is the largest memory config Windows supports at present.
const char* cMapFileBaseName = "RedisQFork";
const int cDeadForkWait = 30000;
size_t pageSize = 0;

#ifndef _WIN64
size_t cDefaultmaxHeap32Bit = pow(2, 29);
#endif

enum class BlockState : std::uint8_t {bsINVALID = 0, bsUNMAPPED = 1, bsMAPPED = 2};

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
DWORD g_systemAllocationGranularity;
int g_SlaveExitCode = 0; // For slave process

bool ReportSpecialSystemErrors(int error) {
    switch (error)
    {
        case ERROR_COMMITMENT_LIMIT:
        {
            ::redisLog(
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
            return true;
        }

        case ERROR_DISK_FULL:
        {
            ::redisLog(
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
            return true;
        }
    
        default:
            return false;
    }
}

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
#ifdef _WIN64           
           HIDWORD(mmSize),
#else      
           0,
#endif
           LODWORD(mmSize),
           string("Could not open file mapping object in slave"));
       g_pQForkControl->heapMemoryMap = sfmhMapFile;


	   // The key to mapping a heap larger than physical memory is to not map it all at once. 
	   SmartFileView<byte> sfvHeap(
            g_pQForkControl->heapMemoryMap,
            FILE_MAP_COPY,
            0, 0, 
			cAllocationGranularity,	// Only map a portion of the heap . Deal with the unmapped pages with a VEH.
            g_pQForkControl->heapStart,
            string("Could not map heap in forked process. Is system swap file large enough?"));

        // setup DLMalloc global data
        if( SetDLMallocGlobalState(g_pQForkControl->DLMallocGlobalStateSize, g_pQForkControl->DLMallocGlobalState) != 0) {
            throw std::runtime_error("DLMalloc global state copy failed.");
        }

        // signal parent that we are ready
        SetEvent(g_pQForkControl->forkedProcessReady);

        // wait for parent to signal operation start
        WaitForSingleObject(g_pQForkControl->startOperation, INFINITE);

        // copy redis globals into fork process
        SetupGlobals(g_pQForkControl->globalData.globalData, g_pQForkControl->globalData.globalDataSize, g_pQForkControl->globalData.dictHashSeed);

        // execute requested operation
        if (g_pQForkControl->typeOfOperation == OperationType::otRDB) {
			g_SlaveExitCode = do_rdbSave(g_pQForkControl->globalData.filename);
        } else if (g_pQForkControl->typeOfOperation == OperationType::otAOF) {
			g_SlaveExitCode = do_aofSave(g_pQForkControl->globalData.filename);
        } else {
            throw runtime_error("unexpected operation type");
        }

        // let parent know we are done
        SetEvent(g_pQForkControl->operationComplete);

        // parent will notify us when to quit
        WaitForSingleObject(g_pQForkControl->terminateForkedProcess, INFINITE);

        g_pQForkControl = NULL;
        return TRUE;
    }
    catch(std::system_error syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            ::redisLog(REDIS_WARNING, "QForkSlaveInit: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
            g_pQForkControl = NULL;
            if (g_pQForkControl != NULL) {
                if (g_pQForkControl->operationFailed != NULL) {
                    SetEvent(g_pQForkControl->operationFailed);
                }
            }
            return FALSE;
        }
    }
    catch(std::runtime_error runerr) {
        ::redisLog(REDIS_WARNING, "QForkSlaveInit: runtime error caught. message=%s\n", runerr.what());
        g_pQForkControl = NULL;
        SetEvent(g_pQForkControl->operationFailed);
        return FALSE;
    }
    return FALSE;
}

string GetLocalAppDataFolder() {
    char localAppDataPath[_MAX_PATH];
    HRESULT hr;
    if (S_OK != (hr = SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, localAppDataPath))) {
        throw std::system_error(hr, system_category(), "SHGetFolderPathA failed");
    }
    char redisAppDataPath[_MAX_PATH];
    if (NULL == PathCombineA(redisAppDataPath, localAppDataPath, "Redis")) {
        throw std::system_error(hr, system_category(), "PathCombineA failed");
    }

    if (PathIsDirectoryA(redisAppDataPath) == FALSE) {
        if (CreateDirectoryA(redisAppDataPath, NULL) == FALSE) {
            throw std::system_error(hr, system_category(), "CreateDirectoryA failed");
        }
    }

    return redisAppDataPath;
}

string g_MMFDir;
string GetWorkingDirectory() {
    if (g_MMFDir.length() == 0) {
        string workingDir;
        if (g_argMap.find(cHeapDir) != g_argMap.end()) {
            workingDir = g_argMap[cHeapDir][0][0];
            std::replace(workingDir.begin(), workingDir.end(), '/', '\\');

            if (PathIsRelativeA(workingDir.c_str())) {
                char cwd[MAX_PATH];
                if (0 == ::GetCurrentDirectoryA(MAX_PATH, cwd)) {
                    throw std::system_error(GetLastError(), system_category(), "GetCurrentDirectoryA failed");
                }
                char fullPath[_MAX_PATH];
                if (NULL == PathCombineA(fullPath, cwd, workingDir.c_str())) {
                    throw std::system_error(GetLastError(), system_category(), "PathCombineA failed");
                }
                workingDir = fullPath;
            }
        } else {
            workingDir = GetLocalAppDataFolder();
        }

        if (workingDir.at(workingDir.length() - 1) != '\\') {
            workingDir = workingDir.append("\\");
        }

        g_MMFDir = workingDir;
    }

    return g_MMFDir;
}

BOOL QForkMasterInit( __int64 maxheapBytes ) {
    try {
        // allocate file map for qfork control so it can be passed to the forked process
        g_hQForkControlFileMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0, sizeof(QForkControl),
            NULL);
        if (g_hQForkControlFileMap == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateFileMapping failed");
        }

        g_pQForkControl = (QForkControl*)MapViewOfFile(
            g_hQForkControlFileMap, 
            FILE_MAP_ALL_ACCESS,
            0, 0,
            0);
        if (g_pQForkControl == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "MapViewOfFile failed");
        }

        // This must be called only once per process! Calling it more times than that will not recreate existing 
        // section, and dlmalloc will ultimately fail with an access violation. Once is good.
        if (dlmallopt(M_GRANULARITY, cAllocationGranularity) == 0) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "DLMalloc failed initializing allocation granularity.");
        }
        g_pQForkControl->heapBlockSize = cAllocationGranularity;

		// ensure the number of blocks is a multiple of cAllocationGranularity
        SIZE_T allocationBlocks = (SIZE_T)maxheapBytes / cAllocationGranularity;
		allocationBlocks += ((maxheapBytes % cAllocationGranularity) != 0);

        g_pQForkControl->availableBlocksInHeap = (int)allocationBlocks;
        if (g_pQForkControl->availableBlocksInHeap <= 0) {
            throw std::runtime_error(
                "Invalid number of heap blocks.");
        }

        // FILE_FLAG_DELETE_ON_CLOSE will not clean up files in the case of a BSOD or power failure.
        // Clean up anything we can to prevent excessive disk usage.
        char heapMemoryMapWildCard[MAX_PATH];
        WIN32_FIND_DATAA fd;
        sprintf_s(
            heapMemoryMapWildCard,
            MAX_PATH,
            "%s%s_*.dat",
            GetWorkingDirectory().c_str(),
            cMapFileBaseName);
        HANDLE hFind = FindFirstFileA(heapMemoryMapWildCard, &fd);
        while (hFind != INVALID_HANDLE_VALUE) {
            // Failure likely means the file is in use by another redis instance.
            DeleteFileA(fd.cFileName);

            if (FALSE == FindNextFileA(hFind, &fd)) {
                FindClose(hFind);
                hFind = INVALID_HANDLE_VALUE;
            }
        }

        string workingDir = GetWorkingDirectory();

        char heapMemoryMapPath[MAX_PATH];
        sprintf_s(
            heapMemoryMapPath,
            MAX_PATH,
            "%s%s_%d.dat",
            workingDir.c_str(),
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
                NULL );
        if (g_pQForkControl->heapMemoryMapFile == INVALID_HANDLE_VALUE) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateFileW failed.");
        }

		// There is a strange random failure toawrds the end of mapping the heap in the forked process in the VEH if
		// the underlying MMF is not larger than the MM space we are using. This seems to be some sort of 
		// memory->file allocation granularity issue. Increasing the size of the file (by 16MB) takes care of the 
		// issue in all cases.
		const size_t extraMMF = 64 * cAllocationGranularity;

		SIZE_T mmSize = g_pQForkControl->availableBlocksInHeap * cAllocationGranularity + extraMMF; 
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
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateFileMapping failed.");
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
            throw std::system_error(
                GetLastError(),
                system_category(),
                "VirtualAllocEx failed.");
        }
        if (VirtualFree(pHigh, 0, MEM_RELEASE) == FALSE) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "VirtualFree failed.");
        }

        g_pQForkControl->heapStart = 
            MapViewOfFileEx(
                g_pQForkControl->heapMemoryMap,
                FILE_MAP_ALL_ACCESS,
                0,0,                            
                0,  
                pHigh);
        if (g_pQForkControl->heapStart == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "MapViewOfFileEx failed.");
        }

        for (int n = 0; n < cMaxBlocks; n++) {
            g_pQForkControl->heapBlockMap[n] = 
                ((n < g_pQForkControl->availableBlocksInHeap) ?
                BlockState::bsUNMAPPED : BlockState::bsINVALID);
        }

        g_pQForkControl->typeOfOperation = OperationType::otINVALID;
        g_pQForkControl->forkedProcessReady = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (g_pQForkControl->forkedProcessReady == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateEvent failed.");
        }
        g_pQForkControl->startOperation = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (g_pQForkControl->startOperation == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateEvent failed.");
        }
        g_pQForkControl->operationComplete = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (g_pQForkControl->operationComplete == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateEvent failed.");
        }
        g_pQForkControl->operationFailed = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (g_pQForkControl->operationFailed == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateEvent failed.");
        }
        g_pQForkControl->terminateForkedProcess = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (g_pQForkControl->terminateForkedProcess == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "CreateEvent failed.");
        }

        return TRUE;
    }
    catch(std::system_error syserr) {
        if (ReportSpecialSystemErrors(syserr.code().value()) == false) {
            ::redisLog(REDIS_WARNING, "QForkMasterInit: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
        }
    }
    catch(std::runtime_error runerr) {
        ::redisLog(REDIS_WARNING, "QForkMasterInit: runtime error caught. message=%s\n", runerr.what());
    }
    catch(...) {
        ::redisLog(REDIS_WARNING, "QForkMasterInit: other exception caught.\n");
    }
    return FALSE;
}

LONG CALLBACK VectoredHeapMapper(PEXCEPTION_POINTERS info) {
	if( info->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION && 
		info->ExceptionRecord->NumberParameters == 2) {
		intptr_t failingMemoryAddress = info->ExceptionRecord->ExceptionInformation[1];
		intptr_t heapStart = (intptr_t)g_pQForkControl->heapStart;
		intptr_t heapEnd = heapStart + ((SIZE_T)g_pQForkControl->availableBlocksInHeap * g_pQForkControl->heapBlockSize);
		if( failingMemoryAddress >= heapStart && failingMemoryAddress <= heapEnd )
		{
			intptr_t startOfMapping = failingMemoryAddress - failingMemoryAddress % g_systemAllocationGranularity;
			intptr_t mmfOffset = startOfMapping - heapStart;
            size_t bytesToMap = min((size_t)g_systemAllocationGranularity, (size_t)(heapEnd - startOfMapping));
			LPVOID pMapped =  MapViewOfFileEx( 
				g_pQForkControl->heapMemoryMap, 
				FILE_MAP_COPY,
#ifdef _WIN64           
                HIDWORD(mmfOffset),
#else      
                0,
#endif
                LODWORD(mmfOffset),
				bytesToMap,
				(LPVOID)startOfMapping);
			if(pMapped != NULL)
			{
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			else
			{
                ::redisLog(REDIS_WARNING, "\nF(0x%p)", startOfMapping);
                ::redisLog(REDIS_WARNING, "\t MapViewOfFileEx failed with error 0x%08X. \n", GetLastError());
                ::redisLog(REDIS_WARNING, "\t heapStart 0x%p\n", heapStart);
                ::redisLog(REDIS_WARNING, "\t heapEnd 0x%p\n", heapEnd);
                ::redisLog(REDIS_WARNING, "\t failing access location 0x%p\n", failingMemoryAddress);
                ::redisLog(REDIS_WARNING, "\t offset into mmf to start mapping 0x%016X\n", mmfOffset);
                ::redisLog(REDIS_WARNING, "\t start of new mapping 0x%p \n", startOfMapping);
                ::redisLog(REDIS_WARNING, "\t bytes to map 0x%08x \n", bytesToMap);
                ::redisLog(REDIS_WARNING, "\t continuing exception handler search \n");
			}
		}
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

// QFork API
StartupStatus QForkStartup(int argc, char** argv) {
    bool foundSlaveFlag = false;
    HANDLE QForkConrolMemoryMapHandle = NULL;
    DWORD PPID = 0;
    __int64 maxheapBytes = -1;
    __int64 maxmemoryBytes = -1;
    int memtollerr = 0;

	SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_systemAllocationGranularity = si.dwAllocationGranularity;

    if (g_argMap.find(cQFork) != g_argMap.end()) {
        // slave command line looks like: --QFork [QForkConrolMemoryMap handle] [parent process id]
        foundSlaveFlag = true;
        char* endPtr;
        QForkConrolMemoryMapHandle = (HANDLE)strtoul(g_argMap[cQFork].at(0).at(0).c_str(),&endPtr,10);
        char* end = NULL;
        PPID = strtoul(g_argMap[cQFork].at(0).at(1).c_str(), &end, 10);
    } else {
        if (g_argMap.find(cMaxHeap) != g_argMap.end()) {
            int mtollerr = 0;
            maxheapBytes = memtoll(g_argMap[cMaxHeap].at(0).at(0).c_str(), &memtollerr);
        }
        if (g_argMap.find(cMaxMemory) != g_argMap.end()) {
            int mtollerr = 0;
            maxmemoryBytes = memtoll(g_argMap[cMaxMemory].at(0).at(0).c_str(), &memtollerr);
        }
    }

	PERFORMANCE_INFORMATION perfinfo;
	perfinfo.cb = sizeof(PERFORMANCE_INFORMATION);
	if (FALSE == GetPerformanceInfo(&perfinfo, sizeof(PERFORMANCE_INFORMATION))) {
        ::redisLog(REDIS_WARNING, "GetPerformanceInfo failed.\n");
        ::redisLog(REDIS_WARNING, "Failing startup.\n");
		return StartupStatus::ssFAILED;
	}
	pageSize = perfinfo.PageSize;

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
	int64_t maxMemoryPlusHalf = (3 * maxmemoryBytes) / 2;
	if( maxmemoryBytes != -1 ) {
		maxheapBytes = (maxheapBytes > maxMemoryPlusHalf) ? maxheapBytes : maxMemoryPlusHalf;
	}
	if( maxheapBytes == -1 )
	{
        if (checkForSentinelMode(argc, argv)) {
            // Sentinel mode does not need a large heap. This conserves disk space and page file reservation requirements.
            maxheapBytes = cSentinelHeapSize;
        } else {
#ifdef _WIN64		
            maxheapBytes = perfinfo.PhysicalTotal * pageSize;
#else
            maxheapBytes = cDefaultmaxHeap32Bit;
#endif
        }
	}

    if (foundSlaveFlag) {
		LPVOID exceptionHandler = AddVectoredExceptionHandler( 1, VectoredHeapMapper );
		StartupStatus retVal = StartupStatus::ssFAILED;
		try {
			retVal = QForkSlaveInit( QForkConrolMemoryMapHandle, PPID ) ? StartupStatus::ssSLAVE_EXIT : StartupStatus::ssFAILED;
		} catch (...) { }
		RemoveVectoredExceptionHandler(exceptionHandler);		
		return retVal;
    } else {
		return QForkMasterInit(maxheapBytes) ? StartupStatus::ssCONTINUE_AS_MASTER : StartupStatus::ssFAILED;
    }
}

BOOL QForkShutdown() {
    if(g_hForkedProcess != NULL) {
        TerminateProcess(g_hForkedProcess, -1);
		CloseHandle(g_hForkedProcess);
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

BOOL BeginForkOperation(OperationType type, char* fileName, LPVOID globalData, int sizeOfGlobalData, DWORD* childPID, uint32_t dictHashSeed, char* logfile) {
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
        sprintf_s(
            arguments,
            _MAX_PATH,
            "\"%s\" --%s %llu %lu --%s \"%s\"",
            fileName,
            cQFork.c_str(),
            (uint64_t)g_hQForkControlFileMap,
            GetCurrentProcessId(),
            cLogfile.c_str(),
            (logfile != NULL && logfile[0] != '\0') ? logfile : "stdout");
        
        if (FALSE == CreateProcessA(fileName, arguments, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            throw system_error( 
                GetLastError(),
                system_category(),
                "Problem creating slave process" );
        }
        (*childPID) = pi.dwProcessId;
		g_hForkedProcess = pi.hProcess;
		CloseHandle(pi.hThread);

        // wait for "forked" process to map memory
		if (WaitForSingleObject(g_pQForkControl->forkedProcessReady, cDeadForkWait) != WAIT_OBJECT_0) {
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
        ::redisLog(REDIS_WARNING, "BeginForkOperation: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
    }
    catch(std::runtime_error runerr) {
        ::redisLog(REDIS_WARNING, "BeginForkOperation: runtime error caught. message=%s\n", runerr.what());
    }
    catch(...) {
        ::redisLog(REDIS_WARNING, "BeginForkOperation: other exception caught.\n");
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
			CloseHandle(g_hForkedProcess);
        }

        return EndForkOperation(NULL);
    }
    catch(std::system_error syserr) {
        ::redisLog(REDIS_WARNING, "AbortForkOperation(): 0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state, then another fork operation is not possible. 
        exit(1);
    }
    catch( ... ) {
        ::redisLog(REDIS_WARNING, "Some other exception caught in EndForkOperation().\n");
        exit(1);
    }
    return FALSE;
}

void RejoinCOWPages(HANDLE mmHandle, byte* mmStart, size_t mmSize) {
    SmartFileView<byte> copyView(
        mmHandle,
        FILE_MAP_WRITE,
        0,
        0,
        mmSize,
        string("RejoinCOWPages: Could not map COW back-copy view."));
    HANDLE hProcess = GetCurrentProcess();
    int pages = (int)(mmSize / pageSize);
    shared_ptr<PSAPI_WORKING_SET_EX_INFORMATION> pwsi(
        new PSAPI_WORKING_SET_EX_INFORMATION[pages],
        [](PSAPI_WORKING_SET_EX_INFORMATION *p) { delete[] p; });
    if (pwsi == NULL) {
        throw new system_error(
            GetLastError(),
            system_category(),
            "pwsi == NULL");
    }
    memset(pwsi.get(), 0, sizeof(PSAPI_WORKING_SET_EX_INFORMATION)* pages);
    int virtualLockFailures = 0;
    for (int page = 0; page < pages; page++) {
        pwsi.get()[page].VirtualAddress = mmStart + page * pageSize;
    }

    if (QueryWorkingSetEx(
        hProcess,
        pwsi.get(),
        sizeof(PSAPI_WORKING_SET_EX_INFORMATION)* pages) == FALSE) {
        throw system_error(
            GetLastError(),
            system_category(),
            "RejoinCOWPages: QueryWorkingSet failure");
    }

    for (int page = 0; page < pages; page++) {
        if (pwsi.get()[page].VirtualAttributes.Valid == 1) {
            // A 0 share count indicates a COW page
            if (pwsi.get()[page].VirtualAttributes.ShareCount == 0) {
                memcpy(copyView + (page*pageSize), mmStart + (page*pageSize), pageSize);
            }
        }
    }

    // If the COWs are not discarded, then there is no way of propagating changes into subsequent fork operations. 
    if (IsWindowsVersionAtLeast(8, 0, 0)) {
        // restores all page protections on the view and culls the COW pages.
        DWORD oldProtect;
        if (FALSE == VirtualProtect(mmStart, pages * pageSize, PAGE_READWRITE | PAGE_REVERT_TO_FILE_MAP, &oldProtect)) {
            throw std::system_error(GetLastError(), std::system_category(), "RejoinCOWPages: COW cull failed");
        }
    } else {
        // Prior to Win8 unmapping the view was the only way to discard the COW pages from the view. Unfortunately this forces
        // the view to be completely flushed to disk, which is a bit inefficient.
        if (UnmapViewOfFile(mmStart) == FALSE) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "RejoinCOWPages: UnmapViewOfFile failed.");
        }
        // There is a race condition here. Something could map into the virtual address space used by the heap at the moment 
        // we are discarding local changes. There is nothing to do but report the problem and exit. This problem does not 
        // exist with the code above in Win8+ as the view is never unmapped.
        LPVOID remapped =
            MapViewOfFileEx(
            mmHandle,
            FILE_MAP_ALL_ACCESS,
            0, 0,
            0,
            mmStart);
        if (remapped == NULL) {
            throw std::system_error(
                GetLastError(),
                system_category(),
                "RejoinCOWPages: MapViewOfFileEx failed. Please upgrade your OS to Win8 or newer.");
        }
    }
}

BOOL EndForkOperation(int * pExitCode) {
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
			
			if (pExitCode != NULL) {
				GetExitCodeProcess(g_hForkedProcess, (DWORD*)pExitCode);
			}

			CloseHandle(g_hForkedProcess);
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

        // move local changes back into memory mapped views for next fork operation
        RejoinCOWPages(
            g_pQForkControl->heapMemoryMap,
            (byte*)g_pQForkControl->heapStart,
            g_pQForkControl->availableBlocksInHeap * cAllocationGranularity);

        RejoinCOWPages(
            g_hQForkControlFileMap,
            (byte*)g_pQForkControl,
            sizeof(QForkControl));

        return TRUE;
    }
    catch(std::system_error syserr) {
        ::redisLog(REDIS_WARNING, "EndForkOperation: 0x%08x - %s\n", syserr.code().value(), syserr.what());

        // If we can not properly restore fork state, then another fork operation is not possible. 
        exit(1);
    }
    catch( ... ) {
        ::redisLog(REDIS_WARNING, "Some other exception caught in EndForkOperation().\n");
        exit(1);
    }
    return FALSE;
}

int blocksMapped = 0;
int totalAllocCalls = 0;
int totalFreeCalls = 0;

LPVOID AllocHeapBlock(size_t size, BOOL allocateHigh) {
    totalAllocCalls++;
    LPVOID retPtr = (LPVOID)NULL;
    if (size % g_pQForkControl->heapBlockSize != 0 ) {
        errno = EINVAL;
        return retPtr;
    }
    int contiguousBlocksToAllocate = (int)(size / g_pQForkControl->heapBlockSize);

    size_t mapped = 0;
    int startIndex = allocateHigh ? g_pQForkControl->availableBlocksInHeap - 1 : contiguousBlocksToAllocate - 1;
    int endIndex = allocateHigh ? -1 : g_pQForkControl->availableBlocksInHeap - contiguousBlocksToAllocate + 1;
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

void SetupLogging() {
    bool serviceRun = g_argMap.find(cServiceRun) != g_argMap.end();
    string syslogEnabledValue = (g_argMap.find(cSyslogEnabled) != g_argMap.end() ? g_argMap[cSyslogEnabled].at(0).at(0) : cNo);
    bool syslogEnabled = (syslogEnabledValue.compare(cYes) == 0) || serviceRun;
    string syslogIdent = (g_argMap.find(cSyslogIdent) != g_argMap.end() ? g_argMap[cSyslogIdent].at(0).at(0) : cDefaultSyslogIdent);
    string logFileName = (g_argMap.find(cLogfile) != g_argMap.end() ? g_argMap[cLogfile].at(0).at(0) : cDefaultLogfile);

    setSyslogEnabled(syslogEnabled);
    if (syslogEnabled) {
        setSyslogIdent(syslogIdent.c_str());
    } else {
        setLogFile(logFileName.c_str());
    }
}

extern "C"
{
    BOOL IsPersistenceAvailable() {
        if (g_argMap.find(cPersistenceAvailable) != g_argMap.end()) {
            return (g_argMap[cPersistenceAvailable].at(0).at(0) != cNo);
        } else {
            return true;
        }
    }

    // The external main() is redefined as redis_main() by Win32_QFork.h.
    // The CRT will call this replacement main() before the previous main()
    // is invoked so that the QFork allocator can be setup prior to anything 
    // Redis will allocate.
    int main(int argc, char* argv[]) {
        try {
            InitTimeFunctions();
            ParseCommandLineArguments(argc, argv);
            SetupLogging();
        } catch (system_error syserr) {
            exit(-1);
        } catch (runtime_error runerr) {
            cout << runerr.what() << endl;
            exit(-1);
        } catch (invalid_argument &iaerr) {
            cout << iaerr.what() << endl;
            exit(-1);
        } catch (exception othererr) {
            cout << othererr.what() << endl;
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

            // service commands do not launch an instance of redis directly
            if (HandleServiceCommands(argc, argv) == TRUE) {
                return 0;
            }

            // Setup memory allocation scheme for persistence mode
            if (IsPersistenceAvailable() == TRUE) {
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

            if (IsPersistenceAvailable() == TRUE) {
  		        StartupStatus status = QForkStartup(argc, argv);
  		        if (status == ssCONTINUE_AS_MASTER) {
  			        int retval = redis_main(argc, argv);
  			        QForkShutdown();
  			        return retval;
  		        } else if (status == ssSLAVE_EXIT) {
  			        // slave is done - clean up and exit
  			        QForkShutdown();
  			        return g_SlaveExitCode;
  		        } else if (status == ssFAILED) {
  			        // master or slave failed initialization
  			        return 1;
  		        } else {
  			        // unexpected status return
  			        return 2;
  		        }
            } else {
                return redis_main(argc, argv);
            }
		} catch (std::system_error syserr) {
            ::redisLog(REDIS_WARNING, "main: system error caught. error code=0x%08x, message=%s\n", syserr.code().value(), syserr.what());
		} catch (std::runtime_error runerr) {
            ::redisLog(REDIS_WARNING, "main: runtime error caught. message=%s\n", runerr.what());
		} catch (...) {
            ::redisLog(REDIS_WARNING, "main: other exception caught.\n");
		}
    }
}
