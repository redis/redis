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

/*
This code implements the following new command line arguments for redis:

--service-install [additional command line arguments to pass to redis when launched as a service]

This must be the first argument on the redis-server command line. Arguments after this are passed in the order they occur to redis when the
service is launched. The service will be configured as Autostart and will be launched as "NT AUTHORITY\NetworkService". Upon successful
installation a success message will be displayed and redis will exit. For instance:

redis-server --service-install redis.conf --loglevel verbose

This command does not start the service.

--service-uninstall

This will remove the redis service configuration information from the registry. Upon successful uninstallation a success message will be
displayed and redis will exit.

This does command not stop the service.

--service-start

This will start the redis service. Upon successful startup a success message will be displayed and redis will exit.

--service-stop

This will stop the redis service. Upon successful termination a success message will be displayed and redis will exit.

The [--service-name name] arguments, modifies the preceding commands to target a specific service name. If present, 
this should preceed the other arguments passed to redis. For instance:

    redis-server --service-install --service-name testServiceName redis.windows.conf --loglevel verbose 
*/

#include "win32_types.h"

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <tchar.h>
#include <strsafe.h>
#include <aclapi.h>
#include "Win32_EventLog.h"
#include <algorithm>
#include <string>
#include <sstream>
#include <vector>
#include <iostream>
#include "Win32_RedisLog.h"
#include "Win32_CommandLine.h"
using namespace std;

#include "Win32_SmartHandle.h"

#pragma comment(lib, "advapi32.lib")

#define DEFAULT_SERVICE_NAME "Redis"  
#define MAX_SERVICE_NAME_LENGTH 256
char g_serviceName[MAX_SERVICE_NAME_LENGTH + 1] = DEFAULT_SERVICE_NAME;

SERVICE_STATUS g_ServiceStatus = { 0 };
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;
HANDLE g_ServiceStoppedEvent = INVALID_HANDLE_VALUE;
vector<string> serviceRunArguments;
SERVICE_STATUS_HANDLE g_StatusHandle;
const ULONGLONG cThirtySeconds = 30 * 1000;
BOOL g_isRunningAsService = FALSE;
const int cPreshutdownInterval = 180000;
const char* cServiceInstallPipeName = "\\\\.\\pipe\\redis-service-install";

extern "C" int main(int argc, char** argv);

typedef class ServicePipeWriter {
public:
    static ServicePipeWriter& getInstance() {
        static ServicePipeWriter    instance;
        return instance;
    }

private:
    HANDLE pipe = INVALID_HANDLE_VALUE;
    ServicePipeWriter() {
        pipe = CreateFileA(cServiceInstallPipeName, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
    }
    ServicePipeWriter(ServicePipeWriter const&);
    void operator=(ServicePipeWriter const&);
    ~ServicePipeWriter() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }

public:
    void Write(string message) {
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD bytesWritten = 0;
            WriteFile(pipe, message.c_str(), (DWORD)message.length(), &bytesWritten, NULL);
        } else {
            ::redisLog(REDIS_WARNING, message.c_str());
        }
    }
} ServicePipeWriter;

BOOL RelaunchAsElevatedProcess(int argc, char** argv) {
    // create pipe for launched process to communicate back on
    SmartHandle pipe =
        CreateNamedPipeA(
        cServiceInstallPipeName, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE, 1, 0, 0, PIPE_NOWAIT, NULL);

    stringstream  paramString;
    bool first = true;
    for (int n = 1; n < argc; n++) {
        if (first) {
            first = false;
        } else {
            paramString << " ";
        }
        string arg = argv[n];
        if (arg.find(' ') != string::npos)  {
            paramString << "\"" << arg << "\"";
        } else {
            paramString << arg;
        }
    }
    CHAR params[32768];
    memset(params, 0, 32768);
    memcpy(params, paramString.str().c_str(), paramString.str().length());

    // Launch itself as administrator.
    SHELLEXECUTEINFOA sei = { 0 };
    sei.cbSize = sizeof(SHELLEXECUTEINFOA);
    sei.lpVerb = "runas";
    sei.lpFile = _pgmptr;
    sei.lpParameters = params;
    sei.hwnd = 0;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpDirectory = 0;
    sei.hInstApp = 0;

    if (ShellExecuteExA(&sei)) {
        if (sei.hProcess != NULL) {
            const int messageBufferSize = 10000;
            char buffer[messageBufferSize + 1];
            DWORD bytesRead;
            while (WaitForSingleObject(sei.hProcess, 0) != WAIT_OBJECT_0) {
                DWORD result = ReadFile(pipe, buffer, messageBufferSize, &bytesRead, NULL);
                if (result != 0 && bytesRead > 0) {
                    buffer[bytesRead] = '\0';    // ensure received message is null terminated;
                    ::redisLog(REDIS_WARNING, (const char*)buffer);
                }
            }
            CloseHandle(sei.hProcess);
        }
        return TRUE;
    } else {
        throw std::system_error(GetLastError(), system_category(), "ShellExecuteExA failed");
    }
}

