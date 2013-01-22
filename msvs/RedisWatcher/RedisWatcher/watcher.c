/***********************************************************************
 * Copyright (c) Microsoft Open Technologies, Inc.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0.
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR
 * A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache License, Version 2.0 for specific language governing
 * permissions and limitations under the License.
 *
 **********************************************************************/

#include "stdafx.h"
#include "watcher.h"

// Global state for the process
static WatcherConfig * Config;
static ProcList * Discovered;

// Local methods
ProcList * findRunningProcesses(wchar_t * executableName);
void startIdleConfigured();
void startMonitoring(ProcInstance * instance);
void startRunning(WatcherConfig * config, ProcInstance * instance);
void ReleaseInstanceHandles(ProcInstance * instance);
void ReleaseInstanceAllocations(ProcInstance * instance);
void CopyMonitoringInstance(ProcInstance * newInstance, ProcInstance * oldInstance);


//
// Purpose:
//   Loads configuration, finds running processes and starts configured instances
//
// Parameters:
//   Configuration
//
// Return value:
//   None
//
void initialize(WatcherConfig * watchConfig)
{
    Lock();
    Config = watchConfig;

    Discovered = findRunningProcesses(Config->ExecutableName);

    // try to start configured process and start monitoring
    startIdleConfigured();

    Unlock();
}

//
// Purpose:
//   Releases resources used and stops monitoring processes
//
// Parameters:
//   none
//
// Return value:
//   None
//
void cleanup()
{
    int i;
    ProcInstance * instance;

    Lock();
    if (Config != NULL)
    {
        instance = Config->ConfiguredInstances.Instances;
        if (instance != NULL)
        {
            for (i = 0; i < Config->ConfiguredInstances.NumInstances; i++, instance++)
            {
                if (instance->ProcessId != -1)
                    ReleaseInstanceHandles(instance);
                ReleaseInstanceAllocations(instance);
            }
        }

        freeConfig(Config);
        Config = NULL;
    }

    if (Discovered != NULL)
    {
        instance = Discovered->Instances;
        if (instance != NULL)
        {
            for (i = 0; i < Discovered->NumInstances; i++, instance++)
            {
                if (instance->ProcessId != -1)
                    ReleaseInstanceHandles(instance);
            }
        }
        free(Discovered);
        Discovered = NULL;
    }
    Unlock();
}

//
// Purpose:
//   Uses new configuration data. Starts new processes if any.
//
// Parameters:
//   none
//
// Return value:
//   None
//
void updateConfig(WatcherConfig * watchConfig)
{
    int iold;
    int inew;
    ProcInstance * oldInstance;
    ProcInstance * newInstance;

    EventWriteConfig_File_Modified();
    Lock();

    // if old config has matching process, copy process info
    if (Config != NULL)
    {
        oldInstance = Config->ConfiguredInstances.Instances;
        for (iold = 0; iold < Config->ConfiguredInstances.NumInstances; iold++, oldInstance++)
        {
            if (oldInstance->ProcessId == -1)
                continue;

            newInstance = watchConfig->ConfiguredInstances.Instances;
            for (inew = 0; inew < watchConfig->ConfiguredInstances.NumInstances; inew++, newInstance++)
            {
                if (newInstance->ProcessId == -1 &&
                    ((oldInstance->WorkingDir != NULL && newInstance->WorkingDir != NULL &&
                    _wcsicmp(oldInstance->WorkingDir, newInstance->WorkingDir) == 0) ||
                    (oldInstance->WorkingDir == NULL && newInstance->WorkingDir == NULL)) &&
                    ((oldInstance->CmdParam != NULL && newInstance->CmdParam != NULL &&
                    _wcsicmp(oldInstance->CmdParam, newInstance->CmdParam) == 0) ||
                    (oldInstance->CmdParam == NULL && newInstance->CmdParam == NULL)))
                {
                    // same instance. Copy properties
                    CopyMonitoringInstance(newInstance, oldInstance);
                }
            }
        }
    }
    // stop all non copied waits
    cleanup();

    // find running processes and monitor everything
    initialize(watchConfig);

    Unlock();
}

//
// Purpose:
//   Find configured instance by pid
//
// Parameters:
//   pid
//
// Return value:
//   ProcInstance or NULL
//
ProcInstance *  FindConfiguredMonitoring(int pid)
{
    int i;
    ProcInstance * instance;

    if (Config == NULL)
        return NULL;

    instance = Config->ConfiguredInstances.Instances;
    for (i = 0; i < Config->ConfiguredInstances.NumInstances; i++, instance++)
    {
        if (instance->ProcessId == pid)
        {
            return instance;
        }
    }

    return NULL;
}

