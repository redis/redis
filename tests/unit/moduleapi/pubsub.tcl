#source tests/support/util.tcl

set testmodule [file normalize tests/modules/pubsub.so]

start_server {tags {"modules"}} {

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

    r module load $testmodule

    test "Module can publish message to channel" {

        set rd1 [redis_deferring_client]

        # First a subscribe need to exist to verify
        # a message is published
        assert_equal {1} [subscribe $rd1 {universe}]

        assert_equal 1 [r pubsub.publish]
        assert_equal {message universe 42} [$rd1 read]

        unsubscribe $rd1 {universe}

        assert_equal 0 [r pubsub.publish]

        # clean up clients
        $rd1 close
    }
}