bool IsProcessElevated() {
    DWORD dwError = ERROR_SUCCESS;
    SmartHandle shToken;

    // Open the primary access token of the process with TOKEN_QUERY.
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, shToken)) {
        throw std::system_error(GetLastError(), system_category(), "OpenProcessTokenFailed failed");
    }

    // Retrieve token elevation information.
    TOKEN_ELEVATION elevation;
    DWORD dwSize;
    if (!GetTokenInformation(shToken, TokenElevation, &elevation,
        sizeof(elevation), &dwSize)) {
        throw std::system_error(GetLastError(), system_category(), "OpenProcessTokenFailed failed");
    }

    return  (elevation.TokenIsElevated != 0);
}

VOID InitializeServiceName() {
    if (g_argMap.find(cServiceName) != g_argMap.end()) {
        if (g_argMap[cServiceName].at(0).at(0).length() > MAX_SERVICE_NAME_LENGTH) {
            throw std::runtime_error("Service name too long.");
        }
        strcpy_s(g_serviceName, MAX_SERVICE_NAME_LENGTH, g_argMap[cServiceName].at(0).at(0).c_str());
    }
}

DWORD AddAceToObjectsSecurityDescriptor(
    LPSTR pszObjName,          
    SE_OBJECT_TYPE ObjectType,  
    LPSTR pszTrustee,          
    TRUSTEE_FORM TrusteeForm,   
    DWORD dwAccessRights,       
    ACCESS_MODE AccessMode,     
    DWORD dwInheritance         
    ) {
    DWORD dwRes = 0;
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSA ea;

    if (NULL == pszObjName)
        return ERROR_INVALID_PARAMETER;

    dwRes = GetNamedSecurityInfoA(pszObjName, ObjectType,
        DACL_SECURITY_INFORMATION,
        NULL, NULL, &pOldDACL, NULL, &pSD);
    if (ERROR_SUCCESS != dwRes) {
        ::redisLog(REDIS_WARNING, "GetNamedSecurityInfo Error %u\n", dwRes);
        goto Cleanup;
    }

    ZeroMemory(&ea, sizeof(EXPLICIT_ACCESS));
    ea.grfAccessPermissions = dwAccessRights;
    ea.grfAccessMode = AccessMode;
    ea.grfInheritance = dwInheritance;
    ea.Trustee.TrusteeForm = TrusteeForm;
    ea.Trustee.ptstrName = pszTrustee;

    dwRes = SetEntriesInAclA(1, &ea, pOldDACL, &pNewDACL);
    if (ERROR_SUCCESS != dwRes) {
        ::redisLog(REDIS_WARNING, "SetEntriesInAcl Error %u\n", dwRes);
        goto Cleanup;
    }

    dwRes = SetNamedSecurityInfoA(pszObjName, ObjectType,
        DACL_SECURITY_INFORMATION,
        NULL, NULL, pNewDACL, NULL);
    if (ERROR_SUCCESS != dwRes) {
        ::redisLog(REDIS_WARNING, "SetNamedSecurityInfo Error %u\n", dwRes);
        goto Cleanup;
    }

Cleanup:

    if (pSD != NULL)
        LocalFree((HLOCAL)pSD);
    if (pNewDACL != NULL)
        LocalFree((HLOCAL)pNewDACL);

    return dwRes;
}

