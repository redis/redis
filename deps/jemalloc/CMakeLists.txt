project(jemalloc)

# Build jemalloc using configure && make install
set(JEMALLOC_INSTALL_DIR ${CMAKE_BINARY_DIR}/jemalloc-build)
set(JEMALLOC_SRC_DIR ${CMAKE_CURRENT_LIST_DIR})
if (NOT EXISTS ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a)
    message(STATUS "Building jemalloc (custom build)")
    message(STATUS "JEMALLOC_SRC_DIR = ${JEMALLOC_SRC_DIR}")
    message(STATUS "JEMALLOC_INSTALL_DIR = ${JEMALLOC_INSTALL_DIR}")

    execute_process(
        COMMAND sh -c "${JEMALLOC_SRC_DIR}/configure --disable-cxx \
            --with-version=5.3.0-0-g0 --with-lg-quantum=3 --disable-cache-oblivious --with-jemalloc-prefix=je_ \
            --enable-static --disable-shared --prefix=${JEMALLOC_INSTALL_DIR}"
        WORKING_DIRECTORY ${JEMALLOC_SRC_DIR} RESULTS_VARIABLE CONFIGURE_RESULT)

    if (NOT ${CONFIGURE_RESULT} EQUAL 0)
        message(FATAL_ERROR "Jemalloc configure failed")
    endif ()

    execute_process(COMMAND make -j${REDIS_PROCESSOR_COUNT} lib/libjemalloc.a install
                    WORKING_DIRECTORY "${JEMALLOC_SRC_DIR}" RESULTS_VARIABLE MAKE_RESULT)

    if (NOT ${MAKE_RESULT} EQUAL 0)
        message(FATAL_ERROR "Jemalloc build failed")
    endif ()
endif ()

# Import the compiled library as a CMake target
add_library(jemalloc STATIC IMPORTED GLOBAL)
set_target_properties(jemalloc PROPERTIES IMPORTED_LOCATION "${JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a"
                                          INCLUDE_DIRECTORIES "${JEMALLOC_INSTALL_DIR}/include")