//
// Purpose:
//   Find discovered instance by pid
//
// Parameters:
//   pid
//
// Return value:
//   ProcInstance or NULL
//
ProcInstance * FindDiscoveredMonitoring(int pid)
{
    int i;
    ProcInstance * instance;

    if (Discovered != NULL)
    {
        instance = Discovered->Instances;
        for (i = 0; i < Discovered->NumInstances; i++, instance++)
        {
            if (instance->ProcessId == pid)
            {
                return instance;
            }
        }
    }
    return NULL;
}

//
// Purpose:
//   Copy instance information to new instance
//
// Parameters:
//   newInstance
//   oldInstance
//
// Return value:
//   none
//
void CopyMonitoringInstance(ProcInstance * newInstance, ProcInstance * oldInstance)
{
    // we are already monitoring this instance. Transfer state
    newInstance->State = oldInstance->State;
    newInstance->ProcessHandle = oldInstance->ProcessHandle;
    newInstance->ProcessWaitHandle = oldInstance->ProcessWaitHandle;
    newInstance->ProcessId = oldInstance->ProcessId;
    newInstance->History = oldInstance->History;

    oldInstance->ProcessHandle = NULL;
    oldInstance->ProcessWaitHandle = NULL;
    oldInstance->ProcessId = -1;
}

//
// Purpose:
//   Find running processes by executable name, and return instances
//
// Parameters:
//   executableName
//
// Return value:
//   ProcList
//
ProcList * findRunningProcesses(wchar_t * executableName)
{
    int i;
    ProcInstance * instance;
    ProcInstance * oldInstance;
    PROCESSENTRY32 procentry; 
    HANDLE snapshot;
    ProcList * list = (ProcList *)malloc(sizeof(ProcList));

    if (list == NULL || Config == NULL)
        return NULL;

    procentry.dwSize = sizeof(PROCESSENTRY32);

    list->NumInstances = 0;
    list->Instances = NULL;

    Lock();

    // find running processes for exe
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        i = 0;
        if (Process32First(snapshot, &procentry) == TRUE)
        {
            // intentionally ignore first process
            while (Process32Next(snapshot, &procentry) == TRUE)
            {
                if (_wcsicmp(procentry.szExeFile, Config->ExecutableName) == 0)
                {
                    // found exe with right name.
                    i++;
                }
            }
        }

        // found a few. Allocate structures for monitoring and start monitoring
        if (i > 0)
        {
            list->Instances = (ProcInstance *)malloc(sizeof(ProcInstance) * i);
            ZeroMemory(list->Instances, sizeof(ProcInstance) * i);
            list->NumInstances = i;

            instance = list->Instances;
            if (Process32First(snapshot, &procentry) == TRUE)
            {
                // intentionally ignore first process
                while (Process32Next(snapshot, &procentry) == TRUE)
                {
                    if (_wcsicmp(procentry.szExeFile, Config->ExecutableName) == 0)
                    {
                        // found exe with right name.
                        if (FindConfiguredMonitoring(procentry.th32ProcessID) != NULL)
                        {
                            // already monitoring a configured instance
                            instance->State = PROC_UNKNOWN;
                            instance->ProcessHandle = NULL;
                            instance->ProcessWaitHandle = NULL;
                            instance->ProcessId = -1;
                        }
                        else
                        {
                            oldInstance = FindDiscoveredMonitoring(procentry.th32ProcessID);

                            if (oldInstance == NULL)
                            {
                                HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, procentry.th32ProcessID);
                                if (hProcess != NULL)
                                {
                                    instance->WorkingDir = NULL;
                                    instance->CmdLine = NULL;
                                    instance->State = PROC_RUNNING;
                                    instance->ProcessHandle = hProcess;
                                    instance->ProcessId = procentry.th32ProcessID;
                                    startMonitoring(instance);
                                }
                                else
                                {
                                    instance->State = PROC_UNKNOWN;
                                    instance->ProcessHandle = NULL;
                                    instance->ProcessWaitHandle = NULL;
                                    instance->ProcessId = -1;
                                }
                            }
                            else
                            {
                                // we are already monitoring this instance. Transfer state
                                instance->WorkingDir = oldInstance->WorkingDir;
                                instance->CmdParam = oldInstance->CmdParam;
                                instance->CmdLine = oldInstance->CmdLine;
                                CopyMonitoringInstance(instance, oldInstance);
                            }
                        }
                        instance++;
                    }
                }
            }
        }
        CloseHandle(snapshot);
    }

    Unlock();

    return list;
}

