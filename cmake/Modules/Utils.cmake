# Return the current host distro name. For example: ubuntu, debian, amzn etc
function (redis_get_distro_name DISTRO_NAME)
    if (LINUX AND NOT APPLE)
        execute_process(
            COMMAND /bin/bash "-c" "cat /etc/os-release |grep ^ID=|cut -d = -f 2"
            OUTPUT_VARIABLE _OUT_VAR
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        # clean the output
        string(REPLACE "\"" "" _OUT_VAR "${_OUT_VAR}")
        string(REPLACE "." "" _OUT_VAR "${_OUT_VAR}")
        set(${DISTRO_NAME}
            "${_OUT_VAR}"
            PARENT_SCOPE)
    elseif (APPLE)
        set(${DISTRO_NAME}
            "darwin"
            PARENT_SCOPE)
    elseif (IS_FREEBSD)
        set(${DISTRO_NAME}
            "freebsd"
            PARENT_SCOPE)
    else ()
        set(${DISTRO_NAME}
            "unknown"
            PARENT_SCOPE)
    endif ()
endfunction ()

function (redis_parse_version OUT_MAJOR OUT_MINOR OUT_PATCH)
    # Read and parse package version from version.h file
    file(STRINGS ${CMAKE_SOURCE_DIR}/src/version.h VERSION_LINES)
    foreach (LINE ${VERSION_LINES})
        string(FIND "${LINE}" "#define REDIS_VERSION " VERSION_STR_POS)
        if (VERSION_STR_POS GREATER -1)
            string(REPLACE "#define REDIS_VERSION " "" LINE "${LINE}")
            string(REPLACE "\"" "" LINE "${LINE}")
            # Change "." to ";" to make it a list
            string(REPLACE "." ";" LINE "${LINE}")
            list(GET LINE 0 _MAJOR)
            list(GET LINE 1 _MINOR)
            list(GET LINE 2 _PATCH)
            message(STATUS "Redis version: ${_MAJOR}.${_MINOR}.${_PATCH}")
            # Set the output variables
            set(${OUT_MAJOR}
                ${_MAJOR}
                PARENT_SCOPE)
            set(${OUT_MINOR}
                ${_MINOR}
                PARENT_SCOPE)
            set(${OUT_PATCH}
                ${_PATCH}
                PARENT_SCOPE)
        endif ()
    endforeach ()
endfunction ()

# Given input argument `OPTION_VALUE`, check that the `OPTION_VALUE` is from the allowed values (one of:
# module/yes/no/1/0/true/false)
#
# Return value:
#
# If ARG is valid, return its number where:
#
# ~~~
# - `no` | `0` | `off` => return `0`
# - `yes` | `1` | `on` => return  `1`
# - `module` => return  `2`
# ~~~
function (redis_parse_build_option OPTION_VALUE OUT_ARG_ENUM)
    list(APPEND VALID_OPTIONS "yes")
    list(APPEND VALID_OPTIONS "1")
    list(APPEND VALID_OPTIONS "on")
    list(APPEND VALID_OPTIONS "no")
    list(APPEND VALID_OPTIONS "0")
    list(APPEND VALID_OPTIONS "off")
    list(APPEND VALID_OPTIONS "module")

    string(TOLOWER "${OPTION_VALUE}" OPTION_VALUE)
    list(FIND VALID_OPTIONS "${ARG}" OPT_INDEX)
    if (VERSION_STR_POS GREATER -1)
        message(FATAL_ERROR "Invalid value passed ''${OPTION_VALUE}'")
    endif ()

    if ("${OPTION_VALUE}" STREQUAL "yes"
        OR "${OPTION_VALUE}" STREQUAL "1"
        OR "${OPTION_VALUE}" STREQUAL "on")
        set(${OUT_ARG_ENUM}
            1
            PARENT_SCOPE)
    elseif (
        "${OPTION_VALUE}" STREQUAL "no"
        OR "${OPTION_VALUE}" STREQUAL "0"
        OR "${OPTION_VALUE}" STREQUAL "off")
        set(${OUT_ARG_ENUM}
            0
            PARENT_SCOPE)
    else ()
        set(${OUT_ARG_ENUM}
            2
            PARENT_SCOPE)
    endif ()
endfunction ()

function (redis_pkg_config PKGNAME OUT_VARIABLE)
    if (NOT FOUND_PKGCONFIG)
        # Locate pkg-config once
        find_package(PkgConfig REQUIRED)
        set(FOUND_PKGCONFIG 1)
    endif ()
    pkg_check_modules(__PREFIX REQUIRED ${PKGNAME})
    message(STATUS "Found library for '${PKGNAME}': ${__PREFIX_LIBRARIES}")
    set(${OUT_VARIABLE}
        "${__PREFIX_LIBRARIES}"
        PARENT_SCOPE)
endfunction ()
