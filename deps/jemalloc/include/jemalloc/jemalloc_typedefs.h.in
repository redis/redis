/*
 * void *
 * chunk_alloc(void *new_addr, size_t size, size_t alignment, bool *zero,
 *     bool *commit, unsigned arena_ind);
 */
typedef void *(chunk_alloc_t)(void *, size_t, size_t, bool *, bool *, unsigned);

/*
 * bool
 * chunk_dalloc(void *chunk, size_t size, bool committed, unsigned arena_ind);
 */
typedef bool (chunk_dalloc_t)(void *, size_t, bool, unsigned);

/*
 * bool
 * chunk_commit(void *chunk, size_t size, size_t offset, size_t length,
 *     unsigned arena_ind);
 */
typedef bool (chunk_commit_t)(void *, size_t, size_t, size_t, unsigned);

/*
 * bool
 * chunk_decommit(void *chunk, size_t size, size_t offset, size_t length,
 *     unsigned arena_ind);
 */
typedef bool (chunk_decommit_t)(void *, size_t, size_t, size_t, unsigned);

/*
 * bool
 * chunk_purge(void *chunk, size_t size, size_t offset, size_t length,
 *     unsigned arena_ind);
 */
typedef bool (chunk_purge_t)(void *, size_t, size_t, size_t, unsigned);

/*
 * bool
 * chunk_split(void *chunk, size_t size, size_t size_a, size_t size_b,
 *     bool committed, unsigned arena_ind);
 */
typedef bool (chunk_split_t)(void *, size_t, size_t, size_t, bool, unsigned);

/*
 * bool
 * chunk_merge(void *chunk_a, size_t size_a, void *chunk_b, size_t size_b,
 *     bool committed, unsigned arena_ind);
 */
typedef bool (chunk_merge_t)(void *, size_t, void *, size_t, bool, unsigned);

typedef struct {
	chunk_alloc_t		*alloc;
	chunk_dalloc_t		*dalloc;
	chunk_commit_t		*commit;
	chunk_decommit_t	*decommit;
	chunk_purge_t		*purge;
	chunk_split_t		*split;
	chunk_merge_t		*merge;
} chunk_hooks_t;
