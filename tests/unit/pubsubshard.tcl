start_server {tags {"pubsubshard external:skip"}} {
    test "SPUBLISH/SSUBSCRIBE basics" {
        set rd1 [redis_deferring_client]

        # subscribe to two channels
        assert_equal {1} [ssubscribe $rd1 {chan1}]
        assert_equal {2} [ssubscribe $rd1 {chan2}]
        assert_equal 1 [r SPUBLISH chan1 hello]
        assert_equal 1 [r SPUBLISH chan2 world]
        assert_equal {smessage chan1 hello} [$rd1 read]
        assert_equal {smessage chan2 world} [$rd1 read]

        # unsubscribe from one of the channels
        sunsubscribe $rd1 {chan1}
        assert_equal 0 [r SPUBLISH chan1 hello]
        assert_equal 1 [r SPUBLISH chan2 world]
        assert_equal {smessage chan2 world} [$rd1 read]

        # unsubscribe from the remaining channel
        sunsubscribe $rd1 {chan2}
        assert_equal 0 [r SPUBLISH chan1 hello]
        assert_equal 0 [r SPUBLISH chan2 world]

        # clean up clients
        $rd1 close
    }

    test "SPUBLISH/SSUBSCRIBE with two clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [ssubscribe $rd1 {chan1}]
        assert_equal {1} [ssubscribe $rd2 {chan1}]
        assert_equal 2 [r SPUBLISH chan1 hello]
        assert_equal {smessage chan1 hello} [$rd1 read]
        assert_equal {smessage chan1 hello} [$rd2 read]

        # clean up clients
        $rd1 close
        $rd2 close
    }

    test "SPUBLISH/SSUBSCRIBE after UNSUBSCRIBE without arguments" {
        set rd1 [redis_deferring_client]
        assert_equal {1} [ssubscribe $rd1 {chan1}]
        assert_equal {2} [ssubscribe $rd1 {chan2}]
        assert_equal {3} [ssubscribe $rd1 {chan3}]
        sunsubscribe $rd1
        assert_equal 0 [r SPUBLISH chan1 hello]
        assert_equal 0 [r SPUBLISH chan2 hello]
        assert_equal 0 [r SPUBLISH chan3 hello]

        # clean up clients
        $rd1 close
    }

    test "SSUBSCRIBE to one channel more than once" {
        set rd1 [redis_deferring_client]
        assert_equal {1 1 1} [ssubscribe $rd1 {chan1 chan1 chan1}]
        assert_equal 1 [r SPUBLISH chan1 hello]
        assert_equal {smessage chan1 hello} [$rd1 read]

        # clean up clients
        $rd1 close
    }

    test "SUNSUBSCRIBE from non-subscribed channels" {
        set rd1 [redis_deferring_client]
        assert_equal {0} [sunsubscribe $rd1 {foo}]
        assert_equal {0} [sunsubscribe $rd1 {bar}]
        assert_equal {0} [sunsubscribe $rd1 {quux}]

        # clean up clients
        $rd1 close
    }

    test "PUBSUB command basics" {
        r pubsub shardnumsub abc def
    } {abc 0 def 0}

    test "SPUBLISH/SSUBSCRIBE with two clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [ssubscribe $rd1 {chan1}]
        assert_equal {1} [ssubscribe $rd2 {chan1}]
        assert_equal 2 [r SPUBLISH chan1 hello]
        assert_equal "chan1 2" [r pubsub shardnumsub chan1]
        assert_equal "chan1" [r pubsub shardchannels]

        # clean up clients
        $rd1 close
        $rd2 close
    }

    test "SPUBLISH/SSUBSCRIBE with PUBLISH/SUBSCRIBE" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [ssubscribe $rd1 {chan1}]
        assert_equal {1} [subscribe $rd2 {chan1}]
        assert_equal 1 [r SPUBLISH chan1 hello]
        assert_equal 1 [r publish chan1 hello]
        assert_equal "chan1 1" [r pubsub shardnumsub chan1]
        assert_equal "chan1 1" [r pubsub numsub chan1]
        assert_equal "chan1" [r pubsub shardchannels]
        assert_equal "chan1" [r pubsub channels]

        $rd1 close
        $rd2 close
    }

    test "PubSubShard with CLIENT REPLY OFF" {
        set rd [redis_deferring_client]
        $rd hello 3
        $rd read ;# Discard the hello reply

        # Test that the ssubscribe notification is ok
        $rd client reply off
        $rd ping
        assert_equal {1} [ssubscribe $rd channel]

        # Test that the spublish notification is ok
        $rd client reply off
        $rd ping
        assert_equal 1 [r spublish channel hello]
        assert_equal {smessage channel hello} [$rd read]

        # Test that sunsubscribe notification is ok
        $rd client reply off
        $rd ping
        assert_equal {0} [sunsubscribe $rd channel]

        $rd close
    }
}

start_server {tags {"pubsubshard external:skip"}} {
start_server {tags {"pubsubshard external:skip"}} {
    set node_0 [srv 0 client]
    set node_0_host [srv 0 host]
    set node_0_port [srv 0 port]

    set node_1 [srv -1 client]
    set node_1_host [srv -1 host]
    set node_1_port [srv -1 port]

    test {setup replication for following tests} {
        $node_1 replicaof $node_0_host $node_0_port
        wait_for_sync $node_1
    }

    test {publish message to master and receive on replica} {
        set rd0 [redis_deferring_client node_0_host node_0_port]
        set rd1 [redis_deferring_client node_1_host node_1_port]

        assert_equal {1} [ssubscribe $rd1 {chan1}]
        $rd0 SPUBLISH chan1 hello
        assert_equal {smessage chan1 hello} [$rd1 read]
        $rd0 SPUBLISH chan1 world
        assert_equal {smessage chan1 world} [$rd1 read]
    }
}
}