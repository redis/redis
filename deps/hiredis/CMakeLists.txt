CMAKE_MINIMUM_REQUIRED(VERSION 3.4.0)

OPTION(ENABLE_SSL "Build hiredis_ssl for SSL support" OFF)
OPTION(DISABLE_TESTS "If tests should be compiled or not" OFF)
OPTION(ENABLE_SSL_TESTS "Should we test SSL connections" OFF)
OPTION(ENABLE_ASYNC_TESTS "Should we run all asynchronous API tests" OFF)

MACRO(getVersionBit name)
  SET(VERSION_REGEX "^#define ${name} (.+)$")
  FILE(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/hiredis.h"
    VERSION_BIT REGEX ${VERSION_REGEX})
  STRING(REGEX REPLACE ${VERSION_REGEX} "\\1" ${name} "${VERSION_BIT}")
ENDMACRO(getVersionBit)

getVersionBit(HIREDIS_MAJOR)
getVersionBit(HIREDIS_MINOR)
getVersionBit(HIREDIS_PATCH)
getVersionBit(HIREDIS_SONAME)
SET(VERSION "${HIREDIS_MAJOR}.${HIREDIS_MINOR}.${HIREDIS_PATCH}")
MESSAGE("Detected version: ${VERSION}")

PROJECT(hiredis LANGUAGES "C" VERSION "${VERSION}")
INCLUDE(GNUInstallDirs)

# Hiredis requires C99
SET(CMAKE_C_STANDARD 99)
SET(CMAKE_POSITION_INDEPENDENT_CODE ON)
SET(CMAKE_DEBUG_POSTFIX d)

SET(ENABLE_EXAMPLES OFF CACHE BOOL "Enable building hiredis examples")

SET(hiredis_sources
    alloc.c
    async.c
    dict.c
    hiredis.c
    net.c
    read.c
    sds.c
    sockcompat.c)

SET(hiredis_sources ${hiredis_sources})

IF(WIN32)
    ADD_COMPILE_DEFINITIONS(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
ENDIF()

ADD_LIBRARY(hiredis SHARED ${hiredis_sources})
ADD_LIBRARY(hiredis_static STATIC ${hiredis_sources})
ADD_LIBRARY(hiredis::hiredis ALIAS hiredis)
ADD_LIBRARY(hiredis::hiredis_static ALIAS hiredis_static)

SET_TARGET_PROPERTIES(hiredis
    PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE
    VERSION "${HIREDIS_SONAME}")
SET_TARGET_PROPERTIES(hiredis_static
    PROPERTIES COMPILE_PDB_NAME hiredis_static)
SET_TARGET_PROPERTIES(hiredis_static
    PROPERTIES COMPILE_PDB_NAME_DEBUG hiredis_static${CMAKE_DEBUG_POSTFIX})
IF(WIN32 OR MINGW)
    TARGET_LINK_LIBRARIES(hiredis PUBLIC ws2_32 crypt32)
    TARGET_LINK_LIBRARIES(hiredis_static PUBLIC ws2_32 crypt32)
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    TARGET_LINK_LIBRARIES(hiredis PUBLIC m)
    TARGET_LINK_LIBRARIES(hiredis_static PUBLIC m)
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "SunOS")
    TARGET_LINK_LIBRARIES(hiredis PUBLIC socket)
    TARGET_LINK_LIBRARIES(hiredis_static PUBLIC socket)
ENDIF()

