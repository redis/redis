#include <Windows.h>
#include <string>
#include <iostream>
using namespace std;

#include "Win32_EventLog.h"
#include "EventLog.h"

class RedisEventLog {
public:
	static RedisEventLog& getInstance() {
		static RedisEventLog    instance; // Instantiated on first use. Guaranteed to be destroyed.
		return instance;
	}

private:
	std::string eventLogName;

	RedisEventLog() {
	}
	~RedisEventLog() {
		UninstallEventLogSource();
	}
	RedisEventLog(RedisEventLog const&);	// Don't implement to guarantee singleton semantics
	void operator=(RedisEventLog const&);	// Don't implement to guarantee singleton semantics

	void UninstallEventLogSource() {
		const std::string keyPath(
			"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\" + this->eventLogName);

		DWORD last_error = RegDeleteKeyA(HKEY_LOCAL_MACHINE, keyPath.c_str());

		if (ERROR_SUCCESS != last_error) {
			std::cerr << "Failed to uninstall source: " << last_error << endl;
		}
	}


	void InstallEventLogSource() {
		const std::string keyPath(
			"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\" + this->eventLogName);

		HKEY key;

		DWORD last_error = RegCreateKeyExA(
			HKEY_LOCAL_MACHINE,
			keyPath.c_str(),
			0,
			0,
			REG_OPTION_NON_VOLATILE,
			KEY_SET_VALUE,
			0,
			&key,
			0);

		char result[MAX_PATH];
		GetModuleFileNameA(NULL, result, MAX_PATH);
		std::string appPath = result;

		if (ERROR_SUCCESS == last_error) {
			DWORD last_error;
			const DWORD types_supported = EVENTLOG_ERROR_TYPE |
				EVENTLOG_WARNING_TYPE |
				EVENTLOG_INFORMATION_TYPE;

			last_error = RegSetValueExA(
				key,
				"EventMessageFile",
				0,
				REG_SZ,
				(const BYTE *)appPath.c_str(),
				(DWORD)(appPath.length()));

			if (ERROR_SUCCESS == last_error) {
				last_error = RegSetValueExA(key,
					"TypesSupported",
					0,
					REG_DWORD,
					(const BYTE*)&types_supported,
					sizeof(types_supported));
			}

			if (ERROR_SUCCESS != last_error) {
				std::cerr << "Failed to install source values: "
					<< last_error << "\n";
			}

			RegCloseKey(key);
		} else {
			std::cerr << "Failed to install source: " << last_error << "\n";
		}
	}

public:
	void EnsureInitialization(LPCSTR eventLogName) {
		if (this->eventLogName.length() == 0)  {
			this->eventLogName = eventLogName;
			InstallEventLogSource();
		}
	}

	void LogMessageToEventLog(LPCSTR msg, const WORD type) {
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
			if (FALSE == ReportEventA(
				hEventLog,
				type,
				0,
				eventID,
				0,
				1,
				0,
				&msg,
				0)) {
				std::cerr << "Failed to write message: " << GetLastError() << endl;
			}

			DeregisterEventSource(hEventLog);
		}
	}

};

extern "C" void LogToEventLog(const char* eventLogName, const char* msg) {
	try {
		RedisEventLog::getInstance().EnsureInitialization(eventLogName);
		RedisEventLog::getInstance().LogMessageToEventLog(msg, EVENTLOG_INFORMATION_TYPE);
	} catch (...) {
	}
}
