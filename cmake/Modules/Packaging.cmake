set(CPACK_PACKAGE_NAME "redis")

redis_parse_version(CPACK_PACKAGE_VERSION_MAJOR CPACK_PACKAGE_VERSION_MINOR CPACK_PACKAGE_VERSION_PATCH)

# WIX strictly requires x.x.x.x version format (integers only).
# Strip any pre-release suffix (like -rc1) from the patch version for CPack.
string(REGEX REPLACE "-.*$" "" CPACK_PACKAGE_VERSION_PATCH "${CPACK_PACKAGE_VERSION_PATCH}")

set(CPACK_PACKAGE_CONTACT "maintainers@lists.redis.io")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Redis is an open source (BSD) high-performance key/value datastore")
set(CPACK_RESOURCE_FILE_LICENSE "${REDIS_ROOT}/LICENSE.txt")
set(CPACK_RESOURCE_FILE_README "${REDIS_ROOT}/README.md")
set(CPACK_STRIP_FILES TRUE)

redis_get_distro_name(DISTRO_NAME)
message(STATUS "Current host distro: ${DISTRO_NAME}")

if (DISTRO_NAME MATCHES ubuntu
    OR DISTRO_NAME MATCHES debian
    OR DISTRO_NAME MATCHES mint)
    message(STATUS "Adding target package for ${DISTRO_NAME}")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/redis")
    # Debian related parameters
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Redis contributors")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_GENERATOR "DEB")
endif ()

include(CPack)
unset(DISTRO_NAME CACHE)

# ---------------------------------------------------
# Create a helper script for creating symbolic links
# ---------------------------------------------------
if (NOT WIN32)
write_file(
    ${CMAKE_BINARY_DIR}/CreateSymlink.sh
    "\
#!/bin/sh                                                   \n\
if [ -z \${DESTDIR} ]; then                                 \n\
    # Script is called during 'make install'                \n\
    PREFIX=${CMAKE_INSTALL_PREFIX}/bin                      \n\
else                                                        \n\
    # Script is called during 'make package'                \n\
    PREFIX=\${DESTDIR}${CPACK_PACKAGING_INSTALL_PREFIX}/bin \n\
fi                                                          \n\
cd \$PREFIX                                                 \n\
ln -sf \$1 \$2")
endif()
