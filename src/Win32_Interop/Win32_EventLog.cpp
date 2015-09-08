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

#include <Windows.h>
#include <string>
#include <iostream>
#include <sstream>
using namespace std;

#include "Win32_EventLog.h"
#include "Win32_SmartHandle.h"
#include "EventLog.h"

static bool eventLogEnabled = true;
static string eventLogIdentity = "redis";

void RedisEventLog::SetEventLogIdentity(const char* identity) {
    eventLogIdentity = string(identity);
}

void RedisEventLog::UninstallEventLogSource() {
    SmartRegistryHandle appKey;
    if (ERROR_SUCCESS == RegOpenKeyA(HKEY_LOCAL_MACHINE, cEventLogApplicitonPath.c_str(), appKey)) {
        SmartRegistryHandle eventLogNameKey;
        if (ERROR_SUCCESS == RegOpenKeyA(appKey, eventLogName.c_str(), eventLogNameKey)) {
            if (ERROR_SUCCESS != RegDeleteKeyA(appKey, eventLogName.c_str())) {
                throw std::system_error(GetLastError(), system_category(), "RegDeleteKeyA failed");
            }
        }
    }

    SmartRegistryHandle eventLogKey;
    if (ERROR_SUCCESS == RegOpenKeyA(HKEY_LOCAL_MACHINE, cEventLogPath.c_str(), eventLogKey)) {
        SmartRegistryHandle eventServiceKey;
        if (ERROR_SUCCESS == RegOpenKeyA(eventLogKey, cRedis.c_str(), eventServiceKey)) {
            SmartRegistryHandle eventServiceSubKey;
            if (ERROR_SUCCESS == RegOpenKeyA(eventServiceKey, cRedisServer.c_str(), eventServiceSubKey)) {
                if (ERROR_SUCCESS != RegDeleteKeyA(eventServiceKey, cRedisServer.c_str())) {
                    throw std::system_error(GetLastError(), system_category(), "RegDeleteKeyA failed");
                }
                if (ERROR_SUCCESS != RegDeleteKeyA(eventLogKey, cRedis.c_str())) {
                    throw std::system_error(GetLastError(), system_category(), "RegDeleteKeyA failed");
                }
            }
        }
    }
}

// sets up the registry keys required for the EventViewer message filter
void RedisEventLog::InstallEventLogSource(string appPath) {
    SmartRegistryHandle eventLogKey;
    if (ERROR_SUCCESS != RegOpenKeyA(HKEY_LOCAL_MACHINE, cEventLogPath.c_str(), eventLogKey)) {
        throw std::system_error(GetLastError(), system_category(), "RegOpenKey failed");
    }
    SmartRegistryHandle redis1;
    if (ERROR_SUCCESS != RegOpenKeyA(eventLogKey, cRedis.c_str(), redis1)) {
        if (ERROR_SUCCESS != RegCreateKeyA(eventLogKey, cRedis.c_str(), redis1)) {
            throw std::system_error(GetLastError(), system_category(), "RegCreateKeyA failed");
        }
    }
    SmartRegistryHandle redisserver;
    if (ERROR_SUCCESS != RegOpenKeyA(redis1, cRedisServer.c_str(), redisserver)) {
        if (ERROR_SUCCESS != RegCreateKeyA(redis1, cRedisServer.c_str(), redisserver)) {
            throw std::system_error(GetLastError(), system_category(), "RegCreateKeyA failed");
        }
    }
    DWORD value = 0;
    DWORD type = REG_DWORD;
    DWORD size = sizeof(DWORD);
    if (ERROR_SUCCESS != RegQueryValueExA(redisserver, cTypesSupported.c_str(), 0, &type, NULL, &size)) {
        if (ERROR_SUCCESS != RegSetValueExA(redisserver, cTypesSupported.c_str(), 0, REG_DWORD, (const BYTE*) &value, sizeof(DWORD))) {
            throw std::system_error(GetLastError(), system_category(), "RegSetValueExA failed");
        }
    }
    type = REG_SZ;
    size = 0;
    if (ERROR_SUCCESS != RegQueryValueExA(redisserver, cEventMessageFile.c_str(), 0, &type, NULL, &size)) {
        if (ERROR_SUCCESS != RegSetValueExA(redisserver, cEventMessageFile.c_str(), 0, REG_SZ, (BYTE*) appPath.c_str(), (DWORD) appPath.length())) {
            throw std::system_error(GetLastError(), system_category(), "RegSetValueExA failed");
        }
    }

    SmartRegistryHandle application;
    if (ERROR_SUCCESS != RegOpenKeyA(eventLogKey, cApplication.c_str(), application)) {
        throw std::system_error(GetLastError(), system_category(), "RegCreateKeyA failed");
    }
    SmartRegistryHandle redis2;
    if (ERROR_SUCCESS != RegOpenKeyA(application, cRedis.c_str(), redis2)) {
        if (ERROR_SUCCESS != RegCreateKeyA(application, cRedis.c_str(), redis2)) {
            throw std::system_error(GetLastError(), system_category(), "RegCreateKeyA failed");
        }
    }
    type = REG_DWORD;
    size = 0;
    if (ERROR_SUCCESS != RegQueryValueExA(redis2, cTypesSupported.c_str(), 0, &type, NULL, &size)) {
        if (ERROR_SUCCESS != RegSetValueExA(redis2, cTypesSupported.c_str(), 0, REG_DWORD, (const BYTE*) &value, sizeof(DWORD))) {
            throw std::system_error(GetLastError(), system_category(), "RegSetValueExA failed");
        }
    }
    if (ERROR_SUCCESS != RegQueryValueExA(redis2, cEventMessageFile.c_str(), 0, &type, NULL, &size)) {
        if (ERROR_SUCCESS != RegSetValueExA(redis2, cEventMessageFile.c_str(), 0, REG_SZ, (BYTE*) appPath.c_str(), (DWORD) appPath.length())) {
            throw std::system_error(GetLastError(), system_category(), "RegSetValueExA failed");
        }
    }
}

