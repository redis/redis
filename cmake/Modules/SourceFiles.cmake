# -------------------------------------------------
# Define the sources to be built
# -------------------------------------------------

# redis-server source files
set(REDIS_SERVER_SRCS
    ${REDIS_ROOT}/src/redisassert.c

    ${REDIS_ROOT}/src/threads_mngr.c
    ${REDIS_ROOT}/src/adlist.c
    ${REDIS_ROOT}/src/quicklist.c
    ${REDIS_ROOT}/src/ae.c
    ${REDIS_ROOT}/src/anet.c
    ${REDIS_ROOT}/src/dict.c
    ${REDIS_ROOT}/src/kvstore.c
    ${REDIS_ROOT}/src/sds.c
    ${REDIS_ROOT}/src/zmalloc.c
    ${REDIS_ROOT}/src/lzf_c.c
    ${REDIS_ROOT}/src/lzf_d.c
    ${REDIS_ROOT}/src/pqsort.c
    ${REDIS_ROOT}/src/zipmap.c
    ${REDIS_ROOT}/src/sha1.c
    ${REDIS_ROOT}/src/ziplist.c
    ${REDIS_ROOT}/src/release.c
    ${REDIS_ROOT}/src/memory_prefetch.c
    ${REDIS_ROOT}/src/iothread.c
    ${REDIS_ROOT}/src/networking.c
    ${REDIS_ROOT}/src/util.c
    ${REDIS_ROOT}/src/object.c
    ${REDIS_ROOT}/src/db.c
    ${REDIS_ROOT}/src/replication.c
    ${REDIS_ROOT}/src/rdb.c
    ${REDIS_ROOT}/src/t_string.c
    ${REDIS_ROOT}/src/t_list.c
    ${REDIS_ROOT}/src/t_set.c
    ${REDIS_ROOT}/src/t_zset.c
    ${REDIS_ROOT}/src/t_hash.c
    ${REDIS_ROOT}/src/config.c
    ${REDIS_ROOT}/src/aof.c
    ${REDIS_ROOT}/src/pubsub.c
    ${REDIS_ROOT}/src/multi.c
    ${REDIS_ROOT}/src/debug.c
    ${REDIS_ROOT}/src/sort.c
    ${REDIS_ROOT}/src/intset.c
    ${REDIS_ROOT}/src/syncio.c
    ${REDIS_ROOT}/src/cluster.c
    ${REDIS_ROOT}/src/cluster_legacy.c
    ${REDIS_ROOT}/src/cluster_slot_stats.c
    ${REDIS_ROOT}/src/crc16.c
    
    ${REDIS_ROOT}/src/eval.c
    ${REDIS_ROOT}/src/bio.c
    ${REDIS_ROOT}/src/rio.c
    ${REDIS_ROOT}/src/rand.c
    ${REDIS_ROOT}/src/memtest.c
    ${REDIS_ROOT}/src/syscheck.c
    ${REDIS_ROOT}/src/crcspeed.c
    ${REDIS_ROOT}/src/crccombine.c
    ${REDIS_ROOT}/src/crc64.c
    ${REDIS_ROOT}/src/bitops.c
    ${REDIS_ROOT}/src/sentinel.c
    ${REDIS_ROOT}/src/notify.c
    ${REDIS_ROOT}/src/setproctitle.c
    ${REDIS_ROOT}/src/blocked.c
    ${REDIS_ROOT}/src/hyperloglog.c
    ${REDIS_ROOT}/src/latency.c
    ${REDIS_ROOT}/src/sparkline.c
    ${REDIS_ROOT}/src/redis-check-rdb.c
    ${REDIS_ROOT}/src/redis-check-aof.c
    ${REDIS_ROOT}/src/geo.c
    ${REDIS_ROOT}/src/lazyfree.c
    ${REDIS_ROOT}/src/module.c
    ${REDIS_ROOT}/src/evict.c
    ${REDIS_ROOT}/src/expire.c
    ${REDIS_ROOT}/src/geohash.c
    ${REDIS_ROOT}/src/geohash_helper.c
    ${REDIS_ROOT}/src/childinfo.c
    ${REDIS_ROOT}/src/defrag.c
    ${REDIS_ROOT}/src/siphash.c
    ${REDIS_ROOT}/src/rax.c
    ${REDIS_ROOT}/src/t_stream.c
    ${REDIS_ROOT}/src/listpack.c
    ${REDIS_ROOT}/src/localtime.c
    ${REDIS_ROOT}/src/lolwut.c
    ${REDIS_ROOT}/src/lolwut5.c
    ${REDIS_ROOT}/src/lolwut6.c
    ${REDIS_ROOT}/src/acl.c
    ${REDIS_ROOT}/src/tracking.c
    ${REDIS_ROOT}/src/socket.c
    ${REDIS_ROOT}/src/tls.c
    ${REDIS_ROOT}/src/sha256.c
    ${REDIS_ROOT}/src/timeout.c
    ${REDIS_ROOT}/src/setcpuaffinity.c
    ${REDIS_ROOT}/src/monotonic.c
    ${REDIS_ROOT}/src/mt19937-64.c
    ${REDIS_ROOT}/src/resp_parser.c
    ${REDIS_ROOT}/src/call_reply.c
    ${REDIS_ROOT}/src/script.c
    ${REDIS_ROOT}/src/functions.c
    ${REDIS_ROOT}/src/commands.c
    ${REDIS_ROOT}/src/strl.c
    ${REDIS_ROOT}/src/connection.c
    ${REDIS_ROOT}/src/unix.c
    ${REDIS_ROOT}/src/server.c
    ${REDIS_ROOT}/src/logreqres.c
    ${REDIS_ROOT}/src/entry.c
    ${REDIS_ROOT}/src/chk.c
    ${REDIS_ROOT}/src/cluster_asm.c
    ${REDIS_ROOT}/src/ebuckets.c
    ${REDIS_ROOT}/src/endianconv.c
    ${REDIS_ROOT}/src/estore.c
    ${REDIS_ROOT}/src/eventnotifier.c
    ${REDIS_ROOT}/src/function_lua.c
    ${REDIS_ROOT}/src/fwtree.c
    ${REDIS_ROOT}/src/gcra.c
    ${REDIS_ROOT}/src/hotkeys.c
    ${REDIS_ROOT}/src/keymeta.c
    ${REDIS_ROOT}/src/lolwut8.c
    ${REDIS_ROOT}/src/mstr.c
    ${REDIS_ROOT}/src/script_lua.c
    ${REDIS_ROOT}/src/slowlog.c
    ${REDIS_ROOT}/src/vector.c
    ${REDIS_ROOT}/src/fast_float_strtod.c
    ${REDIS_ROOT}/src/t_array.c
    ${REDIS_ROOT}/src/sparsearray.c
)



