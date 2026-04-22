project(fpconv)

set(SRCS "${CMAKE_CURRENT_LIST_DIR}/fpconv_dtoa.c" "${CMAKE_CURRENT_LIST_DIR}/fpconv_dtoa.h")
add_library(fpconv STATIC ${SRCS})
# Add -fPIC
set_target_properties(fpconv PROPERTIES POSITION_INDEPENDENT_CODE ON)
