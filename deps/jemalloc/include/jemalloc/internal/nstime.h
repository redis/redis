/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct nstime_s nstime_t;

/* Maximum supported number of seconds (~584 years). */
#define	NSTIME_SEC_MAX	KQU(18446744072)

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct nstime_s {
	uint64_t	ns;
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	nstime_init(nstime_t *time, uint64_t ns);
void	nstime_init2(nstime_t *time, uint64_t sec, uint64_t nsec);
uint64_t	nstime_ns(const nstime_t *time);
uint64_t	nstime_sec(const nstime_t *time);
uint64_t	nstime_nsec(const nstime_t *time);
void	nstime_copy(nstime_t *time, const nstime_t *source);
int	nstime_compare(const nstime_t *a, const nstime_t *b);
void	nstime_add(nstime_t *time, const nstime_t *addend);
void	nstime_subtract(nstime_t *time, const nstime_t *subtrahend);
void	nstime_imultiply(nstime_t *time, uint64_t multiplier);
void	nstime_idivide(nstime_t *time, uint64_t divisor);
uint64_t	nstime_divide(const nstime_t *time, const nstime_t *divisor);
#ifdef JEMALLOC_JET
typedef bool (nstime_monotonic_t)(void);
extern nstime_monotonic_t *nstime_monotonic;
typedef bool (nstime_update_t)(nstime_t *);
extern nstime_update_t *nstime_update;
#else
bool	nstime_monotonic(void);
bool	nstime_update(nstime_t *time);
#endif

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
