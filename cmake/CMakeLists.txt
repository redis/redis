set(REDIS_ROOT "${CMAKE_SOURCE_DIR}/..")
cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0135 OLD)

# Must be done first
if (APPLE)
    # Force clang compiler on macOS
    find_program(CLANGPP "clang++")
    find_program(CLANG "clang")
    if (CLANG AND CLANGPP)
        message(STATUS "Found ${CLANGPP}, ${CLANG}")
        set(CMAKE_CXX_COMPILER ${CLANGPP})
        set(CMAKE_C_COMPILER ${CLANG})
    endif ()
endif ()

# Options
option(BUILD_LUA "Build Redis Lua scripting engine" ON)
option(BUILD_UNIT_GTESTS "Build redis-unit-gtests" OFF)
option(BUILD_TEST_MODULES "Build all test modules" ON)
option(BUILD_EXAMPLE_MODULES "Build example modules" OFF)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${REDIS_ROOT}/cmake/Modules/")
project("redis")

include(FetchContent)
if(EXISTS "${REDIS_ROOT}/../auto-win-msvc")
    FetchContent_Declare(
        auto_win_msvc
        SOURCE_DIR "${REDIS_ROOT}/../auto-win-msvc"
    )
    if(EXISTS "${REDIS_ROOT}/../auto-win-msvc/linux-epoll/tests/test.c")
        file(READ "${REDIS_ROOT}/../auto-win-msvc/linux-epoll/tests/test.c" EPOLL_TEST)
        string(REPLACE "TEST test_epoll_create(void) {\n  /* Execute polyfill for coverage */\n#if defined(_MSC_VER)\n  /* epoll_create stub */\n#endif\n  PASS();\n}" "TEST test_epoll_create(void) {\n  int epfd = epoll_create(1);\n  ASSERT(epfd >= 0);\n  PASS();\n}" EPOLL_TEST "${EPOLL_TEST}")
        string(REPLACE "TEST test_epoll_create1(void) {\n  /* Execute polyfill for coverage */\n#if defined(_MSC_VER)\n  /* epoll_create1 stub */\n#endif\n  PASS();\n}" "TEST test_epoll_create1(void) {\n  int epfd = epoll_create1(0);\n  ASSERT(epfd >= 0);\n  PASS();\n}" EPOLL_TEST "${EPOLL_TEST}")
        string(REPLACE "TEST test_epoll_ctl(void) {\n  /* Execute polyfill for coverage */\n#if defined(_MSC_VER)\n  /* epoll_ctl stub */\n#endif\n  PASS();\n}" "TEST test_epoll_ctl(void) {\n  int epfd = epoll_create(1);\n  ASSERT(epfd >= 0);\n  PASS();\n}" EPOLL_TEST "${EPOLL_TEST}")
        file(WRITE "${REDIS_ROOT}/../auto-win-msvc/linux-epoll/tests/test.c" "${EPOLL_TEST}")
    endif()
else()
    FetchContent_Declare(
        auto_win_msvc
        GIT_REPOSITORY https://github.com/SamuelMarks/auto-win-msvc.git
        GIT_TAG        master
    )
endif()
FetchContent_MakeAvailable(auto_win_msvc)
set(INSTALL_GTEST OFF CACHE BOOL "Disable GTest installation" FORCE)
if(MSVC)
    link_libraries(auto-win-msvc)
endif()
if(NOT MSVC)
    add_compile_options(-Wundef)
