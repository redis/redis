start_server {tags {"pubsub"}} {
    test "PUBLISH when no one is listening" {
        assert_equal 0 [r publish chan hello]
    }

    test "SUBSCRIBE basics" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        # subscribe first client to two channels
        $rd1 subscribe chan1 chan2
        assert_equal {subscribe chan1 1} [$rd1 read]
        assert_equal {subscribe chan2 2} [$rd1 read]

        # publish on both channels
        assert_equal 1 [r publish chan1 hello]
        assert_equal 1 [r publish chan2 world]
        assert_equal {message chan1 hello} [$rd1 read]
        assert_equal {message chan2 world} [$rd1 read]

        # subscribe second client to one channel
        $rd2 subscribe chan1
        assert_equal {subscribe chan1 1} [$rd2 read]

        # publish on channel with two subscribers
        assert_equal 2 [r publish chan1 hello]
        assert_equal {message chan1 hello} [$rd1 read]
        assert_equal {message chan1 hello} [$rd2 read]

        # unsubscribe first client from all channels
        $rd1 unsubscribe
        set msg [$rd1 read]
        assert_equal "unsubscribe" [lindex $msg 0]
        assert_match "chan*" [lindex $msg 1]
        assert_match 1 [lindex $msg 2]
        set msg [$rd1 read]
        assert_equal "unsubscribe" [lindex $msg 0]
        assert_match "chan*" [lindex $msg 1]
        assert_match 0 [lindex $msg 2]

        # publish on channel with only remaining subscriber
        assert_equal 1 [r publish chan1 hello]
        assert_equal {message chan1 hello} [$rd2 read]
    }
}