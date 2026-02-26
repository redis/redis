#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpa_hooks.h"

static void *hpa_hooks_map(size_t size);
static void hpa_hooks_unmap(void *ptr, size_t size);
static void hpa_hooks_purge(void *ptr, size_t size);
static void hpa_hooks_hugify(void *ptr, size_t size);
static void hpa_hooks_dehugify(void *ptr, size_t size);
static void hpa_hooks_curtime(nstime_t *r_nstime, bool first_reading);
static uint64_t hpa_hooks_ms_since(nstime_t *past_nstime);

hpa_hooks_t hpa_hooks_default = {
	&hpa_hooks_map,
	&hpa_hooks_unmap,
	&hpa_hooks_purge,
	&hpa_hooks_hugify,
	&hpa_hooks_dehugify,
	&hpa_hooks_curtime,
	&hpa_hooks_ms_since
};

static void *
hpa_hooks_map(size_t size) {
	bool commit = true;
	return pages_map(NULL, size, HUGEPAGE, &commit);
}

static void
hpa_hooks_unmap(void *ptr, size_t size) {
	pages_unmap(ptr, size);
}

static void
hpa_hooks_purge(void *ptr, size_t size) {
	pages_purge_forced(ptr, size);
}

static void
hpa_hooks_hugify(void *ptr, size_t size) {
	bool err = pages_huge(ptr, size);
	(void)err;
}

static void
hpa_hooks_dehugify(void *ptr, size_t size) {
	bool err = pages_nohuge(ptr, size);
	(void)err;
}

static void
hpa_hooks_curtime(nstime_t *r_nstime, bool first_reading) {
	if (first_reading) {
		nstime_init_zero(r_nstime);
	}
	nstime_update(r_nstime);
}

static uint64_t
hpa_hooks_ms_since(nstime_t *past_nstime) {
	return nstime_ns_since(past_nstime) / 1000 / 1000;
}
