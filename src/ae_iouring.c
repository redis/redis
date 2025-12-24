/* io_uring based ae.c module
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <liburing.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

/* io_uring operation types for user_data encoding */
#define IOURING_OP_POLL_ADD     1
#define IOURING_OP_POLL_REMOVE  2

/* Encode fd and operation type into user_data */
#define IOURING_ENCODE_USERDATA(fd, op) (((uint64_t)(op) << 32) | (uint32_t)(fd))
#define IOURING_DECODE_FD(userdata)     ((int)((userdata) & 0xFFFFFFFF))
#define IOURING_DECODE_OP(userdata)     ((int)((userdata) >> 32))

typedef struct aeApiState {
    struct io_uring ring;
    int ring_fd;
    /* Track which fds have poll requests pending, to handle multishot rearm */
    int *poll_mask;     /* Current registered poll mask per fd */
    int poll_mask_size;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    /* Initialize io_uring with reasonable queue depth */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    /* Use SQPOLL for kernel-side polling if available and configured.
     * This reduces syscall overhead but requires CAP_SYS_NICE.
     * For now, we use standard mode for broader compatibility. */

    int ret = io_uring_queue_init_params(eventLoop->setsize * 2, &state->ring, &params);
    if (ret < 0) {
        zfree(state);
        errno = -ret;
        return -1;
    }

    state->ring_fd = state->ring.ring_fd;
    anetCloexec(state->ring_fd);

    /* Allocate poll mask tracking array */
    state->poll_mask_size = eventLoop->setsize;
    state->poll_mask = zmalloc(sizeof(int) * state->poll_mask_size);
    if (!state->poll_mask) {
        io_uring_queue_exit(&state->ring);
        zfree(state);
        return -1;
    }
    memset(state->poll_mask, 0, sizeof(int) * state->poll_mask_size);

    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    if (setsize <= state->poll_mask_size) return 0;

    int *new_poll_mask = zrealloc(state->poll_mask, sizeof(int) * setsize);
    if (!new_poll_mask) return -1;

    /* Initialize new slots to 0 */
    memset(new_poll_mask + state->poll_mask_size, 0,
           sizeof(int) * (setsize - state->poll_mask_size));

    state->poll_mask = new_poll_mask;
    state->poll_mask_size = setsize;

    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    io_uring_queue_exit(&state->ring);
    zfree(state->poll_mask);
    zfree(state);
}

/* Convert ae mask to poll events */
static unsigned aeMaskToPollEvents(int mask) {
    unsigned events = 0;
    if (mask & AE_READABLE) events |= POLLIN;
    if (mask & AE_WRITABLE) events |= POLLOUT;
    return events;
}

