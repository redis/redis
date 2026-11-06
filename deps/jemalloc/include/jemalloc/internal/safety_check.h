#ifndef JEMALLOC_INTERNAL_SAFETY_CHECK_H
#define JEMALLOC_INTERNAL_SAFETY_CHECK_H

void safety_check_fail_sized_dealloc(bool current_dealloc, const void *ptr,
    size_t true_size, size_t input_size);
void safety_check_fail(const char *format, ...);

typedef void (*safety_check_abort_hook_t)(const char *message);

/* Can set to NULL for a default. */
void safety_check_set_abort(safety_check_abort_hook_t abort_fn);

JEMALLOC_ALWAYS_INLINE void
safety_check_set_redzone(void *ptr, size_t usize, size_t bumped_usize) {
	assert(usize < bumped_usize);
	for (size_t i = usize; i < bumped_usize && i < usize + 32; ++i) {
		*((unsigned char *)ptr + i) = 0xBC;
	}
}

JEMALLOC_ALWAYS_INLINE void
safety_check_verify_redzone(const void *ptr, size_t usize, size_t bumped_usize)
{
	for (size_t i = usize; i < bumped_usize && i < usize + 32; ++i) {
		if (unlikely(*((unsigned char *)ptr + i) != 0xBC)) {
			safety_check_fail("Use after free error\n");
		}
	}
}

#endif /*JEMALLOC_INTERNAL_SAFETY_CHECK_H */
