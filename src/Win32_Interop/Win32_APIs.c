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

#include "Win32_APIs.h"
#include <errno.h>

/* Replace MS C rtl rand which is 15bit with 32 bit */
int replace_random() {
    unsigned int x = 0;
    if (RtlGenRandom == NULL) {
        // Load proc if not loaded
        HMODULE lib = LoadLibraryA("advapi32.dll");
        RtlGenRandom = (RtlGenRandomFunc) GetProcAddress(lib, "SystemFunction036");
        if (RtlGenRandom == NULL) return 1;
    }
    RtlGenRandom(&x, sizeof(unsigned int));
    return (int) (x >> 1);
}

/* Rename which works on Windows when file exists */
int replace_rename(const char *src, const char *dst) {
    int retries = 50;
    while (1) {
        if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            return 0;
        } else {
            errno = GetLastError();
            // Anti-virus may lock file - error code 5.
            if (errno != 5) {
                break;
            }
            retries--;
            if (retries == 0) {
                break;
            }
            Sleep(10);
        }
    }
    // On error we will return generic error code without GetLastError()
    return -1;
}

int truncate(const char *path, PORT_LONGLONG length) {
    LARGE_INTEGER newSize;
    HANDLE toTruncate = CreateFileA(path,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_WRITE | FILE_SHARE_READ,
                                    NULL,
                                    OPEN_EXISTING,
                                    0,
                                    NULL);
    if (toTruncate != INVALID_HANDLE_VALUE) {
        int result = 0;
        newSize.QuadPart = length;
        if (FALSE == (SetFilePointerEx(toTruncate, newSize, NULL, FILE_BEGIN)
                      && SetEndOfFile(toTruncate))) {
            errno = ENOENT;
            result = -1;
        }
        CloseHandle(toTruncate);
        return result;
    } else {
        errno = ENOENT;
        return -1;
    }
}