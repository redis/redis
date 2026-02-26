#ifndef JEMALLOC_INTERNAL_NSTIME_H
#define JEMALLOC_INTERNAL_NSTIME_H

/* Maximum supported number of seconds (~584 years). */
#define NSTIME_SEC_MAX KQU(18446744072)

#define NSTIME_MAGIC ((uint32_t)0xb8a9ce37)
#ifdef JEMALLOC_DEBUG
#  define NSTIME_ZERO_INITIALIZER {0, NSTIME_MAGIC}
#else
#  define NSTIME_ZERO_INITIALIZER {0}
#endif

typedef struct {
	uint64_t ns;
#ifdef JEMALLOC_DEBUG
	uint32_t magic; /* Tracks if initialized. */
#endif
} nstime_t;

static const nstime_t nstime_zero = NSTIME_ZERO_INITIALIZER;

void nstime_init(nstime_t *time, uint64_t ns);
void nstime_init2(nstime_t *time, uint64_t sec, uint64_t nsec);
uint64_t nstime_ns(const nstime_t *time);
uint64_t nstime_sec(const nstime_t *time);
uint64_t nstime_msec(const nstime_t *time);
uint64_t nstime_nsec(const nstime_t *time);
void nstime_copy(nstime_t *time, const nstime_t *source);
int nstime_compare(const nstime_t *a, const nstime_t *b);
void nstime_add(nstime_t *time, const nstime_t *addend);
void nstime_iadd(nstime_t *time, uint64_t addend);
void nstime_subtract(nstime_t *time, const nstime_t *subtrahend);
void nstime_isubtract(nstime_t *time, uint64_t subtrahend);
void nstime_imultiply(nstime_t *time, uint64_t multiplier);
void nstime_idivide(nstime_t *time, uint64_t divisor);
uint64_t nstime_divide(const nstime_t *time, const nstime_t *divisor);
uint64_t nstime_ns_since(const nstime_t *past);

typedef bool (nstime_monotonic_t)(void);
extern nstime_monotonic_t *JET_MUTABLE nstime_monotonic;

typedef void (nstime_update_t)(nstime_t *);
extern nstime_update_t *JET_MUTABLE nstime_update;

typedef void (nstime_prof_update_t)(nstime_t *);
extern nstime_prof_update_t *JET_MUTABLE nstime_prof_update;

void nstime_init_update(nstime_t *time);
void nstime_prof_init_update(nstime_t *time);

enum prof_time_res_e {
	prof_time_res_default = 0,
	prof_time_res_high = 1
};
typedef enum prof_time_res_e prof_time_res_t;

extern prof_time_res_t opt_prof_time_res;
extern const char *prof_time_res_mode_names[];

JEMALLOC_ALWAYS_INLINE void
nstime_init_zero(nstime_t *time) {
	nstime_copy(time, &nstime_zero);
}

JEMALLOC_ALWAYS_INLINE bool
nstime_equals_zero(nstime_t *time) {
	int diff = nstime_compare(time, &nstime_zero);
	assert(diff >= 0);
	return diff == 0;
}

#endif /* JEMALLOC_INTERNAL_NSTIME_H */
