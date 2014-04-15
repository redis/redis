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


#include "win32_rfdmap.h"

RFDMap& RFDMap::getInstance() {
    static RFDMap    instance; // Instantiated on first use. Guaranteed to be destroyed.
    return instance;
}

RFDMap::RFDMap() { maxRFD = minRFD; };                   

RFD RFDMap::getNextRFDAvailable() {
	if( RFDRecyclePool.empty() == false ) {
		int RFD = RFDRecyclePool.front();
		RFDRecyclePool.pop();
		return RFD;
	} else {
        maxRFD = minRFD + (int)SocketToRFDMap.size() + (int)PosixFDToRFDMap.size();
		return maxRFD;
	}
}

RFD RFDMap::addSocket(SOCKET s) {
	if (SocketToRFDMap.find(s) != SocketToRFDMap.end()) {
		return invalidRFD;
	}

	RFD rfd = getNextRFDAvailable();
	SocketToRFDMap[s] = rfd;
	RFDToSocketMap[rfd] = s;
	return rfd;
}

void RFDMap::removeSocket(SOCKET s) {	
	S2RFDIterator mit = SocketToRFDMap.find(s);
    if(mit == SocketToRFDMap.end()) {
//            redisLog( REDIS_DEBUG, "RFDMap::removeSocket() - failed to find socket!" );
        return;
    }
	RFD rfd = (*mit).second;
	RFDRecyclePool.push(rfd);
    RFDToSocketMap.erase(rfd);
	SocketToRFDMap.erase(s);
}

RFD RFDMap::addPosixFD(int posixFD) {
	if (PosixFDToRFDMap.find(posixFD) != PosixFDToRFDMap.end()) {
//      redisLog( REDIS_DEBUG, "RFDMap::addPosixFD() - posixFD already exists!" );
		return invalidRFD;
	}

	RFD rfd = getNextRFDAvailable();
	PosixFDToRFDMap[posixFD] = rfd;
	RFDToPosixFDMap[rfd] = posixFD;
	return rfd;
}

void RFDMap::removePosixFD(int posixFD) {	
	PosixFD2RFDIterator mit = PosixFDToRFDMap.find(posixFD);
    if(mit == PosixFDToRFDMap.end()) {
//      redisLog( REDIS_DEBUG, "RFDMap::removePosixFD() - failed to find posix FD!" );
        return;
    }
	RFD rfd = (*mit).second;
	RFDRecyclePool.push(rfd);
    RFDToPosixFDMap.erase(rfd);
	PosixFDToRFDMap.erase(posixFD);
}

SOCKET RFDMap::lookupSocket(RFD rfd) {
	if (RFDToSocketMap.find(rfd)  != RFDToSocketMap.end()) {
		return RFDToSocketMap[rfd];
	} else {
//      redisLog( REDIS_DEBUG, "RFDMap::lookupSocket() - failed to find socket!" );
		return INVALID_SOCKET;
	}
}

int RFDMap::lookupPosixFD(RFD rfd) {
	if (RFDToPosixFDMap.find(rfd)  != RFDToPosixFDMap.end()) {
		return RFDToPosixFDMap[rfd];
	} else if (rfd >= 0 && rfd <= 2) {
		return rfd;
	}
	else {
//   	redisLog( REDIS_DEBUG, "RFDMap::lookupPosixFD() - failed to find posix FD!" );
		return -1;
	}
}

RFD RFDMap::lookupRFD(SOCKET s) {
	if (SocketToRFDMap.find(s) != SocketToRFDMap.end()) {
		return SocketToRFDMap[s];
	} else {
//		redisLog( REDIS_DEBUG, "RFDMap::lookupFD() - failed to map SOCKET to RFD!" );
		return invalidRFD;
	}
}

RFD RFDMap::lookupRFD(int posixFD) {
	if (PosixFDToRFDMap.find(posixFD) != PosixFDToRFDMap.end()) {
		return PosixFDToRFDMap[posixFD];
	} else {
//		redisLog( REDIS_DEBUG, "RFDMap::lookupFD() - failed to map posixFD to RFD!" );
		return invalidRFD;
	}
}

RFD RFDMap::getMinRFD() {
	return minRFD;
}

RFD RFDMap::getMaxRFD() {
    return maxRFD;
}

bool RFDMap::SetSocketState( SOCKET s, RedisSocketState state )
{
    S2StateIterator sit = SocketToStateMap.find(s);
    if(sit != SocketToStateMap.end() ) {
        SocketToStateMap[s] = state;
        return true;
    } else {
        return false;
    }
}

bool RFDMap::GetSocketState( SOCKET s, RedisSocketState& state )
{
    S2StateIterator sit = SocketToStateMap.find(s);
    if(sit != SocketToStateMap.end() ) {
        state = SocketToStateMap[s];
        return true;
    } else {
        return false;
    }
}
