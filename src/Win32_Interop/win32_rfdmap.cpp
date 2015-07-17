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

#include "win32_types.h"
#include "win32_rfdmap.h"

RFDMap& RFDMap::getInstance() {
    static RFDMap instance; // Instantiated on first use. Guaranteed to be destroyed.
    return instance;
}

#ifndef STDIN_FILENO
#define STDIN_FILENO (_fileno(stdin))
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO (_fileno(stdout))
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO (_fileno(stderr))
#endif

RFDMap::RFDMap() {
    InitializeCriticalSection(&mutex);
    // stdin, assign rfd = 0 (STDIN_FILENO)
    RFD_INFO stdInRFDInfo;
    stdInRFDInfo.flags = 0;
    stdInRFDInfo.type = RFD_TYPE::CRTFD;
    stdInRFDInfo.value.crtFD = STDIN_FILENO;
    RFDToCrtFDInfoMap[STDIN_FILENO] = stdInRFDInfo;
    CrtFDToRFDMap[STDIN_FILENO] = STDIN_FILENO;

    // stdout, assign rfd = 1 (STDOUT_FILENO)
    RFD_INFO stdOutRFDInfo;
    stdOutRFDInfo.flags = 0;
    stdOutRFDInfo.type = RFD_TYPE::CRTFD;
    stdOutRFDInfo.value.crtFD = STDOUT_FILENO;
    RFDToCrtFDInfoMap[STDOUT_FILENO] = stdOutRFDInfo;
    CrtFDToRFDMap[STDOUT_FILENO] = STDOUT_FILENO;

    // stderr, assign rfd = 2 (STDERR_FILENO)
    RFD_INFO stdErrRFDInfo;
    stdErrRFDInfo.flags = 0;
    stdErrRFDInfo.type = RFD_TYPE::CRTFD;
    stdErrRFDInfo.value.crtFD = STDERR_FILENO;
    RFDToCrtFDInfoMap[STDERR_FILENO] = stdErrRFDInfo;
    CrtFDToRFDMap[STDERR_FILENO] = STDERR_FILENO;
}

RFD RFDMap::getNextRFDAvailable() {
    RFD rfd;
    EnterCriticalSection(&mutex);
    if (RFDRecyclePool.empty() == false) {
        rfd = RFDRecyclePool.front();
        RFDRecyclePool.pop();
    } else {
        // We need to make sure a rfd is a unique value ragardless it's a socket or a crt fd
        rfd = (int) RFDToSocketInfoMap.size() + (int) RFDToCrtFDInfoMap.size();
    }
    LeaveCriticalSection(&mutex);
    
    return rfd;
}

RFD RFDMap::addSocket(SOCKET s) {
    RFD rfd = -1;
    EnterCriticalSection(&mutex);
    map<SOCKET, RFD>::iterator iter = SocketToRFDMap.find(s);
    if (iter == SocketToRFDMap.end()) {
        RFD_INFO rfd_info;
        rfd_info.flags = 0;
        rfd_info.type = RFD_TYPE::SOCKET;
        rfd_info.value.socket = s;
        rfd = getNextRFDAvailable();
        RFDToSocketInfoMap[rfd] = rfd_info;
        SocketToRFDMap[s] = rfd;
    } else {
        rfd = iter->second;
    }
    LeaveCriticalSection(&mutex);
    
    return rfd;
}

RFD RFDMap::addCrtFD(int crtFD) {
    RFD rfd = -1;
    EnterCriticalSection(&mutex);
    map<int, RFD>::iterator iter = CrtFDToRFDMap.find(crtFD);
    if (iter == CrtFDToRFDMap.end()) {
        RFD_INFO rfd_info;
        rfd_info.flags = 0;
        rfd_info.type = RFD_TYPE::CRTFD;
        rfd_info.value.crtFD = crtFD;
        rfd = getNextRFDAvailable();
        RFDToCrtFDInfoMap[rfd] = rfd_info;
        CrtFDToRFDMap[crtFD] = rfd;
    } else {
        rfd = iter->second;
    }
    LeaveCriticalSection(&mutex);

    return rfd;
}

void RFDMap::removeRFD(RFD rfd) {
    // stdin, stderr and stdout should never be removed
    if (rfd > 2) {
        EnterCriticalSection(&mutex);
        map<RFD, RFD_INFO>::iterator iter = RFDToSocketInfoMap.find(rfd);
        if (iter != RFDToSocketInfoMap.end()) {
            SocketToRFDMap.erase(iter->second.value.socket);
            RFDToSocketInfoMap.erase(rfd);
            RFDRecyclePool.push(rfd);
        } else {
            iter = RFDToCrtFDInfoMap.find(rfd);
            if (iter != RFDToCrtFDInfoMap.end()) {
                CrtFDToRFDMap.erase(iter->second.value.crtFD);
                RFDToCrtFDInfoMap.erase(rfd);
                RFDRecyclePool.push(rfd);
            }
        }
        LeaveCriticalSection(&mutex);
    }
}

void RFDMap::removeCrtRFD(RFD rfd) {
    // stdin, stderr and stdout should never be removed
    if (rfd > 2) {
        EnterCriticalSection(&mutex);
        map<RFD, RFD_INFO>::iterator iter = RFDToCrtFDInfoMap.find(rfd);
        if (iter != RFDToCrtFDInfoMap.end()) {
            CrtFDToRFDMap.erase(iter->second.value.crtFD);
            RFDToCrtFDInfoMap.erase(rfd);
            RFDRecyclePool.push(rfd);
        }
        LeaveCriticalSection(&mutex);
    }
}

RFD_INFO RFDMap::GetRFDInfo(RFD rfd) {
    RFD_INFO rfd_info;
    rfd_info.type = RFD_TYPE::INVALID;
    EnterCriticalSection(&mutex);
    if (RFDToSocketInfoMap.find(rfd) != RFDToSocketInfoMap.end()) {
        rfd_info = RFDToSocketInfoMap[rfd];
    } else if (RFDToCrtFDInfoMap.find(rfd) != RFDToCrtFDInfoMap.end()) {
        rfd_info = RFDToCrtFDInfoMap[rfd];
    }
    LeaveCriticalSection(&mutex);
    return rfd_info;
}

bool RFDMap::SetRFDInfo(RFD rfd, RFD_INFO rfd_info) {
    bool retVal = false;
    EnterCriticalSection(&mutex);
    if (rfd_info.type == RFD_TYPE::SOCKET) {
        if (RFDToSocketInfoMap.find(rfd) != RFDToSocketInfoMap.end()) {
            RFDToSocketInfoMap[rfd] = rfd_info;
            retVal = true;
        }
    } else if (rfd_info.type == RFD_TYPE::CRTFD) {
        if (RFDToCrtFDInfoMap.find(rfd) != RFDToCrtFDInfoMap.end()) {
            RFDToCrtFDInfoMap[rfd] = rfd_info;
            retVal = true;
        }
    }
    LeaveCriticalSection(&mutex);
    return retVal;
}

SOCKET RFDMap::lookupSocket(RFD rfd) {
    SOCKET socket = INVALID_SOCKET;
    RFD_INFO rfd_info = GetRFDInfo(rfd);
    if (rfd_info.type == RFD_TYPE::SOCKET) {
        socket = rfd_info.value.socket;
    }
    return socket;
}

int RFDMap::lookupCrtFD(RFD rfd) {
    int crtFD = -1;
    RFD_INFO rfd_info = GetRFDInfo(rfd);
    if (rfd_info.type == RFD_TYPE::CRTFD) {
        crtFD = rfd_info.value.crtFD;
    }
    return crtFD;
}
