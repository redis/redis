start_server {tags {"swap string"} keep_persistence true} {
    r config set debug-evict-keys 0
    test {swap out string} {
        r set k v
        assert_match "*keys=1,evicts=0*" [r info keyspace] 
        r evict k 
        wait_for_condition 100 50 {
            [string match "*keys=0,evicts=1*" [r info keyspace]]
        } else {
            fail "evict fail"
        } 
    }

    test {"save + restart_server"} {
        assert_equal [r save] OK
        set dir [lindex [r config get dir] 1]
        set file $dir/dump.rdb
        assert_equal [file exists $file] 1
        r debug reload
        assert_equal [r get k] v
    }
} 