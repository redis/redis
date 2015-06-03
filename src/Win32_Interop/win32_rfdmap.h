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

#pragma once

#include <errno.h>

#define INCL_WINSOCK_API_PROTOTYPES 0 // Important! Do not include Winsock API definitions to avoid conflicts with API entry points defnied below.
#include <WinSock2.h>
#include "ws2tcpip.h"

//#include <io.h>

#include <map>
#include <queue>
using namespace std;

typedef struct {
     bool IsBlockingSocket;
} RedisSocketState;

typedef int RFD;   // Redis File Descriptor
typedef map<SOCKET,RFD> SocketToRFDMapType;
typedef map<SOCKET,RedisSocketState> SocketToStateMapType;
typedef map<int,RFD> PosixFDToRFDMapType;
typedef map<RFD,SOCKET> RFDToSocketMapType;
typedef map<RFD,int> RFDToPosixFDMapType;
typedef queue<RFD> RFDRecyclePoolType;
typedef SocketToRFDMapType::iterator S2RFDIterator;
typedef SocketToStateMapType::iterator S2StateIterator;
typedef PosixFDToRFDMapType::iterator PosixFD2RFDIterator;
typedef RFDToSocketMapType::iterator RFD2SIterator;
typedef RFDToPosixFDMapType::iterator RFD2PosixFDIterator;


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
class RFDMap {
public:
    static RFDMap& getInstance();

private:
    RFDMap();
    RFDMap(RFDMap const&);	  // Don't implement to guarantee singleton semantics
    void operator=(RFDMap const&); // Don't implement to guarantee singleton semantics

private:
	SocketToRFDMapType SocketToRFDMap;
	SocketToStateMapType SocketToStateMap;
    PosixFDToRFDMapType PosixFDToRFDMap;
	RFDToSocketMapType RFDToSocketMap;
	RFDToPosixFDMapType RFDToPosixFDMap;
	RFDRecyclePoolType RFDRecyclePool;

public:
	const static int minRFD = 3;    // 0, 1 and 2 are reserved for stdin, stdout and stderr
    RFD maxRFD;
	const static int invalidRFD = -1;

private:
	/* Gets the next available Redis File Descriptor. Redis File Descriptors are always
	   non-negative integers, with the first three being reserved for stdin(0),
	   stdout(1) and stderr(2). */
	RFD getNextRFDAvailable();

public:
	/* Adds a socket to the socket map. Returns the redis file descriptor value for
	   the socket. Returns invalidRFD if the socket is already added to the
	   collection. */
	RFD addSocket(SOCKET s);

	/* Removes a socket from the list of sockets. Also removes the associated 
	   file descriptor. */
	void removeSocket(SOCKET s);

	/* Adds a posixFD (used with low-level CRT posix file functions) to the posixFD map. Returns
       the redis file descriptor value for the posixFD. Returns invalidRFD if the posicFD is already
       added to the collection. */
	RFD addPosixFD(int posixFD);

	/* Removes a socket from the list of sockets. Also removes the associated 
	   file descriptor. */
	void removePosixFD(int posixFD);

	/* Returns the socket associated with a file descriptor. */
	SOCKET lookupSocket(RFD rfd);

    /* Returns the socket associated with a file descriptor. */
	int lookupPosixFD(RFD rfd);

	/* Returns the RFD associated with a socket. */
	RFD lookupRFD(SOCKET s);

	/* Returns the RFD associated with a posix FD. */
	RFD lookupRFD(int posixFD);

	/* Returns the smallest RFD available */
	RFD getMinRFD();

    /* Returns the largest FD allocated so far */
    RFD getMaxRFD();

    bool SetSocketState( SOCKET s, RedisSocketState state );
    bool GetSocketState( SOCKET s, RedisSocketState& state );
};
