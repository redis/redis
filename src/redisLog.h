#pragma once

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3
#define REDIS_LOG_RAW (1<<10) /* Modifier to log without timestamp */
#define REDIS_MAX_LOGMSG_LEN    1024 /* Default maximum length of syslog messages */

void _cdecl redisLog(int level, const char *fmt, ...);
void _cdecl redisLogRaw(int level, const char *msg);
void setLogVerbosityLevel (int level);
void setLogFile (const char* logFileName);
