add_library(ffc STATIC fast_float_strtod.cpp)
target_include_directories(ffc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})