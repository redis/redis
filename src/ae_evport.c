/* ae.c module for illumos event ports.
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 * Released under the BSD license. See the COPYING file for more info. */

#include <assert.h>
#include <errno.h>
#include <port.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/time.h>

#include <stdio.h>

int evport_debug = 0;

/*
 * This file implements the ae API using event ports, present on Solaris-based
 * systems since Solaris 10.  Using the event port interface, we associate file
 * descriptors with the port.  Each association also includes the set of poll(2)
 * events that the consumer is interested in (e.g., POLLIN and POLLOUT).
 *
 * There's one tricky piece to this implementation: when we return events via
 * aeApiPoll, the corresponding file descriptor becomes dissociated from the
 * port.  This is necessary because poll events are level-triggered, so if the
 * fd didn't become dissociated, it would immediately fire another event since
 * the underlying state hasn't changed yet.  We must reassociate the file
 * descriptor, but only after we know that our caller has actually read from it.
 * The ae API does not tell us exactly when that happens, but we do know that
 * it must happen by the time aeApiPoll is called again.  Our solution is to
 * keep track of the last fd returned by aeApiPoll and reassociate it next time
 * aeApiPoll is invoked.
 *
 * To summarize, in this module, each fd association is EITHER (a) represented
 * only via the in-kernel assocation OR (b) represented by pending_fd and
 * pending_mask.  (b) is only true for the last fd we returned from aeApiPoll,
 * and only until we enter aeApiPoll again (at which point we restore the
 * in-kernel association).
 *
 * We currently only return one fd event per call to aeApiPoll.  This could be
 * extended to return more than one by extending the corresponding pending
 * fields and using port_getn().
 */
typedef struct aeApiState {
    int     portfd;             /* event port */
    int     pending_fd;         /* pending fd */
    int     pending_mask;       /* pending fd's mask */
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    state->portfd = port_create();
    if (state->portfd == -1) {
        zfree(state);
        return -1;
    }

    state->pending_fd = -1;
    state->pending_mask = AE_NONE;

    eventLoop->apidata = state;
    return 0;    
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->portfd);
    zfree(state);
}

/*
 * Helper function to invoke port_associate for the given fd and mask.
 */
static int aeApiAssociate(const char *where, int portfd, int fd, int mask)
{
    int events = 0;
    int rv, err;

    if (mask & AE_READABLE)
        events |= POLLIN;
    if (mask & AE_WRITABLE)
        events |= POLLOUT;

    if (evport_debug)
        fprintf(stderr, "%s: port_associate(%d, 0x%x) = ", where, fd, events);

    rv = port_associate(portfd, PORT_SOURCE_FD, fd, events, (void *)mask);
    err = errno;

    if (evport_debug)
        fprintf(stderr, "%d (%s)\n", rv, rv == 0 ? "no error" : strerror(err));

    if (rv == -1) {
        fprintf(stderr, "%s: port_associate: %s\n", where, strerror(err));

        if (err == EAGAIN)
            fprintf(stderr, "aeApiAssociate: event port limit exceeded.");
    }

    return rv;
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    int fullmask;

    if (evport_debug)
        fprintf(stderr, "aeApiAddEvent: fd %d mask 0x%x\n", fd, mask);

    /*
     * Since port_associate's "events" argument replaces any existing events, we
     * must be sure to include whatever events are already associated when
     * we call port_associate() again.
     */
    fullmask = mask | eventLoop->events[fd].mask;

    if (fd == state->pending_fd) {
        /*
         * This fd was recently returned from aeApiPoll.  It should be safe to
         * assume that the consumer has processed that poll event, but we play
         * it safer by simply updating pending_mask.  The fd will be
         * reassociated as usual when aeApiPoll is called again.
         */
        if (evport_debug)
            fprintf(stderr, "aeApiAddEvent: adding to pending fd %d\n", fd);
        state->pending_mask |= fullmask;
        return 0;
    }

    return (aeApiAssociate("aeApiAddEvent", state->portfd, fd, fullmask));
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    int fullmask;

    if (evport_debug)
        fprintf(stderr, "del fd %d mask 0x%x\n", fd, mask);

    if (fd == state->pending_fd) {
        if (evport_debug)
            fprintf(stderr, "deleting event from pending fd %d\n", fd);

        /*
         * This fd was just returned from aeApiPoll, so it's not currently
         * associated with the port.  All we need to do is update
         * pending_mask appropriately.
         */
        state->pending_mask &= ~mask;

        if (state->pending_mask == AE_NONE)
            state->pending_fd = -1;

        return;
    }

    /*
     * The fd is currently associated with the port.  Like with the add case
     * above, we must look at the full mask for the file descriptor before
     * updating that association.  We don't have a good way of knowing what the
     * events are without looking into the eventLoop state directly.  We rely on
     * the fact that our caller has already updated the mask in the eventLoop.
     */

    fullmask = eventLoop->events[fd].mask;
    if (fullmask == AE_NONE) {
        /*
         * We're removing *all* events, so use port_dissociate to remove the
         * association completely.  Failure here indicates a bug.
         */
        if (evport_debug)
            fprintf(stderr, "aeApiDelEvent: port_dissociate(%d)\n", fd);

        if (port_dissociate(state->portfd, PORT_SOURCE_FD, fd) != 0) {
            perror("aeApiDelEvent: port_dissociate");
            abort(); /* will not return */
        }
    } else if (aeApiAssociate("aeApiDelEvent", state->portfd, fd,
        fullmask) != 0) {
        /*
         * ENOMEM is a potentially transient condition, but the kernel won't
         * generally return it unless things are really bad.  EAGAIN indicates
         * we've reached an resource limit, for which it doesn't make sense to
         * retry (counterintuitively).  All other errors indicate a bug.  In any
         * of these cases, the best we can do is to abort.
         */
        abort(); /* will not return */
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    struct timespec timeout, *tsp;
    int mask;
    port_event_t event;

    /*
     * If we've returned an fd event before, we must reassociated that fd with
     * the port now, before calling port_get().  See the block comment at the
     * top of this file for an explanation of why.
     */
    if (state->pending_mask != AE_NONE) {
        if (aeApiAssociate("aeApiPoll", state->portfd, state->pending_fd,
            state->pending_mask) != 0) {
            /* See aeApiDelEvent for why this case is fatal. */
            abort();
        }

        state->pending_mask = AE_NONE;
        state->pending_fd = -1;
    }

    if (tvp != NULL) {
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        tsp = &timeout;
    } else {
        tsp = NULL;
    }

    if (port_get(state->portfd, &event, tsp) == -1) {
        if (errno == ETIME || errno == EINTR)
            return 0;

        /* Any other error indicates a bug. */
        perror("aeApiPoll: port_get");
        abort();
    }

    mask = 0;
    if (event.portev_events & POLLIN)
        mask |= AE_READABLE;
    if (event.portev_events & POLLOUT)
        mask |= AE_WRITABLE;

    eventLoop->fired[0].fd = event.portev_object;
    eventLoop->fired[0].mask = mask;

    if (evport_debug)
        fprintf(stderr, "aeApiPoll: fd %d mask 0x%x\n", event.portev_object,
            mask);

    state->pending_fd = event.portev_object;
    state->pending_mask = (uintptr_t)event.portev_user;

    return 1;
}

static char *aeApiName(void) {
    return "evport";
}
