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


*** Since service configuration requires administrative privileges, these commands will only work under an elevated command prompt. ***

*/

#include <windows.h>
#include <tchar.h>
#include <strsafe.h>

#include <algorithm>
#include <string>
#include <sstream>
#include <vector>
#include <iostream>
using namespace std;

#include "Win32_SmartHandle.h"

#pragma comment(lib, "advapi32.lib")

#define SERVICE_NAME "Redis"  

SERVICE_STATUS g_ServiceStatus = { 0 };
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;
vector<string> serviceRunArguments;
SERVICE_STATUS_HANDLE g_StatusHandle;
const ULONGLONG cThirtySeconds = 30 * 1000;

extern "C" int main(int argc, char** argv);

VOID ServiceInstall(int argc, char ** argv) {
	SmartServiceHandle shSCManager;
	SmartServiceHandle shService;
	CHAR szPath[MAX_PATH];

	// build arguments to pass to service when it auto starts
	if (!GetModuleFileNameA(NULL, szPath, MAX_PATH)) {
		throw std::system_error(GetLastError(), system_category(), "GetModuleFileNameA failed");
	}
	stringstream args;
	for (int a = 0; a < argc; a++) {
		if (a == 0) args << "\"" << szPath << "\"";
		if (a > 0) args << " ";
		if (a == 1) {
			// replace --service-install argument with --service-run
			args << "--service-run ";
		} else {
			args << argv[a];
		}
	}

	shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (shSCManager.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
	}
	shService = CreateServiceA(
		shSCManager,
		SERVICE_NAME,
		SERVICE_NAME,
		SERVICE_ALL_ACCESS,
		SERVICE_WIN32_OWN_PROCESS,
		SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL,
		args.str().c_str(),
		NULL, NULL, NULL,
		"NT AUTHORITY\\NetworkService",
		NULL);
	if (shService.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "CreateService failed");
	}

	cout << "Redis successfully installed as a service." << endl;
}

VOID ServiceStart() {
	SmartServiceHandle shSCManager;
	SmartServiceHandle shService;

	shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (shSCManager.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
	}
	shService = OpenServiceA(shSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
	if (shService.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "OpenService failed");
	}
	if (FALSE == StartServiceA(shService, 0, NULL)) {
		throw std::system_error(GetLastError(), system_category(), "StartService failed");
	}

	// it will take atleast a couple of seconds for the service to start.
	Sleep(2000);

	SERVICE_STATUS status;
	ULONGLONG start = GetTickCount64();
	while (QueryServiceStatus(shService, &status) == TRUE) {
		if (status.dwCurrentState == SERVICE_RUNNING) {
			cout << "Redis service successfully started." << endl;
			break;
		} else if (status.dwCurrentState == SERVICE_STOPPED) {
			cout << "Redis service failed to start." << endl;
			break;
		}

		ULONGLONG current = GetTickCount64();
		if (current - start >= cThirtySeconds) {
			cout << "Redis service start timed out." << endl;
			break;
		}
	}

}

VOID ServiceStop() {
	SmartServiceHandle shSCManager;
	SmartServiceHandle shService;

	shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (shSCManager.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
	}
	shService = OpenServiceA(shSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
	if (shService.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "OpenService failed");
	}
	SERVICE_STATUS status;
	if (FALSE == ControlService(shService, SERVICE_CONTROL_STOP, &status)) {
		throw std::system_error(GetLastError(), system_category(), "ChangeServiceConfig failed");
	}

	ULONGLONG start = GetTickCount64();
	while (QueryServiceStatus(shService, &status) == TRUE) {
		if (status.dwCurrentState == SERVICE_STOPPED) {
			cout << "Redis service successfully stopped." << endl;
			break;
		}
		ULONGLONG current = GetTickCount64();
		if (current - start >= cThirtySeconds) {
			cout << "Redis service stop timed out." << endl;
			break;
		}
	}
}

