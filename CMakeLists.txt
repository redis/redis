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
option(BUILD_TEST_MODULES "Build all test modules" OFF)
option(BUILD_EXAMPLE_MODULES "Build example modules" OFF)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
project("redis")

include(FetchContent)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../auto-win-msvc")
    FetchContent_Declare(
        auto_win_msvc
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../auto-win-msvc"
    )
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
#ifndef _WINSIZE_DEFINED
#define _WINSIZE_DEFINED
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#define TIOCGWINSZ 0x5413
#endif
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
#define __builtin_clzl __builtin_clzll
static inline int __builtin_ctz(unsigned int x) { unsigned long r; if (_BitScanForward(&r, x)) return r; return 32; }
static inline int __builtin_ctzll(unsigned long long x) { unsigned long r; if (_BitScanForward64(&r, x)) return r; return 64; }
#define __builtin_ctzl __builtin_ctzll
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
static inline int mkstemp(char *tmpl) {
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0) return -1;
    return _open(tmpl, _O_RDWR | _O_CREAT | _O_EXCL, _S_IREAD | _S_IWRITE);
}
#define ftello _ftelli64
static inline char *ctime_r(const time_t *timep, char *buf) {
    ctime_s(buf, 26, timep);
    return buf;
}
static inline int unsetenv(const char *name) {
    return _putenv_s(name, \"\");
}
#undef pthread_self
")
    
    # Extract include directories from auto-win-msvc
    get_target_property(MEGA_INCLUDES auto-win-msvc INTERFACE_INCLUDE_DIRECTORIES)
    get_filename_component(AUTO_WIN_MSVC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../auto-win-msvc" ABSOLUTE)
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
if(EXISTS "${CMAKE_SOURCE_DIR}/src/server.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/server.c" SERVER_C_CONTENT)
    string(REPLACE "void *samples[num_samples];" "void **samples = _alloca(sizeof(void*) * num_samples);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "bucket *buckets[size];" "bucket **buckets = _alloca(sizeof(bucket*) * size);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "robj objects[argc];" "robj *objects = _alloca(sizeof(robj) * argc);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    string(REPLACE "robj *argv[argc];" "robj **argv = _alloca(sizeof(robj*) * argc);" SERVER_C_CONTENT "${SERVER_C_CONTENT}")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/server.c" "${SERVER_C_CONTENT}")
endif()

file(READ src/redis-benchmark.c CONTENT)
string(REPLACE "char data[config.datasize];" "char *data = _alloca(config.datasize);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/redis-benchmark.c" "${CONTENT}")

file(READ src/evict.c CONTENT)
string(REPLACE "dictEntry *samples[server.maxmemory_samples];" "dictEntry **samples = _alloca(sizeof(dictEntry*) * server.maxmemory_samples);" CONTENT "${CONTENT}")
string(REPLACE "void *samples[server.maxmemory_samples];" "void **samples = _alloca(sizeof(void*) * server.maxmemory_samples);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/evict.c" "${CONTENT}")

file(READ src/networking.c CONTENT)
string(REPLACE "struct iovec iov_arr[IOV_MAX];" "struct iovec *iov_arr = _alloca(sizeof(struct iovec) * IOV_MAX);" CONTENT "${CONTENT}")
string(REPLACE "struct iovec iov_arr[iovmax];" "struct iovec *iov_arr = _alloca(sizeof(struct iovec) * iovmax);" CONTENT "${CONTENT}")
string(REPLACE "struct iovec iov[iovmax];" "struct iovec *iov = _alloca(sizeof(struct iovec) * iovmax);" CONTENT "${CONTENT}")
string(REPLACE "char prefixes[iovmax / NUM_OF_IOV_PER_BULK_STR + 1][BULK_STR_LEN_PREFIX_MAX_SIZE];" "char (*prefixes)[BULK_STR_LEN_PREFIX_MAX_SIZE] = _alloca((iovmax / NUM_OF_IOV_PER_BULK_STR + 1) * BULK_STR_LEN_PREFIX_MAX_SIZE);" CONTENT "${CONTENT}")
string(REPLACE "bufWriteMetadata buf_metadata[replyLen];" "bufWriteMetadata *buf_metadata = _alloca(sizeof(bufWriteMetadata) * replyLen);" CONTENT "${CONTENT}")
string(REPLACE "hashtableIncrementalFindState key_incr_states[max_keys];" "hashtableIncrementalFindState *key_incr_states = _alloca(sizeof(hashtableIncrementalFindState) * max_keys);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/networking.c" "${CONTENT}")

file(READ deps/hiredis/net.c CONTENT)
string(REPLACE ".connect =" ".posix_connect =" CONTENT "${CONTENT}")
string(REPLACE ".close =" ".posix_close =" CONTENT "${CONTENT}")
string(REPLACE ".read =" ".posix_read =" CONTENT "${CONTENT}")
string(REPLACE ".write =" ".posix_write =" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/deps/hiredis/net.c" "${CONTENT}")

file(READ src/sha256.h CONTENT)
string(REPLACE "WORD" "REDIS_WORD" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sha256.h" "${CONTENT}")

file(READ src/sha256.c CONTENT)
string(REPLACE "WORD" "REDIS_WORD" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sha256.c" "${CONTENT}")


file(READ src/cluster_legacy.c CONTENT)
string(REPLACE "char buf[length + 1];" "char *buf = _alloca(length + 1);" CONTENT "${CONTENT}")
string(REPLACE "char buf[128];" "char *buf = _alloca(128);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/cluster_legacy.c" "${CONTENT}")

file(READ src/redismodule.h CONTENT)
string(REPLACE "(unsigned long)getapifuncptr" "(size_t)getapifuncptr" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/redismodule.h" "${CONTENT}")

file(READ src/module.c CONTENT)
string(REPLACE "char hyphenfmt[len];" "char *hyphenfmt = _alloca(len);" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)VM_" "(size_t)VM_" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)&VM_" "(size_t)&VM_" CONTENT "${CONTENT}")
string(REPLACE "(unsigned long)dlsym" "(size_t)dlsym" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/module.c" "${CONTENT}")



file(READ src/logreqres.c CONTENT)
string(REPLACE "\ninline " "\n" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/logreqres.c" "${CONTENT}")

file(READ src/t_zset.c CONTENT)
string(REPLACE "_sptr" "_sp" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/t_zset.c" "${CONTENT}")

file(READ src/util.c CONTENT)
string(REPLACE "char full_path[PATH_MAX];" "char *full_path = _alloca(4096);" CONTENT "${CONTENT}")
string(REPLACE "char temp_filename[PATH_MAX];" "char *temp_filename = _alloca(4096);" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/util.c" "${CONTENT}")


include_directories(BEFORE "${CMAKE_BINARY_DIR}/patched/src")
include_directories(BEFORE "${CMAKE_BINARY_DIR}/patched/deps/hiredis/src")


file(READ deps/linenoise/linenoise.c CONTENT)
string(REPLACE "const int seqBufferMaxLength = 8;" "#define seqBufferMaxLength 8" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/deps/linenoise/linenoise.c" "${CONTENT}")


file(COPY src/acl.c DESTINATION "${CMAKE_BINARY_DIR}/patched/src")



file(READ src/mstr.h CONTENT)
string(REGEX REPLACE "#define static_assert\(expr, lit\)[^
]*\n" "#define static_assert(expr, lit) _Static_assert(expr, lit)\n" CONTENT "${CONTENT}")
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/mstr.h" "${CONTENT}")

file(READ "${CMAKE_SOURCE_DIR}/src/mstr.c" MSTRC_CONTENT)
file(WRITE "${CMAKE_BINARY_DIR}/patched/src/mstr.c" "${MSTRC_CONTENT}")




endif()

set(PATCHED_FILES
    "src/bitops.c"
    "src/sdsalloc.h"
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
    "src/lolwut8.c"
    "src/script_lua.c"
    "src/slowlog.c"
    "src/mstr.h"
    "src/mstr.c"
    "src/evict.c"
    "src/acl.c"
    "src/unit/main.cpp"
    "deps/linenoise/linenoise.c"
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


if(EXISTS "${CMAKE_SOURCE_DIR}/src/cluster_asm.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/cluster_asm.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/cluster_asm.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/estore.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/estore.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/estore.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/function_lua.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/function_lua.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/function_lua.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/fwtree.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/fwtree.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/fwtree.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/gcra.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/gcra.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/gcra.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/hotkeys.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/hotkeys.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/hotkeys.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/keymeta.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/keymeta.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/keymeta.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/lolwut8.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/lolwut8.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/lolwut8.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/script_lua.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/script_lua.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/script_lua.c" "${CONTENT}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/slowlog.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/slowlog.c" CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/slowlog.c" "${CONTENT}")
endif()


if(EXISTS "${CMAKE_SOURCE_DIR}/src/server.h")
    file(READ "${CMAKE_SOURCE_DIR}/src/server.h" SERVER_CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/server.h" "${SERVER_CONTENT}")
endif()


if(EXISTS "${CMAKE_SOURCE_DIR}/src/atomicvar.h")
    file(READ "${CMAKE_SOURCE_DIR}/src/atomicvar.h" CONTENT_src_atomicvar_h)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/atomicvar.h" "${CONTENT_src_atomicvar_h}")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/src/dict.h")
    file(READ "${CMAKE_SOURCE_DIR}/src/dict.h" CONTENT_src_dict_h)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/dict.h" "${CONTENT_src_dict_h}")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/src/sds.h")
    file(READ "${CMAKE_SOURCE_DIR}/src/sds.h" CONTENT_src_sds_h)
    string(REPLACE "#ifndef __SDS_H\n#define __SDS_H" "#ifndef __SDS_H\n#define __SDS_H\n#ifndef HIREDIS_SDS_H" CONTENT_src_sds_h "${CONTENT_src_sds_h}")
    set(CONTENT_src_sds_h "${CONTENT_src_sds_h}\n#endif /* HIREDIS_SDS_H */\n")
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sds.h" "${CONTENT_src_sds_h}")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/src/atomicvar.h")
    file(READ "${CMAKE_SOURCE_DIR}/src/atomicvar.h" CONTENT_src_atomicvar_h)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/atomicvar.h" "${CONTENT_src_atomicvar_h}")
endif()





if(EXISTS "${CMAKE_SOURCE_DIR}/src/sdsalloc.h")
    file(READ "${CMAKE_SOURCE_DIR}/src/sdsalloc.h" SDSALLOC_CONTENT)
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sdsalloc.h" "${SDSALLOC_CONTENT}")
endif()


if(EXISTS "${CMAKE_SOURCE_DIR}/src/bitops.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/bitops.c" BITOPS_CONTENT)
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
    file(WRITE "${CMAKE_BINARY_DIR}/patched/src/sds.h" "${CONTENT}")
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
foreach(F IN LISTS PATCHED_FILES)
    if(DEFINED REDIS_SERVER_SRCS)
        string(REPLACE "${CMAKE_SOURCE_DIR}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_SERVER_SRCS "${REDIS_SERVER_SRCS}")
      string(REPLACE "${CMAKE_SOURCE_DIR}/src/redisassert.c" "" REDIS_SERVER_SRCS "${REDIS_SERVER_SRCS}")

    endif()
    if(DEFINED REDIS_CLI_SRCS)
        string(REPLACE "${CMAKE_SOURCE_DIR}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_CLI_SRCS "${REDIS_CLI_SRCS}")
    endif()
    if(DEFINED REDIS_BENCHMARK_SRCS)
        string(REPLACE "${CMAKE_SOURCE_DIR}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_BENCHMARK_SRCS "${REDIS_BENCHMARK_SRCS}")
    endif()
    if(DEFINED REDIS_CHECK_RDB_SRCS)
        string(REPLACE "${CMAKE_SOURCE_DIR}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_CHECK_RDB_SRCS "${REDIS_CHECK_RDB_SRCS}")
    endif()
    if(DEFINED REDIS_CHECK_AOF_SRCS)
        string(REPLACE "${CMAKE_SOURCE_DIR}/${F}" "${CMAKE_BINARY_DIR}/patched/${F}" REDIS_CHECK_AOF_SRCS "${REDIS_CHECK_AOF_SRCS}")
    endif()
endforeach()
endif()

include_directories("${CMAKE_SOURCE_DIR}/src")
add_subdirectory(src)
add_subdirectory(tests)

# Include the packaging module
include(Packaging)

# Clear cached variables from the cache

# Helper to copy runtest scripts to allow running tests with CMake built binaries
function(copy_runtest_script script_name)
    set(src "${CMAKE_SOURCE_DIR}/${script_name}")
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
cd ${CMAKE_SOURCE_DIR}
export REDIS_BIN_DIR=\"${CMAKE_BINARY_DIR}/bin\"
")
    # Reconstruct the full script
    set(new_contents "${shebang_line}${insert_content}${script_body}")
    file(WRITE "${dst}" "${new_contents}")
    file(CHMOD "${dst}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endfunction()

copy_runtest_script("runtest")
copy_runtest_script("runtest-cluster")
copy_runtest_script("runtest-moduleapi")

copy_runtest_script("runtest-sentinel")





































file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "#define read posix_read
#define write posix_write
int posix_read(int fd, void *buf, unsigned int count);
int posix_write(int fd, const void *buf, unsigned int count);
")

file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "#define _read posix_read
#define _write posix_write
int posix_read(int fd, void *buf, unsigned int count);
int posix_write(int fd, const void *buf, unsigned int count);
")
file(APPEND "${CMAKE_BINARY_DIR}/msvc_compat.h" "\n#define close posix_close\n#define _close posix_close\n#define connect posix_connect\nint posix_close(int fd);\n#ifndef __cplusplus\n#define __thread __declspec(thread)\n#define static_assert(expr, lit) _Static_assert(expr, lit)\n#endif\n\n#include <intrin.h>\n#define __sync_add_and_fetch(ptr, val) (sizeof(*(ptr)) == 8 ? _InterlockedExchangeAdd64((__int64 volatile *)(ptr), (__int64)(val)) + (__int64)(val) : _InterlockedExchangeAdd((long volatile *)(ptr), (long)(val)) + (long)(val))\n#define __sync_sub_and_fetch(ptr, val, ...) (sizeof(*(ptr)) == 8 ? _InterlockedExchangeAdd64((__int64 volatile *)(ptr), -(__int64)(val)) - (__int64)(val) : _InterlockedExchangeAdd((long volatile *)(ptr), -(long)(val)) - (long)(val))\n#define __sync_val_compare_and_swap(ptr, comp, exch) (sizeof(*(ptr)) == 8 ? _InterlockedCompareExchange64((__int64 volatile *)(ptr), (__int64)(exch), (__int64)(comp)) : _InterlockedCompareExchange((long volatile *)(ptr), (long)(exch), (long)(comp)))\n#define __sync_bool_compare_and_swap(ptr, comp, exch, ...) (sizeof(*(ptr)) == 8 ? _InterlockedCompareExchange64((__int64 volatile *)(ptr), (__int64)(exch), (__int64)(comp)) == (__int64)(comp) : _InterlockedCompareExchange((long volatile *)(ptr), (long)(exch), (long)(comp)) == (long)(comp))\n#define __sync_fetch_and_add(ptr, val) (sizeof(*(ptr)) == 8 ? _InterlockedExchangeAdd64((__int64 volatile *)(ptr), (__int64)(val)) : _InterlockedExchangeAdd((long volatile *)(ptr), (long)(val)))\n#define __sync_synchronize() _ReadWriteBarrier()\n\n")






if(EXISTS "${CMAKE_SOURCE_DIR}/src/zmalloc.c")
    file(READ "${CMAKE_SOURCE_DIR}/src/zmalloc.c" ZMALLOC_CONTENT)
    string(REGEX REPLACE "atomicCompareExchange\\(size_t, zmalloc_peak, old_peak, used_mem\\)" "msvc_atomicCompareExchange64((__int64 volatile *)(&zmalloc_peak), (__int64 *)&old_peak, (__int64)used_mem)" ZMALLOC_CONTENT "${ZMALLOC_CONTENT}")

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