//
// Purpose:
//   Release handles for an instance
//
// Parameters:
//   instance
//
// Return value:
//   none
//
void ReleaseInstanceHandles(ProcInstance * instance)
{
    if (instance->ProcessHandle != NULL)
    {
        CloseHandle(instance->ProcessHandle);
        instance->ProcessHandle = NULL;
    }
    if (instance->ProcessWaitHandle != NULL)
    {
        UnregisterWait(instance->ProcessWaitHandle);
        instance->ProcessWaitHandle = NULL;
    }

    instance->ProcessId = -1;
    instance->State = PROC_FAILED;
}

//
// Purpose:
//   Release memory for an instance
//
// Parameters:
//   instance
//
// Return value:
//   none
//
void ReleaseInstanceAllocations(ProcInstance * instance)
{
    if (instance->WorkingDir != NULL)
    {
        free(instance->WorkingDir);
        instance->WorkingDir = NULL;
    }
    if (instance->CmdParam != NULL)
    {
        free(instance->CmdParam);
        instance->CmdParam = NULL;
    }
    if (instance->CmdLine != NULL)
    {
        free(instance->CmdLine);
        instance->CmdLine = NULL;
    }
}

//
// Purpose:
//   Handle notification of a proces exit
//   If it is a configured instance, try to restart
//   If it is a discovered instance,try to start non-running
//     configured instances in case the port is now available
//
// Parameters:
//   pid as notification context
//
// Return value:
//   none
//
void CALLBACK ProcessExitCallback(void * context, BOOLEAN timeout)
{
    ProcInstance * instance;

    Lock();
    if (context != NULL && Config != NULL)
    {
        int pid = (int)context;
        instance = FindConfiguredMonitoring(pid);
        if (instance != NULL)
        {
            ReleaseInstanceHandles(instance);

            // we started this instance. Check if we should restart
            instance->History.StopTime = GetTickCount();

            if (instance->History.StopTime - instance->History.StartTime >
                    Config->Policy.FastFailMs)
            {
                // ran for a while. reset count
                instance->History.FastFailCount = 0;
            }
            else
            {
                instance->History.FastFailCount++;
            }

            if (instance->History.FastFailCount > Config->Policy.FastFailRetries)
            {
                EventWriteWatcher_RestartInstance_Giveup();
            }
            else
            {
                // restart instance
                EventWriteWatcher_RestartInstance();
                startRunning(Config, instance);
            }
        }
        else
        {
            instance = FindDiscoveredMonitoring(pid);
            if (instance != NULL)
            {
                EventWriteWatcher_DiscoveredInstance_Exit();
                ReleaseInstanceHandles(instance);

                // we discovered this instance. Try to start a configured instance
                startIdleConfigured();
            }
        }
    }
    Unlock();
}

//
// Purpose:
//   Try to start non-running
//     configured instances in case the port is now available
//
// Parameters:
//   none
//
// Return value:
//   none
//
void startIdleConfigured()
{
    ProcInstance * instance;
    int i;

    if (Config == NULL)
        return;

    Lock();

    instance = Config->ConfiguredInstances.Instances;
    for (i = 0; i < Config->ConfiguredInstances.NumInstances; i++, instance++)
    {
        if (instance->State == PROC_UNKNOWN || instance->State == PROC_FAILED)
        {
            EventWriteWatcher_StartInstance();
            startRunning(Config, instance);
        }
    }
    Unlock();
}

//
// Purpose:
//   Start monitoring an instance for termination
//
// Parameters:
//   instance
//
// Return value:
//   none
//
void startMonitoring(ProcInstance * instance)
{
    BOOL rc = RegisterWaitForSingleObject(&instance->ProcessWaitHandle,
                                            instance->ProcessHandle,
                                            ProcessExitCallback,
                                            (PVOID)instance->ProcessId,
                                            INFINITE,
                                            WT_EXECUTEONLYONCE);

    if (rc == FALSE)
    {
        EventWriteWatcher_Monitor_Fail();
    }
}

