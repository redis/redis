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

#include "Win32_types.h"
#include "Win32_RedisLog.h"
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <process.h>
#include <time.h>
#include "Win32Fixes.h"
#include "Win32_EventLog.h"
#include "Win32_Time.h"
#include <assert.h>

static const char ellipsis[] = "[...]";
static const char ellipsisWithNewLine[] = "[...]\n";
static int verbosity = REDIS_WARNING;
static HANDLE hLogFile = INVALID_HANDLE_VALUE;
static int isStdout = 0;
static char* logFilename = NULL;

void setLogVerbosityLevel(int level)
{
    verbosity = level;
}

const char* getLogFilename() {
    if (logFilename == NULL || logFilename[0] == '\0') {
        return "stdout";
    } else {
        return logFilename;
    }
}

/* We keep the file handle open to improve performance.
 * This assumes that calls to redisLog and setLogFile will not happen
 * concurrently.
 */
void setLogFile(const char* filename)
{
    if (logFilename != NULL) {
        free((void*) logFilename);
        logFilename = NULL;
    }
    logFilename = (char*) malloc(strlen(filename) + 1);
    if (logFilename == NULL) {
        redisLog(REDIS_WARNING, "memory allocation failure");
        return;
    }
    memset(logFilename, 0, strlen(filename) + 1);
    strcpy(logFilename, filename);

    if (hLogFile != INVALID_HANDLE_VALUE) {
        if (!isStdout) CloseHandle(hLogFile);
        hLogFile = INVALID_HANDLE_VALUE;
    }

    if (filename == NULL || (filename[0] == '\0') || (_stricmp(filename, "stdout") == 0)) {
        hLogFile = GetStdHandle(STD_OUTPUT_HANDLE);
        isStdout = 1;
    }
    else {
        int len;
        UINT codePage = CP_ACP;
        wchar_t *widePath;

        /* Convert the path from ansi to unicode, to support paths longer than MAX_PATH */
        if ((len = MultiByteToWideChar(codePage, 0, filename, -1, 0, 0)) == 0) return;
        if ((widePath = (wchar_t*)malloc(len * sizeof(wchar_t))) == NULL) return;
        if (MultiByteToWideChar(codePage, 0, filename, -1, widePath, len) == 0) {
            free(widePath);
            return;
        }

        /* Passing FILE_APPEND_DATA without FILE_WRITE_DATA is essential for getting atomic appends across processes. */
        hLogFile = CreateFileW(
            widePath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, 
            NULL);

        if (hLogFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            LPSTR messageBuffer = NULL;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
            fprintf(stderr, "Could not open logfile %s: %s\n", filename, messageBuffer);
            LocalFree(messageBuffer);
        }

        free(widePath);
        isStdout = 0;
    }
}
    
void redisLogRaw(int level, const char *msg) {
    const char *c = ".-*#";
    DWORD dwBytesWritten;
    /* The complete message needs to be passed to WriteFile at once, to ensure
     * atomicity of log entries across processes.
     * So we format the complete message into a buffer first.
     * Any output that doesn't fit the size of this buffer will be truncated.
     */
    char buf[REDIS_MAX_LOGMSG_LEN];
    const char *completeMessage;
    DWORD completeMessageLength;
    int rawmode = (level & REDIS_LOG_RAW);

    level &= 0xff; /* clear flags */
    if (level < verbosity) return;

    if (hLogFile == INVALID_HANDLE_VALUE) return;

    if (rawmode) {
        completeMessage = msg;
        completeMessageLength = (DWORD)strlen(msg);
    } else {
        int vlen, off = 0;
        time_t secs;
        unsigned int usecs;
        struct tm * now ;

        completeMessage = buf; 
        secs = gettimeofdaysecs(&usecs);
        now = localtime(&secs);
        vlen = snprintf(buf + off, sizeof(buf) - off, "[%d] ", (int)_getpid());
        assert(vlen >= 0);
        off += vlen;
        vlen = (int)strftime(buf + off, sizeof(buf) - off, "%d %b %H:%M:%S.", now);
        assert(vlen >= 0);
        off += vlen;
        vlen = snprintf(buf + off, sizeof(buf) - off, "%03d %c ", usecs / 1000, c[level]);
        assert(vlen >= 0);
        off += vlen;
        vlen = snprintf(buf + off, sizeof(buf) - off, "%s\n", msg);
        if (vlen >= 0 && (off + vlen < sizeof(buf))) {
            completeMessageLength = off + vlen;
        }
        else {
            /* The MS CRT implementation of vsnprintf/snprintf returns -1 if the formatted output doesn't fit the buffer,
            * in addition to when an encoding error occurs. Proceeding with a zero-terminated ellipsis at the end of the
            * buffer seems a better option than not logging this message at all.
            */
            strncpy(buf + sizeof(buf)-sizeof(ellipsisWithNewLine), ellipsisWithNewLine, sizeof(ellipsisWithNewLine));
            completeMessageLength = sizeof(buf)-1;
        }
    }
    WriteFile(hLogFile, completeMessage, completeMessageLength, &dwBytesWritten, NULL);

    /* FlushFileBuffers() ensures that all data and metadata is written to disk, but it's effect
     * on performance is severe.
     */
#ifdef FLUSH_LOG_WRITES
    FlushFileBuffers(hLogFile);
#endif

    if (IsEventLogEnabled() == 1) {
        WriteEventLog(msg);
    }
}

/* Like redisLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[REDIS_MAX_LOGMSG_LEN];
    int vlen;

    if ((level&0xff) < verbosity) return;

    va_start(ap, fmt);
    vlen = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* The MS CRT implementation of vsnprintf/snprintf returns -1 if the formatted output doesn't fit the buffer,
     * in addition to when an encoding error occurs. Proceeding with a zero-terminated ellipsis at the end of the
     * buffer seems a better option than not logging this message at all.
     */
    if (vlen < 0 || vlen >= sizeof(msg)) {
        strncpy(msg + sizeof(msg) - sizeof(ellipsis), ellipsis, sizeof(ellipsis));
    }

    redisLogRaw(level,msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by redisLog(). */
void redisLogFromHandler(int level, const char *msg) {
}





