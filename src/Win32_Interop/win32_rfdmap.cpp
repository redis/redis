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
#include "Win32_Assert.h"

RFDMap& RFDMap::getInstance() {
    static RFDMap instance; // Instantiated on first use. Guaranteed to be destroyed.
    return instance;
}

RFDMap::RFDMap() {
    InitializeCriticalSection(&mutex);
    // stdin, assigned rfd = 0
    addCrtFD(0);
    // stdout, assigned rfd = 1
    addCrtFD(1);
    // stderr, assigned rfd = 2
    addCrtFD(2);
}

RFD RFDMap::getNextRFDAvailable() {
    RFD rfd;
    EnterCriticalSection(&mutex);
    if (RFDRecyclePool.empty() == false) {
        rfd = RFDRecyclePool.front();
        RFDRecyclePool.pop();
    } else {
        if (next_available_rfd < INT_MAX) {
            rfd = RFDMap::next_available_rfd++;
        } else {
            rfd = INVALID_FD;
        }
    }
    LeaveCriticalSection(&mutex);
    return rfd;
}

RFD RFDMap::addSocket(SOCKET s) {
    RFD rfd;
    EnterCriticalSection(&mutex);
    if (SocketToRFDMap.find(s) != SocketToRFDMap.end()) {
        rfd = INVALID_FD;
    } else {
        rfd = getNextRFDAvailable();
        if (rfd != INVALID_FD) {
            SocketToRFDMap[s] = rfd;

            SocketInfo socket_info;
            socket_info.socket = s;
            socket_info.state = NULL;
            socket_info.flags = 0;
            memset(&(socket_info.socketAddrStorage), 0, sizeof(SOCKADDR_STORAGE));
            RFDToSocketInfoMap[rfd] = socket_info;
        }
    }
    LeaveCriticalSection(&mutex);
    return rfd;
}

void RFDMap::removeSocketToRFD(SOCKET s) {
    EnterCriticalSection(&mutex);
    SocketToRFDMap.erase(s);
    LeaveCriticalSection(&mutex);
}

void RFDMap::removeRFDToSocketInfo(RFD rfd) {
    EnterCriticalSection(&mutex);
    RFDToSocketInfoMap.erase(rfd);
    RFDRecyclePool.push(rfd);
    LeaveCriticalSection(&mutex);
}

RFD RFDMap::addCrtFD(int crt_fd) {
    RFD rfd;
    EnterCriticalSection(&mutex);
    if (CrtFDToRFDMap.find(crt_fd) != CrtFDToRFDMap.end()) {
        rfd = CrtFDToRFDMap[crt_fd];
    } else {
        rfd = getNextRFDAvailable();
        if (rfd != INVALID_FD) {
            CrtFDToRFDMap[crt_fd] = rfd;
            RFDToCrtFDMap[rfd] = crt_fd;
        }
    }
    LeaveCriticalSection(&mutex);
    return rfd;
}

void RFDMap::removeCrtFD(int crt_fd) {
    // crt_fd between FIRST_RESERVED_RFD_INDEX and LAST_RESERVED_RFD_INDEX
    // should never be removed.
    ASSERT(FIRST_RESERVED_RFD_INDEX == 0);
    if (crt_fd > RFDMap::LAST_RESERVED_RFD_INDEX) {
        EnterCriticalSection(&mutex);
        map<int, RFD>::iterator mit = CrtFDToRFDMap.find(crt_fd);
        if (mit != CrtFDToRFDMap.end()) {
            RFD rfd = (*mit).second;
            RFDRecyclePool.push(rfd);
            RFDToCrtFDMap.erase(rfd);
            CrtFDToRFDMap.erase(crt_fd);
        }
        LeaveCriticalSection(&mutex);
    }
}

SOCKET RFDMap::lookupSocket(RFD rfd) {
    SOCKET socket = INVALID_SOCKET;
    EnterCriticalSection(&mutex);
    if (RFDToSocketInfoMap.find(rfd) != RFDToSocketInfoMap.end()) {
        socket = RFDToSocketInfoMap[rfd].socket;
    }
    LeaveCriticalSection(&mutex);
    return socket;
}

SocketInfo* RFDMap::lookupSocketInfo(RFD rfd) {
    SocketInfo* socket_info = NULL;
    EnterCriticalSection(&mutex);
    if (RFDToSocketInfoMap.find(rfd) != RFDToSocketInfoMap.end()) {
        socket_info = &RFDToSocketInfoMap[rfd];
    }
    LeaveCriticalSection(&mutex);
    return socket_info;
}

int RFDMap::lookupCrtFD(RFD rfd) {
    int crt_fd = INVALID_FD;
    EnterCriticalSection(&mutex);
    if (RFDToCrtFDMap.find(rfd) != RFDToCrtFDMap.end()) {
        crt_fd = RFDToCrtFDMap[rfd];
    } else if (rfd >= RFDMap::FIRST_RESERVED_RFD_INDEX
        && rfd <= RFDMap::LAST_RESERVED_RFD_INDEX) {
        crt_fd = rfd;
    }
    LeaveCriticalSection(&mutex);
    return crt_fd;
}
