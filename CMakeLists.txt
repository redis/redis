#this file originates from https://raw.githubusercontent.com/liuzhengyang/redis-annotated/annotate/CMakeLists.txt
cmake_minimum_required(VERSION 3.2 FATAL_ERROR)

project(redis VERSION 4.0.2)

message(STATUS "Host is: ${CMAKE_HOST_SYSTEM}.  Build target is: ${CMAKE_SYSTEM}")
get_filename_component(REDIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
message(STATUS "Project root directory is: ${REDIS_ROOT}")

#for now, this is 'redis-server' target in the makefile.
#make a cmakelists.txt for that target.

#as default build process shows, this depends on some resources.
#quote these from .make-settings in src directory:
#STD=-std=c99 -pedantic -DREDIS_STATIC=
#WARN=-Wall -W -Wno-missing-field-initializers
#OPT=-O2
#MALLOC=jemalloc
#CFLAGS=
#LDFLAGS=
#REDIS_CFLAGS=
#REDIS_LDFLAGS=
#PREV_FINAL_CFLAGS=-std=c99 -pedantic -DREDIS_STATIC= -Wall -W -Wno-missing-field-initializers -O2 -g -ggdb -I../deps/hiredis -I../deps/linenoise -I../deps/lua/src -DUSE_JEMALLOC -I../deps/jemalloc/include
#PREV_FINAL_LDFLAGS= -g -ggdb -rdynamic

#redis server obj:
#adlist.o quicklist.o ae.o anet.o dict.o server.o sds.o zmalloc.o lzf_c.o lzf_d.o
#pqsort.o zipmap.o sha1.o ziplist.o release.o networking.o util.o object.o db.o
#replication.o rdb.o t_string.o t_list.o t_set.o t_zset.o t_hash.o config.o aof.o
#pubsub.o multi.o debug.o sort.o intset.o syncio.o cluster.o crc16.o endianconv.o
#slowlog.o scripting.o bio.o rio.o rand.o memtest.o crc64.o bitops.o sentinel.o
#notify.o setproctitle.o blocked.o hyperloglog.o latency.o sparkline.o
#redis-check-rdb.o redis-check-aof.o geo.o lazyfree.o module.o evict.o expire.o
#geohash.o geohash_helper.o childinfo.o defrag.o siphash.o rax.o
#these objs have corresponding dependencies that could be looked up in Makefile.dep
#thus I got these include directories and source files.


include_directories(deps/jemalloc/include/jemalloc deps/lua/src deps/hiredis)
include_directories(src)
add_executable(redis-server
        src/adlist.c
        src/quicklist.c
        src/ae.c
        src/anet.c
        src/dict.c
        src/server.c
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
        src/redis-check-rdb.c
        src/redis-check-aof.c
        src/geo.c
        src/lazyfree.c
        src/module.c
        src/evict.c
        src/expire.c
        src/geohash.c
        src/geohash_helper.c
        src/childinfo.c
        src/defrag.c
        src/siphash.c
        src/rax.c
        )

#take care of compile flags
##STD=-std=c99 -pedantic -DREDIS_STATIC=
#WARN=-Wall -W -Wno-missing-field-initializers
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -pedantic -DREDIS_STATIC= -Wall -W -Wno-missing-field-initializers")
#take care of LDFLAGS= -g -ggdb -rdynamic
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -g -ggdb -rdynamic")

# redis-server
#$(REDIS_SERVER_NAME): $(REDIS_SERVER_OBJ)
#$(REDIS_LD) -o $@ $^ ../deps/hiredis/libhiredis.a ../deps/lua/src/liblua.a $(FINAL_LIBS) that is deps/jemalloc/lib/libjemalloc.a -ldl -pthread


set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(redis-server PRIVATE Threads::Threads)
target_link_libraries(redis-server PRIVATE dl)

target_link_libraries(redis-server
        PRIVATE m
        PRIVATE ${REDIS_ROOT}/deps/lua/src/liblua.a
        PRIVATE ${REDIS_ROOT}/deps/linenoise/linenoise.o
        PRIVATE ${REDIS_ROOT}/deps/hiredis/libhiredis.a
        )