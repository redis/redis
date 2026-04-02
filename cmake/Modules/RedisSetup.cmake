include(CheckIncludeFiles)
include(ProcessorCount)
include(Utils)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Generate compile_commands.json file for IDEs code completion support
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

processorcount(REDIS_PROCESSOR_COUNT)
message(STATUS "Processor count: ${REDIS_PROCESSOR_COUNT}")

# Installed executables will have this permissions
set(REDIS_EXE_PERMISSIONS
    OWNER_EXECUTE
    OWNER_WRITE
    OWNER_READ
    GROUP_EXECUTE
    GROUP_READ
    WORLD_EXECUTE
    WORLD_READ)

set(REDIS_SERVER_CFLAGS "")
set(REDIS_SERVER_LDFLAGS "")

# ----------------------------------------------------
# Helper functions & macros
# ----------------------------------------------------
macro (add_redis_server_compiler_options value)
    set(REDIS_SERVER_CFLAGS "${REDIS_SERVER_CFLAGS} ${value}")
endmacro ()

macro (add_redis_server_linker_option value)
    list(APPEND REDIS_SERVER_LDFLAGS ${value})
endmacro ()

macro (get_redis_server_linker_option return_value)
    list(JOIN REDIS_SERVER_LDFLAGS " " ${value} ${return_value})
endmacro ()

set(IS_FREEBSD 0)
if (CMAKE_SYSTEM_NAME MATCHES "^.*BSD$|DragonFly")
    message(STATUS "Building for FreeBSD compatible system")
    set(IS_FREEBSD 1)
    include_directories("/usr/local/include")
    add_redis_server_compiler_options("-DUSE_BACKTRACE")
endif ()

# Helper function for creating symbolic link so that: link -> source
macro (redis_create_symlink source link)
  if(WIN32)
    add_custom_command(
      TARGET ${source} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
              "$<TARGET_FILE:${source}>"
              "$<TARGET_FILE_DIR:${source}>/${link}.exe"
      VERBATIM
    )
  else()
    add_custom_command(
      TARGET ${source} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink
              "$<TARGET_FILE_NAME:${source}>"
              "$<TARGET_FILE_DIR:${source}>/${link}"
      VERBATIM
    )
  endif()
endmacro ()

# Install a binary
macro (redis_install_bin target)
    # Install cli tool and create a redis symbolic link
    install(
        TARGETS ${target}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        PERMISSIONS ${REDIS_EXE_PERMISSIONS}
        COMPONENT "redis")
endmacro ()

# Helper function that defines, builds and installs `target` In addition, it creates a symbolic link between the target
# and `link_name`
macro (redis_build_and_install_bin target sources ld_flags libs link_name)
    add_executable(${target} ${sources})

    if (USE_JEMALLOC
        OR USE_TCMALLOC
        OR USE_TCMALLOC_MINIMAL)
        # Using custom allocator
        target_link_libraries(${target} ${ALLOCATOR_LIB})
    endif ()

    # Place this line last to ensure that ${ld_flags} is placed last on the linker line
    target_link_libraries(${target} ${libs} ${ld_flags})
    target_link_libraries(${target} hiredis::hiredis)
    if (USE_TLS)
        # Add required libraries needed for TLS
        target_link_libraries(${target} OpenSSL::SSL hiredis::hiredis_ssl)
    endif ()

    if (USE_RDMA)
        # Add required libraries needed for RDMA
        target_link_libraries(${target} hiredis::hiredis_rdma)
    endif ()

    if (IS_FREEBSD)
        target_link_libraries(${target} execinfo)
    endif ()

    # Enable all warnings + fail on warning
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    endif()

    # Install cli tool and create a redis symbolic link
    redis_install_bin(${target})
    redis_create_symlink(${target} ${link_name})
endmacro ()

# Determine if we are building in Release or Debug mode
if (CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DebugFull)
    set(REDIS_DEBUG_BUILD 1)
    set(REDIS_RELEASE_BUILD 0)
    message(STATUS "Building in debug mode")