void RedisEventLog::LogMessage(LPCSTR msg, const WORD type) {
    DWORD eventID;
    switch (type) {
        case EVENTLOG_ERROR_TYPE:
            eventID = MSG_ERROR_1;
            break;
        case EVENTLOG_WARNING_TYPE:
            eventID = MSG_WARNING_1;
            break;
        case EVENTLOG_INFORMATION_TYPE:
            eventID = MSG_INFO_1;
            break;
        default:
            std::cerr << "Unrecognized type: " << type << "\n";
            eventID = MSG_INFO_1;
            break;
    }

    HANDLE hEventLog = RegisterEventSourceA(0, this->eventLogName.c_str());

    if (0 == hEventLog) {
        std::cerr << "Failed open source '" << this->eventLogName << "': " << GetLastError() << endl;
    } else {
        if (FALSE == ReportEventA(hEventLog, type, 0, eventID, 0, 1, 0, &msg, 0)) {
            std::cerr << "Failed to write message: " << GetLastError() << endl;
        }

        DeregisterEventSource(hEventLog);
    }
}

void RedisEventLog::LogError(string msg) {
    try {
        if (eventLogEnabled == true) {
            stringstream ss;
            ss << "syslog-ident = " << eventLogIdentity << endl;
            ss << msg;
            RedisEventLog().LogMessage(ss.str().c_str(), EVENTLOG_ERROR_TYPE);
        }
    }
    catch (...) {
    }
}

string RedisEventLog::GetEventLogIdentity() {
    return eventLogIdentity;
}

void RedisEventLog::EnableEventLog(bool enabled) {
    eventLogEnabled = enabled;
}

bool RedisEventLog::IsEventLogEnabled() {
    return eventLogEnabled;
}

extern "C" void setSyslogEnabled(int enabled) {
    try {
        if (enabled == 1) {
            RedisEventLog().EnableEventLog(true);
        } else {
            RedisEventLog().EnableEventLog(false);
        }
    }
    catch (...) {}
}

extern "C" void setSyslogIdent(char* identity) {
    try {
        RedisEventLog().SetEventLogIdentity(identity);
    }
    catch (...) {}
}

extern "C" void WriteEventLog(const char* msg) {
    try {
        stringstream ss;
        ss << "syslog-ident = " << RedisEventLog().GetEventLogIdentity() << endl;
        ss << msg;
        RedisEventLog().LogMessage(ss.str().c_str(), EVENTLOG_INFORMATION_TYPE);
    }
    catch (...) {}
}

extern "C" int IsEventLogEnabled() {
    try {
        if (RedisEventLog().IsEventLogEnabled() == true) {
            return 1;
        }
    }
    catch (...) {}
    return 0;
}
