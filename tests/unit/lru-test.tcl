start_server {tags {"maxmemory"}} {
    proc __consume_subscribe_messages {client type channels} {
        set numsub -1
        set counts {}

        for {set i [llength $channels]} {$i > 0} {incr i -1} {
            set msg [$client read]
            assert_equal $type [lindex $msg 0]

            # when receiving subscribe messages the channels names
            # are ordered. when receiving unsubscribe messages
            # they are unordered
            set idx [lsearch -exact $channels [lindex $msg 1]]
            if {[string match "*unsubscribe" $type]} {
                assert {$idx >= 0}
            } else {
                assert {$idx == 0}
            }
            set channels [lreplace $channels $idx $idx]

            # aggregate the subscription count to return to the caller
            lappend counts [lindex $msg 2]
        }

        # we should have received messages for channels
        assert {[llength $channels] == 0}
        return $counts
    }

    proc subscribe {client channels} {
        $client subscribe {*}$channels
        __consume_subscribe_messages $client subscribe $channels
    }

    proc unsubscribe {client {channels {}}} {
        $client unsubscribe {*}$channels
        __consume_subscribe_messages $client unsubscribe $channels
    }

    proc psubscribe {client channels} {
        $client psubscribe {*}$channels
        __consume_subscribe_messages $client psubscribe $channels
    }

    proc punsubscribe {client {channels {}}} {
        $client punsubscribe {*}$channels
        __consume_subscribe_messages $client punsubscribe $channels
    }


    test "lru test - real lru eviction algorithm" {
        r config set notify-keyspace-events Ee
        r config set maxmemory-policy allkeys-real-lru
        r flushdb
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]

        r config set maxmemory 10000000;
        for {set name 1} {$name <= 100} {incr name} {
            r set $name "hello world"
        }
        for {set name 1} {$name <= 25} {incr name} {
            r get $name
        }
        
        r config set maxmemory 1
        for {set name 26} {$name <= 100} {incr name} {
            set s [$rd1 read]
            assert {$s == "pmessage * __keyevent@9__:evicted $name"}
        }
        for {set name 1} {$name <= 25} {incr name} {
            set s [$rd1 read]
            assert {$s == "pmessage * __keyevent@9__:evicted $name"}
        }
        r config set maxmemory 0
        $rd1 close
    }
}