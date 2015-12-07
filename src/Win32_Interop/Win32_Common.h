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

#ifndef WIN32_COMMON_H
#define WIN32_COMMON_H

#include <Windows.h>

namespace Globals {
    // forward declarations only
    extern size_t pageSize;
    extern size_t memoryPhysicalTotal;
}

void EnsureMemoryIsMapped(const void *buffer, size_t size);
bool IsWindowsVersionAtLeast(WORD wMajorVersion, WORD wMinorVersion, WORD wServicePackMajor);

class WindowsVersion {
private:
    bool isAtLeast_6_0;
    bool isAtLeast_6_2;

    WindowsVersion() {
        isAtLeast_6_0 = IsWindowsVersionAtLeast(6, 0, 0);
        isAtLeast_6_2 = IsWindowsVersionAtLeast(6, 2, 0);
    }

    WindowsVersion(WindowsVersion const&);      // Don't implement to guarantee singleton semantics
    void operator=(WindowsVersion const&);      // Don't implement to guarantee singleton semantics

public:
    static WindowsVersion& getInstance() {
        static WindowsVersion instance;         // Instantiated on first use. Guaranteed to be destroyed.
        return instance;
    }

    bool IsAtLeast_6_0() {
        return isAtLeast_6_0;
    }

    bool IsAtLeast_6_2() {
        return isAtLeast_6_2;
    }
};
#endif
