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

#include "win32fixes.h"
#include <mswsock.h>

#include "Win32_variadicFunctor.h"
#include "Win32_CommandLine.h"

// Win32_FDAPI.h includes modified winsock definitions that are useful in BindParam below. It
// also redefines the CRT close(FD) call as a macro. This conflicts with the fstream close 
// definition. #undef solves the warning messages.
#undef close

#include <Shlwapi.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <exception>
#include <functional>
using namespace std;

#pragma comment (lib, "Shlwapi.lib")

ArgumentMap g_argMap;
vector<string> g_pathsAccessed;

string stripQuotes(string s) {
    if (s.length() >= 2) {
        if (s.at(0) == '\'' &&  s.at(s.length() - 1) == '\'') {
            if (s.length() > 2) {
                return s.substr(1, s.length() - 2);
            } else {
                return string("");
            }
        }
        if (s.at(0) == '\"' &&  s.at(s.length() - 1) == '\"') {
            if (s.length() > 2) {
                return s.substr(1, s.length() - 2);
            } else {
                return string("");
            }
        }
    }
    return s;
}

typedef class ParamExtractor {
public:
    ParamExtractor() {}
    virtual ~ParamExtractor() {}
    virtual vector<string> Extract(int argStartIndex, int argc, char** argv) = 0;
    virtual vector<string> Extract(vector<string> tokens, int StartIndex = 0) = 0;
} ParamExtractor;

typedef map<string, ParamExtractor*> RedisParamterMapper;

typedef class FixedParam : public ParamExtractor {
private:
    int parameterCount;

public:
    FixedParam(int count) {
        parameterCount = count;
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        if (argStartIndex + parameterCount >= argc) {
            stringstream err;
            err << "Not enough parameters available for " << argv[argStartIndex];
            throw invalid_argument(err.str());
        }
        vector<string> params;
        for (int argIndex = argStartIndex + 1; argIndex < argStartIndex + 1 + parameterCount; argIndex++) {
            string param = string(argv[argIndex]);
            transform(param.begin(), param.end(), param.begin(), ::tolower);
            param = stripQuotes(param);
            params.push_back(param);
        }
        return params;
    }

    vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        if ((int)(tokens.size() - 1) < parameterCount + startIndex) {
            stringstream err;
            err << "Not enough parameters available for " << tokens.at(0);
            throw invalid_argument(err.str());
        }
        vector<string> params;
        int skipCount = 1 + startIndex;
        for (string token : tokens) {
            if (skipCount > 0) {
                skipCount--;
                continue;
            }
            string param = string(token);
            transform(param.begin(), param.end(), param.begin(), ::tolower);
            param = stripQuotes(param);
            params.push_back(param);
        }
        return params;
    };
} FixedParam;

static FixedParam fp0 = FixedParam(0);
static FixedParam fp1 = FixedParam(1);
static FixedParam fp2 = FixedParam(2);
static FixedParam fp3 = FixedParam(3);
static FixedParam fp4 = FixedParam(4);

typedef class SaveParams : public ParamExtractor {
public:
    SaveParams() {}

    bool isStringAnInt(string test)  {
        int x;
        char c;
        istringstream s(test);
 
        if (!(s >> x) ||            // not convertable to an int
            (s >> c)) {             // some character past the int
            return false;
        } else {
            return true;
        }
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        vector<string> params;
        int argIndex = argStartIndex + 1;

        // save [seconds] [changes]
        // or 
        // save ""      -- turns off RDB persistence
        if (strcmp(argv[argIndex], "\"\"") == 0 || strcmp(argv[argIndex], "''") == 0 || strcmp(argv[argIndex], "") == 0) {
            params.push_back(argv[argIndex]);
        } else if (
            isStringAnInt(argv[argIndex]) && 
            isStringAnInt(argv[argIndex+1])) {
            params.push_back(argv[argIndex]);
            params.push_back(argv[argIndex+1]);
        } else {
            stringstream err;
            err << "Not enough parameters available for " << argv[argStartIndex];
            throw invalid_argument(err.str());
        }
        return params;
    }

    virtual vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        vector<string> params;
        unsigned int parameterIndex = 1 + startIndex;
        if ((tokens.size() > parameterIndex) &&
            (tokens.at(parameterIndex) == string("\"\"") ||
            tokens.at(parameterIndex) == string("''"))) {
            params.push_back(tokens.at(parameterIndex));
        } else if ((tokens.size() > parameterIndex + 1) &&
                   isStringAnInt(tokens.at(parameterIndex)) &&
                   isStringAnInt(tokens.at(parameterIndex + 1))) {
            params.push_back(tokens.at(parameterIndex));
            params.push_back(tokens.at(parameterIndex + 1));
        } else {
            stringstream err;
            err << "Not enough parameters available for " << tokens.at(startIndex);
            throw invalid_argument(err.str());
        }
        return params;
    };

} SaveParams;

