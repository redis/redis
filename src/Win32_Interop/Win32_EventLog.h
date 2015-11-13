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

#pragma once

#ifdef __cplusplus
#include <string>
using namespace std;

typedef class RedisEventLog {
public:
    ~RedisEventLog() {}

    void InstallEventLogSource(string appPath);
    void UninstallEventLogSource();

    void SetEventLogIdentity(const char* identity);

    void LogMessage(LPCSTR msg, const WORD type);
    void LogError(string msg);

    string GetEventLogIdentity();
    void EnableEventLog(bool enabled);
    bool IsEventLogEnabled();

private:
    const string eventLogName = "redis";
    const string cEventLogPath = "SYSTEM\\CurrentControlSet\\Services\\EventLog\\";
    const string cEventLogApplicitonPath = cEventLogPath + "Application\\";
    const string cRedis = "redis";
    const string cEventMessageFile = "EventMessageFile";
    const string cRedisServer = "redis-server";
    const string cTypesSupported = "TypesSupported";
    const string cApplication = "Application";
} RedisEventLog;

extern "C" {
#endif

    void setSyslogEnabled(int enabled);
    void setSyslogIdent(char* identity);
    int IsEventLogEnabled();
    void WriteEventLog(const char* msg);

#ifdef __cplusplus
}
#endif