VOID SetAccessACLOnFolder(string user, string folder) {
    if (0 != AddAceToObjectsSecurityDescriptor( 
                (LPSTR)(folder.c_str()), SE_OBJECT_TYPE::SE_FILE_OBJECT,
                (LPSTR)(user.c_str()), TRUSTEE_FORM::TRUSTEE_IS_NAME,
                GENERIC_ALL, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT)) {
        throw std::system_error(GetLastError(), system_category(), "ServiceInstall: AddAceToObjectsSecurityDescriptor failed");
    }
}

VOID ServiceInstall(int argc, char ** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;
    CHAR szPath[MAX_PATH];
    string userName = "NT AUTHORITY\\NetworkService";

    InitializeServiceName();

    // build arguments to pass to service when it auto starts
    if (GetModuleFileNameA(NULL, szPath, MAX_PATH) == 0) {
        throw std::system_error(GetLastError(), system_category(), "ServiceInstall: GetModuleFileNameA failed");
    }

    stringstream args;
    for (int a = 0; a < argc; a++) {
        if (a == 0) {
            args << "\"" << szPath << "\"";
        } else {
            args << " ";
            if (a == 1) {
                // replace --service-install argument with --service-run
                args << "--" << cServiceRun;
            } else {
                string arg = argv[a];
                if (arg.find(' ') != arg.npos)  {
                    args << "\"" << argv[a] << "\"";
                } else {
                    args << argv[a];
                }
            }
        }
    }

    shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }

    shService = CreateServiceA(
        shSCManager,
        g_serviceName,
        g_serviceName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        args.str().c_str(),
        NULL, NULL, NULL,
        userName.c_str(),
        NULL);
    if (shService.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "CreateService failed");
    }

    SERVICE_PRESHUTDOWN_INFO preshutdownInfo;
    preshutdownInfo.dwPreshutdownTimeout = cPreshutdownInterval;
    if (FALSE == ChangeServiceConfig2(shService, SERVICE_CONFIG_PRESHUTDOWN_INFO, &preshutdownInfo)) {
        throw std::system_error(GetLastError(), system_category(), "ChangeServiceConfig2 failed");
    }

    RedisEventLog().InstallEventLogSource(szPath);

    // make sure NT AUTHORITY\\NetworkService" has rights to every directory where a files may be accessed (CONF,AOF,RDB,DAT)
    stringstream aceMessage;
    aceMessage << "Granting read/write access to 'NT AUTHORITY\\NetworkService' on: ";
    for (auto folder : GetAccessPaths()) {
        SetAccessACLOnFolder(userName, folder);
        aceMessage << "\"" << folder.c_str() << "\" ";
    }
    ServicePipeWriter::getInstance().Write(aceMessage.str().c_str());

    ServicePipeWriter::getInstance().Write("Redis successfully installed as a service.");
}

VOID ServiceStart(int argc, char ** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;

    InitializeServiceName();

    shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }
    shService = OpenServiceA(shSCManager, g_serviceName, SERVICE_ALL_ACCESS);
    if (shService.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenService failed");
    }
    if (FALSE == StartServiceA(shService, 0, NULL)) {
        throw std::system_error(GetLastError(), system_category(), "StartService failed");
    }

    // it will take atleast a couple of seconds for the service to start.
    Sleep(2000);

    SERVICE_STATUS status;
    DWORD start = GetTickCount();
    while (QueryServiceStatus(shService, &status) == TRUE) {
        if (status.dwCurrentState == SERVICE_RUNNING) {
            ServicePipeWriter::getInstance().Write("Redis service successfully started.");
            break;
        } else if (status.dwCurrentState == SERVICE_STOPPED) {
            ServicePipeWriter::getInstance().Write("Redis service failed to start.");
            break;
        }

        DWORD current = GetTickCount();
        if (current - start >= cThirtySeconds) {
            ServicePipeWriter::getInstance().Write("Redis service start timed out.");
            break;
        }
    }

}

VOID ServiceStop(int argc, char ** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;

    InitializeServiceName();

    shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }
    shService = OpenServiceA(shSCManager, g_serviceName, SERVICE_ALL_ACCESS);
    if (shService.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenService failed");
    }
    SERVICE_STATUS status;
    if (FALSE == ControlService(shService, SERVICE_CONTROL_STOP, &status)) {
        throw std::system_error(GetLastError(), system_category(), "ControlService failed");
    }

    DWORD start = GetTickCount();
    while (QueryServiceStatus(shService, &status) == TRUE) {
        if (status.dwCurrentState == SERVICE_STOPPED) {
            ServicePipeWriter::getInstance().Write("Redis service successfully stopped.");
            break;
        }
        DWORD current = GetTickCount();
        if (current - start >= cThirtySeconds) {
            ServicePipeWriter::getInstance().Write("Redis service stop timed out.");
            break;
        }
    }
}

