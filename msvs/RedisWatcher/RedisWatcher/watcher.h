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

#include "RedisWatcher.h"

// states for processes
#define PROC_RUNNING    1
#define PROC_UNKNOWN    2
#define PROC_FAILED     3

// failure state for process
typedef struct _ProcHistory
{
    unsigned long StartTime;
    unsigned long StopTime;
    unsigned long FastFailCount;
} ProcHistory;

// restart policy from configuration
typedef struct _RestartPolicy
{
    unsigned long FastFailRetries;
    unsigned long FastFailMs;     // if process fails within this time, it failed during start
} RestartPolicy;

// Process state and configuration
typedef struct _ProcInstance
{
    wchar_t * WorkingDir;
    wchar_t * CmdParam;
    BOOL    SaveOutput;
    DWORD   RunMode;
    wchar_t * CmdLine;
    int     ProcessId;
    HANDLE  ProcessHandle;
    HANDLE  ProcessWaitHandle;
    int     State;
    ProcHistory History;
} ProcInstance;

// Set of processes
typedef struct _ProcList
{
    int     NumInstances;
    ProcInstance * Instances;
} ProcList;

// Configuration and processes
typedef struct _WatcherConfig
{
    wchar_t * ExecutableName;
    wchar_t * ExecutablePath;
    RestartPolicy Policy;
    ProcList ConfiguredInstances;
} WatcherConfig;


// method declarations

// High level start and stop
VOID SvcInstall();
void SvcStart();
BOOL WatcherStart(wchar_t * path);
void WatcherStop();

// initialize after loading or reloading configuration
void initialize(WatcherConfig * watchConfig);
void updateConfig(WatcherConfig * watchConfig);
void cleanup();

// configuration methods
WatcherConfig * parseConfig(wchar_t * configPath);
void freeConfig(WatcherConfig * config);
void startMonitorConfigFile(wchar_t * configPath);
void stopMonitorConfigFile();

// utility methods
void Lock();
void Unlock();
BOOL CopyString(wchar_t * value, wchar_t ** dest);
BOOL CombineFilePath(wchar_t * path, wchar_t * filename, wchar_t ** fullpath);
BOOL MakeAbsolute(wchar_t * filename, wchar_t ** fullpath);
BOOL GetCurrentDir(wchar_t ** fullpath);
BOOL GetModulePath(wchar_t ** fullpath);
wchar_t * Trim(wchar_t * buf);

