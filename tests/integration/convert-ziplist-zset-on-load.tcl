# Copy RDB with ziplist encoded zset to server path
set server_path [tmpdir "server.convert-ziplist-zset-on-load"]

exec cp -f tests/assets/zset-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "zset-ziplist.rdb"]] {
  test "RDB load ziplist zset: converts to listpack" {
    r select 0

    assert_match "*listpack*" [r debug object zset]
    assert_equal 2 [r zcard zset]
    assert_match {m1 1 m2 2} [r zrange zset 0 -1 withscores]
  }
}

exec cp -f tests/assets/zset-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "zset-ziplist.rdb" "zset-max-listpack-entries" 1]] {
  test "RDB load ziplist zset: converts to skiplist when zset-max-listpack-entries is exceeded" {
    r select 0

    assert_match "*skiplist*" [r debug object zset]
    assert_equal 2 [r zcard zset]
    assert_match {m1 1 m2 2} [r zrange zset 0 -1 withscores]
  }
}

exec cp -f tests/assets/zset-ziplist.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "zset-ziplist.rdb" "zset-max-listpack-value" 1]] {
  test "RDB load ziplist zset: converts to skiplist when zset-max-listpack-value is exceeded" {
    r select 0

    assert_match "*skiplist*" [r debug object zset]
    assert_equal 2 [r zcard zset]
    assert_match {m1 1 m2 2} [r zrange zset 0 -1 withscores]
  }
}