VOID ServiceUninstall(int argc, char** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;

    InitializeServiceName();

    shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }
    shService = OpenServiceA(shSCManager, g_serviceName, SERVICE_ALL_ACCESS);
    if (shService.Valid()) {
        if (FALSE == DeleteService(shService)) {
            throw std::system_error(GetLastError(), system_category(), "DeleteService failed");
        }
    }

    RedisEventLog().UninstallEventLogSource();

    ServicePipeWriter::getInstance().Write("Redis service successfully uninstalled.");
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    try {
        int argc = (int)(serviceRunArguments.size());
        char** argv = new char*[argc];
        if (argv == nullptr)
            throw std::runtime_error("new() failed");

        int argIndex = 0;
        for each(string arg in serviceRunArguments) {
            argv[argIndex] = new char[arg.length() + 1];
            if (argv[argIndex] == nullptr)
                throw std::runtime_error("new() failed");
            memcpy_s(argv[argIndex], arg.length() + 1, arg.c_str(), arg.length());
            argv[argIndex][arg.size()] = '\0';
            ++argIndex;
        }

        // When the service starts the current directory is %systemdir%. If the launching user does not have permission there(i.e., NETWORK SERVICE), the 
        // memory mapped file will not be able to be created. Thus Redis will fail to start. Setting the current directory to the executable directory
        // should fix this.
        char szFilePath[MAX_PATH];
        if (GetModuleFileNameA(NULL, szFilePath, MAX_PATH) == 0) {
            throw std::system_error(GetLastError(), system_category(), "ServiceWrokerThread: GetModuleFileName failed");
        }
        string currentDir = szFilePath;
        auto pos = currentDir.rfind("\\");
        currentDir.erase(pos);

        if (FALSE == SetCurrentDirectoryA(currentDir.c_str())) {
            throw std::system_error(GetLastError(), system_category(), "SetCurrentDirectory failed");
        }

        // call redis main without the --service-run argument
        main(argc, argv);

        for (int a = 0; a < argc; a++) {
            delete[] argv[a];
            argv[a] = nullptr;
        }
        delete[] argv;
        argv = nullptr;

        SetEvent(g_ServiceStoppedEvent);

        return ERROR_SUCCESS;
    } catch (std::system_error syserr) {
        stringstream err;
        err << "ServiceWorkerThread: system error caught. error code=0x" << hex << syserr.code().value() << ", message = " << syserr.what() << endl;
        OutputDebugStringA(err.str().c_str());
    } catch (std::runtime_error runerr) {
        stringstream err;
        err << "runtime error caught. message=" << runerr.what() << endl;
        OutputDebugStringA(err.str().c_str());
    } catch (...) {
        OutputDebugStringA("ServiceWorkerThread: other exception caught.\n");
    }

    return  ERROR_PROCESS_ABORTED;
}

DWORD WINAPI ServiceCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
    switch (dwControl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
        {
            SetEvent(g_ServiceStopEvent);

            g_ServiceStatus.dwControlsAccepted = 0;
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_ServiceStatus.dwWin32ExitCode = 0;
            g_ServiceStatus.dwCheckPoint = 4;

            if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
                throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
            }

            break;
        }

        case SERVICE_CONTROL_STOP:
        {
            DWORD start = GetTickCount();
            while (GetTickCount() - start > cPreshutdownInterval) {
                if (WaitForSingleObject(g_ServiceStoppedEvent, cPreshutdownInterval / 10) == WAIT_OBJECT_0) {
                    break;
                }

                g_ServiceStatus.dwControlsAccepted = 0;
                g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
                g_ServiceStatus.dwWin32ExitCode = 0;
                g_ServiceStatus.dwCheckPoint = 4;

                if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
                    throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
                }
            }

            g_ServiceStatus.dwControlsAccepted = 0;
            g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            g_ServiceStatus.dwWin32ExitCode = 0;
            g_ServiceStatus.dwCheckPoint = 4;

            if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
                throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
            }
            break;
        }

        default:
        {
                   break;
        }
    }

    return NO_ERROR;
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    DWORD Status = E_FAIL;

    g_StatusHandle = RegisterServiceCtrlHandlerExA(g_serviceName, ServiceCtrlHandler, NULL);
    if (g_StatusHandle == NULL) {
        return;
    }

    ZeroMemory(&g_ServiceStatus, sizeof (g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
    }

    g_ServiceStoppedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        g_ServiceStatus.dwCheckPoint = 1;

        if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
            throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
        }

        return;
    }

    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
    }

    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);

    WaitForSingleObject(hThread, INFINITE);

    CloseHandle(g_ServiceStopEvent);

    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 3;

    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
    }
}