TARGET_INCLUDE_DIRECTORIES(hiredis PUBLIC $<INSTALL_INTERFACE:include> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
TARGET_INCLUDE_DIRECTORIES(hiredis_static PUBLIC $<INSTALL_INTERFACE:include> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

CONFIGURE_FILE(hiredis.pc.in hiredis.pc @ONLY)

set(CPACK_PACKAGE_VENDOR "Redis")
set(CPACK_PACKAGE_DESCRIPTION "\
Hiredis is a minimalistic C client library for the Redis database.

It is minimalistic because it just adds minimal support for the protocol, \
but at the same time it uses a high level printf-alike API in order to make \
it much higher level than otherwise suggested by its minimal code base and the \
lack of explicit bindings for every Redis command.

Apart from supporting sending commands and receiving replies, it comes with a \
reply parser that is decoupled from the I/O layer. It is a stream parser designed \
for easy reusability, which can for instance be used in higher level language bindings \
for efficient reply parsing.

Hiredis only supports the binary-safe Redis protocol, so you can use it with any Redis \
version >= 1.2.0.

The library comes with multiple APIs. There is the synchronous API, the asynchronous API \
and the reply parsing API.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/redis/hiredis")
set(CPACK_PACKAGE_CONTACT "michael dot grunder at gmail dot com")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)

include(CPack)

INSTALL(TARGETS hiredis hiredis_static
    EXPORT hiredis-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if (MSVC)
    INSTALL(FILES $<TARGET_PDB_FILE:hiredis>
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        CONFIGURATIONS Debug RelWithDebInfo)
    INSTALL(FILES $<TARGET_FILE_DIR:hiredis_static>/$<TARGET_FILE_BASE_NAME:hiredis_static>.pdb
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        CONFIGURATIONS Debug RelWithDebInfo)
endif()

# For NuGet packages
INSTALL(FILES hiredis.targets
    DESTINATION build/native)

INSTALL(FILES hiredis.h read.h sds.h async.h alloc.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hiredis)

INSTALL(DIRECTORY adapters
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hiredis)

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/hiredis.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

export(EXPORT hiredis-targets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/hiredis-targets.cmake"
    NAMESPACE hiredis::)

SET(CMAKE_CONF_INSTALL_DIR share/hiredis)
SET(INCLUDE_INSTALL_DIR include)
include(CMakePackageConfigHelpers)
configure_package_config_file(hiredis-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/hiredis-config.cmake
                              INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                              PATH_VARS INCLUDE_INSTALL_DIR)

INSTALL(EXPORT hiredis-targets
        FILE hiredis-targets.cmake
        NAMESPACE hiredis::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/hiredis-config.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})


IF(ENABLE_SSL)
    IF (NOT OPENSSL_ROOT_DIR)
        IF (APPLE)
            SET(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
        ENDIF()
    ENDIF()
    FIND_PACKAGE(OpenSSL REQUIRED)
    SET(hiredis_ssl_sources
        ssl.c)
    ADD_LIBRARY(hiredis_ssl SHARED
            ${hiredis_ssl_sources})
    ADD_LIBRARY(hiredis_ssl_static STATIC
            ${hiredis_ssl_sources})

    IF (APPLE)
        SET_PROPERTY(TARGET hiredis_ssl PROPERTY LINK_FLAGS "-Wl,-undefined -Wl,dynamic_lookup")
    ENDIF()

    SET_TARGET_PROPERTIES(hiredis_ssl
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS TRUE
        VERSION "${HIREDIS_SONAME}")
    SET_TARGET_PROPERTIES(hiredis_ssl_static
        PROPERTIES COMPILE_PDB_NAME hiredis_ssl_static)
    SET_TARGET_PROPERTIES(hiredis_ssl_static
        PROPERTIES COMPILE_PDB_NAME_DEBUG hiredis_ssl_static${CMAKE_DEBUG_POSTFIX})

    TARGET_INCLUDE_DIRECTORIES(hiredis_ssl PRIVATE "${OPENSSL_INCLUDE_DIR}")
    TARGET_INCLUDE_DIRECTORIES(hiredis_ssl_static PRIVATE "${OPENSSL_INCLUDE_DIR}")

    TARGET_LINK_LIBRARIES(hiredis_ssl PRIVATE ${OPENSSL_LIBRARIES})
    IF (WIN32 OR MINGW)
        TARGET_LINK_LIBRARIES(hiredis_ssl PRIVATE hiredis)
        TARGET_LINK_LIBRARIES(hiredis_ssl_static PUBLIC hiredis_static)
    ENDIF()
    CONFIGURE_FILE(hiredis_ssl.pc.in hiredis_ssl.pc @ONLY)

    INSTALL(TARGETS hiredis_ssl hiredis_ssl_static
        EXPORT hiredis_ssl-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    if (MSVC)
        INSTALL(FILES $<TARGET_PDB_FILE:hiredis_ssl>
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            CONFIGURATIONS Debug RelWithDebInfo)
        INSTALL(FILES $<TARGET_FILE_DIR:hiredis_ssl_static>/$<TARGET_FILE_BASE_NAME:hiredis_ssl_static>.pdb
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            CONFIGURATIONS Debug RelWithDebInfo)
    endif()

    INSTALL(FILES hiredis_ssl.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hiredis)

    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/hiredis_ssl.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

    export(EXPORT hiredis_ssl-targets
           FILE "${CMAKE_CURRENT_BINARY_DIR}/hiredis_ssl-targets.cmake"
           NAMESPACE hiredis::)

    SET(CMAKE_CONF_INSTALL_DIR share/hiredis_ssl)
    configure_package_config_file(hiredis_ssl-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/hiredis_ssl-config.cmake
                                  INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                                  PATH_VARS INCLUDE_INSTALL_DIR)

    INSTALL(EXPORT hiredis_ssl-targets
        FILE hiredis_ssl-targets.cmake
        NAMESPACE hiredis::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/hiredis_ssl-config.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})
ENDIF()

IF(NOT DISABLE_TESTS)
    ENABLE_TESTING()
    ADD_EXECUTABLE(hiredis-test test.c)
    TARGET_LINK_LIBRARIES(hiredis-test hiredis)
    IF(ENABLE_SSL_TESTS)
        ADD_DEFINITIONS(-DHIREDIS_TEST_SSL=1)
        TARGET_LINK_LIBRARIES(hiredis-test hiredis_ssl)
    ENDIF()
    IF(ENABLE_ASYNC_TESTS)
        ADD_DEFINITIONS(-DHIREDIS_TEST_ASYNC=1)
        TARGET_LINK_LIBRARIES(hiredis-test event)
    ENDIF()
    ADD_TEST(NAME hiredis-test
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test.sh)
ENDIF()

# Add examples
IF(ENABLE_EXAMPLES)
  ADD_SUBDIRECTORY(examples)
ENDIF(ENABLE_EXAMPLES)
