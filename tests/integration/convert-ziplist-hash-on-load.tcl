tags {"external:skip"} {

# Copy RDB with ziplist encoded hash to server path
set server_path [tmpdir "server.convert-ziplist-hash-on-load"]

exec cp -f tests/assets/hash-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-ziplist.rdb"]] {
    test "RDB load ziplist hash: converts to listpack when RDB loading" {
        r select 0

        assert_encoding listpack hash
        assert_equal 2 [r hlen hash]
        assert_match {v1 v2} [r hmget hash f1 f2]
    }
}

exec cp -f tests/assets/hash-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-ziplist.rdb" "hash-max-ziplist-entries" 1]] {
    test "RDB load ziplist hash: converts to hash table when hash-max-ziplist-entries is exceeded" {
        r select 0

        assert_encoding hashtable hash
        assert_equal 2 [r hlen hash]
        assert_match {v1 v2} [r hmget hash f1 f2]
    }
}

}
