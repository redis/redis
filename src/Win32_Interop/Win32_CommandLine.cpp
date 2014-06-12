#include "Win32_CommandLine.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <exception>

ArgumentMap g_argMap;

// Map of argument name to number of parameters for this argument.
typedef map<string, int> argumentCountVector;
static argumentCountVector g_argCountVector = 
{
    { cQFork, 2 },
    { cServiceName, 1 },
    { cServiceRun, 0 },
    { cServiceInstall, 0 },
    { cServiceUninstall, 0 },
    { cServiceStart, 0 },
    { cServiceStop, 0 }
};

string stripQuotes(string s) {
    if (s.at(0) == '\'' &&  s.at(s.length()-1) == '\'') {
        if (s.length() > 2) {
            return s.substr(1, s.length() - 2);
        } else {
            return string("");
        }
    }
    if (s.at(0) == '\"' &&  s.at(s.length()-1) == '\"') {
        if (s.length() > 2) {
            return s.substr(1, s.length() - 2);
        } else {
            return string("");
        }
    }
    return s;
}


void ParseConfFile(string confFile, ArgumentMap& argMap) {
    ifstream config;
    string line;
    string token;
    string value;

#ifdef _DEBUG
    cout << "processing " << confFile << endl;
#endif

    config.open(confFile);
    if (config.fail()) {
        stringstream ss;
        char buffer[MAX_PATH];
        ::GetCurrentDirectoryA(MAX_PATH, buffer);
        ss << "Failed to open the .conf file: " << confFile << " CWD=" << buffer;
        throw runtime_error(ss.str());
    }

    while (!config.eof()) {
        getline(config, line);
        istringstream iss(line);
        if (getline(iss, token, ' ')) {
            if (token.compare("#") == 0) continue;

            if (getline(iss, value, ' ')) {
                if (token.compare("include") == 0) {
                    ParseConfFile(value, argMap);
                } else {
                    transform(token.begin(), token.end(), token.begin(), ::tolower);
                    transform(value.begin(), value.end(), value.begin(), ::tolower);
                    value = stripQuotes(value);
                    vector<string> params;
                    params.push_back(value);
                    argMap[token].push_back(params);
                }
            } else {
                stringstream ss;
                ss << "Failed to get value for argument " << token << " in " << confFile;
                throw runtime_error(ss.str());
            }
        }
    }
}

void ParseCommandLineArguments(int argc, char** argv) {
    if (argc < 2) return;

    bool confFile = (string(argv[1]).substr(0, 2).compare("--") != 0);
    for (int n = (confFile ? 2 : 1); n < argc; n++) {
        string argument = string(argv[n]).substr(2, argument.length() - 2);
        transform(argument.begin(), argument.end(), argument.begin(), ::tolower);

        // All of the redis commandline/conf file arguemnts have one parameter. The Windows
        // port intorduces other arguments with diffreent parameter counts.
        int paramCount = 1;
        if (g_argCountVector.find(argument) != g_argCountVector.end()) {
            paramCount = g_argCountVector[argument];
        }
        vector<string> params;
        for (int pi = 0; pi < paramCount; pi++) {
            n++;
            if (n >= argc) {
                stringstream ss;
                ss << "No parameter value for argument on command line. argument = '" << argument << "'";
                throw runtime_error(ss.str());
            }
            string paramValue = argv[n];
            transform(paramValue.begin(), paramValue.end(), paramValue.begin(), ::tolower);
            paramValue = stripQuotes(paramValue);
            params.push_back(paramValue);
        }
        g_argMap[argument].push_back(params);
    }
    if (confFile) ParseConfFile(argv[1], g_argMap);

#ifdef _DEBUG
    cout << "arguments seen:" << endl;
    for (auto key : g_argMap) {
        cout << key.first << endl;
        bool first = true;
        for (auto params : key.second) {
            cout << "\t";
            bool firstParam = true;
            for (auto param : params) {
                if (firstParam == true) {
                    firstParam = false;
                } else {
                    cout << ", ";
                }
                cout << param;
            }
            cout << endl;
        }
    }
#endif
}