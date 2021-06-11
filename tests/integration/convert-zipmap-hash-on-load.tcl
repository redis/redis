tags {"external:skip"} {

# Copy RDB with zipmap encoded hash to server path
set server_path [tmpdir "server.convert-zipmap-hash-on-load"]

exec cp -f tests/assets/hash-zipmap.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-zipmap.rdb"]] {
  test "RDB load zipmap hash: converts to ziplist" {
    r select 0

    assert_match "*ziplist*" [r debug object hash]
    assert_equal 2 [r hlen hash]
    assert_match {v1 v2} [r hmget hash f1 f2]
  }
}

exec cp -f tests/assets/hash-zipmap.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-zipmap.rdb" "hash-max-ziplist-entries" 1]] {
  test "RDB load zipmap hash: converts to hash table when hash-max-ziplist-entries is exceeded" {
    r select 0

    assert_match "*hashtable*" [r debug object hash]
    assert_equal 2 [r hlen hash]
    assert_match {v1 v2} [r hmget hash f1 f2]
  }
}

exec cp -f tests/assets/hash-zipmap.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-zipmap.rdb" "hash-max-ziplist-value" 1]] {
  test "RDB load zipmap hash: converts to hash table when hash-max-ziplist-value is exceeded" {
    r select 0

    assert_match "*hashtable*" [r debug object hash]
    assert_equal 2 [r hlen hash]
    assert_match {v1 v2} [r hmget hash f1 f2]
  }
}

}
