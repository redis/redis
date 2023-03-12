start_server {tags {"pubsub network"}} {
    if {$::singledb} {
        set db 0
    } else {
        set db 9
    }

    foreach resp {2 3} {
        set rd1 [redis_deferring_client]
        if {[lsearch $::denytags "resp3"] >= 0} {
            if {$resp == 3} {continue}
        } elseif {$::force_resp3} {
            if {$resp == 2} {continue}
        }

        $rd1 hello $resp
        $rd1 read

        test "Pub/Sub PING on RESP$resp" {
            subscribe $rd1 somechannel
            # While subscribed to non-zero channels PING works in Pub/Sub mode.
            $rd1 ping
            $rd1 ping "foo"
            # In RESP3, the SUBSCRIBEd client can issue any command and get a reply, so the PINGs are standard
            # In RESP2, only a handful of commands are allowed after a client is SUBSCRIBED (PING is one of them).
            # For some reason, the reply in that case is an array with two elements: "pong"  and argv[1] or an empty string
            # God knows why. Done in commit 2264b981
            if {$resp == 3} {
                assert_equal {PONG} [$rd1 read]
                assert_equal {foo} [$rd1 read]
            } else {
                assert_equal {pong {}} [$rd1 read]
                assert_equal {pong foo} [$rd1 read]
            }
            unsubscribe $rd1 somechannel
            # Now we are unsubscribed, PING should just return PONG.
            $rd1 ping
            assert_equal {PONG} [$rd1 read]

        }
        $rd1 close
    }

    test "PUBLISH/SUBSCRIBE basics" {
        set rd1 [redis_deferring_client]

        # subscribe to two channels
        assert_equal {1 2} [subscribe $rd1 {chan1 chan2}]
        assert_equal 1 [r publish chan1 hello]
        assert_equal 1 [r publish chan2 world]
        assert_equal {message chan1 hello} [$rd1 read]
        assert_equal {message chan2 world} [$rd1 read]

        # unsubscribe from one of the channels
        unsubscribe $rd1 {chan1}
        assert_equal 0 [r publish chan1 hello]
        assert_equal 1 [r publish chan2 world]
        assert_equal {message chan2 world} [$rd1 read]

        # unsubscribe from the remaining channel
        unsubscribe $rd1 {chan2}
        assert_equal 0 [r publish chan1 hello]
        assert_equal 0 [r publish chan2 world]

        # clean up clients
        $rd1 close
    }

    test "PUBLISH/SUBSCRIBE with two clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [subscribe $rd1 {chan1}]
        assert_equal {1} [subscribe $rd2 {chan1}]
        assert_equal 2 [r publish chan1 hello]
        assert_equal {message chan1 hello} [$rd1 read]
        assert_equal {message chan1 hello} [$rd2 read]

        # clean up clients
        $rd1 close
        $rd2 close
    }

    test "PUBLISH/SUBSCRIBE after UNSUBSCRIBE without arguments" {
        set rd1 [redis_deferring_client]
        assert_equal {1 2 3} [subscribe $rd1 {chan1 chan2 chan3}]
        unsubscribe $rd1
        assert_equal 0 [r publish chan1 hello]
        assert_equal 0 [r publish chan2 hello]
        assert_equal 0 [r publish chan3 hello]

        # clean up clients
        $rd1 close
    }

    test "SUBSCRIBE to one channel more than once" {
        set rd1 [redis_deferring_client]
        assert_equal {1 1 1} [subscribe $rd1 {chan1 chan1 chan1}]
        assert_equal 1 [r publish chan1 hello]
        assert_equal {message chan1 hello} [$rd1 read]

        # clean up clients
        $rd1 close
    }

    test "UNSUBSCRIBE from non-subscribed channels" {
        set rd1 [redis_deferring_client]
        assert_equal {0 0 0} [unsubscribe $rd1 {foo bar quux}]

        # clean up clients
        $rd1 close
    }

    test "PUBLISH/PSUBSCRIBE basics" {
        set rd1 [redis_deferring_client]

        # subscribe to two patterns
        assert_equal {1 2} [psubscribe $rd1 {foo.* bar.*}]
        assert_equal 1 [r publish foo.1 hello]
        assert_equal 1 [r publish bar.1 hello]
        assert_equal 0 [r publish foo1 hello]
        assert_equal 0 [r publish barfoo.1 hello]
        assert_equal 0 [r publish qux.1 hello]
        assert_equal {pmessage foo.* foo.1 hello} [$rd1 read]
        assert_equal {pmessage bar.* bar.1 hello} [$rd1 read]

        # unsubscribe from one of the patterns
        assert_equal {1} [punsubscribe $rd1 {foo.*}]
        assert_equal 0 [r publish foo.1 hello]
        assert_equal 1 [r publish bar.1 hello]
        assert_equal {pmessage bar.* bar.1 hello} [$rd1 read]

        # unsubscribe from the remaining pattern
        assert_equal {0} [punsubscribe $rd1 {bar.*}]
        assert_equal 0 [r publish foo.1 hello]
        assert_equal 0 [r publish bar.1 hello]

        # clean up clients
        $rd1 close
    }

    test "PUBLISH/PSUBSCRIBE with two clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [psubscribe $rd1 {chan.*}]
        assert_equal {1} [psubscribe $rd2 {chan.*}]
        assert_equal 2 [r publish chan.foo hello]
        assert_equal {pmessage chan.* chan.foo hello} [$rd1 read]
        assert_equal {pmessage chan.* chan.foo hello} [$rd2 read]

        # clean up clients
        $rd1 close
        $rd2 close
    }

    test "PUBLISH/PSUBSCRIBE after PUNSUBSCRIBE without arguments" {
        set rd1 [redis_deferring_client]
        assert_equal {1 2 3} [psubscribe $rd1 {chan1.* chan2.* chan3.*}]
        punsubscribe $rd1
        assert_equal 0 [r publish chan1.hi hello]
        assert_equal 0 [r publish chan2.hi hello]
        assert_equal 0 [r publish chan3.hi hello]

        # clean up clients
        $rd1 close
    }

    test "PubSub messages with CLIENT REPLY OFF" {
        set rd [redis_deferring_client]
        $rd hello 3
        $rd read ;# Discard the hello reply

        # Test that the subscribe/psubscribe notification is ok
        $rd client reply off
        assert_equal {1} [subscribe $rd channel]
        assert_equal {2} [psubscribe $rd ch*]

        # Test that the publish notification is ok
        $rd client reply off
        assert_equal 2 [r publish channel hello]
        assert_equal {message channel hello} [$rd read]
        assert_equal {pmessage ch* channel hello} [$rd read]

        # Test that the unsubscribe/punsubscribe notification is ok
        $rd client reply off
        assert_equal {1} [unsubscribe $rd channel]
        assert_equal {0} [punsubscribe $rd ch*]

        $rd close
    }

    test "PUNSUBSCRIBE from non-subscribed channels" {
        set rd1 [redis_deferring_client]
        assert_equal {0 0 0} [punsubscribe $rd1 {foo.* bar.* quux.*}]

        # clean up clients
        $rd1 close
    }

    test "NUMSUB returns numbers, not strings (#1561)" {
        r pubsub numsub abc def
    } {abc 0 def 0}

    test "NUMPATs returns the number of unique patterns" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        # Three unique patterns and one that overlaps
        psubscribe $rd1 "foo*"
        psubscribe $rd2 "foo*"
        psubscribe $rd1 "bar*"
        psubscribe $rd2 "baz*"

        set patterns [r pubsub numpat]

        # clean up clients
        punsubscribe $rd1
        punsubscribe $rd2
        assert_equal 3 $patterns
        $rd1 close
        $rd2 close
    }

    test "Mix SUBSCRIBE and PSUBSCRIBE" {
        set rd1 [redis_deferring_client]
        assert_equal {1} [subscribe $rd1 {foo.bar}]
        assert_equal {2} [psubscribe $rd1 {foo.*}]

        assert_equal 2 [r publish foo.bar hello]
        assert_equal {message foo.bar hello} [$rd1 read]
        assert_equal {pmessage foo.* foo.bar hello} [$rd1 read]

        # clean up clients
        $rd1 close
    }

    test "PUNSUBSCRIBE and UNSUBSCRIBE should always reply" {
        # Make sure we are not subscribed to any channel at all.
        r punsubscribe
        r unsubscribe
        # Now check if the commands still reply correctly.
        set reply1 [r punsubscribe]
        set reply2 [r unsubscribe]
        concat $reply1 $reply2
    } {punsubscribe {} 0 unsubscribe {} 0}

    ### Keyspace events notification tests

    test "Keyspace notifications: we receive keyspace notifications" {
        r config set notify-keyspace-events KA
        set rd1 [redis_deferring_client]
        $rd1 CLIENT REPLY OFF ;# Make sure it works even if replies are silenced
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        assert_equal "pmessage * __keyspace@${db}__:foo set" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: we receive keyevent notifications" {
        r config set notify-keyspace-events EA
        set rd1 [redis_deferring_client]
        $rd1 CLIENT REPLY SKIP ;# Make sure it works even if replies are silenced
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        assert_equal "pmessage * __keyevent@${db}__:set foo" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: we can receive both kind of events" {
        r config set notify-keyspace-events KEA
        set rd1 [redis_deferring_client]
        $rd1 CLIENT REPLY ON ;# Just coverage
        assert_equal {OK} [$rd1 read]
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        assert_equal "pmessage * __keyspace@${db}__:foo set" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:set foo" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: we are able to mask events" {
        r config set notify-keyspace-events KEl
        r del mylist
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        r lpush mylist a
        # No notification for set, because only list commands are enabled.
        assert_equal "pmessage * __keyspace@${db}__:mylist lpush" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:lpush mylist" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: general events test" {
        r config set notify-keyspace-events KEg
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        r expire foo 1
        r del foo
        assert_equal "pmessage * __keyspace@${db}__:foo expire" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:expire foo" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:foo del" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:del foo" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: list events test" {
        r config set notify-keyspace-events KEl
        r del mylist
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r lpush mylist a
        r rpush mylist a
        r rpop mylist
        assert_equal "pmessage * __keyspace@${db}__:mylist lpush" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:lpush mylist" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:mylist rpush" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:rpush mylist" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:mylist rpop" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:rpop mylist" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: set events test" {
        r config set notify-keyspace-events Ks
        r del myset
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r sadd myset a b c d
        r srem myset x
        r sadd myset x y z
        r srem myset x
        assert_equal "pmessage * __keyspace@${db}__:myset sadd" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset sadd" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset srem" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: zset events test" {
        r config set notify-keyspace-events Kz
        r del myzset
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r zadd myzset 1 a 2 b
        r zrem myzset x
        r zadd myzset 3 x 4 y 5 z
        r zrem myzset x
        assert_equal "pmessage * __keyspace@${db}__:myzset zadd" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myzset zadd" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myzset zrem" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: hash events test" {
        r config set notify-keyspace-events Kh
        r del myhash
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r hmset myhash yes 1 no 0
        r hincrby myhash yes 10
        assert_equal "pmessage * __keyspace@${db}__:myhash hset" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hincrby" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: stream events test" {
        r config set notify-keyspace-events Kt
        r del mystream
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r xgroup create mystream mygroup $ mkstream
        r xgroup createconsumer mystream mygroup Bob
        set id [r xadd mystream 1 field1 A]
        r xreadgroup group mygroup Alice STREAMS mystream >
        r xclaim mystream mygroup Mike 0 $id force
        # Not notify because of "Lee" not exists.
        r xgroup delconsumer mystream mygroup Lee
        # Not notify because of "Bob" exists.
        r xautoclaim mystream mygroup Bob 0 $id
        r xgroup delconsumer mystream mygroup Bob
        assert_equal "pmessage * __keyspace@${db}__:mystream xgroup-create" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:mystream xgroup-createconsumer" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:mystream xadd" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:mystream xgroup-createconsumer" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:mystream xgroup-createconsumer" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:mystream xgroup-delconsumer" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: expired events (triggered expire)" {
        r config set notify-keyspace-events Ex
        r del foo
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r psetex foo 100 1
        wait_for_condition 50 100 {
            [r exists foo] == 0
        } else {
            fail "Key does not expire?!"
        }
        assert_equal "pmessage * __keyevent@${db}__:expired foo" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: expired events (background expire)" {
        r config set notify-keyspace-events Ex
        r del foo
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r psetex foo 100 1
        assert_equal "pmessage * __keyevent@${db}__:expired foo" [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: evicted events" {
        r config set notify-keyspace-events Ee
        r config set maxmemory-policy allkeys-lru
        r flushdb
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        r config set maxmemory 1
        assert_equal "pmessage * __keyevent@${db}__:evicted foo" [$rd1 read]
        r config set maxmemory 0
        $rd1 close
        r config set maxmemory-policy noeviction
    } {OK} {needs:config-maxmemory}

    test "Keyspace notifications: test CONFIG GET/SET of event flags" {
        r config set notify-keyspace-events gKE
        assert_equal {gKE} [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events {$lshzxeKE}
        assert_equal {$lshzxeKE} [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events KA
        assert_equal {AK} [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events EA
        assert_equal {AE} [lindex [r config get notify-keyspace-events] 1]
    }

    test "Keyspace notifications: new key test" {
        r config set notify-keyspace-events En
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        # second set of foo should not cause a 'new' event
        r set foo baz 
        r set bar bar
        assert_equal "pmessage * __keyevent@${db}__:new foo" [$rd1 read]
        assert_equal "pmessage * __keyevent@${db}__:new bar" [$rd1 read]
        $rd1 close
    }
}
