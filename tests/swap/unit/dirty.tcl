start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0
    test {swap out string} {
        r set k v
        for {set j 0} {$j < 100} {incr j} {
            r evict k
            wait_key_cold r k
            assert_equal [r get k] v
        }

        for {set j 0} {$j < 100} {incr j} {
            r set k v$j
            r evict k 
            wait_key_cold r k
            assert_equal [r get k] v$j
        }
    }
}

start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0
    test {swap out string} {
        r hset h k v
        for {set j 0} {$j < 100} {incr j} {
            r evict h 
            wait_key_cold r h
            assert_equal [r hget h k] v
        }


        for {set j 0} {$j < 100} {incr j} {
            r hset h k v$j
            r evict h 
            wait_key_cold r h
            assert_equal [r hget h k] v$j
        }
    }
}

