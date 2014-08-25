#include <sys/poll.h>
#include <sys/pollset.h>

typedef struct aeApiState {
    pollset_t psfd;
    struct pollfd *events;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct pollfd)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->psfd = pollset_create(1024); /* 1024 is just a hint for the kernel */
    if (state->psfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct pollfd)*setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    pollset_destroy(state->psfd);
    zfree(state->events);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct poll_ctl ee;
    /* If the fd was already monitored for some event, we need a MOD
* operation. Otherwise we need an ADD operation. */
    ee.cmd = eventLoop->events[fd].mask == AE_NONE ?
            PS_ADD : PS_MOD;

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    if (mask & AE_READABLE) ee.events |= POLLIN;
    if (mask & AE_WRITABLE) ee.events |= POLLOUT;
    ee.fd = fd;
    if (pollset_ctl(state->psfd,&ee,1) == -1) return -1;
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct poll_ctl ee;
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= POLLIN;
    if (mask & AE_WRITABLE) ee.events |= POLLOUT;
    ee.fd = fd;
    if (mask != AE_NONE) {
ee.cmd = PS_MOD;
        pollset_ctl(state->psfd,&ee,1);
    } else {
        /* Note, might require a non null event pointer even for
* PS_DELETE. */
ee.cmd = PS_DELETE;
        pollset_ctl(state->psfd,&ee,1);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    retval = pollset_poll(state->psfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct pollfd *e = state->events+j;

            if (e->revents & POLLIN) mask |= AE_READABLE;
            if (e->revents & POLLOUT) mask |= AE_WRITABLE;
            if (e->revents & POLLERR) mask |= AE_WRITABLE; 
            if (e->revents & POLLHUP) mask |= AE_WRITABLE; 
            eventLoop->fired[j].fd = e->fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "pollset";
}

