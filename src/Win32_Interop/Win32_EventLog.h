#pragma once

#ifdef __cplusplus
#include <string>
using namespace std;

typedef class RedisEventLog {
public:
	~RedisEventLog() {}

	void InstallEventLogSource(string appPath);
	void UninstallEventLogSource();

	void LogMessageToEventLog(LPCSTR msg, const WORD type);

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

	void WriteEventLog(const char* sysLogInstance, const char* msg);

#ifdef __cplusplus
}
#endif