static SaveParams savep = SaveParams();

typedef class BindParams : public ParamExtractor {
public:
    BindParams() {}

    dllfunctor_stdcall<int, LPCSTR, INT, LPWSAPROTOCOL_INFO, LPSOCKADDR, LPINT> f_WSAStringToAddressA =
        dllfunctor_stdcall<int, LPCSTR, INT, LPWSAPROTOCOL_INFO, LPSOCKADDR, LPINT>("ws2_32.dll", "WSAStringToAddressA");

    bool IsIPAddress(string address) {
        SOCKADDR_IN sockaddr4;
        sockaddr4.sin_family = AF_INET;
        SOCKADDR_IN6 sockaddr6;
        sockaddr6.sin6_family = AF_INET6;
        int addr4Length = sizeof(SOCKADDR_IN);
        int addr6Length = sizeof(SOCKADDR_IN6);
        DWORD err;
        if (ERROR_SUCCESS ==
            (err = f_WSAStringToAddressA(
            address.c_str(),
            AF_INET,
            NULL,
            (LPSOCKADDR)&sockaddr4,
            &addr4Length))) {
            return true;
        } else if (ERROR_SUCCESS ==
            (err = f_WSAStringToAddressA(
            address.c_str(),
            AF_INET6,
            NULL,
            (LPSOCKADDR)&sockaddr6,
            &addr6Length))) {
            return true;
        } else {
            return false;
        }
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        vector<string> params;
        int argIndex = argStartIndex + 1;

        // bind [address1] [address2] ...
        while (argIndex < argc) {
            if (IsIPAddress(argv[argIndex])) {
                string param = string(argv[argIndex]);
                transform(param.begin(), param.end(), param.begin(), ::tolower);
                param = stripQuotes(param);
                params.push_back(param);
                argIndex++;
            } else {
                break;
            }
        }
        return params;
    }

    virtual vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        vector<string> params;
        int skipCount = 1 + startIndex;
        for (string token : tokens) {
            if (skipCount > 0) {
                skipCount--;
                continue;
            }
            if (IsIPAddress(token)) {
                string param = string(token);
                transform(param.begin(), param.end(), param.begin(), ::tolower);
                param = stripQuotes(param);
                params.push_back(param);
            } else {
                break;
            }
        }
        return params;
    };

} BindParams;

static BindParams bp = BindParams();

