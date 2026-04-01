# -------------------------------------------------
# Define the sources to be built
# -------------------------------------------------

# redis-server source files
set(REDIS_SERVER_SRCS
    ${CMAKE_SOURCE_DIR}/src/redisassert.c

    ${CMAKE_SOURCE_DIR}/src/threads_mngr.c
    ${CMAKE_SOURCE_DIR}/src/adlist.c
    ${CMAKE_SOURCE_DIR}/src/quicklist.c
    ${CMAKE_SOURCE_DIR}/src/ae.c
    ${CMAKE_SOURCE_DIR}/src/anet.c
    ${CMAKE_SOURCE_DIR}/src/dict.c
    ${CMAKE_SOURCE_DIR}/src/kvstore.c
    ${CMAKE_SOURCE_DIR}/src/sds.c
    ${CMAKE_SOURCE_DIR}/src/zmalloc.c
    ${CMAKE_SOURCE_DIR}/src/lzf_c.c
    ${CMAKE_SOURCE_DIR}/src/lzf_d.c
    ${CMAKE_SOURCE_DIR}/src/pqsort.c
    ${CMAKE_SOURCE_DIR}/src/zipmap.c
    ${CMAKE_SOURCE_DIR}/src/sha1.c
    ${CMAKE_SOURCE_DIR}/src/ziplist.c
    ${CMAKE_SOURCE_DIR}/src/release.c
    ${CMAKE_SOURCE_DIR}/src/memory_prefetch.c
    ${CMAKE_SOURCE_DIR}/src/iothread.c
    ${CMAKE_SOURCE_DIR}/src/networking.c
    ${CMAKE_SOURCE_DIR}/src/util.c
    ${CMAKE_SOURCE_DIR}/src/object.c
    ${CMAKE_SOURCE_DIR}/src/db.c
    ${CMAKE_SOURCE_DIR}/src/replication.c
    ${CMAKE_SOURCE_DIR}/src/rdb.c
    ${CMAKE_SOURCE_DIR}/src/t_string.c
    ${CMAKE_SOURCE_DIR}/src/t_list.c
    ${CMAKE_SOURCE_DIR}/src/t_set.c
    ${CMAKE_SOURCE_DIR}/src/t_zset.c
    ${CMAKE_SOURCE_DIR}/src/t_hash.c
    ${CMAKE_SOURCE_DIR}/src/config.c
    ${CMAKE_SOURCE_DIR}/src/aof.c
    ${CMAKE_SOURCE_DIR}/src/pubsub.c
    ${CMAKE_SOURCE_DIR}/src/multi.c
    ${CMAKE_SOURCE_DIR}/src/debug.c
    ${CMAKE_SOURCE_DIR}/src/sort.c
    ${CMAKE_SOURCE_DIR}/src/intset.c
    ${CMAKE_SOURCE_DIR}/src/syncio.c
    ${CMAKE_SOURCE_DIR}/src/cluster.c
    ${CMAKE_SOURCE_DIR}/src/cluster_legacy.c
    ${CMAKE_SOURCE_DIR}/src/cluster_slot_stats.c
    ${CMAKE_SOURCE_DIR}/src/crc16.c
    
    ${CMAKE_SOURCE_DIR}/src/eval.c
    ${CMAKE_SOURCE_DIR}/src/bio.c
    ${CMAKE_SOURCE_DIR}/src/rio.c
    ${CMAKE_SOURCE_DIR}/src/rand.c
    ${CMAKE_SOURCE_DIR}/src/memtest.c
    ${CMAKE_SOURCE_DIR}/src/syscheck.c
    ${CMAKE_SOURCE_DIR}/src/crcspeed.c
    ${CMAKE_SOURCE_DIR}/src/crccombine.c
    ${CMAKE_SOURCE_DIR}/src/crc64.c
    ${CMAKE_SOURCE_DIR}/src/bitops.c
    ${CMAKE_SOURCE_DIR}/src/sentinel.c
    ${CMAKE_SOURCE_DIR}/src/notify.c
    ${CMAKE_SOURCE_DIR}/src/setproctitle.c
    ${CMAKE_SOURCE_DIR}/src/blocked.c
    ${CMAKE_SOURCE_DIR}/src/hyperloglog.c
    ${CMAKE_SOURCE_DIR}/src/latency.c
    ${CMAKE_SOURCE_DIR}/src/sparkline.c
    ${CMAKE_SOURCE_DIR}/src/redis-check-rdb.c
    ${CMAKE_SOURCE_DIR}/src/redis-check-aof.c
    ${CMAKE_SOURCE_DIR}/src/geo.c
    ${CMAKE_SOURCE_DIR}/src/lazyfree.c
    ${CMAKE_SOURCE_DIR}/src/module.c
    ${CMAKE_SOURCE_DIR}/src/evict.c
    ${CMAKE_SOURCE_DIR}/src/expire.c
    ${CMAKE_SOURCE_DIR}/src/geohash.c
    ${CMAKE_SOURCE_DIR}/src/geohash_helper.c
    ${CMAKE_SOURCE_DIR}/src/childinfo.c
    ${CMAKE_SOURCE_DIR}/src/defrag.c
    ${CMAKE_SOURCE_DIR}/src/siphash.c
    ${CMAKE_SOURCE_DIR}/src/rax.c
    ${CMAKE_SOURCE_DIR}/src/t_stream.c
    ${CMAKE_SOURCE_DIR}/src/listpack.c
    ${CMAKE_SOURCE_DIR}/src/localtime.c
    ${CMAKE_SOURCE_DIR}/src/lolwut.c
    ${CMAKE_SOURCE_DIR}/src/lolwut5.c
    ${CMAKE_SOURCE_DIR}/src/lolwut6.c
    ${CMAKE_SOURCE_DIR}/src/acl.c
    ${CMAKE_SOURCE_DIR}/src/tracking.c
    ${CMAKE_SOURCE_DIR}/src/socket.c
    ${CMAKE_SOURCE_DIR}/src/tls.c
    ${CMAKE_SOURCE_DIR}/src/sha256.c
    ${CMAKE_SOURCE_DIR}/src/timeout.c
    ${CMAKE_SOURCE_DIR}/src/setcpuaffinity.c
    ${CMAKE_SOURCE_DIR}/src/monotonic.c
    ${CMAKE_SOURCE_DIR}/src/mt19937-64.c
    ${CMAKE_SOURCE_DIR}/src/resp_parser.c
    ${CMAKE_SOURCE_DIR}/src/call_reply.c
    ${CMAKE_SOURCE_DIR}/src/script.c
    ${CMAKE_SOURCE_DIR}/src/functions.c
    ${CMAKE_SOURCE_DIR}/src/commands.c
    ${CMAKE_SOURCE_DIR}/src/strl.c
    ${CMAKE_SOURCE_DIR}/src/connection.c
    ${CMAKE_SOURCE_DIR}/src/unix.c
    ${CMAKE_SOURCE_DIR}/src/server.c
    ${CMAKE_SOURCE_DIR}/src/logreqres.c
    ${CMAKE_SOURCE_DIR}/src/entry.c
    ${CMAKE_SOURCE_DIR}/src/chk.c
    ${CMAKE_SOURCE_DIR}/src/cluster_asm.c
    ${CMAKE_SOURCE_DIR}/src/ebuckets.c
    ${CMAKE_SOURCE_DIR}/src/endianconv.c
    ${CMAKE_SOURCE_DIR}/src/estore.c
    ${CMAKE_SOURCE_DIR}/src/eventnotifier.c
    ${CMAKE_SOURCE_DIR}/src/function_lua.c
    ${CMAKE_SOURCE_DIR}/src/fwtree.c
    ${CMAKE_SOURCE_DIR}/src/gcra.c
    ${CMAKE_SOURCE_DIR}/src/hotkeys.c
    ${CMAKE_SOURCE_DIR}/src/keymeta.c
    ${CMAKE_SOURCE_DIR}/src/lolwut8.c
    ${CMAKE_SOURCE_DIR}/src/mstr.c
    ${CMAKE_SOURCE_DIR}/src/script_lua.c
    ${CMAKE_SOURCE_DIR}/src/slowlog.c
)