VOID ServiceUninstall() {
	SmartServiceHandle shSCManager;
	SmartServiceHandle shService;

	shSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (shSCManager.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
	}
	shService = OpenServiceA(shSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
	if (shSCManager.Invalid()) {
		throw std::system_error(GetLastError(), system_category(), "OpenService failed");
	}
	if (FALSE == DeleteService(shService)) {
		throw std::system_error(GetLastError(), system_category(), "DeleteService failed");
	}
	cout << "Redis service successfully uninstalled." << endl;
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
			throw std::system_error(GetLastError(), system_category(), "GetModuleFileName failed");
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
			delete argv[a];
			argv[a] = nullptr;
		}
		delete argv;
		argv = nullptr;

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

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
	switch (CtrlCode) {
		case SERVICE_CONTROL_STOP:

			if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING)
				break;

			g_ServiceStatus.dwControlsAccepted = 0;
			g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
			g_ServiceStatus.dwWin32ExitCode = 0;
			g_ServiceStatus.dwCheckPoint = 4;

			if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
				throw std::system_error(GetLastError(), system_category(), "SetServiceStatus failed");
			}

			// This will signal the worker thread to start shutting down
			SetEvent(g_ServiceStopEvent);

			break;

		default:
			break;
	}
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
	DWORD Status = E_FAIL;

	g_StatusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME, ServiceCtrlHandler);
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

	g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
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
		{ SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONA)ServiceMain },
		{ NULL, NULL }
	};

	if (StartServiceCtrlDispatcherA(ServiceTable) == FALSE) {
		throw std::system_error(GetLastError(), system_category(), "StartServiceCtrlDispatcherA failed");
	}
}

void BuildServiceRunArguments(int argc, char** argv) {
	// build argument list to be used by ServiceRun
	for (int n = 0; n < argc; n++) {
		if (n == 0) {
			CHAR szPath[MAX_PATH];
			if (!GetModuleFileNameA(NULL, szPath, MAX_PATH)) {
				throw std::system_error(GetLastError(), system_category(), "GetModuleFileNameA failed");
			}
			stringstream ss;
			ss << "\"" << szPath << "\"";
			serviceRunArguments.push_back(ss.str());
		} else if (n == 1) {
			// bypass --service-run argument
			continue;
		} else {
			serviceRunArguments.push_back(argv[n]);
		}
	}
}

extern "C" BOOL HandleServiceCommands(int argc, char **argv) {
	try {
		if (argc > 1) {

			string servicearg = argv[1];
			std::transform(servicearg.begin(), servicearg.end(), servicearg.begin(), ::tolower);
			if (servicearg == "--service-install") {
				ServiceInstall(argc, argv);
				return TRUE;
			} else if (servicearg == "--service-uninstall") {
				ServiceUninstall();
				return TRUE;
			} else if (servicearg == "--service-run") {
				BuildServiceRunArguments(argc, argv);
				ServiceRun();
				return TRUE;
			} else if (servicearg == "--service-start") {
				ServiceStart();
				return TRUE;
			} else if (servicearg == "--service-stop") {
				ServiceStop();
				return TRUE;
			}
		}

		// not a service command. start redis normally.
		return FALSE;
	} catch (std::system_error syserr) {
		stringstream err;
		err << "HandleServiceCommands: system error caught. error code=0x" << hex << syserr.code().value() << ", message = " << syserr.what() << endl;
		OutputDebugStringA(err.str().c_str());
		exit(1);
	} catch (std::runtime_error runerr) {
		stringstream err;
		err << "HandleServiceCommands: runtime error caught. message=" << runerr.what() << endl;
		OutputDebugStringA(err.str().c_str());
		exit(1);
	} catch (...) {
		OutputDebugStringA("HandleServiceCommands: other exception caught.");
		exit(1);
	}
}

extern "C" BOOL ServiceStopIssued() {
	if (g_ServiceStopEvent == INVALID_HANDLE_VALUE) return FALSE;
	return (WaitForSingleObject(g_ServiceStopEvent, 0) == WAIT_OBJECT_0) ? TRUE : FALSE;
}

