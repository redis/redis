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


#include "redisLog.h"
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <process.h>
#include "redis.h"
#ifdef _WIN32
#include "win32_Interop/win32Fixes.h"
#include "win32_Interop/Win32_EventLog.h"
#endif
#include <time.h>

static int verbosity = REDIS_WARNING;
static char* logFile = NULL;

void setLogVerbosityLevel(int level)
{
    verbosity = level;
}

void setLogFile(const char* logFileName)
{
    if (logFile != NULL) {
        free((void*)logFile);
        logFile = NULL;
    }
    logFile = (char*)malloc(strlen(logFileName));
    if (logFile==NULL) {
        redisLog(REDIS_WARNING, "memory allocation failure");
        return;
    }
    strcpy (logFile,logFileName);
}

void redisLogRaw(int level, const char *msg) {
#ifndef _WIN32
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
#endif
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & REDIS_LOG_RAW);
	int log_to_stdout;
	log_to_stdout = 0;
	if (logFile == NULL) {
		log_to_stdout = 1;
	} else {
		if ((logFile[0] == '\0') || (_stricmp(logFile, "stdout") == 0)) {
			log_to_stdout = 1;
		}
	}

    level &= 0xff; /* clear flags */
    if (level < verbosity) return;

    fp = log_to_stdout ? stdout : fopen(logFile,"a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        int off;
#ifdef _WIN32
        time_t secs;
        unsigned int usecs;
        struct tm * now ;

        secs = gettimeofdaysecs(&usecs);
        now = localtime(&secs);
        off = (int)strftime(buf,sizeof(buf),"%d %b %H:%M:%S.",now);
        snprintf(buf+off,sizeof(buf)-off,"%03d",usecs/1000);
#else
        struct timeval tv;

        gettimeofday(&tv,NULL);
        off = strftime(buf,sizeof(buf),"%d %b %H:%M:%S.",localtime(&tv.tv_sec));
        snprintf(buf+off,sizeof(buf)-off,"%03d",(int)tv.tv_usec/1000);
#endif
        fprintf(fp,"[%d] %s %c %s\n",(int)_getpid(),buf,c[level],msg);
    }
    fflush(fp);
    
	if (log_to_stdout == 0) fclose(fp);

#ifdef _WIN32
	if (server.syslog_enabled) WriteEventLog(server.syslog_ident, msg);
#else
    if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
#endif
}

/* Like redisLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[REDIS_MAX_LOGMSG_LEN];

    if ((level&0xff) < verbosity) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    redisLogRaw(level,msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by redisLog(). */
void redisLogFromHandler(int level, const char *msg) {
#ifndef _WIN32
    int fd;
    int log_to_stdout = logfile[0] == '\0';
    char buf[64];

    if ((level&0xff) < verbosity || (log_to_stdout && server.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO :
                         open(server.logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    if (fd == -1) return;
    ll2string(buf,sizeof(buf),getpid());
    if (write(fd,"[",1) == -1) goto err;
    if (write(fd,buf,(unsigned int)strlen(buf)) == -1) goto err;
    if (write(fd," | signal handler] (",20) == -1) goto err;
    ll2string(buf,sizeof(buf),time(NULL));
    if (write(fd,buf,(unsigned int)strlen(buf)) == -1) goto err;
    if (write(fd,") ",2) == -1) goto err;
    if (write(fd,msg,(unsigned int)strlen(msg)) == -1) goto err;
    if (write(fd,"\n",1) == -1) goto err;
err:
    if (!log_to_stdout) close(fd);
#endif
}