else ()
    set(REDIS_DEBUG_BUILD 0)
    set(REDIS_RELEASE_BUILD 1)
    message(STATUS "Building in release mode")
endif ()

# ----------------------------------------------------
# Helper functions - end
# ----------------------------------------------------

# ----------------------------------------------------
# Build options (allocator, tls, rdma et al)
# ----------------------------------------------------

if (NOT BUILD_MALLOC)
    if (APPLE)
        set(BUILD_MALLOC "libc")
    elseif (UNIX)
        set(BUILD_MALLOC "jemalloc")
    endif ()
endif ()

# User may pass different allocator library. Using -DBUILD_MALLOC=<libname>, make sure it is a valid value
if (BUILD_MALLOC)
    if ("${BUILD_MALLOC}" STREQUAL "jemalloc")
        set(MALLOC_LIB "jemalloc")
        set(ALLOCATOR_LIB "jemalloc")
        add_redis_server_compiler_options("-DUSE_JEMALLOC")
        set(USE_JEMALLOC 1)
    elseif ("${BUILD_MALLOC}" STREQUAL "libc")
        set(MALLOC_LIB "libc")
    elseif ("${BUILD_MALLOC}" STREQUAL "tcmalloc")
        set(MALLOC_LIB "tcmalloc")
        redis_pkg_config(libtcmalloc ALLOCATOR_LIB)

        add_redis_server_compiler_options("-DUSE_TCMALLOC")
        set(USE_TCMALLOC 1)
    elseif ("${BUILD_MALLOC}" STREQUAL "tcmalloc_minimal")
        set(MALLOC_LIB "tcmalloc_minimal")
        redis_pkg_config(libtcmalloc_minimal ALLOCATOR_LIB)

        add_redis_server_compiler_options("-DUSE_TCMALLOC")
        set(USE_TCMALLOC_MINIMAL 1)
    else ()
        message(FATAL_ERROR "BUILD_MALLOC can be one of: jemalloc, libc, tcmalloc or tcmalloc_minimal")
    endif ()
endif ()

message(STATUS "Using ${MALLOC_LIB}")

# TLS support
if (BUILD_TLS)
    redis_parse_build_option(${BUILD_TLS} USE_TLS)
    if (USE_TLS EQUAL 1)
        # Only search for OpenSSL if needed
        find_package(OpenSSL REQUIRED)
        message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
        message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
        include_directories(${OPENSSL_INCLUDE_DIR})
    endif ()

    if (USE_TLS EQUAL 1)
        add_redis_server_compiler_options("-DUSE_OPENSSL=1")
        add_redis_server_compiler_options("-DBUILD_TLS_MODULE=0")
    else ()
        # Build TLS as a module RDMA can only be built as a module. So disable it
        message(WARNING "BUILD_TLS can be one of: [ON | OFF | 1 | 0], but '${BUILD_TLS}' was provided")
        message(STATUS "TLS support is disabled")
        set(USE_TLS 0)
    endif ()
else ()
    # By default, TLS is disabled
    message(STATUS "TLS is disabled")
    set(USE_TLS 0)
endif ()

if (BUILD_RDMA)
    set(BUILD_RDMA_MODULE 0)
    # RDMA support (Linux only)
    if (LINUX AND NOT APPLE)
        redis_parse_build_option(${BUILD_RDMA} USE_RDMA)
        find_package(PkgConfig REQUIRED)
        # Locate librdmacm & libibverbs, fail if we can't find them
        redis_pkg_config(librdmacm RDMACM_LIBS)
        redis_pkg_config(libibverbs IBVERBS_LIBS)
        message(STATUS "${RDMACM_LIBS};${IBVERBS_LIBS}")
        list(APPEND RDMA_LIBS "${RDMACM_LIBS};${IBVERBS_LIBS}")

        if (USE_RDMA EQUAL 2) # Module
            message(STATUS "Building RDMA as module")
            add_redis_server_compiler_options("-DUSE_RDMA=2")
            set(BUILD_RDMA_MODULE 2)
        elseif (USE_RDMA EQUAL 1) # Builtin
            message(STATUS "Building RDMA as builtin")
            add_redis_server_compiler_options("-DUSE_RDMA=1")
            add_redis_server_compiler_options("-DBUILD_RDMA_MODULE=0")
            list(APPEND SERVER_LIBS "${RDMA_LIBS}")
        endif ()
    else ()
        message(WARNING "RDMA is only supported on Linux platforms")
    endif ()