typedef class SentinelParams : public  ParamExtractor {
private:
    RedisParamterMapper subCommands;

public:
    SentinelParams() {
        subCommands = RedisParamterMapper
        {
            { "monitor",                    &fp4 },    // sentinel monitor [master name] [ip] [port] [quorum]
            { "auth-pass",                  &fp2 },    // sentinel auth-pass [master name] [password]
            { "down-after-milliseconds",    &fp2 },    // sentinel down-after-milliseconds [master name] [milliseconds]
            { "parallel-syncs",             &fp2 },    // sentinel parallel-syncs [master name] [number]
            { "failover-timeout",           &fp2 },    // sentinel failover-timeout [master name] [number]
            { "notification-script",        &fp2 },    // sentinel notification-script [master name] [scriptPath]
            { "client-reconfig-script",     &fp2 },    // sentinel client-reconfig-script [master name] [scriptPath]
            { "config-epoch",               &fp2 },    // sentinel config-epoch [name] [epoch]
            { "current-epoch",              &fp1 },    // sentinel current-epoch <epoch>
            { "leader-epoch",               &fp2 },    // sentinel leader-epoch [name] [epoch]
            { "known-slave",                &fp3 },    // sentinel known-slave <name> <ip> <port>
            { "known-sentinel",             &fp4 },    // sentinel known-sentinel <name> <ip> <port> [runid]
            { "announce-ip",                &fp1 },    // sentinel announce-ip <ip>
            { "announce-port",              &fp1 }     // sentinel announce-port <port>
        };
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        stringstream err;
        if (argStartIndex + 1 >= argc) {
            err << "Not enough parameters available for " << argv[argStartIndex];
            throw invalid_argument(err.str());
        }
        if (subCommands.find(argv[argStartIndex + 1]) == subCommands.end()) {
            err << "Could not find sentinal subcommand " << argv[argStartIndex + 1];
            throw invalid_argument(err.str());
        }

        vector<string> params;
        params.push_back(argv[argStartIndex + 1]);
        vector<string> subParams = subCommands[argv[argStartIndex + 1]]->Extract(argStartIndex + 1, argc, argv);
		for (string p : subParams) {
            transform(p.begin(), p.end(), p.begin(), ::tolower);
            p = stripQuotes(p);
            params.push_back(p);
        }
        return params;
    }

    vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        stringstream err;
        if (tokens.size() < 2) {
            err << "Not enough parameters available for " << tokens.at(0);
            throw invalid_argument(err.str());
        }
        string subcommand = tokens.at(startIndex + 1);
        if (subCommands.find(subcommand) == subCommands.end()) {
            err << "Could not find sentinal subcommand " << subcommand;
            throw invalid_argument(err.str());
        }

        vector<string> params;
        params.push_back(subcommand);

        vector<string> subParams = subCommands[subcommand]->Extract(tokens, startIndex + 1);

        for (string p : subParams) {
            transform(p.begin(), p.end(), p.begin(), ::tolower);
            p = stripQuotes(p);
            params.push_back(p);
        }
        return params;
    };

} SentinelParams;

static SentinelParams sp = SentinelParams();

