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

#include "Win32_Signal_Process.h"
#include <errno.h>

/* Redefined here to avoid redis.h so it can be used in other projects */
#define REDIS_NOTUSED(V) ((void) V)

/* Behaves as posix, works without ifdefs, makes compiler happy */
int sigaction(int sig, struct sigaction *in, struct sigaction *out) {
    REDIS_NOTUSED(out);

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
    * is used. Otherwise, sa_handler is used */
    if (in->sa_flags & SA_SIGINFO) {
        signal(sig, in->sa_sigaction);
    } else {
        signal(sig, in->sa_handler);
    }
    return 0;
}

/* Terminates process, implemented only for SIGKILL */
int kill(pid_t pid, int sig) {
    if (sig == SIGUSR1) {
        //AbortForkOperation();
        return 0;
    } else if (sig == SIGKILL) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, 0, pid);
        if (!TerminateProcess(h, 127)) {
            errno = EINVAL; /* GetLastError() */
            CloseHandle(h);
            return -1;
        };
        CloseHandle(h);
        return 0;
    } else {
        errno = EINVAL;
        return -1;
    }
}

/* Redis CPU GetProcessTimes -> rusage  */
int getrusage(int who, struct rusage * r) {
    FILETIME starttime, exittime, kerneltime, usertime;
    ULARGE_INTEGER li;

    if (r == NULL) {
        errno = EFAULT;
        return -1;
    }

    memset(r, 0, sizeof(struct rusage));

    if (who == RUSAGE_SELF) {
        if (!GetProcessTimes(GetCurrentProcess(),
            &starttime,
            &exittime,
            &kerneltime,
            &usertime))
        {
            errno = EFAULT;
            return -1;
        }
    }

    if (who == RUSAGE_CHILDREN) {
        /* Childless on windows */
        starttime.dwLowDateTime = 0;
        starttime.dwHighDateTime = 0;
        exittime.dwLowDateTime = 0;
        exittime.dwHighDateTime = 0;
        kerneltime.dwLowDateTime = 0;
        kerneltime.dwHighDateTime = 0;
        usertime.dwLowDateTime = 0;
        usertime.dwHighDateTime = 0;
    }
    memcpy(&li, &kerneltime, sizeof(FILETIME));
    li.QuadPart /= 10L;
    r->ru_stime.tv_sec = (long) (li.QuadPart / 1000000L);
    r->ru_stime.tv_usec = (long) (li.QuadPart % 1000000L);

    memcpy(&li, &usertime, sizeof(FILETIME));
    li.QuadPart /= 10L;
    r->ru_utime.tv_sec = (long) (li.QuadPart / 1000000L);
    r->ru_utime.tv_usec = (long) (li.QuadPart % 1000000L);

    return 0;
}