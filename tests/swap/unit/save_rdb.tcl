tart_server {tags {"swap string"} keep_persistence true} {
    r config set debug-evict-keys 0
    test {"save + restart_server"} {
        r set k v
        r evict k 
        wait_key_cold r k

        assert_equal [r save] OK
        set dir [lindex [r config get dir] 1]
        set file $dir/dump.rdb
        assert_equal [file exists $file] 1
        r debug reload
        assert_equal [r get k] v
    }
} 

