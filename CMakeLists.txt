cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(redis VERSION 4.0)
set(CMAKE_BUILD_TYPE "Debug")

get_filename_component(REDIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

add_subdirectory(deps)
add_subdirectory(src/modules)

set(SRC_SERVER_TMP
        src/adlist.c
        src/ae.c
        src/anet.c
        src/ae_epoll.c
        src/acl.c
        src/dict.c
        src/sds.c
        src/zmalloc.c
        src/lzf_c.c
        src/lzf_d.c
        src/pqsort.c
        src/zipmap.c
        src/sha1.c
        src/ziplist.c
        src/release.c
        src/networking.c
        src/util.c
        src/object.c
        src/db.c
        src/replication.c
        src/rdb.c
        src/t_string.c
        src/t_list.c
        src/t_set.c
        src/t_zset.c
        src/evict.c
        src/defrag.c
        src/module.c
        src/quicklist.c
        src/expire.c
        src/childinfo.c
        src/redis-check-aof.c
        src/redis-check-rdb.c
        src/lazyfree.c
        src/geohash.c
        src/rax.c
        src/geohash_helper.c
        src/siphash.c
        src/geo.c
        src/t_hash.c
        src/config.c
        src/aof.c
        src/pubsub.c
        src/multi.c
        src/debug.c
        src/sort.c
        src/intset.c
        src/syncio.c
        src/cluster.c
        src/crc16.c
        src/endianconv.c
        src/slowlog.c
        src/scripting.c
        src/bio.c
        src/rio.c
        src/rand.c
        src/memtest.c
        src/crc64.c
        src/bitops.c
        src/sentinel.c
        src/notify.c
        src/setproctitle.c
        src/blocked.c
        src/hyperloglog.c
        src/latency.c
        src/sparkline.c
        src/t_stream.c
        src/lolwut.c
        src/lolwut5.c
        src/listpack.c
        src/localtime.c
        src/tracking.c
        src/gopher.c
        )
FILE(GLOB MyCSources src/*.c)
set(SRC_SERVER src/server.c ${SRC_SERVER_TMP})

set(SRC_CLI
        src/anet.c
        src/sds.c
        src/adlist.c
        src/redis-cli.c
        src/zmalloc.c
        src/release.c
        src/anet.c
        src/ae.c
        src/crc64.c
        )


set(EXECUTABLE_OUTPUT_PATH src)
link_directories(deps/linenoise/ deps/lua/src deps/hiredis)

add_executable(redis-server ${SRC_SERVER})
target_include_directories(redis-server
        PRIVATE ${REDIS_ROOT}/deps/linenoise
        PRIVATE ${REDIS_ROOT}/deps/hiredis
        PRIVATE ${REDIS_ROOT}/deps/lua/src)
target_link_libraries(redis-server
        PRIVATE pthread
        PRIVATE dl
        PRIVATE m
        PRIVATE lua
        PRIVATE linenoise
        PRIVATE hiredis)


add_executable(redis-cli ${SRC_CLI})
target_include_directories(redis-cli
        PRIVATE ${REDIS_ROOT}/deps/linenoise
        PRIVATE ${REDIS_ROOT}/deps/hiredis
        PRIVATE ${REDIS_ROOT}/deps/lua/src)

target_link_libraries(redis-cli
        PRIVATE pthread
        PRIVATE m
        PRIVATE linenoise
        PRIVATE hiredis
        )
