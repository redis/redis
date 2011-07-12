start_server {tags {"repl"}} {
    start_server {} {
        test {First server should have role slave after SLAVEOF} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            after 1000
            s -1 role
        } {slave}

        test {BRPOPLPUSH replication, when blocking against empty list} {
            set rd [redis_deferring_client]
            $rd brpoplpush a b 5
            r lpush a foo
            after 1000
            assert_equal [r debug digest] [r -1 debug digest]
        }

        test {BRPOPLPUSH replication, list exists} {
            set rd [redis_deferring_client]
            r lpush c 1
            r lpush c 2
            r lpush c 3
            $rd brpoplpush c d 5
            after 1000
            assert_equal [r debug digest] [r -1 debug digest]
        }
    }
}

start_server {tags {"repl"}} {
    r set mykey foo
    
    start_server {} {
        test {Second server should have role master at first} {
            s role
        } {master}
        
        test {SLAVEOF should start with link status "down"} {
            r slaveof [srv -1 host] [srv -1 port]
            s master_link_status
        } {down}
        
        test {The role should immediately be changed to "slave"} {
            s role
        } {slave}

        wait_for_sync r
        test {Sync should have transferred keys from master} {
            r get mykey
        } {foo}
        
        test {The link status should be up} {
            s master_link_status
        } {up}
        
        test {SET on the master should immediately propagate} {
            r -1 set mykey bar
            if {$::valgrind} {after 2000}
            r  0 get mykey
        } {bar}
    }
}
