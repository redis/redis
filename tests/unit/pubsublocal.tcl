start_server {tags {"pubsublocal"}} {
    proc __consume_subscribelocal_messages {client type channels} {
        set numsub -1
        set counts {}

        for {set i [llength $channels]} {$i > 0} {incr i -1} {
            set msg [$client read]
            assert_equal $type [lindex $msg 0]

            # when receiving subscribe messages the channels names
            # are ordered. when receiving unsubscribe messages
            # they are unordered
            set idx [lsearch -exact $channels [lindex $msg 1]]
            if {[string match "unsubscribelocal" $type]} {
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
            if {[string match "unsubscribe" $type]} {
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

    proc subscribelocal {client channels} {
        $client subscribelocal {*}$channels
        __consume_subscribelocal_messages $client subscribelocal $channels
    }

    proc subscribe {client channels} {
        $client subscribe {*}$channels
        __consume_subscribe_messages $client subscribe $channels
    }

    proc unsubscribelocal {client {channels {}}} {
        $client unsubscribelocal {*}$channels
        __consume_subscribe_messages $client unsubscribelocal $channels
    }

    proc unsubscribe {client {channels {}}} {
        $client unsubscribe {*}$channels
        __consume_subscribe_messages $client unsubscribe $channels
    }

    test "PUBLISHLOCAL/SUBSCRIBELOCAL basics" {
        set rd1 [redis_deferring_client]

        # subscribe to two channels
        assert_equal {1 2} [subscribelocal $rd1 {chan1 chan2}]
        assert_equal 1 [r publishlocal chan1 hello]
        assert_equal 1 [r publishlocal chan2 world]
        assert_equal {message chan1 hello} [$rd1 read]
        assert_equal {message chan2 world} [$rd1 read]

        # unsubscribe from one of the channels
        unsubscribelocal $rd1 {chan1}
        assert_equal 0 [r publishlocal chan1 hello]
        assert_equal 1 [r publishlocal chan2 world]
        assert_equal {message chan2 world} [$rd1 read]

        # unsubscribe from the remaining channel
        unsubscribelocal $rd1 {chan2}
        assert_equal 0 [r publishlocal chan1 hello]
        assert_equal 0 [r publishlocal chan2 world]

        # clean up clients
        $rd1 close
    }

    test "PUBLISHLOCAL/SUBSCRIBELOCAL with two clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [subscribelocal $rd1 {chan1}]
        assert_equal {1} [subscribelocal $rd2 {chan1}]
        assert_equal 2 [r publishlocal chan1 hello]
        assert_equal {message chan1 hello} [$rd1 read]
        assert_equal {message chan1 hello} [$rd2 read]

        # clean up clients
        $rd1 close
        $rd2 close
    }

    test "PUBLISH/SUBSCRIBE after UNSUBSCRIBE without arguments" {
        set rd1 [redis_deferring_client]
        assert_equal {1 2 3} [subscribelocal $rd1 {chan1 chan2 chan3}]
        unsubscribelocal $rd1
        assert_equal 0 [r publishlocal chan1 hello]
        assert_equal 0 [r publishlocal chan2 hello]
        assert_equal 0 [r publishlocal chan3 hello]

        # clean up clients
        $rd1 close
    }

    test "SUBSCRIBE to one channel more than once" {
        set rd1 [redis_deferring_client]
        assert_equal {1 1 1} [subscribelocal $rd1 {chan1 chan1 chan1}]
        assert_equal 1 [r publishlocal chan1 hello]
        assert_equal {message chan1 hello} [$rd1 read]

        # clean up clients
        $rd1 close
    }

    test "UNSUBSCRIBE from non-subscribed channels" {
        set rd1 [redis_deferring_client]
        assert_equal {0 0 0} [unsubscribelocal $rd1 {foo bar quux}]

        # clean up clients
        $rd1 close
    }

    test "PUBSUB command basics" {
        r pubsub local numsub abc def
    } {abc 0 def 0}

    test "PUBLISHLOCAL/SUBSCRIBELOCAL with two clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [subscribelocal $rd1 {chan1}]
        assert_equal {1} [subscribelocal $rd2 {chan1}]
        assert_equal 2 [r publishlocal chan1 hello]
        assert_equal "chan1 2" [r pubsub local numsub chan1]
        assert_equal "chan1" [r pubsub local channels]

        # clean up clients
        $rd1 close
        $rd2 close
    }

    test "PUBLISHLOCAL/SUBSCRIBELOCAL with PUBLISH/SUBSCRIBE" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [subscribelocal $rd1 {chan1}]
        assert_equal {1} [subscribe $rd2 {chan1}]
        assert_equal 1 [r publishlocal chan1 hello]
        assert_equal 1 [r publish chan1 hello]
        assert_equal "chan1 1" [r pubsub local numsub chan1]
        assert_equal "chan1 1" [r pubsub numsub chan1]
        assert_equal "chan1" [r pubsub local channels]
        assert_equal "chan1" [r pubsub channels]
    }
}

start_server {tags {"pubsublocal"}} {
start_server {} {
    set node_0 [srv 0 client]
    set node_0_host [srv 0 host]
    set node_0_port [srv 0 port]
    set node_0_pid [srv 0 pid]

    set node_1 [srv -1 client]
    set node_1_host [srv -1 host]
    set node_1_port [srv -1 port]
    set node_1_pid [srv -1 pid]

    test {setup replication for following tests} {
        $node_1 replicaof $node_0_host $node_0_port
        wait_for_sync $node_1
    }

    test {publish message to replica and receive on master} {
        set rd0 [redis_deferring_client node_0_host node_0_port]
        set rd1 [redis_deferring_client node_1_host node_1_port]

        assert_equal {1} [subscribelocal $rd0 {chan1}]
        $rd1 PUBLISHLOCAL chan1 hello
        assert_equal {message chan1 hello} [$rd0 read]
        $rd1 PUBLISHLOCAL chan1 world
        assert_equal {message chan1 world} [$rd0 read]
    }

    test {setup reverse replication for following tests} {
        $node_1 replicaof no one
        $node_0 replicaof $node_1_host $node_1_port
        wait_for_sync $node_0
    }

    test {publish message to master and receive on replica} {
        set rd0 [redis_deferring_client node_0_host node_0_port]
        set rd1 [redis_deferring_client node_1_host node_1_port]

        assert_equal {1} [subscribelocal $rd1 {chan1}]
        $rd0 PUBLISHLOCAL chan1 hello
        assert_equal {message chan1 hello} [$rd1 read]
        $rd0 PUBLISHLOCAL chan1 world
        assert_equal {message chan1 world} [$rd1 read]
    }
}
}