#ifndef JEMALLOC_INTERNAL_HPA_HOOKS_H
#define JEMALLOC_INTERNAL_HPA_HOOKS_H

typedef struct hpa_hooks_s hpa_hooks_t;
struct hpa_hooks_s {
	void *(*map)(size_t size);
	void (*unmap)(void *ptr, size_t size);
	void (*purge)(void *ptr, size_t size);
	void (*hugify)(void *ptr, size_t size);
	void (*dehugify)(void *ptr, size_t size);
	void (*curtime)(nstime_t *r_time, bool first_reading);
	uint64_t (*ms_since)(nstime_t *r_time);
};

extern hpa_hooks_t hpa_hooks_default;

#endif /* JEMALLOC_INTERNAL_HPA_HOOKS_H */
