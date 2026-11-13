/* Kqueue(2)-based ae.c module
 *
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#define MAX_QUEUED_EVENTS 1024

typedef struct aeApiState {
    int kqfd;
    struct kevent *events;
    /* changes is used to buffer incoming events that will be
     * registered in bulk via kevent(2). */
    struct kevent changes[MAX_QUEUED_EVENTS];
    unsigned int num_changes;

    /* Events mask for merge read and write event.
     * To reduce memory consumption, we use 2 bits to store the mask
     * of an event, so that 1 byte will store the mask of 4 events. */
    char *eventsMask;
} aeApiState;

#define EVENT_MASK_MALLOC_SIZE(sz) (((sz) + 3) / 4)
#define EVENT_MASK_OFFSET(fd) ((fd) % 4 * 2)
#define EVENT_MASK_ENCODE(fd, mask) (((mask) & 0x3) << EVENT_MASK_OFFSET(fd))

static inline int getEventMask(const char *eventsMask, int fd) {
    return (eventsMask[fd/4] >> EVENT_MASK_OFFSET(fd)) & 0x3;
}

static inline void addEventMask(char *eventsMask, int fd, int mask) {
    eventsMask[fd/4] |= EVENT_MASK_ENCODE(fd, mask);
}

static inline void resetEventMask(char *eventsMask, int fd) {
    eventsMask[fd/4] &= ~EVENT_MASK_ENCODE(fd, 0x3);
}

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct kevent)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    anetCloexec(state->kqfd);
    state->num_changes = 0;
    state->eventsMask = zmalloc(EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct kevent)*setsize);
    state->eventsMask = zrealloc(state->eventsMask, EVENT_MASK_MALLOC_SIZE(setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(setsize));
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);
    zfree(state->events);
    zfree(state->eventsMask);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    /* Instead of registering events to kqueue one by one, we buffer events and
     * register them at once along with retrieving pending events in aeApiPoll. */
    while (mask & AE_READABLE || mask & AE_WRITABLE) {
        if (mask & AE_READABLE) {
            mask &= ~AE_READABLE;
            EV_SET(state->changes + state->num_changes, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        } else if (mask & AE_WRITABLE) {
            mask &= ~AE_WRITABLE;
            EV_SET(state->changes + state->num_changes, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        }
        /* The current changelist is full, register it to kqueue now and
         * then rewind it to make room for follow-up events. */
        if (++state->num_changes == MAX_QUEUED_EVENTS) {
            if (kevent(state->kqfd, state->changes, state->num_changes, NULL, 0, NULL))
                /* An error occurs while processing an element of the changelist,
                 * this is unexpected and indicates somewhere went wrong.
                 * We panic for this situation directly because we won't be unable to
                 * learn about this failure later. */
                panic("aeApiAddEvent: kevent, %s", strerror(errno));
            state->num_changes = 0; /* rewind the changelist. */
        }
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    /* Due to the deferred nature of event registration, we may receive
     * deletion requests for events that are not registered in kqueue yet,
     * which could cause kevent(2) to fail and return ENOENT. Therefore, we
     * need to use aeEventLoop->events to mask out the events that are not
     * registered in kqueue and get the valid events which are requested to
     * be deleted. */
    int delmask = eventLoop->events[fd].mask & mask;

    /* Instead of applying events to kqueue one by one, we buffer events
     * and apply them at once. */
    while (delmask & AE_READABLE || delmask & AE_WRITABLE) {
        if (delmask & AE_READABLE) {
            delmask &= ~AE_READABLE;
            EV_SET(state->changes + state->num_changes, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        } else if (delmask & AE_WRITABLE) {
            delmask &= ~AE_WRITABLE;
            EV_SET(state->changes + state->num_changes, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        }
        /* The current changelist is full, apply it to kqueue now and
         * then rewind it to make room for follow-up events. */
        if (++state->num_changes == MAX_QUEUED_EVENTS) {
            if (kevent(state->kqfd, state->changes, state->num_changes, NULL, 0, NULL))
                panic("aeApiDelEvent: kevent, %s", strerror(errno));
            state->num_changes = 0; /* rewind the changelist. */
        }
    }

    /* When it comes to EV_DELETE events, we don't defer but apply them immediately
     * because the caller often closed the file descriptor right after they called
     * aeDeleteFileEvent(), and kevent(2) would report ENOENT or EBADF if the changelist
     * contained any closed file descriptors. */
    if (state->num_changes > 0) {
        if (kevent(state->kqfd, state->changes, state->num_changes, NULL, 0, NULL))
            panic("aeApiDelEvent: kevent, %s", strerror(errno));
        state->num_changes = 0; /* rewind the changelist. */
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    struct timespec ts, *timeout = NULL;
    if (tvp != NULL) {
        ts.tv_sec = tvp->tv_sec;
        ts.tv_nsec = tvp->tv_usec * 1000;
        timeout = &ts;
    }
    retval = kevent(state->kqfd, state->changes, state->num_changes, state->events, eventLoop->setsize,
                    timeout);
    state->num_changes = 0; /* rewind the changelist. */

    if (retval > 0) {
        int j;

        /* Normally we execute the read event first and then the write event.
         * When the barrier is set, we will do it reverse.
         *
         * However, under kqueue, read and write events would be separate
         * events, which would make it impossible to control the order of
         * reads and writes. So we store the event's mask we've got and merge
         * the same fd events later. */
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events+j;
            int fd = e->ident;
            int mask = 0;

            if (e->filter == EVFILT_READ) mask = AE_READABLE;
            else if (e->filter == EVFILT_WRITE) mask = AE_WRITABLE;
            addEventMask(state->eventsMask, fd, mask);
        }

        /* Re-traversal to merge read and write events, and set the fd's mask to
         * 0 so that events are not added again when the fd is encountered again. */
        numevents = 0;
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events+j;
            int fd = e->ident;
            int mask = getEventMask(state->eventsMask, fd);

            if (mask) {
                eventLoop->fired[numevents].fd = fd;
                eventLoop->fired[numevents].mask = mask;
                resetEventMask(state->eventsMask, fd);
                numevents++;
            }
        }
    } else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: kevent, %s", strerror(errno));
    }

    return numevents;
}

static char *aeApiName(void) {
    return "kqueue";
}
