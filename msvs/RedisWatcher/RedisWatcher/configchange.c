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

// Configuration file and directory to watch for changes
wchar_t * ConfigFile;
wchar_t * ConfigDir;

// Last checked file time
FILETIME LastUpdate;

// Handles used to monitor for config file changes
HANDLE ConfigNotify = INVALID_HANDLE_VALUE;
HANDLE WaitHandle = INVALID_HANDLE_VALUE;

// trying to load file durng notify fails. Delay before reading
const ULONG ConfigLoadDelay = 2000;

// forward declaration
void monitorConfigfile(ULONG ms);

//
// Purpose:
//   Test if file update time has changed since last check
//
// Parameters:
//   path to file
//
// Return value:
//   TRUE or FALSE
//
BOOL TestFileChange(wchar_t * configPath)
{
    HANDLE hFile;
    FILETIME ftCreate, ftAccess, ftWrite;
    BOOL changed = FALSE;

    hFile = CreateFile(configPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        BOOL rc = GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite);
        if (rc)
        {
            if (LastUpdate.dwHighDateTime != ftWrite.dwHighDateTime ||
                LastUpdate.dwLowDateTime != ftWrite.dwLowDateTime)
            {
                changed = TRUE;
                LastUpdate = ftWrite;
            }
        }

        CloseHandle(hFile);
    }
    return changed;
}

//
// Purpose:
//   Notification callback for configuration file change or timeout
//   File may not be accessible during the change notification.
//   Continue monitoring until there are no changes for some time.
//   After the time delay, check if file time has changed, and if so,
//     reload configuration and start new instances
//
// Parameters:
//   context (not used) and timeout flag
//
// Return value:
//   none
//
void CALLBACK FileChangeCallback(void * context, BOOLEAN timeout)
{
    WatcherConfig * config;

    if (ConfigNotify == INVALID_HANDLE_VALUE)
        return;

    UnregisterWait(WaitHandle);
    WaitHandle = INVALID_HANDLE_VALUE;

    if (!timeout)
    {
        // something changed, delay before reloading
        FindNextChangeNotification(ConfigNotify);
        monitorConfigfile(ConfigLoadDelay);
    }
    else
    {
        if (TestFileChange(ConfigFile))
        {
            config = parseConfig(ConfigFile);
            if (config != NULL)
            {
                updateConfig(config);
            }
        }

        FindNextChangeNotification(ConfigNotify);
        monitorConfigfile(INFINITE);
    }
}

//
// Purpose:
//   Start monitoring the configuration file for updates
//
// Parameters:
//   path to file
//
// Return value:
//   none
//
void startMonitorConfigFile(wchar_t * configPath)
{
    ConfigFile = NULL;
    ConfigDir = NULL;

    // keep copies of path as file and directory
    if (CopyString(configPath, &ConfigFile) &&
        CopyString(configPath, &ConfigDir))
    {
        wchar_t * lastSlash = NULL;
        wchar_t * pos = ConfigDir;

        // replace last '\' with null for directory
        while (*pos != L'\0')
        {
            if (*pos == L'\\')
            {
                lastSlash = pos;
            }
            pos++;
        }

        if (lastSlash != NULL)
        {
            *lastSlash = L'\0';
        }

        // get current update time for file
        TestFileChange(ConfigFile);

        ConfigNotify = FindFirstChangeNotification(ConfigDir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (ConfigNotify != INVALID_HANDLE_VALUE)
        {
            monitorConfigfile(INFINITE);
        }
        else
        {
            EventWriteConfig_Monitor_Fail();
        }
    }
}

//
// Purpose:
//   Stop monitoring the configuration file for updates
//
// Parameters:
//   none
//
// Return value:
//   none
//
void stopMonitorConfigFile()
{
    if (WaitHandle != INVALID_HANDLE_VALUE)
    {
        UnregisterWait(WaitHandle);
        WaitHandle = INVALID_HANDLE_VALUE;
    }
    if (ConfigNotify != INVALID_HANDLE_VALUE)
    {
        FindCloseChangeNotification(ConfigNotify);
        ConfigNotify = INVALID_HANDLE_VALUE;
    }
}

//
// Purpose:
//   Monitor the configuration file for updates or timeout
//
// Parameters:
//   timeout in ms or INFINITE
//
// Return value:
//   none
//
void monitorConfigfile(ULONG ms)
{
    if (ConfigNotify != INVALID_HANDLE_VALUE)
    {
        BOOL rc = RegisterWaitForSingleObject(&WaitHandle,
                                                ConfigNotify,
                                                FileChangeCallback,
                                                (PVOID)ConfigNotify,
                                                ms,
                                                WT_EXECUTEONLYONCE);
        if (rc == FALSE)
        {
            EventWriteConfig_Monitor_Fail();
        }
    }
}

