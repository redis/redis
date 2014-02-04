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

// Globals

// Name of configuration file
wchar_t * ConfigFile = L"watcher.conf";
wchar_t * configPath = NULL;

// Lock used to prevent threads from corrupting data
CRITICAL_SECTION ConfigLock;

// Locking functions
void Lock()
{
    EnterCriticalSection(&ConfigLock);
}

void Unlock()
{
    LeaveCriticalSection(&ConfigLock);
}

void InitLock()
{
    InitializeCriticalSection(&ConfigLock);
}

void TermLock()
{
    DeleteCriticalSection(&ConfigLock);
}

//
// Purpose:
//   Initialize the watcher
//
// Parameters:
//   configuration path
//
// Return value:
//   TRUE or FALSE
//
BOOL WatcherStart(wchar_t * path)
{
    WatcherConfig * config;

    EventRegisterMsOpenTech_RedisWatcher();
    InitLock();

    EventWriteWatcher_Start();
    if (CombineFilePath(path, ConfigFile, &configPath))
    {
        config = parseConfig(configPath);

        if (config != NULL)
        {
            initialize(config);
        }

        startMonitorConfigFile(configPath);

        return TRUE;
    }

    return FALSE;
}

//
// Purpose:
//   Terminate the watcher
//
// Parameters:
//   none
//
// Return value:
//   none
//
void WatcherStop()
{
    stopMonitorConfigFile();
    cleanup();
    EventWriteWatcher_Stop();

    EventUnregisterMsOpenTech_RedisWatcher();
    TermLock();
}

//
// Purpose:
//   Main entry point
//   Start as either a service or a console application
//
// Parameters:
//   argc, argv
//   no arguments means start as service
//   "console" means start as console.
//
// Return value:
//   exit code
//
int wmain(int argc, wchar_t* argv[])
{
    wchar_t * path = NULL;

    // If command-line parameter is "console", run as console app.
    // Otherwise, the service is probably being started by the SCM.

    if (argc > 1)
    {
        if (_wcsicmp(argv[1], L"console") == 0)
        {
            
            if (GetCurrentDir(&path))
            {
                wchar_t buff[100];

                // Start redis watcher
                if (!WatcherStart(path))
                {
                    printf("Failed to start watcher\n");
                    return 1;
                }

                // run until user enters x
                while (1)
                {
                    _getws_s(buff, 100);
                    if (buff[0] == L'x') break;
                }

                WatcherStop();

                return 0;
            }
            else
            {
                return 1;
            }
        }
        else
        {
            printf("Parameter not valid\n");
            return 1;
        }
    }
    else
    {
        SvcStart();
        return 0;
    }

    return 1;
}

