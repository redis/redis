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
        wait_for_condition 100 10 {
            [regexp {cmd=unsubscribe} [r client list]] eq 1
        } else {
            fail "unsubscribe did not arrive"
        }
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
        wait_for_condition 100 10 {
            [regexp {cmd=punsubscribe} [r client list]] eq 1
        } else {
            fail "punsubscribe did not arrive"
        }
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
    } {0} {resp3}

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

    foreach {type max_lp_entries} {listpackex 512 hashtable 0} {
    test "Keyspace notifications: hash events test ($type)" {
        r config set hash-max-listpack-entries $max_lp_entries
        r config set notify-keyspace-events Khg
        r del myhash
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r hmset myhash yes 1 no 0 f1 1 f2 2 f3_hdel 3
        r hincrby myhash yes 10
        r hexpire myhash 999999 FIELDS 1 yes
        r hexpireat myhash [expr {[clock seconds] + 999999}] NX FIELDS 1 no
        r hpexpire myhash 999999 FIELDS 1 yes
        r hpersist myhash FIELDS 1 yes
        r hpexpire myhash 0 FIELDS 1 yes
        assert_encoding $type myhash
        assert_equal "pmessage * __keyspace@${db}__:myhash hset" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hincrby" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hpersist" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hdel" [$rd1 read]

        # Test that we will get `hexpired` notification when
        # a hash field is removed by active expire.
        r hpexpire myhash 10 FIELDS 1 no
        after 100 ;# Wait for active expire
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpired" [$rd1 read]

        # Test that when a field with TTL is deleted by commands like hdel without
        # updating the global DS, active expire will not send a notification.
        r hpexpire myhash 100 FIELDS 1 f3_hdel
        r hdel myhash f3_hdel
        after 200 ;# Wait for active expire
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hdel" [$rd1 read]

        # Test that we will get `hexpired` notification when
        # a hash field is removed by lazy expire.
        r debug set-active-expire 0
        r hpexpire myhash 10 FIELDS 2 f1 f2
        after 20
        r hmget myhash f1 f2 ;# Trigger lazy expire
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]
        # We should get only one `hexpired` notification even two fields was expired.
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpired" [$rd1 read]
        # We should get a `del` notification after all fields were expired.
        assert_equal "pmessage * __keyspace@${db}__:myhash del" [$rd1 read]
        r debug set-active-expire 1


        # Test HSETEX, HGETEX and HGETDEL notifications
        r hsetex myhash FIELDS 3 f4 v4 f5 v5 f6 v6
        assert_equal "pmessage * __keyspace@${db}__:myhash hset" [$rd1 read]

        # hgetex sets ttl in past
        r hgetex myhash PX 0 FIELDS 1 f4
        assert_equal "pmessage * __keyspace@${db}__:myhash hdel" [$rd1 read]

        # hgetex sets ttl
        r hgetex myhash EXAT [expr {[clock seconds] + 999999}] FIELDS 1 f5
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]

        # hgetex persists field
        r hgetex myhash PERSIST FIELDS 1 f5
        assert_equal "pmessage * __keyspace@${db}__:myhash hpersist" [$rd1 read]

        # hgetdel deletes a field
        r hgetdel myhash FIELDS 1 f5
        assert_equal "pmessage * __keyspace@${db}__:myhash hdel" [$rd1 read]

        # hsetex sets field and expiry time
        r hsetex myhash EXAT [expr {[clock seconds] + 999999}] FIELDS 1 f6 v6
        assert_equal "pmessage * __keyspace@${db}__:myhash hset" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]

        # hsetex sets field and ttl in the past
        r hsetex myhash PX 0 FIELDS 1 f6 v6
        assert_equal "pmessage * __keyspace@${db}__:myhash hset" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hdel" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash del" [$rd1 read]

        # Test that we will get `hexpired` notification when a hash field is
        # removed by lazy expire using hgetdel command
        r debug set-active-expire 0
        r hsetex myhash PX 10 FIELDS 1 f1 v1
        assert_equal "pmessage * __keyspace@${db}__:myhash hset" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpire" [$rd1 read]

        # Set another field
        r hsetex myhash FIELDS 1 f2 v2
        assert_equal "pmessage * __keyspace@${db}__:myhash hset" [$rd1 read]
        # Wait until field expires
        after 20
        r hgetdel myhash FIELDS 1 f1
        assert_equal "pmessage * __keyspace@${db}__:myhash hexpired" [$rd1 read]
        # Get and delete the only field
        r hgetdel myhash FIELDS 1 f2
        assert_equal "pmessage * __keyspace@${db}__:myhash hdel" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myhash del" [$rd1 read]
        r debug set-active-expire 1

        $rd1 close
    } {0} {needs:debug}
    } ;# foreach

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

    test "publish to self inside multi" {
        r hello 3
        r subscribe foo
        r multi
        r ping abc
        r publish foo bar
        r publish foo vaz
        r ping def
        assert_equal [r exec] {abc 1 1 def}
        assert_equal [r read] {message foo bar}
        assert_equal [r read] {message foo vaz}
    } {} {resp3}

    test "publish to self inside script" {
        r hello 3
        r subscribe foo
        set res [r eval {
                redis.call("ping","abc")
                redis.call("publish","foo","bar")
                redis.call("publish","foo","vaz")
                redis.call("ping","def")
                return "bla"} 0]
        assert_equal $res {bla}
        assert_equal [r read] {message foo bar}
        assert_equal [r read] {message foo vaz}
    } {} {resp3}

    test "unsubscribe inside multi, and publish to self" {
        r hello 3

        # Note: SUBSCRIBE and UNSUBSCRIBE with multiple channels in the same command,
        # breaks the multi response, see https://github.com/redis/redis/issues/12207
        # this is just a temporary sanity test to detect unintended breakage.

        # subscribe for 3 channels actually emits 3 "responses"
        assert_equal "subscribe foo 1" [r subscribe foo bar baz]
        assert_equal "subscribe bar 2" [r read]
        assert_equal "subscribe baz 3" [r read]

        r multi
        r ping abc
        r unsubscribe bar
        r unsubscribe baz
        r ping def
        assert_equal [r exec] {abc {unsubscribe bar 2} {unsubscribe baz 1} def}

        # published message comes after the publish command's response.
        assert_equal [r publish foo vaz] {1}
        assert_equal [r read] {message foo vaz}
    } {} {resp3}

}
