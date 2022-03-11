#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

static void (*safety_check_abort)(const char *message);

void safety_check_set_abort(void (*abort_fn)(const char *)) {
	safety_check_abort = abort_fn;
}

void safety_check_fail(const char *format, ...) {
	char buf[MALLOC_PRINTF_BUFSIZE];

	va_list ap;
	va_start(ap, format);
	malloc_vsnprintf(buf, MALLOC_PRINTF_BUFSIZE, format, ap);
	va_end(ap);

	if (safety_check_abort == NULL) {
		malloc_write(buf);
		abort();
	} else {
		safety_check_abort(buf);
	}
}
