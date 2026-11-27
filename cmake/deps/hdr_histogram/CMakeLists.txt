project(hdr_histogram)

set(SRCS "${REDIS_ROOT}/deps/hdr_histogram/hdr_histogram.c" "${REDIS_ROOT}/deps/hdr_histogram/hdr_histogram.h"
         "${REDIS_ROOT}/deps/hdr_histogram/hdr_atomic.h" "${REDIS_ROOT}/deps/hdr_histogram/hdr_redis_malloc.h")

add_library(hdr_histogram STATIC ${SRCS})
target_compile_definitions(hdr_histogram PRIVATE HDR_MALLOC_INCLUDE=\"hdr_redis_malloc.h\")