/* Convert poll events to ae mask */
static int pollEventsToAeMask(unsigned events) {
    int mask = 0;
    if (events & POLLIN) mask |= AE_READABLE;
    if (events & POLLOUT) mask |= AE_WRITABLE;
    if (events & POLLERR) mask |= AE_WRITABLE | AE_READABLE;
    if (events & POLLHUP) mask |= AE_WRITABLE | AE_READABLE;
    return mask;
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (fd >= state->poll_mask_size) {
        if (aeApiResize(eventLoop, fd + 1) == -1) return -1;
    }

    /* Calculate the new combined mask */
    int current_mask = state->poll_mask[fd];
    int new_mask = current_mask | mask;

    if (new_mask == current_mask) {
        /* No change needed */
        return 0;
    }

    /* If there's an existing poll, we need to update it.
     * With multishot poll, we cancel the old one and add a new one. */
    if (current_mask != 0) {
        /* Cancel existing poll */
        struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
        if (!sqe) {
            /* Queue full, try to submit and get a new sqe */
            io_uring_submit(&state->ring);
            sqe = io_uring_get_sqe(&state->ring);
            if (!sqe) return -1;
        }
        io_uring_prep_poll_remove(sqe, IOURING_ENCODE_USERDATA(fd, IOURING_OP_POLL_ADD));
        io_uring_sqe_set_data64(sqe, IOURING_ENCODE_USERDATA(fd, IOURING_OP_POLL_REMOVE));
    }

    /* Add new poll with updated mask */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        io_uring_submit(&state->ring);
        sqe = io_uring_get_sqe(&state->ring);
        if (!sqe) return -1;
    }

    unsigned poll_events = aeMaskToPollEvents(new_mask);

    /* Use multishot poll to avoid resubmitting after each event.
     * The poll will keep firing until explicitly canceled. */
    io_uring_prep_poll_multishot(sqe, fd, poll_events);
    io_uring_sqe_set_data64(sqe, IOURING_ENCODE_USERDATA(fd, IOURING_OP_POLL_ADD));

    /* Submit the changes */
    int ret = io_uring_submit(&state->ring);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }

    state->poll_mask[fd] = new_mask;
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;

    if (fd >= state->poll_mask_size) return;

    int current_mask = state->poll_mask[fd];
    int new_mask = current_mask & (~delmask);

    if (new_mask == current_mask) {
        /* No change needed */
        return;
    }

    /* Cancel the existing poll request */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        io_uring_submit(&state->ring);
        sqe = io_uring_get_sqe(&state->ring);
        if (!sqe) return;
    }
    io_uring_prep_poll_remove(sqe, IOURING_ENCODE_USERDATA(fd, IOURING_OP_POLL_ADD));
    io_uring_sqe_set_data64(sqe, IOURING_ENCODE_USERDATA(fd, IOURING_OP_POLL_REMOVE));

    if (new_mask != 0) {
        /* Add new poll with reduced mask */
        sqe = io_uring_get_sqe(&state->ring);
        if (!sqe) {
            io_uring_submit(&state->ring);
            sqe = io_uring_get_sqe(&state->ring);
            if (!sqe) {
                state->poll_mask[fd] = 0;
                return;
            }
        }

        unsigned poll_events = aeMaskToPollEvents(new_mask);
        io_uring_prep_poll_multishot(sqe, fd, poll_events);
        io_uring_sqe_set_data64(sqe, IOURING_ENCODE_USERDATA(fd, IOURING_OP_POLL_ADD));
    }

    io_uring_submit(&state->ring);
    state->poll_mask[fd] = new_mask;
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int numevents = 0;
    struct io_uring_cqe *cqe;
    int ret;

    /* Calculate timeout */
    struct __kernel_timespec ts;
    struct __kernel_timespec *pts = NULL;

    if (tvp != NULL) {
        ts.tv_sec = tvp->tv_sec;
        ts.tv_nsec = tvp->tv_usec * 1000;
        pts = &ts;
    }

    /* Wait for at least one completion */
    ret = io_uring_wait_cqe_timeout(&state->ring, &cqe, pts);

    if (ret < 0) {
        if (ret == -ETIME || ret == -EINTR) {
            /* Timeout or interrupted, not an error */
            return 0;
        }
        /* Real error */
        return 0;
    }

    /* Process all available completions */
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&state->ring, head, cqe) {
        uint64_t userdata = io_uring_cqe_get_data64(cqe);
        int fd = IOURING_DECODE_FD(userdata);
        int op = IOURING_DECODE_OP(userdata);

        /* Skip poll remove completions */
        if (op == IOURING_OP_POLL_REMOVE) {
            count++;
            continue;
        }

        /* Handle poll completion */
        if (op == IOURING_OP_POLL_ADD) {
            int res = cqe->res;

            /* Check for errors */
            if (res < 0) {
                /* Poll was canceled or error occurred */
                if (res == -ECANCELED) {
                    count++;
                    continue;
                }
                /* For other errors, treat as readable+writable to let handler deal with it */
                res = POLLERR;
            }

            /* Check if this is a multishot poll that needs rearming.
             * If IORING_CQE_F_MORE is not set, the multishot poll ended. */
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                /* Multishot ended, need to rearm if we still want events */
                if (fd < state->poll_mask_size && state->poll_mask[fd] != 0) {
                    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
                    if (sqe) {
                        unsigned poll_events = aeMaskToPollEvents(state->poll_mask[fd]);
                        io_uring_prep_poll_multishot(sqe, fd, poll_events);
                        io_uring_sqe_set_data64(sqe, IOURING_ENCODE_USERDATA(fd, IOURING_OP_POLL_ADD));
                        io_uring_submit(&state->ring);
                    }
                }
            }

            int mask = pollEventsToAeMask(res);
            if (mask && numevents < eventLoop->setsize) {
                eventLoop->fired[numevents].fd = fd;
                eventLoop->fired[numevents].mask = mask;
                numevents++;
            }
        }
        count++;
    }

    /* Advance the CQ ring */
    io_uring_cq_advance(&state->ring, count);

    return numevents;
}

static char *aeApiName(void) {
    return "io_uring";
}
