# To the extent possible under law, the author(s) have dedicated all
# copyright and related and neighboring rights to this software to
# the public domain worldwide. This software is distributed without
# any warranty.
#
# For details, see <https://creativecommons.org/publicdomain/zero/1.0/>.

cmake_minimum_required (VERSION 3.10 FATAL_ERROR)

# This CMakeLists.txt is in a nested directory, which makes relative paths
# (e.g. ../..) show up in include flags and logs.
#
# To keep build output readable, we resolve these relative paths into
# absolute, cleaner ones. For example:
#   /path/xxHash/build/cmake/../..   ->   /path/xxHash
#
# We use get_filename_component() to step up two levels and store the result in XXHASH_DIR.
get_filename_component(XXHASH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

file(STRINGS "${XXHASH_DIR}/xxhash.h" XXHASH_VERSION_MAJOR REGEX "^#define XXH_VERSION_MAJOR +([0-9]+) *$")
string(REGEX REPLACE "^#define XXH_VERSION_MAJOR +([0-9]+) *$" "\\1" XXHASH_VERSION_MAJOR "${XXHASH_VERSION_MAJOR}")
file(STRINGS "${XXHASH_DIR}/xxhash.h" XXHASH_VERSION_MINOR REGEX "^#define XXH_VERSION_MINOR +([0-9]+) *$")
string(REGEX REPLACE "^#define XXH_VERSION_MINOR +([0-9]+) *$" "\\1" XXHASH_VERSION_MINOR "${XXHASH_VERSION_MINOR}")
file(STRINGS "${XXHASH_DIR}/xxhash.h" XXHASH_VERSION_RELEASE REGEX "^#define XXH_VERSION_RELEASE +([0-9]+) *$")
string(REGEX REPLACE "^#define XXH_VERSION_RELEASE +([0-9]+) *$" "\\1" XXHASH_VERSION_RELEASE "${XXHASH_VERSION_RELEASE}")
set(XXHASH_VERSION_STRING "${XXHASH_VERSION_MAJOR}.${XXHASH_VERSION_MINOR}.${XXHASH_VERSION_RELEASE}")
set(XXHASH_LIB_VERSION ${XXHASH_VERSION_STRING})
set(XXHASH_LIB_SOVERSION "${XXHASH_VERSION_MAJOR}")
mark_as_advanced(XXHASH_VERSION_MAJOR XXHASH_VERSION_MINOR XXHASH_VERSION_RELEASE XXHASH_VERSION_STRING XXHASH_LIB_VERSION XXHASH_LIB_SOVERSION)

if("${CMAKE_VERSION}" VERSION_LESS "3.13")
    #message(WARNING "CMake ${CMAKE_VERSION} has no CMP0077 policy: options will erase uncached/untyped normal vars!")
else()
    cmake_policy (SET CMP0077 NEW)
endif()
cmake_policy (SET CMP0048 NEW)
project(xxHash
    VERSION ${XXHASH_VERSION_STRING}
    LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Project build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE
    PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "xxHash build type: ${CMAKE_BUILD_TYPE}")
endif()

# Enable assert() statements in debug builds
if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    if("${CMAKE_VERSION}" VERSION_LESS "3.12")
        # add_compile_definitions is not available for older cmake => do nothing
    else()
        add_compile_definitions(XXH_DEBUGLEVEL=1)
    endif()
endif()

option(BUILD_SHARED_LIBS "Build shared library" ON)
option(XXHASH_BUILD_XXHSUM "Build the xxhsum binary" ON)

# If XXHASH is being bundled in another project, we don't want to
# install anything.  However, we want to let people override this, so
# we'll use the XXHASH_BUNDLED_MODE variable to let them do that; just
# set it to OFF in your project before you add_subdirectory(xxhash/cmake_unofficial).
if(NOT DEFINED XXHASH_BUNDLED_MODE)
  if("${PROJECT_SOURCE_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
    set(XXHASH_BUNDLED_MODE OFF)
  else()
    set(XXHASH_BUNDLED_MODE ON)
  endif()
endif()
set(XXHASH_BUNDLED_MODE ${XXHASH_BUNDLED_MODE} CACHE BOOL "" FORCE)
mark_as_advanced(XXHASH_BUNDLED_MODE)

# Allow people to choose whether to build shared or static libraries
# via the BUILD_SHARED_LIBS option unless we are in bundled mode, in
# which case we always use static libraries.
include(CMakeDependentOption)
CMAKE_DEPENDENT_OPTION(BUILD_SHARED_LIBS "Build shared libraries" ON "NOT XXHASH_BUNDLED_MODE" OFF)

# detect architecture for DISPATCH mode
CMAKE_HOST_SYSTEM_INFORMATION(RESULT PLATFORM QUERY OS_PLATFORM)
message(STATUS "Architecture: ${PLATFORM}")

# libxxhash
if((DEFINED DISPATCH) AND (DEFINED PLATFORM))
  # Only support DISPATCH option on x86_64.
  if(("${PLATFORM}" STREQUAL "x86_64") OR ("${PLATFORM}" STREQUAL "AMD64"))
    set(XXHSUM_DISPATCH ON)
    message(STATUS "Enable xxHash dispatch mode")
    add_library(xxhash "${XXHASH_DIR}/xxh_x86dispatch.c"
                       "${XXHASH_DIR}/xxhash.c"
               )
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DXXHSUM_DISPATCH=1")
  else()
    add_library(xxhash "${XXHASH_DIR}/xxhash.c")
  endif()
else()
  add_library(xxhash "${XXHASH_DIR}/xxhash.c")
endif()
add_library(${PROJECT_NAME}::xxhash ALIAS xxhash)

target_include_directories(xxhash
  PUBLIC
    $<BUILD_INTERFACE:${XXHASH_DIR}>
    $<INSTALL_INTERFACE:include/>)
if (BUILD_SHARED_LIBS)
  target_compile_definitions(xxhash PUBLIC XXH_EXPORT)
endif ()
set_target_properties(xxhash PROPERTIES
  SOVERSION "${XXHASH_LIB_SOVERSION}"
  VERSION "${XXHASH_VERSION_STRING}")

if(XXHASH_BUILD_XXHSUM)
  set(XXHSUM_DIR "${XXHASH_DIR}/cli")
  # xxhsum
  set(XXHSUM_SOURCES)
  if (XXHSUM_DISPATCH)
    list(APPEND XXHSUM_SOURCES "${XXHASH_DIR}/xxh_x86dispatch.c")
  endif()
  list(APPEND XXHSUM_SOURCES "${XXHSUM_DIR}/xxhsum.c"
                             "${XXHSUM_DIR}/xsum_arch.c"
                             "${XXHSUM_DIR}/xsum_os_specific.c"
                             "${XXHSUM_DIR}/xsum_output.c"
                             "${XXHSUM_DIR}/xsum_sanity_check.c"
                             "${XXHSUM_DIR}/xsum_bench.c"
      )
  add_executable(xxhsum ${XXHSUM_SOURCES})
  add_executable(${PROJECT_NAME}::xxhsum ALIAS xxhsum)

  target_link_libraries(xxhsum PRIVATE xxhash)
  target_include_directories(xxhsum PRIVATE "${XXHASH_DIR}")
endif(XXHASH_BUILD_XXHSUM)

# Extra warning flags
include (CheckCCompilerFlag)
if (XXHASH_C_FLAGS)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${XXHASH_C_FLAGS}")
endif()

if(NOT XXHASH_BUNDLED_MODE)
  include(GNUInstallDirs)

  install(TARGETS xxhash
    EXPORT xxHashTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(FILES "${XXHASH_DIR}/xxhash.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES "${XXHASH_DIR}/xxh3.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  if(DISPATCH)
    install(FILES "${XXHASH_DIR}/xxh_x86dispatch.h"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  if(XXHASH_BUILD_XXHSUM)
    install(TARGETS xxhsum
      EXPORT xxHashTargets
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${XXHSUM_DIR}/xxhsum.1"
      DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
  endif(XXHASH_BUILD_XXHSUM)

  include(CMakePackageConfigHelpers)

  set(xxHash_VERSION_CONFIG "${PROJECT_BINARY_DIR}/xxHashConfigVersion.cmake")
  set(xxHash_PROJECT_CONFIG "${PROJECT_BINARY_DIR}/xxHashConfig.cmake")
  set(xxHash_TARGETS_CONFIG "${PROJECT_BINARY_DIR}/xxHashTargets.cmake")
  set(xxHash_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/xxHash/")
  write_basic_package_version_file(${xxHash_VERSION_CONFIG}
    VERSION ${XXHASH_VERSION_STRING}
    COMPATIBILITY AnyNewerVersion)
  configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/xxHashConfig.cmake.in
    ${xxHash_PROJECT_CONFIG}
    INSTALL_DESTINATION ${xxHash_CONFIG_INSTALL_DIR})
    export(EXPORT xxHashTargets
      FILE ${xxHash_TARGETS_CONFIG}
      NAMESPACE ${PROJECT_NAME}::)

  install(FILES ${xxHash_PROJECT_CONFIG} ${xxHash_VERSION_CONFIG}
    DESTINATION ${xxHash_CONFIG_INSTALL_DIR})
  install(EXPORT xxHashTargets
    DESTINATION ${xxHash_CONFIG_INSTALL_DIR}
    NAMESPACE ${PROJECT_NAME}::)

  # configure and install pkg-config
  include(JoinPaths.cmake)
  set(PREFIX ${CMAKE_INSTALL_PREFIX})
  set(EXECPREFIX "\${prefix}")
  join_paths(INCLUDEDIR "\${prefix}" "${CMAKE_INSTALL_INCLUDEDIR}")
  join_paths(LIBDIR "\${prefix}" "${CMAKE_INSTALL_LIBDIR}")
  set(VERSION "${XXHASH_VERSION_STRING}")
  configure_file(${XXHASH_DIR}/libxxhash.pc.in ${CMAKE_BINARY_DIR}/libxxhash.pc @ONLY)

  install(FILES ${CMAKE_BINARY_DIR}/libxxhash.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

endif(NOT XXHASH_BUNDLED_MODE)

include(CPack)
