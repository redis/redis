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

#include "win32fixes.h"
#include "win32_socketmap.h"
#include <map>
#include <queue>
using namespace std;

extern "C" {
#include "redisLog.h"
}

typedef map<SOCKET,int> SocketToFDMapType;
typedef map<int,SOCKET> FDToSocketMapType;
typedef queue<int> FDRecyclePoolType;
typedef SocketToFDMapType::iterator S2FDIterator;
typedef FDToSocketMapType::iterator FD2SIterator;

/* In UNIX File Descriptors increment by one for each new one. Windows handles 
 * do not follow the same rule.  Additionally UNIX uses a 32-bit int to 
 * represent a FD while Windows_x64 uses a 64-bit value to represent a handle.
 * There is no documented guarantee that a Windows SOCKET value will be 
 * entirely constrained in 32-bits (though it seems to be currently). SOCKETs
 * should be treated as an opaque value and not be cast to a 32-bit int. In 
 * order to not break existing code that relies on the maximum FD value to 
 * indicate the number of handles that have been created (and other UNIXisms),
 * this code maps SOCKET handles to a virtual FD number starting at 3 (0,1 and
 * 2 are reserved for stdin, stdout and stderr).
 */
class SocketMap {
public:
    static SocketMap& getInstance() {
        static SocketMap    instance; // Instantiated on first use. Guaranteed to be destroyed.
        return instance;
    }
private:
    SocketMap() {};                   
    SocketMap(SocketMap const&);	  // Don't implement to guarantee singleton semantics
    void operator=(SocketMap const&); // Don't implement to guarantee singleton semantics

private:
	SocketToFDMapType SocketToFDMap;
	FDToSocketMapType FDToSocketMap;
	FDRecyclePoolType FDRecyclePool;

public:
	const static int minFD = 3;
	const static int invalidFD = -1;

private:
	/* Gets the next available File Descriptor. File Descriptors are always
	   non-negative integers, with the first three being reserved for stdin(0),
	   stdout(1) and stderr(2). */
	int getNextFDAvailable() {
		if( FDRecyclePool.empty() == false ) {
			int FD = FDRecyclePool.front();
			FDRecyclePool.pop();
			return FD;
		} else {
			return (int)(minFD + SocketToFDMap.size());
		}
	}

public:
	/* Adds a socket to the socket map. Returns the file descriptor value for
	   the socket. Returns invalidFD if the socket is already added to the
	   collection. */
	int addSocket(SOCKET s) {
		if (SocketToFDMap.find(s) != SocketToFDMap.end()) {
            redisLog( REDIS_DEBUG, "SocketMap::addSocket() - socket already exists!" );
			return invalidFD;
		}

		int FD = getNextFDAvailable();
		SocketToFDMap[s] = FD;
		FDToSocketMap[FD] = s;
		return FD;
	}

	/* Removes a socket from the list of sockets. Also removes the associated 
	   file descriptor. */
	void removeSocket(SOCKET s) {	
		S2FDIterator smit = SocketToFDMap.find(s);
        if(smit == SocketToFDMap.end()) {
            redisLog( REDIS_DEBUG, "SocketMap::removeSocket() - failed to find socket!" );
        }
		int FD = (*smit).second;
		FDRecyclePool.push(FD);
		SocketToFDMap.erase(s);
	}

	/* Returns the socket associated with a file descriptor. */
	SOCKET lookupSocket(int FD) {
		if (FDToSocketMap.find(FD)  != FDToSocketMap.end()) {
			return FDToSocketMap[FD];
		} else {
			redisLog( REDIS_DEBUG, "SocketMap::lookupSocket() - failed to find socket!" );
			return invalidFD;
		}
	}

	/* Returns the file descriptor associated with a socket. */
	int lookupFD(SOCKET s) {
		if (SocketToFDMap.find(s) != SocketToFDMap.end()) {
			return SocketToFDMap[s];
		} else {
			redisLog( REDIS_DEBUG, "SocketMap::lookupFD() - failed to find FD!" );
			return invalidFD;
		}
	}

	/* Returns the number of socket->FD mappings allocated. */
	int getCount() {
		return (int)(SocketToFDMap.size());
	}
};

extern "C" {
	int smAddSocket(SOCKET s) {
		return SocketMap::getInstance().addSocket(s);
	}

	void smRemoveSocket(SOCKET s) {
		SocketMap::getInstance().removeSocket(s);
	}

	SOCKET smLookupSocket(int fd) {
		return SocketMap::getInstance().lookupSocket(fd);
	}

	int smLookupFD(SOCKET s) {
		return SocketMap::getInstance().lookupFD(s);
	}

	int smGetSocketCount() {
		return SocketMap::getInstance().getCount();
	}
}