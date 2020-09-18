CMAKE_MINIMUM_REQUIRED(VERSION 3.4.0)
INCLUDE(GNUInstallDirs)
PROJECT(hiredis)

OPTION(ENABLE_SSL "Build hiredis_ssl for SSL support" OFF)
OPTION(DISABLE_TESTS "If tests should be compiled or not" OFF)
OPTION(ENABLE_SSL_TESTS, "Should we test SSL connections" OFF)

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

PROJECT(hiredis VERSION "${VERSION}")

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

SET_TARGET_PROPERTIES(hiredis
    PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE
    VERSION "${HIREDIS_SONAME}")
IF(WIN32 OR MINGW)
    TARGET_LINK_LIBRARIES(hiredis PRIVATE ws2_32)
ENDIF()

TARGET_INCLUDE_DIRECTORIES(hiredis PUBLIC $<INSTALL_INTERFACE:.> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

CONFIGURE_FILE(hiredis.pc.in hiredis.pc @ONLY)

INSTALL(TARGETS hiredis
    EXPORT hiredis-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

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

    IF (APPLE)
        SET_PROPERTY(TARGET hiredis_ssl PROPERTY LINK_FLAGS "-Wl,-undefined -Wl,dynamic_lookup")
    ENDIF()

    SET_TARGET_PROPERTIES(hiredis_ssl
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS TRUE
        VERSION "${HIREDIS_SONAME}")

    TARGET_INCLUDE_DIRECTORIES(hiredis_ssl PRIVATE "${OPENSSL_INCLUDE_DIR}")
    TARGET_LINK_LIBRARIES(hiredis_ssl PRIVATE ${OPENSSL_LIBRARIES})
    IF (WIN32 OR MINGW)
        TARGET_LINK_LIBRARIES(hiredis_ssl PRIVATE hiredis)
    ENDIF()
    CONFIGURE_FILE(hiredis_ssl.pc.in hiredis_ssl.pc @ONLY)

    INSTALL(TARGETS hiredis_ssl
        EXPORT hiredis_ssl-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

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
    IF(ENABLE_SSL_TESTS)
        ADD_DEFINITIONS(-DHIREDIS_TEST_SSL=1)
        TARGET_LINK_LIBRARIES(hiredis-test hiredis hiredis_ssl)
    ELSE()
        TARGET_LINK_LIBRARIES(hiredis-test hiredis)
    ENDIF()
    ADD_TEST(NAME hiredis-test
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test.sh)
ENDIF()

# Add examples
IF(ENABLE_EXAMPLES)
  ADD_SUBDIRECTORY(examples)
ENDIF(ENABLE_EXAMPLES)
