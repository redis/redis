CMAKE_MINIMUM_REQUIRED(VERSION 3.4.0)
INCLUDE(GNUInstallDirs)
PROJECT(hiredis)

OPTION(ENABLE_SSL "Build hiredis_ssl for SSL support" OFF)

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

ADD_LIBRARY(hiredis SHARED
    async.c
    dict.c
    hiredis.c
    net.c
    read.c
    sds.c
    sockcompat.c)

SET_TARGET_PROPERTIES(hiredis
    PROPERTIES
    VERSION "${HIREDIS_SONAME}")
IF(WIN32 OR MINGW)
    TARGET_LINK_LIBRARIES(hiredis PRIVATE ws2_32)
ENDIF()
TARGET_INCLUDE_DIRECTORIES(hiredis PUBLIC .)

CONFIGURE_FILE(hiredis.pc.in hiredis.pc @ONLY)

INSTALL(TARGETS hiredis
    DESTINATION "${CMAKE_INSTALL_LIBDIR}")

INSTALL(FILES hiredis.h read.h sds.h async.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hiredis)
    
INSTALL(DIRECTORY adapters
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hiredis)
    
INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/hiredis.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

IF(ENABLE_SSL)
    IF (NOT OPENSSL_ROOT_DIR)
        IF (APPLE)
            SET(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
        ENDIF()
    ENDIF()
    FIND_PACKAGE(OpenSSL REQUIRED)
    ADD_LIBRARY(hiredis_ssl SHARED
        ssl.c)
    TARGET_INCLUDE_DIRECTORIES(hiredis_ssl PRIVATE "${OPENSSL_INCLUDE_DIR}")
    TARGET_LINK_LIBRARIES(hiredis_ssl PRIVATE ${OPENSSL_LIBRARIES})
    CONFIGURE_FILE(hiredis_ssl.pc.in hiredis_ssl.pc @ONLY)

    INSTALL(TARGETS hiredis_ssl
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")

    INSTALL(FILES hiredis_ssl.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hiredis)
    
    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/hiredis_ssl.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
ENDIF()

IF(NOT (WIN32 OR MINGW))
    ENABLE_TESTING()
    ADD_EXECUTABLE(hiredis-test test.c)
    TARGET_LINK_LIBRARIES(hiredis-test hiredis)
    ADD_TEST(NAME hiredis-test
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test.sh)
ENDIF()

# Add examples
IF(ENABLE_EXAMPLES)
  ADD_SUBDIRECTORY(examples)
ENDIF(ENABLE_EXAMPLES)
