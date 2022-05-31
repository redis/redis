start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0
    set host [srv 0 host]
    set port [srv 0 port]
    test {swap out string} {
        for {set j 0} {$j < 10} {incr j} {
            r setex k 1 v 
            after 999
            r evict k 
            after 1
            assert_equal [r get k] {}
        }
    }
} 