else ()
    # By default, RDMA is disabled
    message(STATUS "RDMA is disabled")
    set(USE_RDMA 0)
endif ()

set(BUILDING_ARM64 0)
set(BUILDING_ARM32 0)

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set(BUILDING_ARM64 1)
endif ()

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm")
    set(BUILDING_ARM32 1)
endif ()

message(STATUS "Building on ${CMAKE_HOST_SYSTEM_NAME}")
if (BUILDING_ARM64)
    message(STATUS "Compiling redis for ARM64")
    add_redis_server_linker_option("-funwind-tables")
endif ()

if (APPLE)
    add_redis_server_linker_option("-rdynamic")
    add_redis_server_linker_option("-ldl")
elseif (UNIX)
    add_redis_server_linker_option("-rdynamic")
    add_redis_server_linker_option("-pthread")
    add_redis_server_linker_option("-ldl")
    add_redis_server_linker_option("-lm")
endif ()

if (REDIS_DEBUG_BUILD)
    # Debug build, use enable "-fno-omit-frame-pointer"
    add_redis_server_compiler_options("-fno-omit-frame-pointer")
endif ()

# Check for Atomic
if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "/std:c11 /experimental:c11atomics")
endif()
check_include_files(stdatomic.h HAVE_C11_ATOMIC)
if (HAVE_C11_ATOMIC)
    if(MSVC)
        add_redis_server_compiler_options("/std:c11 /experimental:c11atomics")
    else()
        add_redis_server_compiler_options("-std=gnu11")
    endif()
else ()
    if(MSVC)
        add_redis_server_compiler_options("/std:c11 /experimental:c11atomics")
    else()
        add_redis_server_compiler_options("-std=c99")
    endif()
endif ()

# Sanitizer
if (BUILD_SANITIZER)
    # Common CFLAGS
    list(APPEND REDIS_SANITAIZER_CFLAGS "-fno-sanitize-recover=all")
    list(APPEND REDIS_SANITAIZER_CFLAGS "-fno-omit-frame-pointer")
    if ("${BUILD_SANITIZER}" STREQUAL "address")
        list(APPEND REDIS_SANITAIZER_CFLAGS "-fsanitize=address")
        list(APPEND REDIS_SANITAIZER_LDFLAGS "-fsanitize=address")
    elseif ("${BUILD_SANITIZER}" STREQUAL "thread")
        list(APPEND REDIS_SANITAIZER_CFLAGS "-fsanitize=thread")
        list(APPEND REDIS_SANITAIZER_LDFLAGS "-fsanitize=thread")
    elseif ("${BUILD_SANITIZER}" STREQUAL "undefined")
        list(APPEND REDIS_SANITAIZER_CFLAGS "-fsanitize=undefined")
        list(APPEND REDIS_SANITAIZER_LDFLAGS "-fsanitize=undefined")
    else ()
        message(FATAL_ERROR "Unknown sanitizer: ${BUILD_SANITIZER}")
    endif ()
endif ()

include_directories("${CMAKE_SOURCE_DIR}/deps/hiredis")
include_directories("${CMAKE_SOURCE_DIR}/deps/lua/src")
include_directories("${CMAKE_SOURCE_DIR}/deps/linenoise")
include_directories("${CMAKE_SOURCE_DIR}/deps/hdr_histogram")
include_directories("${CMAKE_SOURCE_DIR}/deps/fpconv")

