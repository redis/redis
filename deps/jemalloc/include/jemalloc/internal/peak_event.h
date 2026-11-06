#ifndef JEMALLOC_INTERNAL_PEAK_EVENT_H
#define JEMALLOC_INTERNAL_PEAK_EVENT_H

/*
 * While peak.h contains the simple helper struct that tracks state, this
 * contains the allocator tie-ins (and knows about tsd, the event module, etc.).
 */

/* Update the peak with current tsd state. */
void peak_event_update(tsd_t *tsd);
/* Set current state to zero. */
void peak_event_zero(tsd_t *tsd);
uint64_t peak_event_max(tsd_t *tsd);

/* Manual hooks. */
/* The activity-triggered hooks. */
uint64_t peak_alloc_new_event_wait(tsd_t *tsd);
uint64_t peak_alloc_postponed_event_wait(tsd_t *tsd);
void peak_alloc_event_handler(tsd_t *tsd, uint64_t elapsed);
uint64_t peak_dalloc_new_event_wait(tsd_t *tsd);
uint64_t peak_dalloc_postponed_event_wait(tsd_t *tsd);
void peak_dalloc_event_handler(tsd_t *tsd, uint64_t elapsed);

#endif /* JEMALLOC_INTERNAL_PEAK_EVENT_H */