void ServiceRun() {
    SERVICE_TABLE_ENTRYA ServiceTable[] =
    {
        { g_serviceName, (LPSERVICE_MAIN_FUNCTIONA)ServiceMain },
        { NULL, NULL }
    };

    if (StartServiceCtrlDispatcherA(ServiceTable) == FALSE) {
        throw std::system_error(GetLastError(), system_category(), "StartServiceCtrlDispatcherA failed");
    }
}

void BuildServiceRunArguments(int argc, char** argv) {
    InitializeServiceName();
    string serviceNameFullArgument = "--" + cServiceName;

    // build argument list to be used by ServiceRun
    for (int n = 0; n < argc; n++) {
        if (n == 0) {
            CHAR szPath[MAX_PATH];
            if (GetModuleFileNameA(NULL, szPath, MAX_PATH) == 0) {
                throw std::system_error(GetLastError(), system_category(), "BuildServiceRunArguments: GetModuleFileNameA failed");
            }
            stringstream ss;
            ss << "\"" << szPath << "\"";
            serviceRunArguments.push_back(ss.str());
        } else if (n == 1) {
            // bypass --service-run argument
            continue;
        } else {
            if (_stricmp(argv[n], serviceNameFullArgument.c_str()) == 0) {
                // bypass --service-name argument and the name of the service
                n++;
                continue; 
            } else {
                serviceRunArguments.push_back(argv[n]);
            }
        }
    }
}

extern "C" BOOL HandleServiceCommands(int argc, char **argv) {
    try {
        if (argc > 1) {
            string servicearg = string(argv[1]);
            servicearg = servicearg.substr(2, servicearg.length());
            std::transform(servicearg.begin(), servicearg.end(), servicearg.begin(), ::tolower);
            if (servicearg == cServiceInstall) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceInstall(argc, argv);
                    return TRUE;
                }
            } else if (servicearg == cServiceUninstall) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceUninstall(argc, argv);
                    return TRUE;
                }
            } else if (servicearg == cServiceRun) {
                g_isRunningAsService = TRUE;
                BuildServiceRunArguments(argc, argv);
                ServiceRun();
                return TRUE;
            } else if (servicearg == cServiceStart) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceStart(argc, argv);
                    return TRUE;
                }
            } else if (servicearg == cServiceStop) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceStop(argc, argv);
                    return TRUE;
                }
            }
        }

        // not a service command. start redis normally.
        return FALSE;
    } catch (std::system_error syserr) {
        stringstream ss;
        ss << "HandleServiceCommands: system error caught. error code=" << syserr.code().value() << ", message = " << syserr.what() << endl;
        ServicePipeWriter::getInstance().Write(ss.str());
        exit(1);
    } catch (std::runtime_error runerr) {
        stringstream err;
        err << "HandleServiceCommands: runtime error caught. message=" << runerr.what() << endl;
        ServicePipeWriter::getInstance().Write(err.str());
        exit(1);
    } catch (...) {
        stringstream ss;
        ss << "HandleServiceCommands: other exception caught." << endl;
        ServicePipeWriter::getInstance().Write(ss.str());
        exit(1);
    }
}

extern "C" BOOL ServiceStopIssued() {
    if (g_ServiceStopEvent == INVALID_HANDLE_VALUE) return FALSE;
    return (WaitForSingleObject(g_ServiceStopEvent, 0) == WAIT_OBJECT_0) ? TRUE : FALSE;
}

extern "C" BOOL RunningAsService() {
    return g_isRunningAsService;
}

extern "C" const char* GetServiceName()  {
    return g_serviceName;
}

