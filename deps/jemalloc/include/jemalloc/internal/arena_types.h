#ifndef JEMALLOC_INTERNAL_ARENA_TYPES_H
#define JEMALLOC_INTERNAL_ARENA_TYPES_H

#include "jemalloc/internal/sc.h"

/* Default decay times in milliseconds. */
#define DIRTY_DECAY_MS_DEFAULT	ZD(10 * 1000)
#define MUZZY_DECAY_MS_DEFAULT	(0)
/* Number of event ticks between time checks. */
#define ARENA_DECAY_NTICKS_PER_UPDATE	1000

typedef struct arena_decay_s arena_decay_t;
typedef struct arena_s arena_t;

typedef enum {
	percpu_arena_mode_names_base   = 0, /* Used for options processing. */

	/*
	 * *_uninit are used only during bootstrapping, and must correspond
	 * to initialized variant plus percpu_arena_mode_enabled_base.
	 */
	percpu_arena_uninit            = 0,
	per_phycpu_arena_uninit        = 1,

	/* All non-disabled modes must come after percpu_arena_disabled. */
	percpu_arena_disabled          = 2,

	percpu_arena_mode_names_limit  = 3, /* Used for options processing. */
	percpu_arena_mode_enabled_base = 3,

	percpu_arena                   = 3,
	per_phycpu_arena               = 4  /* Hyper threads share arena. */
} percpu_arena_mode_t;

#define PERCPU_ARENA_ENABLED(m)	((m) >= percpu_arena_mode_enabled_base)
#define PERCPU_ARENA_DEFAULT	percpu_arena_disabled

/*
 * When allocation_size >= oversize_threshold, use the dedicated huge arena
 * (unless have explicitly spicified arena index).  0 disables the feature.
 */
#define OVERSIZE_THRESHOLD_DEFAULT (8 << 20)

struct arena_config_s {
	/* extent hooks to be used for the arena */
	extent_hooks_t *extent_hooks;

	/*
	 * Use extent hooks for metadata (base) allocations when true.
	 */
	bool metadata_use_hooks;
};

typedef struct arena_config_s arena_config_t;

extern const arena_config_t arena_config_default;

#endif /* JEMALLOC_INTERNAL_ARENA_TYPES_H */