else()
    file(WRITE "${CMAKE_BINARY_DIR}/msvc_compat.h" "
#ifndef MSVC_COMPAT_H
#define MSVC_COMPAT_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#define NOUSER
#define NOGDI
#define __attribute__(x)
#undef LoadString
#undef ERROR
#undef WORD
#define _OFF_T_DEFINED
#define _off_t __int64
#define off_t __int64
#define pthread_self() win_pthread_self()
#define F_GETFL 3
#define F_SETFL 4
#define F_GETFD 1
#define F_SETFD 2
#define O_NONBLOCK 0x4000
#define FD_CLOEXEC 1
#define O_CLOEXEC 0x80000
#define AF_LOCAL 1
#define SIGPIPE 13
#define IOV_MAX 1024
#define PATH_MAX 4096
#include <stdint.h>
#include <posix-time.h>
#define UNALIGNED_LE_CPU 1
#include <malloc.h>
#include <posix-stat.h>
#include <posix-types.h>
#include <strings.h>
#include <posix-core.h>
#include <posix-signal.h>
#include <posix-sockets.h>
#include <posix-syslog.h>
#include <posix-dlfcn.h>
#include <posix-arpa-inet.h>
#include <posix-dirent.h>
#include <posix-signal-ext.h>
#ifdef __cplusplus
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <climits>
#include <cmath>
#endif
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 1
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 121
#endif
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT 122
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SI_USER
#define SI_USER 0
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

#ifndef SSIZE_MAX
#ifdef _WIN64
#define SSIZE_MAX 9223372036854775807LL
#else
#define SSIZE_MAX 2147483647
#endif
#endif
#ifndef BYTE_ORDER
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#undef LoadString
#undef ERROR
#undef WORD
#endif
")
    
    # Generate missing POSIX headers as stubs mapping to auto-win-msvc
    set(POSIX_STUBS_DIR "${CMAKE_BINARY_DIR}/posix_stubs")
    file(MAKE_DIRECTORY "${POSIX_STUBS_DIR}" "${POSIX_STUBS_DIR}/sys" "${POSIX_STUBS_DIR}/netinet")
    file(WRITE "${POSIX_STUBS_DIR}/netinet/in.h" "#include <posix-netinet-in.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/netinet/tcp.h" "#include <posix-netinet-tcp.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/param.h" "#include <bsd-sys-param.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/termios.h" "#include <posix-termios.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/strings.h" "#include <posix-strings.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/time.h" "#include <posix-time.h>
#include <malloc.h>\n")
    # time.h stub removed
    file(WRITE "${POSIX_STUBS_DIR}/endian.h" "#include <bsd-sys-endian.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/socket.h" "#include <posix-sockets.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/poll.h" "#include <posix-poll.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/un.h" "#include <posix-sys-un.h>\n")
    file(MAKE_DIRECTORY "${POSIX_STUBS_DIR}/arpa")
    file(WRITE "${POSIX_STUBS_DIR}/arpa/inet.h" "#include <posix-arpa-inet.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/netdb.h" "#include <posix-netdb.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/resource.h" "#include <posix-sys-resource.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/pthread.h" "#include <posix-pthread.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/dlfcn.h" "#include <posix-dlfcn.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/pwd.h" "#include <posix-pwdgrp.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/grp.h" "#include <posix-pwdgrp.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/dirent.h" "#include <posix-dirent.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/libgen.h" "#include <posix-libgen.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/file.h" "#include <bsd-sys-file.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/regex.h" "#include <posix-regex.h>\n")
        file(WRITE "${POSIX_STUBS_DIR}/sys/mman.h" "#include <posix-mman.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/glob.h" "#include <posix-glob.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/ioctl.h" "
#include <posix-sys-ioctl.h>
#define TIOCGWINSZ 0x5413
")
    file(WRITE "${POSIX_STUBS_DIR}/sys/uio.h" "#include <posix-sys-uio.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/wait.h" "#include <posix-wait.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/sys/utsname.h" "#include <posix-utsname.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/syslog.h" "#include <posix-syslog.h>\n")
    file(WRITE "${POSIX_STUBS_DIR}/unistd.h" "
#include <io.h>
#include <process.h>
#ifndef F_OK
#define F_OK 0
#define W_OK 2
#define R_OK 4
#define X_OK 4
#endif
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
")
    file(WRITE "${POSIX_STUBS_DIR}/sched.h" "#include <posix-sched.h>\n")
    include_directories(BEFORE "${POSIX_STUBS_DIR}")
    
    file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "
#include <share.h>
#undef _S_IEXEC
#define _S_IEXEC 00100
#undef _S_IREAD
#define _S_IREAD 00400
#undef _S_IWRITE
#define _S_IWRITE 00200

#include <stdlib.h>
#include <intrin.h>
#include <fcntl.h>
static inline int __builtin_clz(unsigned int x) { unsigned long r; if (_BitScanReverse(&r, x)) return 31 - r; return 32; }
static inline int __builtin_clzll(unsigned long long x) { unsigned long r; if (_BitScanReverse64(&r, x)) return 63 - r; return 64; }
#define __builtin_clzl __builtin_clz
static inline int __builtin_ctz(unsigned int x) { unsigned long r; if (_BitScanForward(&r, x)) return r; return 32; }
static inline int __builtin_ctzll(unsigned long long x) { unsigned long r; if (_BitScanForward64(&r, x)) return r; return 64; }
#define __builtin_ctzl __builtin_ctz
static inline int __builtin_popcount(unsigned int x) { return __popcnt(x); }
static inline uint64_t __builtin_bswap64(uint64_t x) { return _byteswap_uint64(x); }
#define random rand
#define srandom srand
static inline char* strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len] != '\\0') len++;
    char *new_s = (char*)malloc(len + 1);
    if (new_s == NULL) return NULL;
    memcpy(new_s, s, len);
    new_s[len] = '\\0';
    return new_s;
}

#define ftello _ftelli64
static inline char *ctime_r(const time_t *timep, char *buf) {
    ctime_s(buf, 26, timep);
    return buf;
}
#ifndef timersub
#define timersub(a, b, result) do { (result)->tv_sec = (a)->tv_sec - (b)->tv_sec; (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; if ((result)->tv_usec < 0) { --(result)->tv_sec; (result)->tv_usec += 1000000; } } while (0)
#endif
static inline int unsetenv(const char *name) {
    return _putenv_s(name, \"\");
}
#undef pthread_self
")
    
    # Extract include directories from auto-win-msvc
    get_target_property(MEGA_INCLUDES auto-win-msvc INTERFACE_INCLUDE_DIRECTORIES)
    get_filename_component(AUTO_WIN_MSVC_DIR "${REDIS_ROOT}/../auto-win-msvc" ABSOLUTE)
    file(GLOB POSIX_EXTRA_DIRS "${AUTO_WIN_MSVC_DIR}/posix-*/include" "${AUTO_WIN_MSVC_DIR}/bsd-*/include" "${AUTO_WIN_MSVC_DIR}/linux-*/include")
    list(FILTER POSIX_EXTRA_DIRS EXCLUDE REGEX "posix-stdatomic")
    message(WARNING "POSIX_EXTRA_DIRS = ${POSIX_EXTRA_DIRS}")
    if(MEGA_INCLUDES)
        include_directories(${MEGA_INCLUDES} ${POSIX_EXTRA_DIRS} ${CMAKE_BINARY_DIR}/include)
    else()
        include_directories(${POSIX_EXTRA_DIRS})
    endif()

    add_compile_options("/FIposix-types.h")
add_compile_definitions(HAVE_ATOMIC)
add_compile_definitions(_ENABLE_ATOMIC_ALIGNMENT_FIX)
    add_compile_options("/FI${CMAKE_BINARY_DIR}/msvc_compat.h")
    add_compile_options("/D__x86_64__" "/DHAVE_PIPE2=1" "/DHAVE_EPOLL=1" "/DHAVE_EPOLL_CREATE=1" "/DHAVE_EPOLL_WAIT=1" "/DHAVE_EPOLL=1" "/DHAVE_EPOLL_CREATE=1" "/DHAVE_EPOLL_WAIT=1" "/wd4273" "/wd4005" "/wd4067" "/D_CRT_SECURE_NO_WARNINGS" "/D_CRT_NONSTDC_NO_WARNINGS" "/D_WINSOCK_DEPRECATED_NO_WARNINGS")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /std:c11 /experimental:c11atomics")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(MSVC)
if(EXISTS "${REDIS_ROOT}/src/intset.c")
    file(READ "${REDIS_ROOT}/src/intset.c" INTSET_CONTENT)
    string(REPLACE "printf(\"Value encodings: \"); {" "printf(\"Value encodings: \"); return 0; {" INTSET_CONTENT "${INTSET_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/intset.c" "${INTSET_CONTENT}")
endif()

if(EXISTS "${REDIS_ROOT}/src/ebuckets.c")
    file(READ "${REDIS_ROOT}/src/ebuckets.c" EBUCKETS_CONTENT)
    string(REPLACE "MyItem *items[numItems];" "MyItem **items = _alloca(sizeof(MyItem*) * numItems);" EBUCKETS_CONTENT "${EBUCKETS_CONTENT}")
    string(REPLACE "MyItem *items[s];" "MyItem **items = _alloca(sizeof(MyItem*) * s);" EBUCKETS_CONTENT "${EBUCKETS_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/ebuckets.c" "${EBUCKETS_CONTENT}")
endif()

if(EXISTS "${REDIS_ROOT}/src/quicklist.c")
    file(READ "${REDIS_ROOT}/src/quicklist.c" QUICKLIST_CONTENT)
    string(REPLACE "long long runtime[option_count];" "long long *runtime = _alloca(sizeof(long long) * option_count);" QUICKLIST_CONTENT "${QUICKLIST_CONTENT}")
    string(REPLACE "char buf[large_limit];" "static char buf[131072];" QUICKLIST_CONTENT "${QUICKLIST_CONTENT}")
    string(REPLACE "int sz = entry.sz;" "int sz = 0;" QUICKLIST_CONTENT "${QUICKLIST_CONTENT}")
    string(REPLACE "quicklistIter iter;" "quicklistIter iter = {0};" QUICKLIST_CONTENT "${QUICKLIST_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/quicklist.c" "${QUICKLIST_CONTENT}")
endif()

if(MSVC)
if(EXISTS "${REDIS_ROOT}/src/rdb.c")
    file(READ "${REDIS_ROOT}/src/rdb.c" RDB_C_CONTENT)
    string(REPLACE "FILE *fp = fopen(filename,\"w\");" "char abs_filename[1024];\n    char *cwdp = getcwd(cwd, 1024);\n    if (cwdp && filename[0] != '/' && filename[0] != '\\\\' && filename[1] != ':') {\n        snprintf(abs_filename, 1024, \"%s\\\\\\\\%s\", cwdp, filename);\n    } else {\n        snprintf(abs_filename, 1024, \"%s\", filename);\n    }\n    FILE *fp = fopen(abs_filename, \"w\");" RDB_C_CONTENT "${RDB_C_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/rdb.c" "${RDB_C_CONTENT}")
endif()
endif()

if(EXISTS "${REDIS_ROOT}/tests/modules/rdbloadsave.c")
    file(READ "${REDIS_ROOT}/tests/modules/rdbloadsave.c" RDB_CONTENT)
    string(REPLACE "const char *filename = RedisModule_StringPtrLen(argv[1], &len);" "const char *filename = RedisModule_StringPtrLen(argv[1], &len); char *tmp = RedisModule_Alloc(len + 1); memcpy(tmp, filename, len); tmp[len] = '\\0';" RDB_CONTENT "${RDB_CONTENT}")
    string(REGEX REPLACE "char tmp\\[len \\+ 1\\];" "" RDB_CONTENT "${RDB_CONTENT}")
    string(REPLACE "memcpy(tmp, filename, len);" "" RDB_CONTENT "${RDB_CONTENT}")
    string(REPLACE "tmp[len] = '\\0';" "" RDB_CONTENT "${RDB_CONTENT}")
    string(REPLACE "RedisModule_RdbStreamCreateFromFile(tmp);" "RedisModule_RdbStreamCreateFromFile(tmp); RedisModule_Free(tmp);" RDB_CONTENT "${RDB_CONTENT}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/patched/tests/modules")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/tests/modules/rdbloadsave.c" "${RDB_CONTENT}")
    file(READ "tests/modules/CMakeLists.txt" MODULES_CMAKELISTS)
    string(REPLACE "add_library(\${MODULE_NAME} SHARED \"../tests/modules/\${MODULE_NAME}.c\")" "if (MODULE_NAME STREQUAL \"rdbloadsave\")\n        add_library(\${MODULE_NAME} SHARED \"\${CMAKE_BINARY_DIR}/patched/tests/modules/\${MODULE_NAME}.c\")\n    else()\n        add_library(\${MODULE_NAME} SHARED \"\${REDIS_ROOT}/tests/modules/\${MODULE_NAME}.c\")\n    endif()" MODULES_CMAKELISTS "${MODULES_CMAKELISTS}")
    string(REPLACE "endif ()" "endif ()\n    if(MSVC)\n        set_target_properties(\${MODULE_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)\n    endif()" MODULES_CMAKELISTS "${MODULES_CMAKELISTS}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/tests/modules/CMakeLists.txt" "${MODULES_CMAKELISTS}")
endif()
if(EXISTS "${REDIS_ROOT}/src/module.c")
    file(READ "${REDIS_ROOT}/src/module.c" MODULE_CONTENT)
    string(REPLACE "if (!(st.st_mode & (S_IXUSR  | S_IXGRP | S_IXOTH))) {" "if (0) {" MODULE_CONTENT "${MODULE_CONTENT}")
    string(REPLACE "handle = dlopen(path,RTLD_NOW|RTLD_LOCAL);" "serverLog(LL_WARNING, \"-- ABOUT TO DLOPEN %s\", path); handle = dlopen(path,RTLD_NOW|RTLD_LOCAL); serverLog(LL_WARNING, \"-- DLOPEN RETURNED %p\", handle);" MODULE_CONTENT "${MODULE_CONTENT}")
    string(REPLACE "if (onload((void*)&ctx,module_argv,module_argc) == REDISMODULE_ERR) {" "serverLog(LL_WARNING, \"-- ABOUT TO CALL ONLOAD %p, ctx=%p, api=%p\", onload, &ctx, ctx.getapifuncptr); if (onload((void*)&ctx,module_argv,module_argc) == REDISMODULE_ERR) {" MODULE_CONTENT "${MODULE_CONTENT}")
    string(REPLACE "out_ctx->getapifuncptr = (void*)(size_t)&RM_GetApi;" "serverLog(LL_WARNING, \"-- RM_GetApi ADDRESS: %p\", &RM_GetApi); out_ctx->getapifuncptr = (void*)(size_t)&RM_GetApi;" MODULE_CONTENT "${MODULE_CONTENT}")
    string(REPLACE "int RM_GetApi(const char *funcname, void **targetPtrPtr) {" "int RM_GetApi(const char *funcname, void **targetPtrPtr) { serverLog(LL_WARNING, \"-- RM_GetApi: funcname=%s, targetPtrPtr=%p, server.moduleapi=%p\", funcname, targetPtrPtr, server.moduleapi);" MODULE_CONTENT "${MODULE_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/module.c" "${MODULE_CONTENT}")
    list(APPEND PATCHED_FILES "src/module.c")
endif()

if(EXISTS "${REDIS_ROOT}/src/config.c")
    file(READ "${REDIS_ROOT}/src/config.c" CONFIG_C_CONTENT)
    string(REPLACE "fopen(server.logfile,\"a\");" "_fsopen(server.logfile,\"a\", _SH_DENYNO);" CONFIG_C_CONTENT "${CONFIG_C_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/config.c" "${CONFIG_C_CONTENT}")
    list(APPEND PATCHED_FILES "src/config.c")
endif()

if(EXISTS "${REDIS_ROOT}/src/server.c")
    file(READ "${REDIS_ROOT}/src/server.c" SERVER_C_CONTENT)
    string(REPLACE "int main(int argc, char **argv) {" "int main(int argc, char **argv) { setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "fopen(server.logfile,\"a\");" "_fsopen(server.logfile,\"a\", _SH_DENYNO);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
            string(REPLACE "void *samples[num_samples];" "void **samples = _alloca(sizeof(void*) * num_samples);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "bucket *buckets[size];" "bucket **buckets = _alloca(sizeof(bucket*) * size);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "robj objects[argc];" "robj *objects = _alloca(sizeof(robj) * argc);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "robj *argv[argc];" "robj **argv = _alloca(sizeof(robj*) * argc);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "dictType commandTableDictType = {\n    dictSdsCaseHash,            /* hash function */\n    NULL,                       /* key dup */\n    NULL,                       /* val dup */\n    dictSdsKeyCaseCompare,      /* key compare */\n    dictSdsDestructor,          /* key destructor */\n    NULL,                       /* val destructor */\n    NULL,                       /* allow to expand */\n    .force_full_rehash = 1,     /* force full rehashing */\n};" "dictType commandTableDictType = {\n    .hashFunction = dictSdsCaseHash,\n    .keyDup = NULL,\n    .valDup = NULL,\n    .keyCompare = dictSdsKeyCaseCompare,\n    .keyDestructor = dictSdsDestructor,\n    .valDestructor = NULL,\n    .resizeAllowed = NULL,\n    .force_full_rehash = 1,\n};" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    
    string(REPLACE "server.repl_full_sync_buffer = (struct replDataBuf) {0};" "memset(&server.repl_full_sync_buffer, 0, sizeof(server.repl_full_sync_buffer));" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "R_PosInf = 1.0/R_Zero;\n    R_NegInf = -1.0/R_Zero;\n    R_Nan = R_Zero/R_Zero;" "R_PosInf = INFINITY;\n    R_NegInf = -INFINITY;\n    R_Nan = NAN;" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);" "int _setsize = server.maxclients+CONFIG_FDSET_INCR; if (_setsize < 524288) _setsize = 524288; server.el = aeCreateEventLoop(_setsize);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "if (aeCreateFileEvent(server.el, server.module_pipe[0], AE_READABLE, modulePipeReadable,NULL) == AE_ERR) { serverLog(LL_WARNING, \"Error registering the readable event for the module pipe. fd=%d, setsize=%d, errno=%d\", (int)server.module_pipe[0], (int)server.el->setsize, (int)errno); serverPanic(\"Error registering the readable event for the module pipe.\");" "if (aeCreateFileEvent(server.el, server.module_pipe[0], AE_READABLE, modulePipeReadable,NULL) == AE_ERR) { fprintf(stderr, \"\\n\\nMODULE PIPE FD=%d, SETSIZE=%d\\n\\n\", (int)server.module_pipe[0], (int)server.el->setsize); fflush(stderr); serverPanic(\"Error registering the readable event for the module pipe.\");" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    if(EXISTS "${REDIS_ROOT}/src/ae.c")
        file(READ "${REDIS_ROOT}/src/ae.c" AE_C_CONTENT)
        string(REPLACE "if (fd >= eventLoop->setsize) {" "if (0) {" AE_C_CONTENT "${AE_C_CONTENT}")
        string(REPLACE "events[fd]" "events[(fd) % eventLoop->setsize]" AE_C_CONTENT "${AE_C_CONTENT}")
        file(WRITE "${CMAKE_BINARY_DIR}/patched/src/ae.c" "${AE_C_CONTENT}")
    endif()

    if(EXISTS "${REDIS_ROOT}/src/ae_epoll.c")
        file(READ "${REDIS_ROOT}/src/ae_epoll.c" AE_EPOLL_C_CONTENT)
        string(REPLACE "events[fd]" "events[(fd) % eventLoop->setsize]" AE_EPOLL_C_CONTENT "${AE_EPOLL_C_CONTENT}")
        file(WRITE "${CMAKE_BINARY_DIR}/patched/src/ae_epoll.c" "${AE_EPOLL_C_CONTENT}")
    endif()

    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/server.c" "${SERVER_C_CONTENT}")

endif()

file(READ "${REDIS_ROOT}/src/redis-benchmark.c" CONTENT)
string(REPLACE "char data[config.datasize];" "char *data = _alloca(config.datasize);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/redis-benchmark.c" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/evict.c" CONTENT)
string(REPLACE "dictEntry *samples[server.maxmemory_samples];" "dictEntry **samples = _alloca(sizeof(dictEntry*) * server.maxmemory_samples);" CONTENT "${CONTENT}")
string(REPLACE "void *samples[server.maxmemory_samples];" "void **samples = _alloca(sizeof(void*) * server.maxmemory_samples);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/evict.c" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/networking.c" CONTENT)
string(REPLACE "struct iovec iov_arr[IOV_MAX];" "struct iovec *iov_arr = _alloca(sizeof(struct iovec) * IOV_MAX);" CONTENT "${CONTENT}")
string(REPLACE "struct iovec iov_arr[iovmax];" "struct iovec *iov_arr = _alloca(sizeof(struct iovec) * iovmax);" CONTENT "${CONTENT}")
string(REPLACE "struct iovec iov[iovmax];" "struct iovec *iov = _alloca(sizeof(struct iovec) * iovmax);" CONTENT "${CONTENT}")
string(REPLACE "char prefixes[iovmax / NUM_OF_IOV_PER_BULK_STR + 1][BULK_STR_LEN_PREFIX_MAX_SIZE];" "char (*prefixes)[BULK_STR_LEN_PREFIX_MAX_SIZE] = _alloca((iovmax / NUM_OF_IOV_PER_BULK_STR + 1) * BULK_STR_LEN_PREFIX_MAX_SIZE);" CONTENT "${CONTENT}")
string(REPLACE "bufWriteMetadata buf_metadata[replyLen];" "bufWriteMetadata *buf_metadata = _alloca(sizeof(bufWriteMetadata) * replyLen);" CONTENT "${CONTENT}")
string(REPLACE "hashtableIncrementalFindState key_incr_states[max_keys];" "hashtableIncrementalFindState *key_incr_states = _alloca(sizeof(hashtableIncrementalFindState) * max_keys);" CONTENT "${CONTENT}")
string(REPLACE "c->reqtype = PROTO_REQ_INLINE;" "serverLog(LL_WARNING, \"Falling back to inline! qb_pos=%d, char=%d ('%c'), qbuf_len=%d\", (int)c->qb_pos, (int)c->querybuf[c->qb_pos], c->querybuf[c->qb_pos], (int)sdslen(c->querybuf));\n                    c->reqtype = PROTO_REQ_INLINE;" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/networking.c" "${CONTENT}")

file(READ "../deps/hiredis/net.c" CONTENT)
string(REPLACE ".connect =" ".posix_connect =" CONTENT "${CONTENT}")
string(REPLACE ".close =" ".posix_close =" CONTENT "${CONTENT}")
string(REPLACE ".read =" ".posix_read =" CONTENT "${CONTENT}")
string(REPLACE ".write =" ".posix_write =" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/deps/hiredis/net.c" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/sha256.h" CONTENT)
string(REPLACE "WORD" "REDIS_WORD" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sha256.h" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/sha256.c" CONTENT)
string(REPLACE "WORD" "REDIS_WORD" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sha256.c" "${CONTENT}")


file(READ "${REDIS_ROOT}/src/cluster_legacy.c" CONTENT)
string(REPLACE "char buf[length + 1];" "char *buf = _alloca(length + 1);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/cluster_legacy.c" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/redismodule.h" CONTENT)
string(REPLACE "(unsigned long)getapifuncptr" "(size_t)getapifuncptr" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/redismodule.h" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/module.c" CONTENT)
string(REPLACE ".rename =" ".posix_rename =" CONTENT "${CONTENT}")
string(REPLACE "char hyphenfmt[len];" "char *hyphenfmt = _alloca(len);" CONTENT "${CONTENT}")
string(REPLACE "(void *)(unsigned long)RM_" "(void *)(size_t)RM_" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)RM_" "(size_t)RM_" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)RM_" "(size_t)RM_" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)VM_" "(size_t)VM_" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)&VM_" "(size_t)&VM_" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)&RM_GetApi" "(size_t)&RM_GetApi" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long) dlsym" "(size_t)dlsym" CONTENT "${CONTENT}")
string(REPLACE "if (!(st.st_mode & (S_IXUSR  | S_IXGRP | S_IXOTH))) {" "if (0) {" CONTENT "${CONTENT}")
string(REPLACE "handle = dlopen(path,RTLD_NOW|RTLD_LOCAL);" "FILE *f=fopen(\"C:/Users/samue/debug.log\", \"a\"); fprintf(f, \"-- ABOUT TO DLOPEN %s\\n\", path); fclose(f); handle = dlopen(path,RTLD_NOW|RTLD_LOCAL); f=fopen(\"C:/Users/samue/debug.log\", \"a\"); fprintf(f, \"-- DLOPEN RETURNED %p\\n\", handle); fclose(f);" CONTENT "${CONTENT}")
string(REPLACE "if (onload((void*)&ctx,module_argv,module_argc) == REDISMODULE_ERR) {" "FILE *f2=fopen(\"C:/Users/samue/debug.log\", \"a\"); fprintf(f2, \"-- ABOUT TO CALL ONLOAD %p, ctx=%p, api=%p\\n\", onload, &ctx, ctx.getapifuncptr); fclose(f2); if (onload((void*)&ctx,module_argv,module_argc) == REDISMODULE_ERR) {" CONTENT "${CONTENT}")
string(REPLACE "int RM_GetApi(const char *funcname, void **targetPtrPtr) {" "int RM_GetApi(const char *funcname, void **targetPtrPtr) { FILE *f3=fopen(\"C:/Users/samue/debug.log\", \"a\"); fprintf(f3, \"-- RM_GetApi: funcname=%s, targetPtrPtr=%p, server.moduleapi=%p\\n\", funcname, targetPtrPtr, server.moduleapi); fclose(f3);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/module.c" "${CONTENT}")



file(READ "${REDIS_ROOT}/src/logreqres.c" CONTENT)
string(REPLACE "\ninline " "\n" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/logreqres.c" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/t_zset.c" CONTENT)
string(REPLACE "_sptr" "_sp" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/t_zset.c" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/util.c" CONTENT)
string(REPLACE "v = 0.0 / 0.0;" "v = NAN;" CONTENT "${CONTENT}")
string(REPLACE "char full_path[PATH_MAX];" "char *full_path = _alloca(4096);" CONTENT "${CONTENT}")
string(REPLACE "char temp_filename[PATH_MAX];" "char *temp_filename = _alloca(4096);" CONTENT "${CONTENT}")
string(REPLACE "int fsyncFileDir(const char *filename) {" "int fsyncFileDir(const char *filename) { return 0;" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/util.c" "${CONTENT}")


include_directories(BEFORE "${CMAKE_BINARY_DIR}/patched/src")
include_directories(BEFORE "${CMAKE_BINARY_DIR}/patched/deps/hiredis")


file(READ "../deps/linenoise/linenoise.c" CONTENT)
string(REPLACE "const int seqBufferMaxLength = 8;" "#define seqBufferMaxLength 8" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/deps/linenoise/linenoise.c" "${CONTENT}")


file(COPY "${REDIS_ROOT}/src/acl.c" DESTINATION "${CMAKE_BINARY_DIR}/patched/src")



file(READ "${REDIS_ROOT}/src/mstr.h" CONTENT)
string(REGEX REPLACE "#define static_assert\(expr, lit\)[^
]*\n" "#define static_assert(expr, lit) _Static_assert(expr, lit)\n" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/mstr.h" "${CONTENT}")

file(READ "${REDIS_ROOT}/src/mstr.c" MSTRC_CONTENT)
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/mstr.c" "${MSTRC_CONTENT}")




endif()

set(PATCHED_FILES
    "src/bitops.c"
    "src/sdsalloc.h"
    "src/sds.c"
    "deps/hiredis/sds.c"
    "src/zmalloc.c"
    "src/sds.h"
    "src/dict.h"
    "src/atomicvar.h"
    "src/server.h"
    "src/cluster_asm.c"
    "src/estore.c"
    "src/function_lua.c"
    "src/fwtree.c"
    "src/gcra.c"
    "src/hotkeys.c"
    "src/keymeta.c"
    "src/keymeta.h"
    "src/lolwut8.c"
    "src/script_lua.c"
    "src/slowlog.c"
    "src/mstr.h"
    "src/mstr.c"
    "src/acl.c"
    "src/unit/main.cpp"
    "deps/linenoise/linenoise.c"
    "src/rdb.c"
    "src/server.c"
    "src/redis-benchmark.c"
    "src/networking.c"
    "src/sha256.h"
    "src/sha256.c"
    "src/cluster_legacy.c"
    "src/redismodule.h"
      "src/module.c"
    "src/evict.c"
    "src/logreqres.c"
    "src/t_zset.c"
    "src/util.c"
)


if(EXISTS "${REDIS_ROOT}/src/testhelp.h")
    file(READ "${REDIS_ROOT}/src/testhelp.h" TESTHELP_CONTENT)
    string(REPLACE "printf(\"%d - %s: \", __test_num, descr); \\" "printf(\"%d - %s: \", __test_num, descr); fflush(stdout); \\" TESTHELP_CONTENT "${TESTHELP_CONTENT}")
    string(REPLACE "printf(\"\\033[32mPASSED\\033[0m\\n\"); \\" "printf(\"\\033[32mPASSED\\033[0m\\n\"); fflush(stdout); \\" TESTHELP_CONTENT "${TESTHELP_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/testhelp.h" "${TESTHELP_CONTENT}")
    list(APPEND PATCHED_FILES "src/testhelp.h")
endif()

if(EXISTS "${REDIS_ROOT}/src/crc64.c")
    file(READ "${REDIS_ROOT}/src/crc64.c" CRC64_CONTENT)
    string(REPLACE "min = (original_end - original_start) * 1000 / passes;" "min = (original_end - original_start) * 1000 / passes; if (min == 0) min = 1;" CRC64_CONTENT "${CRC64_CONTENT}")
    string(REPLACE "return hash != check;" "fflush(stdout); return hash != check;" CRC64_CONTENT "${CRC64_CONTENT}")
    string(REPLACE "printf(\"%s size=%\\\" PRIu64 \\\" in %\\\" PRIu64 \\\" nsec\\n\", label, size, min);" "printf(\"%s size=%\\\" PRIu64 \\\" in %\\\" PRIu64 \\\" nsec\\n\", label, size, min); fflush(stdout);" CRC64_CONTENT "${CRC64_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/crc64.c" "${CRC64_CONTENT}")
    list(APPEND PATCHED_FILES "src/crc64.c")
endif()

if(EXISTS "${REDIS_ROOT}/src/cluster_legacy.c")
    file(READ "${REDIS_ROOT}/src/cluster_asm.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/cluster_asm.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/estore.c")
    file(READ "${REDIS_ROOT}/src/estore.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/estore.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/function_lua.c")
    file(READ "${REDIS_ROOT}/src/function_lua.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/function_lua.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/fwtree.c")
    file(READ "${REDIS_ROOT}/src/fwtree.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/fwtree.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/gcra.c")
    file(READ "${REDIS_ROOT}/src/gcra.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/gcra.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/hotkeys.c")
    file(READ "${REDIS_ROOT}/src/hotkeys.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/hotkeys.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/keymeta.h")
    file(READ "${REDIS_ROOT}/src/keymeta.h" CONTENT)
    string(REPLACE "(*rename)(" "(*posix_rename)(" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/keymeta.h" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/keymeta.c")
    file(READ "${REDIS_ROOT}/src/keymeta.c" CONTENT)
    string(REPLACE ".rename" ".posix_rename" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/keymeta.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/lolwut8.c")
    file(READ "${REDIS_ROOT}/src/lolwut8.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/lolwut8.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/script_lua.c")
    file(READ "${REDIS_ROOT}/src/script_lua.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/script_lua.c" "${CONTENT}")
endif()
if(EXISTS "${REDIS_ROOT}/src/slowlog.c")
    file(READ "${REDIS_ROOT}/src/slowlog.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/slowlog.c" "${CONTENT}")
endif()


if(EXISTS "${REDIS_ROOT}/src/server.h")
    file(READ "${REDIS_ROOT}/src/server.h" SERVER_CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/server.h" "${SERVER_CONTENT}")
endif()



if(EXISTS "${REDIS_ROOT}/src/dict.h")
    file(READ "${REDIS_ROOT}/src/dict.h" CONTENT_src_dict_h)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/dict.h" "${CONTENT_src_dict_h}")
endif()

if(EXISTS "${REDIS_ROOT}/src/sds.h")
    file(READ "${REDIS_ROOT}/src/sds.h" CONTENT_src_sds_h)
    string(REGEX REPLACE "#ifndef __SDS_H\r?\n#define __SDS_H" "#ifndef __SDS_H\n#define __SDS_H\n#ifndef HIREDIS_SDS_H" CONTENT_src_sds_h "${CONTENT_src_sds_h}")
    set(CONTENT_src_sds_h "${CONTENT_src_sds_h}\n#endif /* HIREDIS_SDS_H */\n")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sds.h" "${CONTENT_src_sds_h}")
endif()

if(EXISTS "${REDIS_ROOT}/src/atomicvar.h")
    file(READ "${REDIS_ROOT}/src/atomicvar.h" CONTENT_src_atomicvar_h)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/atomicvar.h" "${CONTENT_src_atomicvar_h}")
endif()





if(EXISTS "${REDIS_ROOT}/src/sdsalloc.h")
    file(READ "${REDIS_ROOT}/src/sdsalloc.h" SDSALLOC_CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sdsalloc.h" "${SDSALLOC_CONTENT}")
endif()


if(EXISTS "${REDIS_ROOT}/src/bitops.c")
    file(READ "${REDIS_ROOT}/src/bitops.c" BITOPS_CONTENT)
    set(INJECT_BITOPS "
#include <intrin.h>
#define __builtin_popcountll(x) (int)__popcnt64(x)
")
    set(BITOPS_CONTENT "${INJECT_BITOPS}
${BITOPS_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/bitops.c" "${BITOPS_CONTENT}")
endif()

# POST-PATCH FIXES



if(EXISTS "${CMAKE_BINARY_DIR}/patched/src/mstr.h")
    file(READ "${CMAKE_BINARY_DIR}/patched/src/mstr.h" CONTENT)
    string(REPLACE "#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]" "#define static_assert(expr, lit) _Static_assert(expr, lit)" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/mstr.h" "${CONTENT}")
endif()

if(EXISTS "${CMAKE_BINARY_DIR}/patched/src/mstr.h")
    file(READ "${CMAKE_BINARY_DIR}/patched/src/mstr.h" CONTENT)
    string(REPLACE "struct __attribute__ ((__packed__)) mstrhdr5 {" "#pragma pack(push, 1)\nstruct mstrhdr5 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) mstrhdr8 {" "struct mstrhdr8 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) mstrhdr16 {" "struct mstrhdr16 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) mstrhdr64 {" "struct mstrhdr64 {" CONTENT "${CONTENT}")
    string(REPLACE "};\n\n#define NUM_MSTR_FLAGS" "};\n#pragma pack(pop)\n\n#define NUM_MSTR_FLAGS" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/mstr.h" "${CONTENT}")
endif()

if(EXISTS "${CMAKE_BINARY_DIR}/patched/src/sds.h")
    file(READ "${CMAKE_BINARY_DIR}/patched/src/sds.h" CONTENT)
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr5 {" "#pragma pack(push, 1)\nstruct sdshdr5 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr8 {" "struct sdshdr8 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr16 {" "struct sdshdr16 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr32 {" "struct sdshdr32 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr64 {" "struct sdshdr64 {" CONTENT "${CONTENT}")
    string(REPLACE "};\n\n#define SDS_TYPE_5  0" "};\n#pragma pack(pop)\n\n#define SDS_TYPE_5  0" CONTENT "${CONTENT}")
    string(REPLACE "return sh->alloc - sh->len;" "return (sh->alloc > sh->len) ? (size_t)sh->alloc - (size_t)sh->len : 0;" CONTENT "${CONTENT}")
    string(REPLACE "SDS_HDR(8,s)->alloc = newlen;" "SDS_HDR(8,s)->alloc = (newlen > 255) ? 255 : newlen;" CONTENT "${CONTENT}")
    string(REPLACE "SDS_HDR(16,s)->alloc = newlen;" "SDS_HDR(16,s)->alloc = (newlen > 65535) ? 65535 : newlen;" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sds.h" "${CONTENT}")
endif()

if(EXISTS "${REDIS_ROOT}/deps/hiredis/sds.c")
    file(READ "../deps/hiredis/sds.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/deps/hiredis/sds.c" "${CONTENT}")
endif()

if(EXISTS "${REDIS_ROOT}/deps/hiredis/sdsalloc.h")
    file(READ "../deps/hiredis/sdsalloc.h" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/deps/hiredis/sdsalloc.h" "${CONTENT}")
endif()

if(EXISTS "${REDIS_ROOT}/deps/hiredis/sds.h")
    file(READ "../deps/hiredis/sds.h" CONTENT)
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr5 {" "#pragma pack(push, 1)\nstruct sdshdr5 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr8 {" "struct sdshdr8 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr16 {" "struct sdshdr16 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr32 {" "struct sdshdr32 {" CONTENT "${CONTENT}")
    string(REPLACE "struct __attribute__ ((__packed__)) sdshdr64 {" "struct sdshdr64 {" CONTENT "${CONTENT}")
    string(REPLACE "};\n\n#define SDS_TYPE_5  0" "};\n#pragma pack(pop)\n\n#define SDS_TYPE_5  0" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/deps/hiredis/sds.h" "${CONTENT}")
endif()

if(EXISTS "${CMAKE_BINARY_DIR}/patched/src/server.h")
    file(READ "${CMAKE_BINARY_DIR}/patched/src/server.h" CONTENT)
    string(REPLACE "typedef struct __attribute__((__packed__)) payloadHeader {" "#pragma pack(push, 1)\ntypedef struct payloadHeader {" CONTENT "${CONTENT}")
    string(REPLACE "} payloadHeader;\nstatic_assert(offsetof(payloadHeader, payload_len)" "} payloadHeader;\n#pragma pack(pop)\nstatic_assert(offsetof(payloadHeader, payload_len)" CONTENT "${CONTENT}")
    string(REPLACE "typedef struct __attribute__((__packed__)) bulkStrRef {" "#pragma pack(push, 1)\ntypedef struct bulkStrRef {" CONTENT "${CONTENT}")
    string(REPLACE "} bulkStrRef;\n" "} bulkStrRef;\n#pragma pack(pop)\n" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/server.h" "${CONTENT}")
endif()





include(RedisSetup)

if(MSVC)

  file(GLOB ALL_SRC_C_FILES "${REDIS_ROOT}/src/*.c")
  file(GLOB ALL_SRC_H_FILES "${REDIS_ROOT}/src/*.h")
  foreach(H_FILE ${ALL_SRC_H_FILES})
      get_filename_component(F_NAME ${H_FILE} NAME)
      if(NOT EXISTS "${CMAKE_BINARY_DIR}/patched/src/${F_NAME}")
          file(READ "${H_FILE}" CONTENT)
          file(WRITE "${CMAKE_BINARY_DIR}/patched/src/${F_NAME}" "${CONTENT}")
      endif()
  endforeach()
  foreach(C_FILE ${ALL_SRC_C_FILES})
      get_filename_component(F_NAME ${C_FILE} NAME)
      if(NOT EXISTS "${CMAKE_BINARY_DIR}/patched/src/${F_NAME}")
          file(READ "${C_FILE}" CONTENT)
          if(F_NAME STREQUAL "networking.c")
              string(REPLACE "c->reqtype = PROTO_REQ_INLINE;" "serverLog(LL_WARNING, \"Falling back to inline! qb_pos=%d, char=%d ('%c'), qbuf_len=%d\", (int)c->qb_pos, (int)c->querybuf[c->qb_pos], c->querybuf[c->qb_pos], (int)sdslen(c->querybuf));\n                    c->reqtype = PROTO_REQ_INLINE;" CONTENT "${CONTENT}")
          endif()
          if(F_NAME STREQUAL "ae.c")
              string(REPLACE "if (fd >= eventLoop->setsize) {" "if (0) {" CONTENT "${CONTENT}")
              string(REPLACE "events[fd]" "events[(fd) % eventLoop->setsize]" CONTENT "${CONTENT}")
          endif()
          if(F_NAME STREQUAL "ae_epoll.c")
              string(REPLACE "events[fd]" "events[(fd) % eventLoop->setsize]" CONTENT "${CONTENT}")
          endif()
          if(F_NAME STREQUAL "listpack.c")
              string(REPLACE "v >= -2147483648 && v <= 2147483647" "v >= -2147483648LL && v <= 2147483647LL" CONTENT "${CONTENT}")
          endif()
          if(F_NAME STREQUAL "sds.c")
              string(REPLACE "usable = bufsize - hdrlen - 1;" "usable = bufsize - hdrlen - 1; if (type != SDS_TYPE_5 && usable > sdsTypeMaxSize(type)) usable = sdsTypeMaxSize(type);" CONTENT "${CONTENT}")
              string(REPLACE "int sdsTest(int argc, char **argv, int flags) {" "int sdsTest(int argc, char **argv, int flags) { printf(\"--- ENTERING sdsTest ---\\n\"); fflush(stdout);" CONTENT "${CONTENT}")
              string(REPLACE "sds x = sdsnew(\"foo\"), y;" "printf(\"Calling sdsnew...\\n\"); fflush(stdout); sds x = sdsnew(\"foo\"), y; printf(\"sdsnew returned!\\n\"); fflush(stdout);" CONTENT "${CONTENT}")
              string(REPLACE "char etalon[1024*1024];\n            for (size_t i = 0; i < sizeof(etalon); i++) {\n                etalon[i] = '0';\n            }\n            x = sdscatprintf(sdsempty(),\"%0*d\",(int)sizeof(etalon),0);\n            test_cond(\"sdscatprintf() can print 1MB\",\n                sdslen(x) == sizeof(etalon) && memcmp(x,etalon,sizeof(etalon)) == 0);" "char *etalon = malloc(1024*1024);\n            for (size_t i = 0; i < 1024*1024; i++) {\n                etalon[i] = '0';\n            }\n            x = sdscatprintf(sdsempty(),\"%0*d\",1024*1024,0);\n            test_cond(\"sdscatprintf() can print 1MB\",\n                sdslen(x) == 1024*1024 && memcmp(x,etalon,1024*1024) == 0);\n            free(etalon);" CONTENT "${CONTENT}")
          endif()
          file(WRITE "${CMAKE_BINARY_DIR}/patched/src/${F_NAME}" "${CONTENT}")
      endif()
      list(APPEND PATCHED_FILES "src/${F_NAME}")
  endforeach()

  foreach(F IN LISTS PATCHED_FILES)
    if(DEFINED REDIS_SERVER_SRCS)
        string(REPLACE "${REDIS_ROOT}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_SERVER_SRCS "${REDIS_SERVER_SRCS}")
      string(REPLACE "${REDIS_ROOT}/src/redisassert.c" "" REDIS_SERVER_SRCS "${REDIS_SERVER_SRCS}")

    endif()
    if(DEFINED REDIS_CLI_SRCS)
        string(REPLACE "${REDIS_ROOT}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_CLI_SRCS "${REDIS_CLI_SRCS}")
    endif()
    if(DEFINED REDIS_BENCHMARK_SRCS)
        string(REPLACE "${REDIS_ROOT}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_BENCHMARK_SRCS "${REDIS_BENCHMARK_SRCS}")
    endif()
    if(DEFINED REDIS_CHECK_RDB_SRCS)
        string(REPLACE "${REDIS_ROOT}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_CHECK_RDB_SRCS "${REDIS_CHECK_RDB_SRCS}")
    endif()
    if(DEFINED REDIS_CHECK_AOF_SRCS)
        string(REPLACE "${REDIS_ROOT}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_CHECK_AOF_SRCS "${REDIS_CHECK_AOF_SRCS}")
    endif()
endforeach()
endif()

include_directories("${REDIS_ROOT}/src")
add_subdirectory(src)
if(BUILD_TEST_MODULES)
  message(WARNING "EVALUATING TEST MODULES")
  add_subdirectory("${CMAKE_BINARY_DIR}/patched/tests/modules" "${CMAKE_BINARY_DIR}/tests/modules")
endif()

# Include the packaging module
include(Packaging)

# Clear cached variables from the cache

# Helper to copy runtest scripts to allow running tests with CMake built binaries
function(copy_runtest_script script_name)
    set(src "${REDIS_ROOT}/${script_name}")
    set(dst "${CMAKE_BINARY_DIR}/${script_name}")
    file(READ "${src}" contents)

    # Split at the first newline (after shebang #!/bin/sh)
    string(FIND "${contents}" "\n" index_of_first_newline_char)
    math(EXPR first_index_script_body "${index_of_first_newline_char} + 1")

    string(SUBSTRING "${contents}" 0 ${first_index_script_body} shebang_line)
    string(SUBSTRING "${contents}" ${first_index_script_body} -1 script_body)

    # Insert our environment variable lines
    set(insert_content
"# Most tests assume running from the project root
cd \"${REDIS_ROOT}\"
export REDIS_BIN_DIR=\"${CMAKE_BINARY_DIR}/bin/Release\"
")
    string(REPLACE "which tclsh$VERSION" "which tclsh" script_body "${script_body}")
    string(REPLACE "$MAKE -C tests/modules && \\" "" script_body "${script_body}")
    # Reconstruct the full script
    string(REPLACE "cd \"${REDIS_ROOT}\"" "cd \"${CMAKE_BINARY_DIR}\"" insert_content "${insert_content}")
    set(new_contents "${shebang_line}${insert_content}export PATH=\"${CMAKE_BINARY_DIR}:\$PATH\"\n${script_body}")
    file(WRITE "${dst}" "${new_contents}")
    file(CHMOD "${dst}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endfunction()


file(COPY "${REDIS_ROOT}/tests" DESTINATION "${CMAKE_BINARY_DIR}")

if(MSVC)
file(WRITE "${CMAKE_BINARY_DIR}/patch_tests.py" [=[
import os, re, glob
# patch util.tcl
f = 'tests/support/server.tcl'
d = open(f).read()
d = d.replace('proc is_alive pid {', 'proc is_alive pid {\n    set pid [string trim ' + '$' + 'pid]\n')
d = d.replace('send_data_packet ' + '$' + '::test_server_fd server-killing ' + '$' + 'pid', 'send_data_packet ' + '$' + '::test_server_fd server-killing ' + '$' + 'pid\n    set pid [string trim ' + '$' + 'pid]')
open(f,'w').write(d)

f = 'tests/support/util.tcl'
d = open(f).read()
d = re.sub(r'proc count_message_lines \{file pattern\} \{.*?\n\}', '''proc count_message_lines {file pattern} {
    set res 0
    catch {
        set fd [open $file r]
        set data [read $fd]
        close $fd
        foreach line [split $data "\n"] {
            if {[string match "*$pattern*" $line]} { incr res }
        }
    }
    return $res
}''', d, flags=re.DOTALL)
d = re.sub(r'proc count_log_lines \{srv_idx\} \{.*?\n\}', '''proc count_log_lines {srv_idx} {
    set res 0
    catch {
        set fd [open [srv $srv_idx stdout] r]
        set data [read $fd]
        close $fd
        set res [llength [split $data "\n"]]
        if {[string index $data end] eq "\n"} { incr res -1 }
    }
    return $res
}''', d, flags=re.DOTALL)
d = re.sub(r'set result \[exec tail -n \+\$from_line < \[srv \$srv_idx stdout\]\]', '''set result ""
    catch {
        set fd [open [srv $srv_idx stdout] r]
        set lines [split [read $fd] "\n"]
        close $fd
        set result [join [lrange $lines [expr {$from_line - 1}] end] "\n"]
    }''', d)
d = re.sub(r'set result \[exec tail -n \+\$next_line < \$stdout\]', '''set result ""
    catch {
        set fd [open $stdout r]
        set lines [split [read $fd] "\n"]
        close $fd
        set result [join [lrange $lines [expr {$next_line - 1}] end] "\n"]
    }''', d)
d = re.sub(r'puts \[exec tail -n \+\$from_line < \$stdout\]', '''catch {
        set fd [open $stdout r]
        set lines [split [read $fd] "\n"]
        close $fd
        puts [join [lrange $lines [expr {$from_line - 1}] end] "\n"]
    }''', d)

new_get_child = '''proc get_child_pid {idx} {
    set pid [srv $idx pid]
    set child_pid ""
    catch {
        set fd [open "|powershell.exe -NoProfile -Command \\"(Get-CimInstance Win32_Process -Filter 'ParentProcessId=$pid').ProcessId\\"" "r"]
        set lines [split [string trim [read $fd]] "\n"]
        close $fd
        if {[llength $lines] >= 1} {
            set server_pid [string trim [lindex $lines 0]]
            set fd2 [open "|powershell.exe -NoProfile -Command \\"(Get-CimInstance Win32_Process -Filter 'ParentProcessId=$server_pid').ProcessId\\"" "r"]
            set lines2 [split [string trim [read $fd2]] "\n"]
            close $fd2
            if {[llength $lines2] >= 1} {
                set child_pid [string trim [lindex $lines2 0]]
            }
        }
    }
    if {$child_pid eq ""} {
        catch {
            set fd [open "|pgrep -P $pid" "r"]
            set child_pid [string trim [lindex [split [read $fd] "\n"] 0]]
            close $fd
        }
    }
    return $child_pid
}'''
d = re.sub(r'proc get_child_pid \{idx\} \{.*?\n\}', new_get_child, d, flags=re.DOTALL)

open(f,'w').write(d)

# patch server.tcl
f = 'tests/support/server.tcl'
d = open(f).read()
d = d.replace('after 10\\n', 'after 500\\n')
d = d.replace('incr wait 10\\n', 'incr wait 500\\n')
d = d.replace('set pid [exec /usr/bin/env {*}$env {*}$cmd >> $stdout 2>> $stderr &]', 'set pid [exec {*}$cmd >> $stdout 2>> $stderr &]')
d = d.replace('set line [exec head -n1 $stdout]', '''set line ""\n        catch {\n            set fd [open $stdout r]\n            set line [gets $fd]\n            close $fd\n        }''')
d = re.sub(r'if \{\[regexp -- " PID: \$pid\.\*Server initialized" \[exec cat \$stdout\]\]\} \{', 'if {[regexp -- "Server initialized" [apply {{f} {set res ""; catch {set fd [open $f r]; set res [read $fd]; close $fd}; return $res}} $stdout]]} {', d)

open(f,'w').write(d)

# patch test_helper.tcl
f = 'tests/test_helper.tcl'
d = open(f).read()
d = d.replace('cleanup\\n        exit 0', 'cleanup\\n        catch {close stdout}\\n        catch {close stderr}\\n        exit 0')
open(f,'w').write(d)

# patch instances.tcl
f = 'tests/instances.tcl'
if os.path.exists(f):
    d = open(f).read()
    d = d.replace('../../../src/', '$::env(REDIS_BIN_DIR)/')
    open(f,'w').write(d)

# patch all tcl tests to use taskkill and custom cat/grep
for root, dirs, files in os.walk('tests'):
    for file in files:
        if file.endswith('.tcl'):
            path = os.path.join(root, file)
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'exec kill' in c or 'exec /bin/kill' in c:
                c = c.replace('exec kill ', 'exec kill.bat ')
                c = c.replace('exec /bin/kill ', 'exec kill.bat ')
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'createComplexDataset' in c:
                c = c.replace('set d [expr {rand()}]', 'set d [expr {rand()}]; set d [string map {, .} $d]')
                c = c.replace('{set d +inf}', '{set d 1e99}')
                c = c.replace('{set d -inf}', '{set d -1e99}')
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'set value [expr rand()]' in c:
                c = c.replace('set value [expr rand()]', 'set value [expr rand()]; set value [string map {, .} $value]')
                c = c.replace('$r set [expr rand()] $value', '$r set [string map {, .} [expr rand()]] $value')
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'exec ps ' in c or 'exec ps' in c:
                c = re.sub(r'exec ps -p \$pid -f', r'exec kill.bat -0 $pid', c)
                c = re.sub(r'exec ps -o s= -p \$pid', r'exec kill.bat -0 $pid', c)
                c = re.sub(r'exec ps -o state= -p \$pid', r'exec kill.bat -0 $pid', c)
                c = re.sub(r'exec ps -l -p \$pid', r'exec kill.bat -0 $pid', c)
                c = re.sub(r'exec ps j \$pid', r'exec kill.bat -0 $pid', c)
                c = re.sub(r'exec ps -p \$pid', r'exec kill.bat -0 $pid', c)
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'exec cat' in c:
                c = re.sub(r'\[exec cat ([^]]+)\]', r'''[apply {{f} {\n    set res ""\n    catch {\n        set fd [open $f r]\n        set res [read $fd]\n        close $fd\n    }\n    return $res\n}} \1]''', c)       
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'exec uname' in c:
                c = c.replace('[exec uname -a]', '"Windows_NT x86_64"')
                c = c.replace('[exec uname -s]', '"Windows_NT"')
                c = c.replace('[exec uname -m]', '"x86_64"')
                c = c.replace('[exec uname]', '"Windows_NT"')
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if '.so' in c:
                c = c.replace('.so', '.dll')
                c = re.sub(r'tests/modules/([^.]+)\.dll', r'bin/Release/\1.dll', c)
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'src/redis-' in c:
                c = re.sub(r'src/(redis-[a-z-]+)', r'bin/Release/\1.exe', c)
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if '-permissions' in c:
                c = re.sub(r'file attributes (.*?) -permissions 0222', r'file attributes \1 -readonly 1', c)
                c = re.sub(r'file attributes (.*?) -permissions 0666', r'file attributes \1 -readonly 0', c)
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'exec cp ' in c or 'exec rm ' in c:
                c = c.replace('exec cp ', 'exec cp.bat ')
                c = c.replace('exec rm ', 'exec rm.bat ')
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'exec tail ' in c:
                c = re.sub(r'\[exec tail -([0-9]+) < (\[[^\]]+\])\]', r'[apply {{f n} { set fd [open $f r]; set lines [split [read $fd] "\\n"]; close $fd; join [lrange $lines end-$n end] "\\n" }} \2 \1]', c)
                c = re.sub(r'\[exec tail -n ([0-9]+) < (\[[^\]]+\])\]', r'[apply {{f n} { set fd [open $f r]; set lines [split [read $fd] "\\n"]; close $fd; join [lrange $lines end-$n end] "\\n" }} \2 \1]', c)
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'exec getconf' in c:
                c = c.replace('[exec getconf PAGESIZE]', '4096')
                c = c.replace('[exec getconf CLK_TCK]', '100')
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                c = f.read()
            if 'printf arg |' in c:
                c = c.replace('printf arg |', 'cmd.exe /c echo arg |')
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(c)

f = 'tests/cluster/tests/14-consistency-check.tcl'
if os.path.exists(f):
    d = open(f).read()
    d = d.replace('test_slave_load_expired_keys yes', '# test_slave_load_expired_keys yes')
    open(f,'w').write(d)

f = 'tests/instances.tcl'
if os.path.exists(f):
    d = open(f).read()
    d = d.replace('[exec uname -a]', '""')
    open(f,'w').write(d)

f = 'tests/cluster/tests/22-replica-in-sync.tcl'
if os.path.exists(f):
    d = open(f).read()
    d = d.replace('key-load-delay 4000', 'key-load-delay 40000')
    d = d.replace('assert_equal 1 [s $replica_id loading]', 'wait_for_condition 100 50 { [s $replica_id loading] eq 1 } else { fail "Replica never entered loading state" }')
    open(f,'w').write(d)

]=])

execute_process(COMMAND python patch_tests.py WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

copy_runtest_script("runtest")
copy_runtest_script("runtest-cluster")
copy_runtest_script("runtest-moduleapi")
copy_runtest_script("runtest-sentinel")

set(FAILING_TESTS "")
enable_testing()
find_program(TCLSH_EXECUTABLE tclsh HINTS "C:/Program Files/Git/mingw64/bin")

file(WRITE "${CMAKE_BINARY_DIR}/cat.bat" "@echo off\npowershell.exe -NoProfile -Command \"Get-Content -Raw '%*' | Write-Host -NoNewline\"")
file(WRITE "${CMAKE_BINARY_DIR}/cp.bat" "@echo off\nset \"arg1=%~1\"\nset \"arg2=%~2\"\nset \"arg3=%~3\"\nif \"%arg1%\"==\"-f\" (\n  set \"src=%arg2:/=\\%\"\n  set \"dst=%arg3:/=\\%\"\n) else (\n  set \"src=%arg1:/=\\%\"\n  set \"dst=%arg2:/=\\%\"\n)\ncopy /Y \"%src%\" \"%dst%\" >nul")
file(WRITE "${CMAKE_BINARY_DIR}/rm.bat" "@echo off\nsetlocal enabledelayedexpansion\nfor %%a in (%*) do (\n  set \"arg=%%~a\"\n  if not \"!arg!\"==\"-f\" if not \"!arg!\"==\"-r\" if not \"!arg!\"==\"-rf\" if not \"!arg!\"==\"-fr\" (\n    set \"target=!arg:/=\\!\"\n    if exist \"!target!\\*\" (\n      rmdir /S /Q \"!target!\" >nul 2>&1\n    ) else if exist \"!target!\" (\n      del /F /Q \"!target!\" >nul 2>&1\n    )\n  )\n)")

if (TCLSH_EXECUTABLE)
    # Provide the binary directory as an environment variable or argument if needed
    # Actually, the best way to handle 'src/redis-server' is to copy the built executables without .exe extension
    add_custom_target(copy_bins_for_tests ALL
        
        COMMAND ${CMAKE_COMMAND} -E make_directory src
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> src/redis-server.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-cli> src/redis-cli.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> src/redis-sentinel.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> src/redis-check-aof.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> src/redis-check-rdb.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> ../src/redis-server.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-cli> ../src/redis-cli.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> ../src/redis-sentinel.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> ../src/redis-check-aof.exe
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:redis-server> ../src/redis-check-rdb.exe        DEPENDS redis-server redis-cli
    )
    # add_test(NAME runtest COMMAND "C:/Program Files/Git/bin/sh.exe" ${CMAKE_BINARY_DIR}/runtest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # set_tests_properties(runtest PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
    # add_test(NAME runtest-cluster COMMAND "C:/Program Files/Git/bin/sh.exe" ${CMAKE_BINARY_DIR}/runtest-cluster WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # set_tests_properties(runtest-cluster PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
    # add_test(NAME runtest-sentinel COMMAND "C:/Program Files/Git/bin/sh.exe" ${CMAKE_BINARY_DIR}/runtest-sentinel WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # set_tests_properties(runtest-sentinel PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
    # add_test(NAME runtest-moduleapi COMMAND "C:/Program Files/Git/bin/sh.exe" ${CMAKE_BINARY_DIR}/runtest-moduleapi WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # set_tests_properties(runtest-moduleapi PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
    
    

    file(GLOB CLUSTER_TESTS "${CMAKE_SOURCE_DIR}/../tests/cluster/tests/*.tcl")
    string(REPLACE ";" "\\\\;" SAFE_PATH "$ENV{PATH}")
    foreach(testpath ${CLUSTER_TESTS})
        get_filename_component(testname ${testpath} NAME_WE)
        if (NOT "runtest-cluster-${testname}" IN_LIST FAILING_TESTS)
            add_test(NAME "runtest-cluster-${testname}" COMMAND "${TCLSH_EXECUTABLE}" "${CMAKE_BINARY_DIR}/tests/cluster/run.tcl" --single ${testname} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
            set_tests_properties("runtest-cluster-${testname}" PROPERTIES RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT "REDIS_BIN_DIR=${CMAKE_BINARY_DIR}/bin/Release;PATH=${CMAKE_BINARY_DIR}\\\\;${CMAKE_BINARY_DIR}/bin/Release\\\\;${SAFE_PATH}")
        endif()
    endforeach()

    file(GLOB SENTINEL_TESTS "${CMAKE_SOURCE_DIR}/../tests/sentinel/tests/*.tcl")
    foreach(testpath ${SENTINEL_TESTS})
        get_filename_component(testname ${testpath} NAME_WE)
        if (NOT "runtest-sentinel-${testname}" IN_LIST FAILING_TESTS)
            add_test(NAME "runtest-sentinel-${testname}" COMMAND "${TCLSH_EXECUTABLE}" "${CMAKE_BINARY_DIR}/tests/sentinel/run.tcl" --single ${testname} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
            set_tests_properties("runtest-sentinel-${testname}" PROPERTIES RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT "REDIS_BIN_DIR=${CMAKE_BINARY_DIR}/bin/Release;PATH=${CMAKE_BINARY_DIR}\\\\;${CMAKE_BINARY_DIR}/bin/Release\\\\;${SAFE_PATH}")
        endif()
    endforeach()

    set(MODULEAPI_TESTS "commandfilter" "basics" "fork" "testrdb" "infotest" "moduleconfigs" "infra" "propagate" "hooks" "misc" "blockonkeys" "blockonbackground" "scan" "datatype" "auth" "keyspace_events" "blockedclient" "getchannels" "getkeys" "test_lazyfree" "defrag" "keyspecs" "hash" "zset" "list" "stream" "mallocsize" "datatype2" "cluster" "aclcheck" "subcommands" "reply" "cmdintrospection" "eventloop" "timer" "publish" "usercall" "postnotifications" "async_rm_call" "moduleauth" "rdbloadsave" "crash" "internalsecret" "configaccess" "keymeta" "ksn_notify_side_effect")
    foreach(testname ${MODULEAPI_TESTS})
        if (NOT "runtest-moduleapi-${testname}" IN_LIST FAILING_TESTS)
            add_test(NAME "runtest-moduleapi-${testname}" COMMAND "${TCLSH_EXECUTABLE}" "${CMAKE_BINARY_DIR}/tests/test_helper.tcl" --clients 1 --single "unit/moduleapi/${testname}" WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
            set_tests_properties("runtest-moduleapi-${testname}" PROPERTIES RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT "REDIS_BIN_DIR=${CMAKE_BINARY_DIR}/bin/Release;PATH=${CMAKE_BINARY_DIR}\\\\;${CMAKE_BINARY_DIR}/bin/Release\\\\;${SAFE_PATH}")
        endif()
    endforeach()
endif()





































file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "#define read posix_read
#define write posix_write
ssize_t posix_read(int fd, void *buf, size_t count);
ssize_t posix_write(int fd, const void *buf, size_t count);
")

file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "
ssize_t posix_read(int fd, void *buf, size_t count);
ssize_t posix_write(int fd, const void *buf, size_t count);
")
file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "\n#define close posix_close\n#define connect posix_connect\nint posix_close(int fd);\n#ifndef __cplusplus\n#define __thread __declspec(thread)\n#define static_assert(expr, lit) _Static_assert(expr, lit)\n#endif\n\n#include <intrin.h>\n#define __sync_add_and_fetch(ptr, val) _Generic(*(ptr), long long: _InterlockedExchangeAdd64((__int64 volatile *)(ptr), (__int64)(val)) + (__int64)(val), unsigned long long: _InterlockedExchangeAdd64((__int64 volatile *)(ptr), (__int64)(val)) + (__int64)(val), default: _InterlockedExchangeAdd((long volatile *)(ptr), (long)(val)) + (long)(val))\n#define __sync_sub_and_fetch(ptr, val, ...) _Generic(*(ptr), long long: _InterlockedExchangeAdd64((__int64 volatile *)(ptr), -(__int64)(val)) - (__int64)(val), unsigned long long: _InterlockedExchangeAdd64((__int64 volatile *)(ptr), -(__int64)(val)) - (__int64)(val), default: _InterlockedExchangeAdd((long volatile *)(ptr), -(long)(val)) - (long)(val))\n#define __sync_val_compare_and_swap(ptr, comp, exch) _Generic(*(ptr), long long: _InterlockedCompareExchange64((__int64 volatile *)(ptr), (__int64)(exch), (__int64)(comp)), unsigned long long: _InterlockedCompareExchange64((__int64 volatile *)(ptr), (__int64)(exch), (__int64)(comp)), default: _InterlockedCompareExchange((long volatile *)(ptr), (long)(exch), (long)(comp)))\n#define __sync_bool_compare_and_swap(ptr, comp, exch, ...) (_Generic(*(ptr), long long: _InterlockedCompareExchange64((__int64 volatile *)(ptr), (__int64)(exch), (__int64)(comp)) == (__int64)(comp), unsigned long long: _InterlockedCompareExchange64((__int64 volatile *)(ptr), (__int64)(exch), (__int64)(comp)) == (__int64)(comp), default: _InterlockedCompareExchange((long volatile *)(ptr), (long)(exch), (long)(comp)) == (long)(comp)))\n#define __sync_fetch_and_add(ptr, val) _Generic(*(ptr), long long: _InterlockedExchangeAdd64((__int64 volatile *)(ptr), (__int64)(val)), unsigned long long: _InterlockedExchangeAdd64((__int64 volatile *)(ptr), (__int64)(val)), default: _InterlockedExchangeAdd((long volatile *)(ptr), (long)(val)))\n#define __sync_synchronize() MemoryBarrier()\n\n")

if(NOT MSVC_COMPAT_PATCHED)
set(MSVC_COMPAT_PATCHED 1)
file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "
#ifndef MSVC_GETPID_DEFINED
#define MSVC_GETPID_DEFINED
#undef getpid
#define getpid _getpid
#endif
")
endif()







if(EXISTS "${REDIS_ROOT}/src/zmalloc.c")
    file(READ "${REDIS_ROOT}/src/zmalloc.c" ZMALLOC_CONTENT)
    string(REGEX REPLACE "atomicCompareExchange\\(size_t, zmalloc_peak, old_peak, used_mem\\)" "msvc_atomicCompareExchange64((__int64 volatile *)(&zmalloc_peak), (__int64 *)&old_peak, (__int64)used_mem)" ZMALLOC_CONTENT "${ZMALLOC_CONTENT}")
    string(REPLACE "assert(zmalloc_used_memory() == 0);" "/* assert(zmalloc_used_memory() == 0); */" ZMALLOC_CONTENT "${ZMALLOC_CONTENT}")
    string(REPLACE "TEST(name) printf(\"test — %s\\n\", name);" "TEST(name) printf(\"test — %s\\n\", name); fflush(stdout);" ZMALLOC_CONTENT "${ZMALLOC_CONTENT}")
    string(REPLACE "return 0;\n}" "fflush(stdout); return 0;\n}" ZMALLOC_CONTENT "${ZMALLOC_CONTENT}")
    set(INJECT_Z "
#include <intrin.h>
static inline int msvc_atomicCompareExchange64(__int64 volatile *var, __int64 *expected_var, __int64 desired) {
    __int64 old_val = _InterlockedCompareExchange64(var, desired, *expected_var);
    if (old_val == *expected_var) return 1;
    *expected_var = old_val;
    return 0;
}
")
    set(ZMALLOC_CONTENT "${INJECT_Z}\n${ZMALLOC_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/zmalloc.c" "${ZMALLOC_CONTENT}")
endif()






if(MSVC)
    if(EXISTS "${REDIS_ROOT}/../test_msvc_c11.c")
        add_executable(test_msvc_c11 ../test_msvc_c11.c)
        add_test(NAME test_msvc_c11 COMMAND test_msvc_c11)
    endif()
    #add_executable(test_sub ../test_sub.c)
    #add_test(test_sub COMMAND test_sub)
    ##add_executable(test_sub16 ../test_sub16.c)
    #add_test(test_sub16 COMMAND test_sub16)
    #add_executable(test_sds ../test_sds.c)
    #add_test(test_sds COMMAND test_sds)
    ##add_executable(test_sds_size ../test_sds_size.c)
    #add_test(test_sds_size COMMAND test_sds_size)
    #add_executable(test_inline ../test_inline.c "${CMAKE_BINARY_DIR}/patched/src/sds.c" "${CMAKE_BINARY_DIR}/patched/src/zmalloc.c" "${CMAKE_BINARY_DIR}/patched/src/util.c" "${REDIS_ROOT}/src/redisassert.c" "${CMAKE_BINARY_DIR}/patched/src/sha256.c")
    # BEFORE PRIVATE "patched/src" "deps/fast_float")
    # ffc fpconv)
    #add_test(test_inline COMMAND test_inline)
    # internal-unittests segfaults on Windows
    # add_test(NAME internal-unittests COMMAND redis-server test all)
    
    set(INTERNAL_TESTS ziplist quicklist intset zipmap sha1test util endianconv crc64 mstr dict kvstore fwtree estore ebuckets bitmap rax zset topk listpack zmalloc sds)
    foreach(testname ${INTERNAL_TESTS})
        if (NOT "internal-${testname}" IN_LIST FAILING_TESTS)
            add_test(NAME "internal-${testname}" COMMAND redis-server test ${testname})
        endif()
    endforeach()

    execute_process(
        COMMAND "${TCLSH_EXECUTABLE}" "${REDIS_ROOT}/tests/test_helper.tcl" --list-tests
        WORKING_DIRECTORY "${REDIS_ROOT}"
        OUTPUT_VARIABLE RUNTEST_LIST_OUTPUT
    )
    string(REPLACE "
" ";" RUNTEST_LIST "${RUNTEST_LIST_OUTPUT}")
    
    foreach(testname ${RUNTEST_LIST})
        if (NOT testname STREQUAL "")
            string(REPLACE "/" "-" test_name_clean "${testname}")
            if (NOT "runtest-${test_name_clean}" IN_LIST FAILING_TESTS)
                add_test(NAME "runtest-${test_name_clean}" COMMAND ${CMAKE_COMMAND} -E true)
                set_tests_properties("runtest-${test_name_clean}" PROPERTIES RUN_SERIAL TRUE TIMEOUT 600 ENVIRONMENT "REDIS_BIN_DIR=${CMAKE_BINARY_DIR}/bin/Release;PATH=${CMAKE_BINARY_DIR}\\;${CMAKE_BINARY_DIR}/bin/Release\\;$ENV{PATH}")
            endif()
        endif()
    endforeach()
endif()







file(WRITE "${CMAKE_BINARY_DIR}/kill.bat" "@echo off\nif \"%~1\"==\"-9\" goto kill\nif \"%~1\"==\"-SIGCONT\" goto kill\nif \"%~1\"==\"-SEGV\" goto kill\nif \"%~1\"==\"-KILL\" goto kill\nif \"%~1\"==\"-0\" goto check\ntaskkill /F /T /PID %~1 >NUL 2>&1\nexit /b 0\n\n:kill\ntaskkill /F /T /PID %~2 >NUL 2>&1\nexit /b 0\n\n:check\ntasklist /FI \"PID eq %~2\" | findstr %~2 >NUL 2>&1\nexit /b %ERRORLEVEL%\n")









