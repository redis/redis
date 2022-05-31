start_server {tags {"swap string"}} {
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

    test {swap in string} {
        assert_equal [r get k] v
        assert_match "*keys=1,evicts=0*" [r info keyspace] 
    }
}


start_server {tags {"swap  small hash"}} {
    r config set debug-evict-keys 0
    test {swap out hash} {

        r hset h k v
        assert_match "*keys=1,evicts=0*" [r info keyspace] 
        r evict h
        wait_for_condition 100 50 {
            [string match "*keys=0,evicts=1*" [r info keyspace]]
        } else {
            fail "evict fail"
        } 
        assert_match "*keys=0,evicts=1*" [r info keyspace] 
    }

    test {swap in hash} {
        assert_equal [r hget h k] v
        assert_match "*keys=1,evicts=0*" [r info keyspace] 
    }
} 