# redis-cli
set(REDIS_CLI_SRCS
    ${CMAKE_SOURCE_DIR}/src/cli_commands.c
    ${CMAKE_SOURCE_DIR}/src/cli_common.c
    ${CMAKE_SOURCE_DIR}/src/anet.c
    ${CMAKE_SOURCE_DIR}/src/adlist.c
    ${CMAKE_SOURCE_DIR}/src/dict.c
    ${CMAKE_SOURCE_DIR}/src/sds.c
    ${CMAKE_SOURCE_DIR}/src/sha256.c
    ${CMAKE_SOURCE_DIR}/src/util.c
    ${CMAKE_SOURCE_DIR}/src/redis-cli.c
    ${CMAKE_SOURCE_DIR}/src/zmalloc.c
    ${CMAKE_SOURCE_DIR}/src/release.c
    ${CMAKE_SOURCE_DIR}/src/ae.c
    ${CMAKE_SOURCE_DIR}/src/redisassert.c
    ${CMAKE_SOURCE_DIR}/src/crcspeed.c
    ${CMAKE_SOURCE_DIR}/src/crccombine.c
    ${CMAKE_SOURCE_DIR}/src/crc64.c
    ${CMAKE_SOURCE_DIR}/src/siphash.c
    ${CMAKE_SOURCE_DIR}/src/crc16.c
    ${CMAKE_SOURCE_DIR}/src/monotonic.c
    ${CMAKE_SOURCE_DIR}/src/mt19937-64.c
    ${CMAKE_SOURCE_DIR}/src/strl.c
)


# redis-benchmark
set(REDIS_BENCHMARK_SRCS
    ${CMAKE_SOURCE_DIR}/src/ae.c
    ${CMAKE_SOURCE_DIR}/src/anet.c
    ${CMAKE_SOURCE_DIR}/src/sds.c
    ${CMAKE_SOURCE_DIR}/src/sha256.c
    ${CMAKE_SOURCE_DIR}/src/util.c
    ${CMAKE_SOURCE_DIR}/src/redis-benchmark.c
    ${CMAKE_SOURCE_DIR}/src/adlist.c
    ${CMAKE_SOURCE_DIR}/src/dict.c
    ${CMAKE_SOURCE_DIR}/src/zmalloc.c
    ${CMAKE_SOURCE_DIR}/src/redisassert.c
    ${CMAKE_SOURCE_DIR}/src/release.c
    ${CMAKE_SOURCE_DIR}/src/crcspeed.c
    ${CMAKE_SOURCE_DIR}/src/crccombine.c
    ${CMAKE_SOURCE_DIR}/src/crc64.c
    ${CMAKE_SOURCE_DIR}/src/siphash.c
    ${CMAKE_SOURCE_DIR}/src/crc16.c
    
    ${CMAKE_SOURCE_DIR}/src/monotonic.c
    ${CMAKE_SOURCE_DIR}/src/mt19937-64.c
    ${CMAKE_SOURCE_DIR}/src/strl.c
)

# redis-rdma module

# redis-tls module
set(REDIS_TLS_MODULE_SRCS ${CMAKE_SOURCE_DIR}/src/tls.c)
