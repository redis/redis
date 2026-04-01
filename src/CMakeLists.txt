include_directories(BEFORE "${CMAKE_SOURCE_DIR}/deps/lua/src")
include_directories(BEFORE "${CMAKE_SOURCE_DIR}/deps/xxhash")
project(redis-server)

set(INSTALL_BIN_PATH ${CMAKE_INSTALL_PREFIX}/bin)
set_directory_properties(PROPERTIES CLEAN_NO_CUSTOM 1)

# Target: redis-server
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${REDIS_SERVER_CFLAGS}")
message(STATUS "CFLAGS: ${CMAKE_C_FLAGS}")

get_redis_server_linker_option(REDIS_SERVER_LDFLAGS)
list(APPEND SERVER_LIBS "fpconv")
list(APPEND SERVER_LIBS "hdr_histogram")
list(APPEND SERVER_LIBS "ffc")
list(APPEND SERVER_LIBS "lualib")
list(APPEND SERVER_LIBS "xxhash")
redis_build_and_install_bin(redis-server "${REDIS_SERVER_SRCS}" "${REDIS_SERVER_LDFLAGS}" "${SERVER_LIBS}"
                             "redis-server")
add_dependencies(redis-server generate_commands_def)
add_dependencies(redis-server generate_fmtargs_h)
add_dependencies(redis-server release_header)

if (BUILD_LUA)
    message(STATUS "Build Lua scripting engine module")
    
    add_dependencies(redis-server lualib)
    target_compile_definitions(redis-server PRIVATE LUA_ENABLED)
    target_include_directories(redis-server PUBLIC "${CMAKE_SOURCE_DIR}/deps/lua/src" "${CMAKE_SOURCE_DIR}/deps/xxhash")
    if (UNIX AND NOT APPLE)
        target_compile_definitions(redis-server PRIVATE LUA_LIB=hilualib.so)
        target_link_options(redis-server PRIVATE -Wl,--disable-new-dtags)
    else ()
        target_compile_definitions(redis-server PRIVATE LUA_LIB=hilualib.dylib)
    endif ()
    set(REDIS_INSTALL_RPATH "")
    set_target_properties(redis-server PROPERTIES
        INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR};${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
        INSTALL_RPATH_USE_LINK_PATH TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()
unset(BUILD_LUA CACHE)

if (REDIS_RELEASE_BUILD)
    # Enable LTO for Release build
    set_property(TARGET redis-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (DEBUG_FORCE_DEFRAG)
    message(STATUS "Forcing Active Defrag run on redis-server")
    target_compile_definitions(redis-server PRIVATE DEBUG_FORCE_DEFRAG)
    target_compile_definitions(redis-server PRIVATE HAVE_DEFRAG)
endif ()

if (BUILD_SANITIZER)
    # 'BUILD_SANITIZER' is defined in RedisSetup module (based on user input)
    # If defined, the variables 'REDIS_SANITAIZER_CFLAGS' and 'REDIS_SANITAIZER_LDFLAGS'
    # are set with the link & compile flags required
    message(STATUS "Adding sanitizer flags for target redis-server")
    target_compile_options(redis-server PRIVATE ${REDIS_SANITAIZER_CFLAGS})
    target_link_options(redis-server PRIVATE ${REDIS_SANITAIZER_LDFLAGS})
endif ()
unset(BUILD_SANITIZER CACHE)

# Target: redis-cli
list(APPEND CLI_LIBS "fpconv")
list(APPEND CLI_LIBS "linenoise")
list(APPEND CLI_LIBS "ffc")
list(APPEND CLI_LIBS "hdr_histogram")
list(APPEND CLI_LIBS "xxhash")
redis_build_and_install_bin(redis-cli "${REDIS_CLI_SRCS}" "${REDIS_SERVER_LDFLAGS}" "${CLI_LIBS}" "redis-cli")
add_dependencies(redis-cli generate_commands_def)
add_dependencies(redis-cli generate_fmtargs_h)



# Targets: redis-sentinel, redis-check-aof and redis-check-rdb are just symbolic links
redis_create_symlink("redis-server" "redis-sentinel")
redis_create_symlink("redis-server" "redis-check-rdb")
redis_create_symlink("redis-server" "redis-check-aof")

# Target redis-rdma
if (BUILD_RDMA_MODULE)
    set(MODULE_NAME "redis-rdma")
    message(STATUS "Building RDMA module")
    add_library(${MODULE_NAME} SHARED "${REDIS_RDMA_MODULE_SRCS}")
    target_compile_options(${MODULE_NAME} PRIVATE -DBUILD_RDMA_MODULE=2 -DUSE_RDMA=1)
    target_link_libraries(${MODULE_NAME} "${RDMA_LIBS}")
    # remove the "lib" prefix from the module
    set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "")
    redis_install_bin(${MODULE_NAME})
endif ()

# Target redis-tls (a module)
if (BUILD_TLS_MODULE)
    message(STATUS "Building TLS as a module")
    set(MODULE_NAME "redis-tls")
    add_library(${MODULE_NAME} SHARED ${REDIS_TLS_MODULE_SRCS})
    target_compile_options(${MODULE_NAME} PRIVATE -DUSE_OPENSSL=2 -DBUILD_TLS_MODULE=2)
    if (APPLE)
        # Some symbols can only be resolved during runtime (they exist in the executable)
        target_link_options(${MODULE_NAME} PRIVATE -undefined dynamic_lookup)
    endif ()
    target_link_libraries(${MODULE_NAME} hiredis::hiredis_ssl OpenSSL::SSL)
    set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "")
endif ()

if (BUILD_EXAMPLE_MODULES)
    # Include the modules ("hello*")
    message(STATUS "Building example modules")
    add_subdirectory(modules)
endif ()

if (BUILD_UNIT_GTESTS)
    if(NOT MSVC)
    add_subdirectory(unit)
endif()
endif ()

# Friendly hint like the Makefile one
file(RELATIVE_PATH _CMAKE_DIR_RELATIVE_PATH "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
add_custom_target(hint ALL
    DEPENDS redis-server redis-cli 
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Hint: It is a good idea to run tests with your CMake-built binaries \\;\\)"
    COMMAND ${CMAKE_COMMAND} -E echo "      ./${_CMAKE_DIR_RELATIVE_PATH}/runtest"
    COMMAND ${CMAKE_COMMAND} -E echo ""
)


target_link_libraries(redis-server lualib hdr_histogram ffc xxhash)
target_link_libraries(redis-cli hdr_histogram ffc xxhash)



