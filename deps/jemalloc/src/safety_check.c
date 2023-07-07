#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

static safety_check_abort_hook_t safety_check_abort;

void safety_check_fail_sized_dealloc(bool current_dealloc, const void *ptr,
    size_t true_size, size_t input_size) {
	char *src = current_dealloc ? "the current pointer being freed" :
	    "in thread cache, possibly from previous deallocations";

	safety_check_fail("<jemalloc>: size mismatch detected (true size %zu "
	    "vs input size %zu), likely caused by application sized "
	    "deallocation bugs (source address: %p, %s). Suggest building with "
	    "--enable-debug or address sanitizer for debugging. Abort.\n",
	    true_size, input_size, ptr, src);
}

void safety_check_set_abort(safety_check_abort_hook_t abort_fn) {
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
