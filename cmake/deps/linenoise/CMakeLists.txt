project(linenoise)

if(MSVC)
    set(SRCS "${CMAKE_BINARY_DIR}/patched/deps/linenoise/linenoise.c" "${REDIS_ROOT}/deps/linenoise/linenoise.h")
else()
    set(SRCS "${REDIS_ROOT}/deps/linenoise/linenoise.c" "${REDIS_ROOT}/deps/linenoise/linenoise.h")
endif()

include_directories("${REDIS_ROOT}/deps/linenoise")
add_library(linenoise STATIC ${SRCS})
