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
#include <map>
#include <queue>

using namespace std;

typedef int RFD; // Redis File Descriptor, just an index in the SocketOrCrtFD_To_RFD map

enum class RFD_TYPE { SOCKET, CRTFD, INVALID };

union RFD_VALUE {
    SOCKET socket;
    int    crtFD;
};

typedef struct {
    int flags;
    RFD_TYPE type;
    RFD_VALUE value;
} RFD_INFO;

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
    RFDMap(RFDMap const&);         // Don't implement to guarantee singleton semantics
    void operator=(RFDMap const&); // Don't implement to guarantee singleton semantics

private:
    map<SOCKET, RFD>    SocketToRFDMap;
    map<RFD, RFD_INFO>  RFDToSocketInfoMap;

    map<int, RFD>       CrtFDToRFDMap;
    map<RFD, RFD_INFO>  RFDToCrtFDInfoMap;

    queue<int> RFDRecyclePool;

private:
    CRITICAL_SECTION mutex;

private:
    /* Gets the next available Redis File Descriptor. Redis File Descriptors
       are always non-negative integers, with the first three being reserved
       for stdin(0), stdout(1) and stderr(2). */
    RFD getNextRFDAvailable();

public:
    /* Adds a socket to the socket map and returns the RFD value for the socket.
       If the socket already exists, returns the RFD.
    */
    RFD addSocket(SOCKET s);

    /* Adds a fd obtained with low-level CRT file/pipe functions and returns the
       RFD value for the CrtFD. If the crtFD already exists returns the RFD. */
    RFD addCrtFD(int crtFD);

    /* Removes a generic RFD from the list of RFDs (SOCKET or CRTFD). */
    void removeRFD(RFD rfd);
    
    /* Removes a RFD from the list of the CRTFDs */
    void removeCrtRFD(RFD rfd);

    /* Gets the RFD_INFO data associated with a RFD */
    RFD_INFO GetRFDInfo(RFD rfd);

    /* Sets the RFD_INFO data associated with a RFD */
    bool SetRFDInfo(RFD rfd, RFD_INFO rfd_info);

    /* Returns the SOCKET associated with a RFD. */
    SOCKET lookupSocket(RFD rfd);

    /* Returns the CRTFD associated with a RFD. */
    int lookupCrtFD(RFD rfd);
};
