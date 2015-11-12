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

/* IOCP-based ae.c module  */

#include "win32_Interop/win32fixes.h"
#include "adlist.h"
#include "win32_Interop/win32_wsiocp.h"

#define MAX_COMPLETE_PER_POLL   100
#define MAX_SOCKET_LOOKUP       65535

/* Use GetQueuedCompletionStatusEx if possible.
 * Try to load the function pointer dynamically.
 * If it is not available, use GetQueuedCompletionStatus */
typedef BOOL(WINAPI *sGetQueuedCompletionStatusEx)
            (HANDLE CompletionPort,
            LPOVERLAPPED_ENTRY lpCompletionPortEntries,
            ULONG ulCount,
            PULONG ulNumEntriesRemoved,
            DWORD dwMilliseconds,
            BOOL fAlertable);
sGetQueuedCompletionStatusEx pGetQueuedCompletionStatusEx;

/* Structure that keeps state of sockets and Completion port handle */
typedef struct aeApiState {
    HANDLE iocp;
    int setsize;
    OVERLAPPED_ENTRY entries[MAX_COMPLETE_PER_POLL];
} aeApiState;

/* Find matching value in list and remove. If found return TRUE */
BOOL removeMatchFromList(list *requestlist, void *value) {
    listNode *node;
    if (requestlist == NULL) {
        return FALSE;
    }
    node = listFirst(requestlist);
    while (node != NULL) {
        if (listNodeValue(node) == value) {
            listDelNode(requestlist, node);
            return TRUE;
        }
        node = listNextNode(node);
    }
    return FALSE;
}

/* Called by ae to initialize state */
static int aeApiCreate(aeEventLoop *eventLoop) {
    HMODULE kernel32_module;
    aeApiState *state = (aeApiState *) CallocMemoryNoCOW(sizeof(aeApiState));

    if (!state) return -1;

    // Create a single IOCP to be shared by all sockets
    state->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
                                         NULL,
                                         0,
                                         1);
    if (state->iocp == NULL) {
        FreeMemoryNoCOW(state);
        return -1;
    }

    pGetQueuedCompletionStatusEx = NULL;
    kernel32_module = GetModuleHandleA("kernel32.dll");
    if (kernel32_module != NULL) {
        pGetQueuedCompletionStatusEx = (sGetQueuedCompletionStatusEx) GetProcAddress(
                                        kernel32_module,
                                        "GetQueuedCompletionStatusEx");
    }

    state->setsize = eventLoop->setsize;
    eventLoop->apidata = state;
    WSIOCP_Init(state->iocp);
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    ((aeApiState *) (eventLoop->apidata))->setsize = setsize;
    return 0;
}

/* Termination */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    CloseHandle(state->iocp);
    FreeMemoryNoCOW(state);
    WSIOCP_Cleanup();
}

/* Monitor state changes for a socket */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    iocpSockState *sockstate = WSIOCP_GetSocketState(fd);
    if (sockstate == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    if (mask & AE_READABLE) {
        sockstate->masks |= AE_READABLE;
        if ((sockstate->masks & CONNECT_PENDING) == 0) {
            if (sockstate->masks & LISTEN_SOCK) {
                // Actually a listen. Do not treat as read
            } else {
                if ((sockstate->masks & READ_QUEUED) == 0) {
                    // Queue up a 0 byte read
                    WSIOCP_QueueNextRead(fd);
                }
            }
        }
    }
    if (mask & AE_WRITABLE) {
        sockstate->masks |= AE_WRITABLE;
        if ((sockstate->masks & CONNECT_PENDING) == 0) {
            // If no write active, then need to queue write ready
            if (sockstate->wreqs == 0) {
                asendreq *areq = (asendreq *) CallocMemoryNoCOW(sizeof(asendreq));
                aeApiState *state = (aeApiState *) eventLoop->apidata;
                if (PostQueuedCompletionStatus(state->iocp,
                                               0,
                                               fd,
                                               &areq->ov) == 0) {
                    errno = GetLastError();
                    FreeMemoryNoCOW(areq);
                    return -1;
                }
                sockstate->wreqs++;
                listAddNodeTail(&sockstate->wreqlist, areq);
            }
        }
    }
    return 0;
}

/* Stop monitoring state changes for a socket */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    iocpSockState *sockstate = WSIOCP_GetExistingSocketState(fd);
    if (sockstate == NULL) {
        errno = WSAEINVAL;
        return;
    }

    if (mask & AE_READABLE) sockstate->masks &= ~AE_READABLE;
    if (mask & AE_WRITABLE) sockstate->masks &= ~AE_WRITABLE;
}

