start_server {tags {"perform eviction"}} {
    r config set debug-evict-keys 0

    test {limit maxmemory to trigger eviction} {
        set maxmemory [s used_memory]
        set newmemory [expr {$maxmemory+1024*1024}]
        r config set maxmemory $newmemory
        r config set maxmemory-policy allkeys-lfu
    }

    test {disk mode evict - hash} {
        set duration 20
        set keys 32
        set fields [expr 1024*256]

        set start_time [clock seconds]
        set elapsed 0
        set prev_elapsed 0

        r flushdb

        while 1 {
            for {set i 0} {$i < 64} {incr i} {
                set key "myhash-[randomInt $keys]"
                set field "field-[randomInt $fields]"
                set val "val-[randomInt $fields]"
                r HSET $key $field $val
            }
            set elapsed [expr [clock seconds]-$start_time]

            if {$elapsed > $prev_elapsed} {
                if {$elapsed > $duration} {
                    break;
                }
                set prev_elapsed $elapsed
            }
        }
    }

    test {disk mode evict - set} {
        set duration 20
        set keys 32
        set members [expr 1024*256]

        set start_time [clock seconds]
        set elapsed 0
        set prev_elapsed 0

        r flushdb

        while 1 {
            for {set i 0} {$i < 64} {incr i} {
                set key "myset-[randomInt $keys]"
                set member [randomInt $members]
                r SADD $key $member
            }
            set elapsed [expr [clock seconds]-$start_time]

            if {$elapsed > $prev_elapsed} {
                if {$elapsed > $duration} {
                    break;
                }
                set prev_elapsed $elapsed
            }
        }
    }

    test {disk mode evict zset} {
        set duration 20
        set keys 32
        set members [expr 1024*256]

        set start_time [clock seconds]
        set elapsed 0
        set prev_elapsed 0

        r flushdb

        while 1 {
            for {set i 0} {$i < 64} {incr i} {
                set key "myzset-[randomInt $keys]"
                set member "member-[randomInt $members]"
                set score [randomInt $members]
                r zadd $key $score $member
            }
            set elapsed [expr [clock seconds]-$start_time]

            if {$elapsed > $prev_elapsed} {
                if {$elapsed > $duration} {
                    break;
                }
                set prev_elapsed $elapsed
            }
        }
    }
    
    test {disk mode evict list} {
        set duration 20
        set keys 32
        set eles [expr 1024*256]

        set start_time [clock seconds]
        set elapsed 0
        set prev_elapsed 0

        r flushdb

        while 1 {
            for {set i 0} {$i < 64} {incr i} {
                set key "mylist-[randomInt $keys]"
                set ele [randomInt $eles]
                r rpush $key $ele
                r ltrim $key 0 $eles
            }
            set elapsed [expr [clock seconds]-$start_time]

            if {$elapsed > $prev_elapsed} {
                if {$elapsed > $duration} {
                    break;
                }
                set prev_elapsed $elapsed
            }
        }
    }
} 

start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0

    test {swap out string} {
        for {set j 0} {$j < 60} {incr j} {
            r setex k 1 v 
            after 999
            r evict k 
            after 1
            r get k
            after 10
            assert_equal [r get k] {}
        }
    }
} 

