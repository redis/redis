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
        # make the previous test is really done before sampling used_memory
        wait_lazyfree_done r

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
        # make the previous test is really done before doing RESETSTAT
        wait_for_condition 50 100 {
            [s lazyfree_pending_objects] == 0
        } else {
            fail "lazyfree isn't done"
        }

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
        wait_for_condition 50 100 {
            [s lazyfree_pending_objects] == 0
        } else {
            fail "lazyfree isn't done"
        }
        assert_equal [s lazyfreed_objects] 1
    } {} {needs:config-resetstat}

    test "lazy free a stream with deleted cgroup" {
        r config resetstat
        r xadd s * a b
        r xgroup create s bla $
        r xgroup destroy s bla
        r unlink s

        # make sure it was not lazy freed
        wait_for_condition 50 100 {
            [s lazyfree_pending_objects] == 0
        } else {
            fail "lazyfree isn't done"
        }
        assert_equal [s lazyfreed_objects] 0
    } {} {needs:config-resetstat}

    test "FLUSHALL SYNC optimized to run in bg as blocking FLUSHALL ASYNC" {
        set num_keys 1000
        r config resetstat

        # Verify at start there are no lazyfree pending objects
        assert_equal [s lazyfree_pending_objects] 0

        # Fillup DB with items
        populate $num_keys

        # Run FLUSHALL SYNC command, optimized as blocking ASYNC
        r flushall

        # Verify all keys counted as lazyfreed
        assert_equal [s lazyfreed_objects] $num_keys
    }

    test "Run consecutive blocking FLUSHALL ASYNC successfully" {
        r config resetstat
        set rd [redis_deferring_client]

        # Fillup DB with items
        r set x 1
        r set y 2

        $rd write "FLUSHALL\r\nFLUSHALL\r\nFLUSHDB\r\n"
        $rd flush
        assert_equal [$rd read] {OK}
        assert_equal [$rd read] {OK}
        assert_equal [$rd read] {OK}
        assert_equal [s lazyfreed_objects] 2
        $rd close
    }

    test "FLUSHALL SYNC in MULTI not optimized to run as blocking FLUSHALL ASYNC" {
        r config resetstat

        # Fillup DB with items
        r set x 11
        r set y 22

        # FLUSHALL SYNC in multi
        r multi
        r flushall
        r exec

        # Verify flushall not run as lazyfree
        assert_equal [s lazyfree_pending_objects] 0
        assert_equal [s lazyfreed_objects] 0
    }

    test "Client closed in the middle of blocking FLUSHALL ASYNC" {
        set num_keys 100000
        r config resetstat

        # Fillup DB with items
        populate $num_keys

        # close client in the middle of ongoing Blocking FLUSHALL ASYNC
        set rd [redis_deferring_client]
        $rd flushall
        $rd close

        # Wait to verify all keys counted as lazyfreed
        wait_for_condition 50 100 {
            [s lazyfreed_objects] == $num_keys
        } else {
            fail "Unexpected number of lazyfreed_objects: [s lazyfreed_objects]"
        }
    }

    test "Pending commands in querybuf processed once unblocking FLUSHALL ASYNC" {
        r config resetstat
        set rd [redis_deferring_client]

        # Fillup DB with items
        r set x 1
        r set y 2

        $rd write "FLUSHALL\r\nPING\r\n"
        $rd flush
        assert_equal [$rd read] {OK}
        assert_equal [$rd read] {PONG}
        assert_equal [s lazyfreed_objects] 2
        $rd close
    }

    test "Unblocks client blocked on lazyfree via REPLICAOF command" {
        r config resetstat
        set rd [redis_deferring_client]

        populate 50000 ;# Just to make flushdb async slower
        $rd flushdb

        # Verify flushdb run as lazyfree
        wait_for_condition 50 100 {
            [s lazyfree_pending_objects] > 0 ||
            [s lazyfreed_objects] > 0
        } else {
            fail "FLUSHDB didn't run as lazyfree"
        }

        # Test that slaveof command unblocks clients without assertion failure
        r slaveof 127.0.0.1 0
        assert_equal [$rd read] {OK}
        $rd close
        r ping
        r slaveof no one
    } {OK} {external:skip}
}

start_server {tags {"lazyfree external:skip"}} {
    start_server {} {
        set slave [srv 0 client]
        set slave_host [srv 0 host]
        set slave_port [srv 0 port]
        set slave_log [srv 0 stdout]

        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set master_log [srv -1 stdout]

        $slave slaveof $master_host $master_port
        wait_for_condition 50 1000 {
            [status $slave master_link_status] == "up"
        } else {
            fail "replication failed"ma
        }
        test "slave expire list will async free" {
            r config set replica-read-only no
            r flushall async
            wait_lazyfree_done r
            # Create 100 keys with expiration on replica
            # These keys will be stored in two places:
            # 1. Replica's main DB (as regular key-value pairs)
            # 2. Replica's slaveExpireList (maintained for expiration tracking)
            for {set j 0} {$j < 100} {incr j} {
                set key "slave_$j"
                set value $j
                r set $key $value ex 100
            }
            set prev [status r lazyfreed_objects]
            r flushall async

            wait_lazyfree_done r
            # Assert total lazyfreed objects increased by 200:
            # - 100 from the main DB (the key-value pairs we created)
            # - 100 from the slaveKeysWithExpire
            assert_equal [expr $prev+200] [status r lazyfreed_objects]
        }
        
        test "slave expire list async free after master-replica switch" {
            $slave config set replica-read-only no
            $slave flushall async
            wait_lazyfree_done $slave

            for {set j 0} {$j < 100} {incr j} {
                set key "slave_switch_$j"
                set value $j
                $slave set $key $value ex 100
            }

            $slave slaveof no one

            # Verify switch success: new master's role should be "master"
            wait_for_condition 50 100 {
                [status $slave role] eq "master"
            } else {
                fail "Master-replica switch failed: replica did not become master"
            }

            set prev_switch [status $slave lazyfreed_objects]

            $slave flushall async
            wait_lazyfree_done $slave

            # Assert total lazyfreed objects increased by 200:
            # - 100: key-value pairs from new master's DB
            # - 100: expiration metadata from slaveKeysWithExpire
            assert_equal [expr $prev_switch+200] [status $slave lazyfreed_objects]
        }
    }
}