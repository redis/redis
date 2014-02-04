#pragma once

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3
#define REDIS_LOG_RAW (1<<10) /* Modifier to log without timestamp */
#define REDIS_DEFAULT_VERBOSITY REDIS_NOTICE
#define REDIS_MAX_LOGMSG_LEN    1024 /* Default maximum length of syslog messages */

#ifdef __cplusplus
extern "C" {
#endif

void setLogVerbosityLevel (int level);
void setLogFile (const char* logFileName);
void redisLogRaw(int level, const char *msg);
void redisLog(int level, const char *fmt, ...);
void redisLogFromHandler(int level, const char *msg);

#ifdef __cplusplus
}
#endif

