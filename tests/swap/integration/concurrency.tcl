
start_server {tags {"concurrency "}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set load_handle0 [start_run_load $master_host $master_port 0 1000000 {
        $r RPUSH mylist "one"
        $r swap.evict mylist
        after 1
    }]
    set load_handle1 [start_run_load $master_host $master_port 0 1000000 {
        $r LPUSH mylist "two"
        $r swap.evict mylist
        after 1
    }]

    set load_handle2 [start_run_load $master_host $master_port 0 1000000 {
        $r RPUSH mylist1 "one"
        $r swap.evict mylist1
        after 1
    }]

    set load_handle3 [start_run_load $master_host $master_port 0 1000000 {
        $r LPUSH mylist1 "two"
        $r swap.evict mylist1
        after 1
    }]
    set load_handle4 [start_run_load $master_host $master_port 0 1000000 {
        $r del mylist
        after 1
    }]
    set load_handle5 [start_run_load $master_host $master_port 0 1000000 {
        $r del mylist1
        after 1
    }]
    for {set j 0} {$j < 1000} {incr j} {
        assert_equal [$master ping] PONG 
        after 10
    }
    
    stop_bg_complex_data $load_handle0
    stop_bg_complex_data $load_handle1
    stop_bg_complex_data $load_handle2
    stop_bg_complex_data $load_handle3
    stop_bg_complex_data $load_handle4
    stop_bg_complex_data $load_handle5
}
