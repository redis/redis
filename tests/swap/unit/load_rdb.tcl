tags {"rdb"} {

    set server_path [tmpdir "server.rdb-test"]
    # Copy RDB with different kv in server path

    test "load rdb (kv)" {
        exec cp tests/assets/swap/kv.rdb $server_path
        start_server [list overrides [list "dir" $server_path "dbfilename" "kv.rdb"]] {
            assert_equal [r get k] v
        }
    }

    test "load rdb (hash)" {
        exec cp tests/assets/swap/hash.rdb $server_path
        start_server [list overrides [list "dir" $server_path "dbfilename" "hash.rdb"]] {
            assert_equal [r dbsize] 1
        }
    }
}