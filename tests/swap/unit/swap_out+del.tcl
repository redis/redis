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
        r del k
        assert_equal {# Keyspace} [string trim [r info keyspace]]
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
    }

    test {swap in hash} {
        r del h
        assert_equal {# Keyspace} [string trim [r info keyspace]]
    }
} 