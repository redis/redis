start_server {tags {"lazyfree"}} {
    test "UNLINK can reclaim memory in background" {
        set orig_mem [s used_memory]
        set args {}
        for {set i 0} {$i < 100000} {incr i} {
            lappend args $i
        }
        r sadd myset {*}$args
        assert {[r scard myset] == 100000}
        set peak_mem [s used_memory]
        assert {[r unlink myset] == 1}
        assert {$peak_mem > $orig_mem+1000000}
        wait_for_condition 50 100 {
            [s used_memory] < $peak_mem &&
            [s used_memory] < $orig_mem*2
        } else {
            fail "Memory is not reclaimed by UNLINK"
        }
    }

    test "FLUSHDB ASYNC can reclaim memory in background" {
        set orig_mem [s used_memory]
        set args {}
        for {set i 0} {$i < 100000} {incr i} {
            lappend args $i
        }
        r sadd myset {*}$args
        assert {[r scard myset] == 100000}
        set peak_mem [s used_memory]
        r flushdb async
        assert {$peak_mem > $orig_mem+1000000}
        wait_for_condition 50 100 {
            [s used_memory] < $peak_mem &&
            [s used_memory] < $orig_mem*2
        } else {
            fail "Memory is not reclaimed by FLUSHDB ASYNC"
        }
    }

    test "lazy free a stream with all types of metadata" {
        r config resetstat
        r config set stream-node-max-entries 5
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r xadd stream * foo $j
            } else {
                r xadd stream * bar $j
            }
        }
        r xgroup create stream mygroup 0
        set records [r xreadgroup GROUP mygroup Alice COUNT 2 STREAMS stream >]
        r xdel stream [lindex [lindex [lindex [lindex $records 0] 1] 1] 0]
        r xack stream mygroup [lindex [lindex [lindex [lindex $records 0] 1] 0] 0]
        r unlink stream

        # make sure it was lazy freed
        wait_for_condition 5 100 {
            [s lazyfree_pending_objects] == 0
        } else {
            fail "lazyfree isn't done"
        }
        assert_equal [s lazyfreed_objects] 1
    }

    test "lazy free a stream with deleted cgroup" {
        r config resetstat
        r xadd s * a b
        r xgroup create s bla $
        r xgroup destroy s bla
        r unlink s

        # make sure it was not lazy freed
        wait_for_condition 5 100 {
            [s lazyfree_pending_objects] == 0
        } else {
            fail "lazyfree isn't done"
        }
        assert_equal [s lazyfreed_objects] 0
    }
}
