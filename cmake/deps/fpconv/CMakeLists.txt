project(fpconv)

set(SRCS "${REDIS_ROOT}/deps/fpconv/fpconv_dtoa.c" "${REDIS_ROOT}/deps/fpconv/fpconv_dtoa.h")
add_library(fpconv STATIC ${SRCS})
# Add -fPIC
set_target_properties(fpconv PROPERTIES POSITION_INDEPENDENT_CODE ON)
