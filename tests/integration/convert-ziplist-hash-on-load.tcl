# Copy RDB with ziplist encoded hash to server path
set server_path [tmpdir "server.convert-ziplist-hash-on-load"]

exec cp -f tests/assets/hash-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-ziplist.rdb"]] {
  test "RDB load ziplist hash: converts to listpack" {
    r select 0

    assert_match "*listpack*" [r debug object hash]
    assert_equal 2 [r hlen hash]
    assert_match {v1 v2} [r hmget hash f1 f2]
  }
}

exec cp -f tests/assets/hash-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-ziplist.rdb" "hash-max-listpack-entries" 1]] {
  test "RDB load ziplist hash: converts to hash table when hash-max-listpack-entries is exceeded" {
    r select 0

    assert_match "*hashtable*" [r debug object hash]
    assert_equal 2 [r hlen hash]
    assert_match {v1 v2} [r hmget hash f1 f2]
  }
}

exec cp -f tests/assets/hash-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "hash-ziplist.rdb" "hash-max-listpack-value" 1]] {
  test "RDB load ziplist hash: converts to hash table when hash-max-listpack-value is exceeded" {
    r select 0

    assert_match "*hashtable*" [r debug object hash]
    assert_equal 2 [r hlen hash]
    assert_match {v1 v2} [r hmget hash f1 f2]
  }
}