// Map of argument name to argument processing engine.
static RedisParamterMapper g_redisArgMap =
{
    // QFork flags
    { cQFork,                           &fp2 },    // qfork [QForkControlMemoryMap handle] [parent process id]
    { cPersistenceAvailable,            &fp1 },    // persistence-available [yes/no]

    // service commands
    { cServiceName,                     &fp1 },    // service-name [name]
    { cServiceRun,                      &fp0 },    // service-run
    { cServiceInstall,                  &fp0 },    // service-install
    { cServiceUninstall,                &fp0 },    // service-uninstall
    { cServiceStart,                    &fp0 },    // service-start
    { cServiceStop,                     &fp0 },    // service-stop

    // redis commands
    { "daemonize",                      &fp1 },    // daemonize [yes/no]
    { "pidfile",                        &fp1 },    // pidfile [file]
    { "port",                           &fp1 },    // port [port number]
    { "tcp-backlog",                    &fp1 },    // tcp-backlog [number]
    { "bind",                           &bp },     // bind [address] [address] ...
    { "unixsocket",                     &fp1 },    // unixsocket [path] 
    { "timeout",                        &fp1 },    // timeout [value] 
    { "tcp-keepalive",                  &fp1 },    // tcp-keepalive [value]
    { "loglevel",                       &fp1 },    // lovlevel [value]
    { "logfile",                        &fp1 },    // logfile [file]
    { "syslog-enabled",                 &fp1 },    // syslog-enabled [yes/no]
    { "syslog-ident",                   &fp1 },    // syslog-ident [string]
    { "syslog-facility",                &fp1 },    // syslog-facility [string]
    { "databases",                      &fp1 },    // databases [number]
    { "save",                           &savep },  // save [seconds] [changes] or save ""
    { "stop-writes-on-bgsave-error",    &fp1 },    // stop-writes-on-bgsave-error [yes/no] 
    { "rdbcompression",                 &fp1 },    // rdbcompression [yes/no]
    { "rdbchecksum",                    &fp1 },    // rdbchecksum [yes/no]
    { "dbfilename",                     &fp1 },    // dbfilename [filename]
    { cDir,                             &fp1 },    // dir [path]
    { "slaveof",                        &fp2 },    // slaveof [masterip] [master port] 
    { "masterauth",                     &fp1 },    // masterauth [master-password]
    { "slave-serve-stale-data",         &fp1 },    // slave-serve-stale-data [yes/no]
    { "slave-read-only",                &fp1 },    // slave-read-only [yes/no]
    { "repl-ping-slave-period",         &fp1 },    // repl-ping-slave-period [number]
    { "repl-timeout",                   &fp1 },    // repl-timeout [number]
    { "repl-disable-tcp-nodelay",       &fp1 },    // repl-disable-tcp-nodelay [yes/no]
    { "repl-diskless-sync",             &fp1 },    // repl-diskless-sync [yes/no]
    { "repl-diskless-sync-delay",       &fp1 },    // repl-diskless-sync-delay [number]
    { "repl-backlog-size",              &fp1 },    // repl-backlog-size [number]
    { "repl-backlog-ttl",               &fp1 },    // repl-backlog-ttl [number]
    { "slave-priority",                 &fp1 },    // slave-priority [number]
    { "min-slaves-to-write",            &fp1 },    // min-slaves-to-write [number]
    { "min-slaves-max-lag",             &fp1 },    // min-slaves-max-lag [number]
    { "requirepass",                    &fp1 },    // requirepass [string]
    { "rename-command",                 &fp2 },    // rename-command [command] [string]
    { "maxclients",                     &fp1 },    // maxclients [number]
    { "maxmemory",                      &fp1 },    // maxmemory [bytes]
    { "maxmemory-policy",               &fp1 },    // maxmemory-policy [policy]
    { "maxmemory-samples",              &fp1 },    // maxmemory-samples [number]
    { "appendonly",                     &fp1 },    // appendonly [yes/no]
    { "appendfilename",                 &fp1 },    // appendfilename [filename]
    { "appendfsync",                    &fp1 },    // appendfsync [value]
    { "no-appendfsync-on-rewrite",      &fp1 },    // no-appendfsync-on-rewrite [value]
    { "auto-aof-rewrite-percentage",    &fp1 },    // auto-aof-rewrite-percentage [number]
    { "auto-aof-rewrite-min-size",      &fp1 },    // auto-aof-rewrite-min-size [number]
    { "lua-time-limit",                 &fp1 },    // lua-time-limit [number]
    { "slowlog-log-slower-than",        &fp1 },    // slowlog-log-slower-than [number]
    { "slowlog-max-len",                &fp1 },    // slowlog-max-len [number]
    { "notify-keyspace-events",         &fp1 },    // notify-keyspace-events [string]
    { "hash-max-ziplist-entries",       &fp1 },    // hash-max-ziplist-entries [number]
    { "hash-max-ziplist-value",         &fp1 },    // hash-max-ziplist-value [number]
    { "list-max-ziplist-entries",       &fp1 },    // list-max-ziplist-entries [number]
    { "list-max-ziplist-value",         &fp1 },    // list-max-ziplist-value [number]
    { "set-max-intset-entries",         &fp1 },    // set-max-intset-entries [number]
    { "zset-max-ziplist-entries",       &fp1 },    // zset-max-ziplist-entries [number]
    { "zset-max-ziplist-value",         &fp1 },    // zset-max-ziplist-value [number]
    { "hll-sparse-max-bytes",           &fp1 },    // hll-sparse-max-bytes [number]
    { "activerehashing",                &fp1 },    // activerehashing [yes/no]
    { "client-output-buffer-limit",     &fp4 },    // client-output-buffer-limit [class] [hard limit] [soft limit] [soft seconds]
    { "hz",                             &fp1 },    // hz [number]
    { "aof-rewrite-incremental-fsync",  &fp1 },    // aof-rewrite-incremental-fsync [yes/no]
    { "aof-load-truncated",             &fp1 },    // aof-load-truncated [yes/no]
    { "latency-monitor-threshold",      &fp1 },    // latency-monitor-threshold [number]
    { cInclude,                         &fp1 },    // include [path]

    // sentinel commands
    { "sentinel",                       &sp },

    // cluster commands
    {"cluster-enabled",                 &fp1},     // [yes/no]
    {"cluster-config-file",             &fp1},     // [filename]
    {"cluster-node-timeout",            &fp1},     // [number]
    {"cluster-slave-validity-factor",   &fp1},     // [number]
    {"cluster-migration-barrier",       &fp1},     // [1/0]
    {"cluster-require-full-coverage",   &fp1}      // [yes/no]
};

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty())
            elems.push_back(item);
    }
    return elems;
}

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, elems);
    return elems;
}