//
// Purpose:
//   Create a command line to start a new instance
//
// Parameters:
//   instance, configuration
//
// Return value:
//   command line or NULL
//
wchar_t * makeCmdLine(WatcherConfig * config, ProcInstance * instance)
{
    size_t cmdlineLen = 1;
    wchar_t * cmdLine;
    BOOL failed = FALSE;

    if (config == NULL)
        return NULL;

    cmdlineLen += wcslen(config->ExecutablePath) + 3;
    if (instance->CmdParam != NULL)
    {
        cmdlineLen += wcslen(instance->CmdParam) + 1;
    }

    cmdLine = (wchar_t *)malloc(cmdlineLen * sizeof(wchar_t));
    if (cmdLine == NULL)
    {
        return NULL;
    }

    cmdLine[0] = L'\0';
    // add quotes in case of spaces in path
    if (FAILED(StringCchCat(cmdLine, cmdlineLen, L"\"")))
    {
        failed = TRUE;
    }
    if (FAILED(StringCchCat(cmdLine, cmdlineLen, config->ExecutablePath)))
    {
        failed = TRUE;
    }
    if (FAILED(StringCchCat(cmdLine, cmdlineLen, L"\"")))
    {
        failed = TRUE;
    }

    if (instance->CmdParam != NULL)
    {
        if (FAILED(StringCchCat(cmdLine, cmdlineLen, L" ")))
        {
            failed = TRUE;
        }
        if (FAILED(StringCchCat(cmdLine, cmdlineLen, instance->CmdParam)))
        {
            failed = TRUE;
        }
    }

    if (failed)
    {
        free(cmdLine);
        return NULL;
    }

    return cmdLine;
}

//
// Purpose:
//   Start running a process for a configured instance
//
// Parameters:
//   instance, configuration
//
// Return value:
//   none
//
void startRunning(WatcherConfig * config, ProcInstance * instance)
{
    PROCESS_INFORMATION ProcInfo; 
    STARTUPINFO StartInfo;
    BOOL rc;
    wchar_t *stdoutpath = NULL;
    wchar_t *stderrpath = NULL;
    HANDLE lStdOutHandle = INVALID_HANDLE_VALUE;
    HANDLE lStdErrHandle = INVALID_HANDLE_VALUE;

    ZeroMemory(&ProcInfo, sizeof(ProcInfo));
    ZeroMemory(&StartInfo, sizeof(StartInfo));
    StartInfo.cb = sizeof(StartInfo);

    instance->CmdLine = makeCmdLine(config, instance);
    if (instance->CmdLine == NULL)
    {
        // log error
        return;
    }

    if (instance->SaveOutput)
    {
        SECURITY_ATTRIBUTES  sec;
        sec.bInheritHandle = TRUE;
        sec.nLength = sizeof(SECURITY_ATTRIBUTES);
        sec.lpSecurityDescriptor = NULL;

        if (CombineFilePath(instance->WorkingDir, L"stdout.log",&stdoutpath))
        {
            lStdOutHandle = CreateFile(stdoutpath, GENERIC_WRITE |GENERIC_READ,
                                      FILE_SHARE_READ, &sec, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
        }

        if (CombineFilePath(instance->WorkingDir, L"stderr.log",&stderrpath))
        {
            lStdErrHandle = CreateFile(stderrpath, GENERIC_WRITE |GENERIC_READ,
                                      FILE_SHARE_READ, &sec, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
        }

        if (lStdOutHandle != INVALID_HANDLE_VALUE && lStdErrHandle != INVALID_HANDLE_VALUE)
        {
            StartInfo.hStdError = lStdErrHandle;
            StartInfo.hStdOutput = lStdOutHandle;
            StartInfo.hStdInput = INVALID_HANDLE_VALUE;
            StartInfo.dwFlags = STARTF_USESTDHANDLES;
        }
    }

    rc = CreateProcess(NULL,
                        instance->CmdLine,
                        NULL,
                        NULL,
                        TRUE,
                        instance->RunMode,
                        NULL,
                        instance->WorkingDir,
                        &StartInfo,
                        &ProcInfo);
    if (rc == TRUE)
    {
        CloseHandle(ProcInfo.hThread);
        instance->ProcessHandle = ProcInfo.hProcess;
        instance->ProcessId = ProcInfo.dwProcessId;
        instance->ProcessWaitHandle = NULL;
        instance->History.StartTime = GetTickCount();
        instance->State = PROC_RUNNING;

        startMonitoring(instance);
    }
    else
    {
        EventWriteWatcher_StartInstance_Failure();
        instance->ProcessHandle = NULL;
        instance->ProcessId = -1;
        instance->State = PROC_FAILED;
    }

    // close file handles & free paths
    if (lStdOutHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(lStdOutHandle);
    }
    if (lStdErrHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(lStdErrHandle);
    }
    if (stderrpath != NULL)
        free(stderrpath);
    if (stdoutpath != NULL)
        free(stdoutpath);
}

