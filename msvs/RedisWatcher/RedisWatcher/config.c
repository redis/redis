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

// strings used for parsing the config file
const wchar_t commentChar = L'#';
const wchar_t exepathToken[] = L"exepath";
const wchar_t exenameToken[] = L"exename";
const wchar_t fastfailmsToken[] = L"fastfailms";
const wchar_t fastfailretriesToken[] = L"fastfailretries";
const wchar_t startInstanceToken[] = L"{";
const wchar_t stopInstanceToken[] = L"}";
const wchar_t runmodeToken[] = L"runmode";
const wchar_t workingdirToken[] = L"workingdir";
const wchar_t cmdparmsToken[] = L"cmdparms";
const wchar_t saveoutToken[] = L"saveout";
const wchar_t runmodeHidden[] = L"hidden";
const wchar_t runmodeConsole[] = L"console";

#define CONFIGLINE_MAX  1024


//
// Purpose:
//   Release config structure memory
//
// Parameters:
//   WatcherConfig
//
// Return value:
//   none
//
void freeConfig(WatcherConfig * config)
{
    if (config->ExecutablePath != NULL)
    {
        free(config->ExecutablePath);
        config->ExecutablePath = NULL;
    }
    if (config->ExecutableName != NULL)
    {
        free(config->ExecutableName);
        config->ExecutableName = NULL;
    }
    if (config->ConfiguredInstances.Instances != NULL)
    {
        free(config->ConfiguredInstances.Instances);
        config->ConfiguredInstances.Instances = NULL;
    }
    free(config);
}