vector<string> Tokenize(string line)  {
    vector<string> tokens;
    stringstream token;

    // no need to parse empty lines, or comment lines (which may have unbalanced quotes)
    if ((line.length() == 0)  || 
        ((line.length() != 0) && (*line.begin()) == '#')) {
        return tokens;
    }

    for (string::const_iterator sit = line.begin(); sit != line.end(); sit++) {
        char c = *(sit);
        if (isspace(c) && token.str().length() > 0) {
            tokens.push_back(token.str());
            token.str("");
        } else if (c == '\'' || c == '\"') {
            char endQuote = c;
            string::const_iterator endQuoteIt = sit;
            while (++endQuoteIt != line.end()) {
                if (*endQuoteIt == endQuote) break;
            }
            if (endQuoteIt != line.end())  {
                while (++sit != endQuoteIt) {
                    token << (*sit);
                }

                // The code above strips quotes. In certain cases (save "") the quotes should be preserved around empty strings
                if (token.str().length() == 0)
                    token << endQuote << endQuote;

                // correct paths for windows nomenclature
                string path = token.str();
                replace(path.begin(), path.end(), '/', '\\');
                tokens.push_back(path);

                token.str("");
            } else {
                // stuff the imbalanced quote character and continue
                token << (*sit);
            }
        } else {
            token << c;
        }
    }
    if (token.str().length() > 0) {
        tokens.push_back(token.str());
    }

    return tokens;
}

void ParseConfFile(string confFile, string cwd, ArgumentMap& argMap) {
    ifstream config;
    string line;

    char fullConfFilePath[MAX_PATH];
    if (PathIsRelativeA(confFile.c_str())) {
        if (NULL == PathCombineA(fullConfFilePath, cwd.c_str(), confFile.c_str())) {
            throw std::system_error(GetLastError(), system_category(), "PathCombineA failed");
        }
    } else {
        strcpy(fullConfFilePath, confFile.c_str());
    }

    config.open(fullConfFilePath);
    if (config.fail()) {
        stringstream ss;
        ss << "Failed to open the .conf file: " << confFile << " CWD=" << cwd.c_str();
        throw invalid_argument(ss.str());
    } else  {
        char confFileDir[MAX_PATH];
        strcpy(confFileDir, fullConfFilePath);
        if (FALSE == PathRemoveFileSpecA(confFileDir)) {
            throw std::system_error(GetLastError(), system_category(), "PathRemoveFileSpecA failed");
        }
        g_pathsAccessed.push_back(confFileDir);
    }

    while (!config.eof()) {
        getline(config, line);
        vector<string> tokens = Tokenize(line);
        if (tokens.size() > 0) {
            string parameter = tokens.at(0);
            if (parameter.at(0) == '#') {
                continue;
            } else if (parameter.compare(cInclude) == 0) {
                ParseConfFile(tokens.at(1), cwd, argMap);
            } else if (g_redisArgMap.find(parameter) == g_redisArgMap.end()) {
                stringstream err;
                err << "unknown conf file parameter : " + parameter;
                throw invalid_argument(err.str());
            }

            vector<string> params = g_redisArgMap[parameter]->Extract(tokens);
            g_argMap[parameter].push_back(params);
        }
    }
}

vector<string> incompatibleNoPersistenceCommands{
    "min_slaves_towrite",
    "min_slaves_max_lag",
    "appendonly",
    "appendfilename",
    "appendfsync",
    "no_append_fsync_on_rewrite",
    "auto_aof_rewrite_percentage",
    "auto_aof_rewrite_on_size",
    "aof_rewrite_incremental_fsync",
    "save"
};

void ValidateCommandlineCombinations() {
    if (g_argMap.find(cPersistenceAvailable) != g_argMap.end()) {
        if (g_argMap[cPersistenceAvailable].at(0).at(0) == cNo) {
            string incompatibleCommand = "";
            for (auto command : incompatibleNoPersistenceCommands) {
                if (g_argMap.find(command) != g_argMap.end()) {
                    incompatibleCommand = command;
                    break;
                }
            }
            if (incompatibleCommand.length() > 0) {
                stringstream ss;
                ss << "'" << cPersistenceAvailable << " " << cNo << "' command not compatible with '" << incompatibleCommand << "'. Exiting.";
                throw std::invalid_argument(ss.str().c_str());
            }
        }
    }
}

