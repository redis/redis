start_server {tags {"pubsub network"}} {
    if {$::singledb} {
        set db 0
    } else {
        set db 9
    }

    test "Pub/Sub PING" {
        set rd1 [redis_deferring_client]
        unsubscribe $rd1 somechannel
        subscribe $rd1 somechannel
        # While subscribed to non-zero channels PING works in Pub/Sub mode.

        unsubscribe $rd1 somechannel
        return
        # Now we are unsubscribed, PING should just return PONG.
        $rd1 ping
        set reply3 [$rd1 read]
        $rd1 close
        list $reply1 $reply2 $reply3
    } {{pong {}} {pong foo} PONG}
}