# redis-cli
set(REDIS_CLI_SRCS
    ${REDIS_ROOT}/src/cli_commands.c
    ${REDIS_ROOT}/src/cli_common.c
    ${REDIS_ROOT}/src/anet.c
    ${REDIS_ROOT}/src/adlist.c
    ${REDIS_ROOT}/src/dict.c
    ${REDIS_ROOT}/src/sds.c
    ${REDIS_ROOT}/src/sha256.c
    ${REDIS_ROOT}/src/util.c
    ${REDIS_ROOT}/src/redis-cli.c
    ${REDIS_ROOT}/src/zmalloc.c
    ${REDIS_ROOT}/src/release.c
    ${REDIS_ROOT}/src/ae.c
    ${REDIS_ROOT}/src/redisassert.c
    ${REDIS_ROOT}/src/crcspeed.c
    ${REDIS_ROOT}/src/crccombine.c
    ${REDIS_ROOT}/src/crc64.c
    ${REDIS_ROOT}/src/siphash.c
    ${REDIS_ROOT}/src/crc16.c
    ${REDIS_ROOT}/src/monotonic.c
    ${REDIS_ROOT}/src/mt19937-64.c
    ${REDIS_ROOT}/src/strl.c
    ${REDIS_ROOT}/src/fast_float_strtod.c
)


# redis-benchmark
set(REDIS_BENCHMARK_SRCS
    ${REDIS_ROOT}/src/ae.c
    ${REDIS_ROOT}/src/anet.c
    ${REDIS_ROOT}/src/sds.c
    ${REDIS_ROOT}/src/sha256.c
    ${REDIS_ROOT}/src/util.c
    ${REDIS_ROOT}/src/redis-benchmark.c
    ${REDIS_ROOT}/src/adlist.c
    ${REDIS_ROOT}/src/dict.c
    ${REDIS_ROOT}/src/zmalloc.c
    ${REDIS_ROOT}/src/redisassert.c
    ${REDIS_ROOT}/src/release.c
    ${REDIS_ROOT}/src/crcspeed.c
    ${REDIS_ROOT}/src/crccombine.c
    ${REDIS_ROOT}/src/crc64.c
    ${REDIS_ROOT}/src/siphash.c
    ${REDIS_ROOT}/src/crc16.c
    
    ${REDIS_ROOT}/src/monotonic.c
    ${REDIS_ROOT}/src/mt19937-64.c
    ${REDIS_ROOT}/src/strl.c
    ${REDIS_ROOT}/src/fast_float_strtod.c
)

# redis-rdma module

# redis-tls module
set(REDIS_TLS_MODULE_SRCS ${REDIS_ROOT}/src/tls.c)
