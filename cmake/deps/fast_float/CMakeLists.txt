add_library(ffc STATIC ${REDIS_ROOT}/deps/fast_float/fast_float_strtod.cpp)
target_include_directories(ffc PUBLIC ${REDIS_ROOT}/deps/fast_float)