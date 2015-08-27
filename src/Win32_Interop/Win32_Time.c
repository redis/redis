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

/* Credits Henry Rawas (henryr@schakra.com) */

#include "Win32_Time.h"
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <assert.h>

#define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64

struct timezone {
    int  tz_minuteswest; /* minutes W of Greenwich */
    int  tz_dsttime;     /* type of dst correction */
};

/* fnGetSystemTimePreciseAsFileTime is NULL if and only if it hasn't been initialized. */
static VOID(WINAPI *fnGetSystemTimePreciseAsFileTime)(LPFILETIME) = NULL;

/* Interval (in seconds) of the high-resolution clock.
* Special values:
*   0 : it hasn't been initialized
*  -1 : the system doesn't have high-resolution clock support
*/
static double highResTimeInterval = 0;

void InitHighResRelativeTime() {
    LARGE_INTEGER perfFrequency;

    if (highResTimeInterval != 0)
        return;

    /* Retrieve high-resolution timer frequency
    * and precompute its reciprocal.
    */
    if (QueryPerformanceFrequency(&perfFrequency)) {
        highResTimeInterval = 1.0 / perfFrequency.QuadPart;
    } else {
        highResTimeInterval = -1;
    }

    assert(highResTimeInterval != 0);
}

void InitHighResAbsoluteTime() {
    FARPROC fp;
    HMODULE module;

    if (fnGetSystemTimePreciseAsFileTime != NULL)
        return;

    /* Use GetSystemTimeAsFileTime as fallbcak where GetSystemTimePreciseAsFileTime is not available */
    fnGetSystemTimePreciseAsFileTime = GetSystemTimeAsFileTime;
    module = GetModuleHandleA("kernel32.dll");
    if (module) {
        fp = GetProcAddress(module, "GetSystemTimePreciseAsFileTime");
        if (fp) {
            fnGetSystemTimePreciseAsFileTime = (VOID(WINAPI*)(LPFILETIME)) fp;
        }
    }

    assert(fnGetSystemTimePreciseAsFileTime != NULL);
}

void InitTimeFunctions() {
    InitHighResRelativeTime();
    InitHighResAbsoluteTime();
}

uint64_t GetHighResRelativeTime(double scale) {
    LARGE_INTEGER counter;

    if (highResTimeInterval <= 0) {
        if (highResTimeInterval == 0) {
            InitHighResRelativeTime();
        }

        /* If the performance interval is less than zero, there's no support. */
        if (highResTimeInterval < 0) {
            return 0;
        }
    }

    if (!QueryPerformanceCounter(&counter)) {
        return 0;
    }

    /* Because we have no guarantee about the order of magnitude of the
    * performance counter interval, integer math could cause this computation
    * to overflow. Therefore we resort to floating point math.
    */
    return (uint64_t) ((double) counter.QuadPart * highResTimeInterval * scale);
}

time_t gettimeofdaysecs(unsigned int *usec) {
    FILETIME ft;
    time_t tmpres = 0;

    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    /*converting file time to unix epoch*/
    tmpres /= 10;  /*convert into microseconds*/
    tmpres -= DELTA_EPOCH_IN_MICROSECS;
    if (usec != NULL) {
        *usec = (unsigned int) (tmpres % 1000000UL);
    }
    return (tmpres / 1000000UL);
}

int gettimeofday_fast(struct timeval *tv, struct timezone *tz) {
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag;

    if (NULL != tv) {
        GetSystemTimeAsFileTime(&ft);

        tmpres |= ft.dwHighDateTime;
        tmpres <<= 32;
        tmpres |= ft.dwLowDateTime;

        /*converting file time to unix epoch*/
        tmpres /= 10;  /*convert into microseconds*/
        tmpres -= DELTA_EPOCH_IN_MICROSECS;
        tv->tv_sec = (long) (tmpres / 1000000UL);
        tv->tv_usec = (long) (tmpres % 1000000UL);
    }

    if (NULL != tz) {
        if (!tzflag)
        {
            _tzset();
            tzflag++;
        }
        tz->tz_minuteswest = _timezone / 60;
        tz->tz_dsttime = _daylight;
    }

    return 0;
}

int gettimeofday_highres(struct timeval *tv, struct timezone *tz) {
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag;

    if (NULL == fnGetSystemTimePreciseAsFileTime) {
        InitHighResAbsoluteTime();
    }

    if (NULL != tv) {
        fnGetSystemTimePreciseAsFileTime(&ft);

        tmpres |= ft.dwHighDateTime;
        tmpres <<= 32;
        tmpres |= ft.dwLowDateTime;

        /*converting file time to unix epoch*/
        tmpres /= 10;  /*convert into microseconds*/
        tmpres -= DELTA_EPOCH_IN_MICROSECS;
        tv->tv_sec = (long) (tmpres / 1000000UL);
        tv->tv_usec = (long) (tmpres % 1000000UL);
    }

    if (NULL != tz) {
        if (!tzflag) {
            _tzset();
            tzflag++;
        }
        tz->tz_minuteswest = _timezone / 60;
        tz->tz_dsttime = _daylight;
    }

    return 0;
}


/* ctime_r is documented (http://www.mkssoftware.com/docs/man3/ctime_r.3.asp)
 * to be reentrant. 
 * _ctime64 is not thread safe. 
 * Since this is used only in sentinel.c and Redis is single threaded this
 * is not a problem */
char *ctime_r(const time_t *clock, char *buf)  {

    char* t = _ctime64(clock);
    if (t != NULL) {
        strcpy(buf, t);
    } else {
        buf[0] = 0;
    }
    return buf;
}