void ParseCommandLineArguments(int argc, char** argv) {
    if (argc < 2) {
        return;
    }

    bool confFile = false;
    string confFilePath;
    for (int n = (confFile ? 2 : 1); n < argc; n++) {
        if (string(argv[n]).substr(0, 2) == "--") {
            string argument = string(argv[n]).substr(2, argument.length() - 2);
            transform(argument.begin(), argument.end(), argument.begin(), ::tolower);

            // Some -- arguments are passed directly to redis.c::main()
            if (find(cRedisArgsForMainC.begin(), cRedisArgsForMainC.end(), argument) != cRedisArgsForMainC.end()) {
                if (strcasecmp(argument.c_str(), "test-memory") == 0) {
                    // The test-memory argument is followed by a integer value
                    n++;
                }
            } else {
                // -- arguments processed before calling redis.c::main()
                if (g_redisArgMap.find(argument) == g_redisArgMap.end()) {
                    stringstream err;
                    err << "unknown argument: " << argument;
                    throw invalid_argument(err.str());
                }

                vector<string> params;
                if (argument == cSentinel) {
                    try {
                        vector<string> sentinelSubCommands = g_redisArgMap[argument]->Extract(n, argc, argv);
                        for (auto p : sentinelSubCommands) {
                            params.push_back(p);
                        }
                    }
                    catch (invalid_argument iaerr) {
                        // if no subcommands could be mapped, then assume this is the parameterless --sentinel command line only argument
                    }
                } else if (argument == cServiceRun) {
                    // When the service starts the current directory is %systemdir%. This needs to be changed to the 
                    // directory the executable is in so that the .conf file can be loaded.
                    char szFilePath[MAX_PATH];
                    if (GetModuleFileNameA(NULL, szFilePath, MAX_PATH) == 0) {
                        throw std::system_error(GetLastError(), system_category(), "ParseCommandLineArguments: GetModuleFileName failed");
                    }
                    string currentDir = szFilePath;
                    auto pos = currentDir.rfind("\\");
                    currentDir.erase(pos);

                    if (FALSE == SetCurrentDirectoryA(currentDir.c_str())) {
                        throw std::system_error(GetLastError(), system_category(), "SetCurrentDirectory failed");
                    }
                } else {
                    params = g_redisArgMap[argument]->Extract(n, argc, argv);
                }
                g_argMap[argument].push_back(params);
                n += (int) params.size();
            }
        } else if (string(argv[n]).substr(0, 1) == "-") {
            // Do nothing, the - arguments are passed to redis.c::main() as they are
        } else {
            confFile = true;
            confFilePath = argv[n];
        }
    }

    char cwd[MAX_PATH];
    if (0 == ::GetCurrentDirectoryA(MAX_PATH, cwd)) {
        throw std::system_error(GetLastError(), system_category(), "ParseCommandLineArguments: GetCurrentDirectoryA failed");
    }
    
    if (confFile) {
        ParseConfFile(confFilePath, cwd, g_argMap);
    }

    // grab directory where RDB/AOF files will be created so that service install can add access allowed ACE to path
    string fileCreationDirectory = ".\\";
    if (g_argMap.find(cDir) != g_argMap.end()) {
        fileCreationDirectory = g_argMap[cDir][0][0];
        replace(fileCreationDirectory.begin(), fileCreationDirectory.end(), '/', '\\');
    }
    if (PathIsRelativeA(fileCreationDirectory.c_str())) {
        char fullPath[MAX_PATH];
        if (NULL == PathCombineA(fullPath, cwd, fileCreationDirectory.c_str())) {
            throw std::system_error(GetLastError(), system_category(), "PathCombineA failed");
        }
        fileCreationDirectory = fullPath;
    }
    g_pathsAccessed.push_back(fileCreationDirectory);

    ValidateCommandlineCombinations();
}

vector<string> GetAccessPaths() {
    return g_pathsAccessed;
}

