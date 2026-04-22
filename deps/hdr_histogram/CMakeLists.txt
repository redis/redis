project(hdr_histogram)

set(SRCS "${CMAKE_CURRENT_LIST_DIR}/hdr_histogram.c" "${CMAKE_CURRENT_LIST_DIR}/hdr_histogram.h"
         "${CMAKE_CURRENT_LIST_DIR}/hdr_atomic.h" "${CMAKE_CURRENT_LIST_DIR}/hdr_redis_malloc.h")

add_library(hdr_histogram STATIC ${SRCS})
target_compile_definitions(hdr_histogram PRIVATE HDR_MALLOC_INCLUDE=\"hdr_redis_malloc.h\")
