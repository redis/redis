project(linenoise)

if(MSVC)
    set(SRCS "${CMAKE_BINARY_DIR}/patched/deps/linenoise/linenoise.c" "${CMAKE_CURRENT_LIST_DIR}/linenoise.h")
else()
    set(SRCS "${CMAKE_CURRENT_LIST_DIR}/linenoise.c" "${CMAKE_CURRENT_LIST_DIR}/linenoise.h")
endif()

include_directories("${CMAKE_CURRENT_LIST_DIR}")
add_library(linenoise STATIC ${SRCS})