//
// Purpose:
//   Read and parse a configuration file
//
// Parameters:
//   FILE reference
//
// Return value:
//   WatcherConfig or NULL
//
WatcherConfig * parseConfigFile(FILE * fp)
{
    ProcInstance * proc;
    WatcherConfig * config;
    wchar_t buf[CONFIGLINE_MAX+1];
    wchar_t * line;
    wchar_t * toksep;
    int instanceCount = 0;
    int numInstances = 0;
    BOOL inInstance = FALSE;
    int linenum = 0;
    wchar_t * key;
    wchar_t * value;
    DWORD dwAttrib;

    // scan for instances
    while (fgetws(buf, CONFIGLINE_MAX+1, fp) != NULL)
    {
        line = Trim(buf);
        if (_wcsnicmp(line, startInstanceToken, 1) == 0)
        {
            if (inInstance)
            {
                EventWriteConfig_File_MismatchBraces();
                return NULL;
            }
            numInstances++;
            inInstance = TRUE;
        }
        else if (_wcsnicmp(line, stopInstanceToken, 1) == 0)
        {
            if (!inInstance)
            {
                EventWriteConfig_File_MismatchBraces();
                return NULL;
            }
            inInstance = FALSE;
        }

        if (feof(fp) != 0)
            break;
    }

    if (inInstance)
    {
        EventWriteConfig_File_MismatchBraces();
        return NULL;
    }

    config = (WatcherConfig *)malloc(sizeof(WatcherConfig));
    if (config == NULL)
    {
        _set_errno(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    
    config->ExecutableName = NULL;
    config->ExecutablePath = NULL;
    config->Policy.FastFailRetries = 0;
    config->Policy.FastFailMs = 1000;
    config->ConfiguredInstances.NumInstances = 0;
    config->ConfiguredInstances.Instances = (ProcInstance *)malloc(sizeof(ProcInstance) * numInstances);

    // start from beginning
    fseek(fp, 0, SEEK_SET);
    instanceCount = 0;
    proc = NULL;

    while (fgetws(buf, CONFIGLINE_MAX+1, fp) != NULL)
    {
        line = Trim(buf);
        if (line[0] == commentChar || line[0] == L'\0')
        {
            continue;
        }


        if (_wcsnicmp(line, startInstanceToken, 1) == 0)
        {
            proc = config->ConfiguredInstances.Instances + instanceCount;
            instanceCount++;
            proc->CmdParam = NULL;
            proc->CmdLine = NULL;
            proc->WorkingDir = NULL;
            proc->State = PROC_UNKNOWN;
            proc->ProcessHandle = NULL;
            proc->ProcessId = -1;
            proc->SaveOutput = FALSE;
            proc->History.StartTime = 0;
            proc->History.StopTime = 0;
            proc->History.FastFailCount = 0;
        }
        else if (_wcsnicmp(line, stopInstanceToken, 1) == 0)
        {
            if (proc->WorkingDir == NULL)
            {
                EventWriteConfig_File_Invalid_WorkingDir();
                freeConfig(config);
                return NULL;
            }
            config->ConfiguredInstances.NumInstances = instanceCount;
            proc = NULL;
        }
        else
        {
            // each line except '{' and '}' has key and value
            // find first space or tab
            toksep = wcschr(line, L' ');
            if (toksep == NULL)
            {
                toksep = wcschr(line, L'\t');
                if (toksep == NULL)
                {
                    continue;
                }
            }

            // first token is key
            key = line;
            *toksep = L'\0';
            // trim remainder to get value
            value = Trim(toksep + 1);

            if (_wcsicmp(key, exepathToken) == 0)
            {
                if (!CopyString(value, &config->ExecutablePath))
                {
                    freeConfig(config);
                    return NULL;
                }
            }
            else if (_wcsicmp(key, exenameToken) == 0)
            {
                if (!CopyString(value, &config->ExecutableName))
                {
                    freeConfig(config);
                    return NULL;
                }
            }
            else if (_wcsicmp(key, fastfailmsToken) == 0)
            {
                unsigned long ms = wcstoul(value, NULL, 10);
                if (ms > 0)
                {
                    config->Policy.FastFailMs = ms;
                }
            }
            else if (_wcsicmp(key, fastfailretriesToken) == 0)
            {
                unsigned long retries = wcstoul(value, NULL, 10);
                if (retries > 0)
                {
                    config->Policy.FastFailRetries = retries;
                }
            }
            else if (_wcsicmp(key, runmodeToken) == 0)
            {
                if (proc != NULL)
                {
                    if (_wcsicmp(value, runmodeHidden) == 0)
                    {
                        proc->RunMode = CREATE_NO_WINDOW;
                    }
                    if (_wcsicmp(value, runmodeConsole) == 0)
                    {
                        proc->RunMode = CREATE_NEW_CONSOLE;
                    }
                }
            }
            else if (_wcsicmp(key, workingdirToken) == 0)
            {
                if (proc != NULL)
                {
                    if (!CopyString(value, &proc->WorkingDir))
                    {
                        freeConfig(config);
                        return NULL;
                    }
                    dwAttrib = GetFileAttributes(proc->WorkingDir);
                    if (dwAttrib == INVALID_FILE_ATTRIBUTES ||
                        (dwAttrib & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY)
                    {
                        EventWriteConfig_File_Invalid_WorkingDir();
                        freeConfig(config);
                        return NULL;
                    }
                }
            }
            else if (_wcsicmp(key, cmdparmsToken) == 0)
            {
                if (proc != NULL)
                {
                    if (!CopyString(value, &proc->CmdParam))
                    {
                        freeConfig(config);
                        return NULL;
                    }
                }
            }
            else if (_wcsicmp(key, saveoutToken) == 0)
            {
                if (proc != NULL)
                {
                    if (_wcsicmp(value, L"1") == 0)
                    {
                        proc->SaveOutput = TRUE;
                    }
                }
            }
        }
    }

    if (config->ExecutablePath == NULL)
    {
        EventWriteConfig_File_Invalid_ExePath();
        freeConfig(config);
        return NULL;
    }
    if (config->ExecutableName == NULL)
    {
        EventWriteConfig_File_Invalid_ExeName();
        freeConfig(config);
        return NULL;
    }

    CombineFilePath(config->ExecutablePath, config->ExecutableName, &config->ExecutablePath);

    dwAttrib = GetFileAttributes(config->ExecutablePath);
    if (dwAttrib == INVALID_FILE_ATTRIBUTES ||
        (dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
    {
        EventWriteConfig_File_Invalid_ExePath();
        freeConfig(config);
        return NULL;
    }

    return config;
}

//
// Purpose:
//   Read and parse a configuration file
//
// Parameters:
//   file path
//
// Return value:
//   WatcherConfig or NULL
//
WatcherConfig * parseConfig(wchar_t * configPath)
{
    FILE * fp;
    WatcherConfig * config;

    if ((_wfopen_s(&fp, configPath, L"r")) != 0)
    {
        EventWriteConfig_File_Not_Found();
        return NULL;
    }

    config = parseConfigFile(fp);

    fclose(fp);

    return config;
}

