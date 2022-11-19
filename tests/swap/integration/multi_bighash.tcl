

start_server {tags {"repl"}} {
    set host [srv 0 host]
    set port [srv 0 port]
    test "evit big hash(max-subkeys + 1)" {
        
        r config set swap-debug-evict-keys 0
        r config set swap-evict-step-max-subkeys 1
        r config set hash-max-ziplist-entries 1000000000
        set num 4
        set value [randomValue]
        for {set j 0} {$j < $num} {incr j} {
            r hset key $j $value
        }        
        r evict key
        set load_handler1 [start_run_load  $host $port 10 0 {
            $r1 hget key 1
            $r1 evict key
        }]
        set load_handler2 [start_run_load  $host $port 10 0 {
            $r1 hget key 1
            $r1 evict key
        }]
        set load_handler3 [start_run_load  $host $port 10 0 {
            $r1 hget key 2
            $r1 evict key 
        }]
        set load_handler4 [start_run_load  $host $port 10 0 {
            $r1 hget key 3
            $r1 evict key 
        }]
        set load_handler5 [start_run_load  $host $port 10 0 {
            $r1 hget key 0
            $r1 evict key 
        }]
        set load_handler6 [start_run_load  $host $port 10 0 {
            $r1 evict key
            $r1 evict key8
            $r1 evict key9
            $r1 evict key 
        }]
        set load_handler7 [start_run_load  $host $port 10 0 {
            $r1 evict key
            $r1 evict key0
            $r1 evict key1
            $r1 evict key2
            $r1 evict key 
        }]
        set load_handler8 [start_run_load  $host $port 10 0 {
            $r1 evict key
            $r1 evict key3
            $r1 evict key4
            $r1 evict key 
        }]
        set load_handler9 [start_run_load  $host $port 10 0 {
            $r1 evict key
            $r1 evict key5
            $r1 evict key6
            $r1 evict key7
            $r1 evict key 
        }]

        set load_handler10 [start_run_load  $host $port 10 0 {
            $r1 multi
            $r1 hset key 0 [randomValue]
            $r1 set key4 v 
            $r1 set key8 v 
            $r1 exec
            $r1 evict key 
        }]
        set load_handler11 [start_run_load  $host $port 10 0 {
            $r1 multi
            $r1 hset key 1 [randomValue]
            $r1 set key2 v 
            $r1 set key3 v 
            $r1 set key6 v 
            $r1 exec
            $r1 evict key 
        }]
        set load_handler12 [start_run_load  $host $port 10 0 {
            $r1 multi
            $r1 hset key 2 [randomValue]
            $r1 set key5 v
            $r1 set key7 v
            $r1 exec
            $r1 evict key 
        }]
        set load_handler13 [start_run_load  $host $port 10 0 {
            $r1 multi
            $r1 hset key 3 [randomValue]
            $r1 set key9 v
            $r1 set key0 v
            $r1 set key1 v
            $r1 exec
            $r1 evict key 
        }]
        after 10000
        stop_write_load $load_handler1
        stop_write_load $load_handler2
        stop_write_load $load_handler3
        stop_write_load $load_handler4
        stop_write_load $load_handler5
        stop_write_load $load_handler6
        stop_write_load $load_handler7
        stop_write_load $load_handler8
        stop_write_load $load_handler9
        stop_write_load $load_handler10
        stop_write_load $load_handler11
        stop_write_load $load_handler12
        stop_write_load $load_handler13
        assert_equal [r ping] PONG
    }
}





start_server {tags {"repl"}} {
    set host1 [srv 0 host]
    set port1 [srv 0 port]
    test "evit big hash(max-subkeys + 1)" {
        
        r config set swap-debug-evict-keys 0
        r config set swap-evict-step-max-subkeys 1
        r config set hash-max-ziplist-entries 1000000000
        set num 4
        set evict_num [expr 3*$num/8]
        set count 10000000
        set value [randomValue]
        set fail_n 0 
        for {set j 0} {$j < $num} {incr j} {
            r hset key $j $value
            if {[expr $j % 4] == 0} {
                if {[r evict key] == 0} {
                    incr fail_n
                }
            }
        }
        puts $fail_n
        r hset key0 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
        r hset key1 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key2 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key3 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key4 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key5 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key6 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key7 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key8 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        r hset key9 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v
        start_server {tags {"repl"}} {
            set host [srv 0 host]
            set port [srv 0 port]
            r slaveof $host1 $port1
            r config set swap-debug-evict-keys 0
            r config set swap-evict-step-max-subkeys 1
            r config set hash-max-ziplist-entries 1000000000
            wait_for_condition 50 3000 {
                [status r master_link_status] eq "up"
            } else {
                fail "replica didn't sync in time"
            }
            set load_handler0 [start_run_load  $host1 $port1 100 0 {
                $r hset key0 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 0 [randomValue]
                $r hset key9 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 9 [randomValue]
                $r hset key1 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 8 [randomValue]
                $r hset key5 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key8 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 1 [randomValue]
                
                $r hset key2 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key4 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 2 [randomValue]
                $r hset key7 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 7 [randomValue]
                $r hset key3 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 5 [randomValue]
                $r hset key6 0 v 1 v 2 v 3 v 4 v 5 v 6 v 7 v 8 v 9 v 
                $r hset key 6 [randomValue]
                after 1
            }]
            set load_handler1 [start_run_load  $host $port 100 0 {
                $r1 evict key
                $r1 evict key0  
                $r1 evict key9 
            }]
            set load_handler2 [start_run_load  $host $port 100 0 {
                $r1 evict key
                $r1 evict key1  
                $r1 evict key5 
                $r1 evict key8
            }]
            set load_handler3 [start_run_load  $host $port 100 0 {
                $r1 evict key
                $r1 evict key2  
                $r1 evict key7
            }]
            set load_handler4 [start_run_load  $host $port 100 0 {
                $r1 evict key
                $r1 evict key3 
                $r1 evict key4
                $r1 evict key6
            }]
            set load_handler5 [start_run_load  $host $port 100 0 {
                $r1 evict key
            }]
            
            after 100000
            stop_write_load $load_handler0 
            stop_write_load $load_handler1
            stop_write_load $load_handler2
            stop_write_load $load_handler3
            stop_write_load $load_handler4
            stop_write_load $load_handler5
            assert_equal [r ping] PONG
        }
        assert_equal [r ping] PONG
    }
}
