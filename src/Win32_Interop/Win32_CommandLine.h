#pragma once

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

void ParseConfFile(string confFile, ArgumentMap& argMap);
void ParseCommandLineArguments(int argc, char** argv);

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

const string cMaxHeap = "maxheap";
const string cMaxMemory = "maxmemory";

const string cYes = "yes";
const string cNo = "no";
const string cDefaultSyslogIdent = "redis";
const string cDefaultLogfile = "stdout";

