tags {"external:skip"} {

# Copy RDB with ziplist encoded hash to server path
set server_path [tmpdir "server.convert-ziplist-hash-on-load"]

exec cp -f tests/assets/zset-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "zset-ziplist.rdb"]] {
    test "RDB load ziplist zset: converts to listpack when RDB loading" {
        r select 0

        assert_encoding listpack zset
        assert_equal 2 [r zcard zset]
        assert_match {one 1 two 2} [r zrange zset 0 -1 withscores]
    }
}

exec cp -f tests/assets/zset-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "zset-ziplist.rdb" "zset-max-ziplist-entries" 1]] {
    test "RDB load ziplist zset: converts to skiplist when zset-max-ziplist-entries is exceeded" {
        r select 0

        assert_encoding skiplist zset
        assert_equal 2 [r zcard zset]
        assert_match {one 1 two 2} [r zrange zset 0 -1 withscores]
    }
}

}
