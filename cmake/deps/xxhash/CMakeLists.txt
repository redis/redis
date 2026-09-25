add_library(xxhash STATIC ${REDIS_ROOT}/deps/xxhash/xxhash.c)
target_include_directories(xxhash PUBLIC ${REDIS_ROOT}/deps/xxhash)
