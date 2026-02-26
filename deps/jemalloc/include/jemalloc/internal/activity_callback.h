#ifndef JEMALLOC_INTERNAL_ACTIVITY_CALLBACK_H
#define JEMALLOC_INTERNAL_ACTIVITY_CALLBACK_H

/*
 * The callback to be executed "periodically", in response to some amount of
 * allocator activity.
 *
 * This callback need not be computing any sort of peak (although that's the
 * intended first use case), but we drive it from the peak counter, so it's
 * keeps things tidy to keep it here.
 *
 * The calls to this thunk get driven by the peak_event module.
 */
#define ACTIVITY_CALLBACK_THUNK_INITIALIZER {NULL, NULL}
typedef void (*activity_callback_t)(void *uctx, uint64_t allocated,
    uint64_t deallocated);
typedef struct activity_callback_thunk_s activity_callback_thunk_t;
struct activity_callback_thunk_s {
	activity_callback_t callback;
	void *uctx;
};

#endif /* JEMALLOC_INTERNAL_ACTIVITY_CALLBACK_H */
