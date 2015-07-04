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

RFDMap::RFDMap() {
    InitializeCriticalSection(&mutex);
    maxRFD = minRFD;
}

RFD RFDMap::getNextRFDAvailable() {
    RFD rfd;
    EnterCriticalSection(&mutex);
    if (RFDRecyclePool.empty() == false) {
        rfd = RFDRecyclePool.front();
        RFDRecyclePool.pop();
    } else {
        maxRFD = minRFD + (int) SocketToRFDMap.size() + (int) PosixFDToRFDMap.size();
        rfd = maxRFD;
    }
    LeaveCriticalSection(&mutex);
    return rfd;
}

RFD RFDMap::addSocket(SOCKET s) {
    RFD rfd;
    EnterCriticalSection(&mutex);
    if (SocketToRFDMap.find(s) != SocketToRFDMap.end()) {
        rfd = invalidRFD;
    } else {
        rfd = getNextRFDAvailable();
        SocketToRFDMap[s] = rfd;
        RFDToSocketMap[rfd] = s;
    }
    LeaveCriticalSection(&mutex);
    return rfd;
}

void RFDMap::removeSocket(SOCKET s) {
    EnterCriticalSection(&mutex);
    S2RFDIterator mit = SocketToRFDMap.find(s);
    if (mit != SocketToRFDMap.end()) {
        RFD rfd = (*mit).second;
        RFDRecyclePool.push(rfd);
        RFDToSocketMap.erase(rfd);
        SocketToRFDMap.erase(s);
    }
    LeaveCriticalSection(&mutex);
}

RFD RFDMap::addPosixFD(int posixFD) {
    RFD rfd;
    EnterCriticalSection(&mutex);
    if (PosixFDToRFDMap.find(posixFD) != PosixFDToRFDMap.end()) {
        rfd = invalidRFD;
    } else {
        rfd = getNextRFDAvailable();
        PosixFDToRFDMap[posixFD] = rfd;
        RFDToPosixFDMap[rfd] = posixFD;
    }
    LeaveCriticalSection(&mutex);
    return rfd;
}

void RFDMap::removePosixFD(int posixFD) {
    EnterCriticalSection(&mutex);
    PosixFD2RFDIterator mit = PosixFDToRFDMap.find(posixFD);
    if (mit != PosixFDToRFDMap.end()) {
        RFD rfd = (*mit).second;
        RFDRecyclePool.push(rfd);
        RFDToPosixFDMap.erase(rfd);
        PosixFDToRFDMap.erase(posixFD);
    }
    LeaveCriticalSection(&mutex);
}

SOCKET RFDMap::lookupSocket(RFD rfd) {
    SOCKET socket = INVALID_SOCKET;
    EnterCriticalSection(&mutex);
    if (RFDToSocketMap.find(rfd) != RFDToSocketMap.end()) {
        socket = RFDToSocketMap[rfd];
    }
    LeaveCriticalSection(&mutex);
    return socket;
}

int RFDMap::lookupPosixFD(RFD rfd) {
    int posixFD = -1;
    EnterCriticalSection(&mutex);
    if (RFDToPosixFDMap.find(rfd) != RFDToPosixFDMap.end()) {
        posixFD = RFDToPosixFDMap[rfd];
    } else if (rfd >= 0 && rfd <= 2) {
        posixFD = rfd;
    }
    LeaveCriticalSection(&mutex);
    return posixFD;
}

bool RFDMap::SetSocketState(SOCKET s, RedisSocketState state) {
    bool result = false;
    EnterCriticalSection(&mutex);
    S2StateIterator sit = SocketToStateMap.find(s);
    if (sit != SocketToStateMap.end()) {
        SocketToStateMap[s] = state;
        result = true;
    }
    LeaveCriticalSection(&mutex);
    return result;
}

bool RFDMap::GetSocketState(SOCKET s, RedisSocketState& state) {
    bool result = false;
    EnterCriticalSection(&mutex);
    S2StateIterator sit = SocketToStateMap.find(s);
    if (sit != SocketToStateMap.end()) {
        state = SocketToStateMap[s];
        result = true;
    }
    LeaveCriticalSection(&mutex);
    return result;
}
