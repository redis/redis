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

#ifndef WIN32_INTEROPA_COMMANDLINE_H
#define WIN32_INTEROPA_COMMANDLINE_H

#include <map>
#include <vector>
#include <string>

using namespace std;

// A map of arguments encountered to the set of parameters for those arguments, in the order in which they
// were encountered. If 'maxmemory' is encountered 3 times, ArgumentMap["maxmemory"] will return a vector 
// of an array of values, with the first being the value od the first 'maxmemory' instance enountered. 
// Order of encounter is command line, conf file, nested conf file #1 (via include statement), ...
typedef map<string, vector<vector<string>>> ArgumentMap;
extern ArgumentMap g_argMap;

void ParseConfFile(string confFile, string cwd, ArgumentMap& argMap);
void ParseCommandLineArguments(int argc, char** argv);
vector<string> GetAccessPaths();

const string cQFork = "qfork";
const string cServiceRun = "service-run";
const string cServiceInstall = "service-install";
const string cServiceUninstall = "service-uninstall"; 
const string cServiceStart = "service-start";
const string cServiceStop = "service-stop";
const string cServiceName = "service-name";
const string cSyslogEnabled = "syslog-enabled";
const string cSyslogIdent= "syslog-ident";
const string cLogfile = "logfile";
const string cInclude = "include";
const string cDir = "dir";
const string cPersistenceAvailable = "persistence-available";
const string cMaxMemory = "maxmemory";
const string cSentinel = "sentinel";

const string cYes = "yes";
const string cNo = "no";
const string cDefaultSyslogIdent = "redis";
const string cDefaultLogfile = "stdout";

/* List of -- command arguments to be passed to redis::main() unaltered */
const vector<string> cRedisArgsForMainC = {"help", "version", "test-memory"};

#endif