/* Return array of sockets that are ready for read or write
 * depending on the mask for each socket */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    iocpSockState *sockstate;
    ULONG j;
    int numevents = 0;
    ULONG numComplete = 0;
    BOOL rc;
    int mswait = (tvp == NULL) ? 100 : (tvp->tv_sec * 1000) + (tvp->tv_usec / 1000);

    if (pGetQueuedCompletionStatusEx != NULL) {
        // First get an array of completion notifications
        rc = pGetQueuedCompletionStatusEx(state->iocp,
                                          state->entries,
                                          MAX_COMPLETE_PER_POLL,
                                          &numComplete,
                                          mswait,
                                          FALSE);
    } else {
        // Need to get one at a time. Use first array element
        rc = GetQueuedCompletionStatus(state->iocp,
                                       &state->entries[0].dwNumberOfBytesTransferred,
                                       &state->entries[0].lpCompletionKey,
                                       &state->entries[0].lpOverlapped,
                                       mswait);
        if (!rc && state->entries[0].lpOverlapped == NULL) {
            // Timeout. Return.
            return 0;
        } else {
            // Check if more completions are ready
            BOOL lrc = TRUE;
            rc = TRUE;
            numComplete = 1;

            while (numComplete < MAX_COMPLETE_PER_POLL) {
                lrc = GetQueuedCompletionStatus(state->iocp,
                                                &state->entries[numComplete].dwNumberOfBytesTransferred,
                                                &state->entries[numComplete].lpCompletionKey,
                                                &state->entries[numComplete].lpOverlapped,
                                                0);
                if (lrc) {
                    numComplete++;
                } else {
                    if (state->entries[numComplete].lpOverlapped == NULL) break;
                }
            }
        }
    }

    if (rc && numComplete > 0) {
        LPOVERLAPPED_ENTRY entry = state->entries;
        for (j = 0; j < numComplete && numevents < state->setsize; j++, entry++) {
            // The competion key is the rfd identifying the socket 
            int rfd = (int) entry->lpCompletionKey;
            sockstate = WSIOCP_GetExistingSocketState(rfd);
            if (sockstate == NULL) {
                continue;
            }

            if ((sockstate->masks & CLOSE_PENDING) == FALSE) {
                if ((sockstate->masks & LISTEN_SOCK) && entry->lpOverlapped != NULL) {
                    // Need to set event for listening
                    aacceptreq *areq = (aacceptreq *) entry->lpOverlapped;
                    areq->next = sockstate->reqs;
                    sockstate->reqs = areq;
                    sockstate->masks &= ~ACCEPT_PENDING;
                    if (sockstate->masks & AE_READABLE) {
                        eventLoop->fired[numevents].fd = rfd;
                        eventLoop->fired[numevents].mask = AE_READABLE;
                        numevents++;
                    }
                } else if (sockstate->masks & CONNECT_PENDING) {
                    // Check if connect complete
                    if (entry->lpOverlapped == &sockstate->ov_read) {
                        sockstate->masks &= ~CONNECT_PENDING;
                        // Enable read and write events for this connection
                        aeApiAddEvent(eventLoop, rfd, sockstate->masks);
                    }
                } else {
                    BOOL matched = FALSE;
                    // Check if event is read complete (may be 0 length read)
                    if (entry->lpOverlapped == &sockstate->ov_read) {
                        matched = TRUE;
                        sockstate->masks &= ~READ_QUEUED;
                        if (sockstate->masks & AE_READABLE) {
                            eventLoop->fired[numevents].fd = rfd;
                            eventLoop->fired[numevents].mask = AE_READABLE;
                            numevents++;
                        }
                    } else if (sockstate->wreqs > 0 && entry->lpOverlapped != NULL) {
                        // Should be write complete. Get results
                        asendreq *areq = (asendreq *) entry->lpOverlapped;
                        matched = removeMatchFromList(&sockstate->wreqlist, areq);
                        if (matched == TRUE) {
                            // Call write complete callback so buffers can be freed
                            if (areq->proc != NULL) {
                                DWORD written = 0;
                                DWORD flags;
                                FDAPI_WSAGetOverlappedResult(rfd, &areq->ov, &written, FALSE, &flags);
                                areq->proc(areq->eventLoop, rfd, &areq->req, (int) written);
                            }
                            sockstate->wreqs--;
                            FreeMemoryNoCOW(areq);
                            // If no active write requests, set ready to write
                            if (sockstate->wreqs == 0 && sockstate->masks & AE_WRITABLE) {
                                eventLoop->fired[numevents].fd = rfd;
                                eventLoop->fired[numevents].mask = AE_WRITABLE;
                                numevents++;
                            }
                        }
                    }
                    if (matched == 0 && sockstate->unknownComplete == 0) {
                        sockstate->unknownComplete = 1;
                        close(rfd);
                    }
                }
            } else {
                if (sockstate->masks & CONNECT_PENDING) {
                    // Check if connect complete
                    if (entry->lpOverlapped == &sockstate->ov_read) {
                        sockstate->masks &= ~CONNECT_PENDING;
                    }
                } else if (entry->lpOverlapped == &sockstate->ov_read) {
                    // Read complete
                    sockstate->masks &= ~READ_QUEUED;
                } else {
                    // Check pending writes
                    asendreq *areq = (asendreq *) entry->lpOverlapped;
                    if (removeMatchFromList(&sockstate->wreqlist, areq)) {
                        sockstate->wreqs--;
                        FreeMemoryNoCOW(areq);
                    }
                }
                if (sockstate->wreqs == 0 &&
                    (sockstate->masks & (CONNECT_PENDING | READ_QUEUED | SOCKET_ATTACHED)) == 0) {
                    sockstate->masks &= ~(CLOSE_PENDING);
                    if (WSIOCP_CloseSocketState(sockstate)) {
                        FDAPI_ClearSocketInfo(rfd);
                    }
                }
            }
        }
    }
    return numevents;
}

/* Name of this event handler */
static char *aeApiName(void) {
    return "WinSock_IOCP";
}
