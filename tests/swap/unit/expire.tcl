start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0
    test {swap out string} {
        r set k v
        r expire k 2
        assert_match "*keys=1,evicts=0*" [r info keyspace] 
        r evict k
        wait_for_condition 100 50 {
            [string match "*keys=0,evicts=1,expires=1*" [r info keyspace]]
        } else {
            fail "evict fail"
        } 
        after 2500
        assert_equal {# Keyspace} [string trim [r info keyspace]]
        assert_match [r get k] {}
    }
} 


start_server {tags "evict/expire"} {
    
}