add_subdirectory("${CMAKE_SOURCE_DIR}/deps")

# Update linker flags for the allocator
if (USE_JEMALLOC)
    include_directories("${CMAKE_SOURCE_DIR}/deps/jemalloc/include")
endif ()

# Common compiler flags
if(NOT MSVC)
    add_redis_server_compiler_options("-pedantic")
endif()

if (NOT BUILD_LUA)
    message(STATUS "Lua scripting engine is disabled")
endif()

# ----------------------------------------------------
# Build options (allocator, tls, rdma et al) - end
# ----------------------------------------------------

# -------------------------------------------------
# Code Generation section
# -------------------------------------------------
find_program(PYTHON_EXE python3)
if (PYTHON_EXE)
    # Python based code generation
    message(STATUS "Found python3: ${PYTHON_EXE}")
    # Rule for generating commands.def file from json files
    message(STATUS "Adding target generate_commands_def")
    file(GLOB COMMAND_FILES_JSON "${CMAKE_SOURCE_DIR}/src/commands/*.json")
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/commands_def_generated
        DEPENDS ${COMMAND_FILES_JSON}
        COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/utils/generate-command-code.py
        COMMAND touch ${CMAKE_BINARY_DIR}/commands_def_generated
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")
    add_custom_target(generate_commands_def DEPENDS ${CMAKE_BINARY_DIR}/commands_def_generated)

    # Rule for generating fmtargs.h
    message(STATUS "Adding target generate_fmtargs_h")
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/fmtargs_generated
        DEPENDS ${CMAKE_SOURCE_DIR}/utils/generate-fmtargs.py
        COMMAND sed '/Everything/,$$d' fmtargs.h > fmtargs.h.tmp
        COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/utils/generate-fmtargs.py >> fmtargs.h.tmp
        COMMAND mv fmtargs.h.tmp fmtargs.h
        COMMAND touch ${CMAKE_BINARY_DIR}/fmtargs_generated
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")
    add_custom_target(generate_fmtargs_h DEPENDS ${CMAKE_BINARY_DIR}/fmtargs_generated)
else ()
    # Fake targets
    add_custom_target(generate_commands_def)
    add_custom_target(generate_fmtargs_h)
endif ()

if (WIN32)
    file(WRITE ${CMAKE_BINARY_DIR}/patched/src/release.h "#define REDIS_GIT_SHA1 \"00000000\"\n#define REDIS_GIT_DIRTY \"0\"\n#define REDIS_BUILD_ID \"0\"\n#include \"version.h\"\n#define REDIS_BUILD_ID_RAW \"redis \" REDIS_VERSION REDIS_BUILD_ID REDIS_GIT_DIRTY REDIS_GIT_SHA1\n")
    add_custom_target(release_header)
else()
    # Generate release.h file (always)
    add_custom_target(
        release_header
        COMMAND sh -c '${CMAKE_SOURCE_DIR}/src/mkreleasehdr.sh'
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")
endif()

# -------------------------------------------------
# Code Generation section - end
# -------------------------------------------------

# ----------------------------------------------------------
# All our source files are defined in SourceFiles.cmake file
# ----------------------------------------------------------
include(SourceFiles)

# Clear the below variables from the cache
unset(CMAKE_C_FLAGS CACHE)
unset(REDIS_SERVER_LDFLAGS CACHE)
unset(REDIS_SERVER_CFLAGS CACHE)
unset(PYTHON_EXE CACHE)
unset(HAVE_C11_ATOMIC CACHE)
unset(USE_TLS CACHE)
unset(USE_RDMA CACHE)
unset(BUILD_TLS CACHE)
unset(BUILD_RDMA CACHE)
unset(BUILD_MALLOC CACHE)
unset(USE_JEMALLOC CACHE)
unset(BUILD_TLS_MODULE CACHE)
unset(BUILD_TLS_BUILTIN CACHE)



include_directories("${CMAKE_SOURCE_DIR}/deps/xxhash")
