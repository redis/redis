start_server {tags {"pubsub network"}} {
    test "Pub/Sub PING" {
        set rd1 [redis_deferring_client]
        subscribe $rd1 somechannel
        # While subscribed to non-zero channels PING works in Pub/Sub mode.
        $rd1 ping
        $rd1 ping "foo"
        set reply1 [$rd1 read]
        set reply2 [$rd1 read]
        unsubscribe $rd1 somechannel
        # Now we are unsubscribed, PING should just return PONG.
        $rd1 ping
        set reply3 [$rd1 read]
        $rd1 close
        list $reply1 $reply2 $reply3
    } {{pong {}} {pong foo} PONG}

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
        assert_equal {pmessage * __keyspace@9__:foo set} [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: we receive keyevent notifications" {
        r config set notify-keyspace-events EA
        set rd1 [redis_deferring_client]
        $rd1 CLIENT REPLY SKIP ;# Make sure it works even if replies are silenced
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        assert_equal {pmessage * __keyevent@9__:set foo} [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: we can receive both kind of events" {
        r config set notify-keyspace-events KEA
        set rd1 [redis_deferring_client]
        $rd1 CLIENT REPLY ON ;# Just coverage
        assert_equal {OK} [$rd1 read]
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        assert_equal {pmessage * __keyspace@9__:foo set} [$rd1 read]
        assert_equal {pmessage * __keyevent@9__:set foo} [$rd1 read]
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
        assert_equal {pmessage * __keyspace@9__:mylist lpush} [$rd1 read]
        assert_equal {pmessage * __keyevent@9__:lpush mylist} [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: general events test" {
        r config set notify-keyspace-events KEg
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r set foo bar
        r expire foo 1
        r del foo
        assert_equal {pmessage * __keyspace@9__:foo expire} [$rd1 read]
        assert_equal {pmessage * __keyevent@9__:expire foo} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:foo del} [$rd1 read]
        assert_equal {pmessage * __keyevent@9__:del foo} [$rd1 read]
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
        assert_equal {pmessage * __keyspace@9__:mylist lpush} [$rd1 read]
        assert_equal {pmessage * __keyevent@9__:lpush mylist} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:mylist rpush} [$rd1 read]
        assert_equal {pmessage * __keyevent@9__:rpush mylist} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:mylist rpop} [$rd1 read]
        assert_equal {pmessage * __keyevent@9__:rpop mylist} [$rd1 read]
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
        assert_equal {pmessage * __keyspace@9__:myset sadd} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:myset sadd} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:myset srem} [$rd1 read]
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
        assert_equal {pmessage * __keyspace@9__:myzset zadd} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:myzset zadd} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:myzset zrem} [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: hash events test" {
        r config set notify-keyspace-events Kh
        r del myhash
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r hmset myhash yes 1 no 0
        r hincrby myhash yes 10
        assert_equal {pmessage * __keyspace@9__:myhash hset} [$rd1 read]
        assert_equal {pmessage * __keyspace@9__:myhash hincrby} [$rd1 read]
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
        assert_equal {pmessage * __keyevent@9__:expired foo} [$rd1 read]
        $rd1 close
    }

    test "Keyspace notifications: expired events (background expire)" {
        r config set notify-keyspace-events Ex
        r del foo
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r psetex foo 100 1
        assert_equal {pmessage * __keyevent@9__:expired foo} [$rd1 read]
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
        assert_equal {pmessage * __keyevent@9__:evicted foo} [$rd1 read]
        r config set maxmemory 0
        $rd1 close
    }

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
}

start_server {tags {"pubsub network"}} {
    # Helper proc for tests that subscribe multiple times until hitting OOM
    proc test_subscribe_oom_loop {cmd description clients} {
        test "$cmd $description fails with OOM when memory limit exceeded" {
            # Set 10MB memory limit
            r config set maxmemory 10485760
            r config set maxmemory-policy noeviction
            
            # Create clients
            if {$clients == 1} {
                set rd [redis_deferring_client]
            } else {
                set rd1 [redis_deferring_client]
                set rd2 [redis_deferring_client]
            }
            
            set base_str [string repeat "a" 2048]
            set success_count 0
            set oom_occurred 0
            
            # Try to subscribe until we hit OOM
            for {set i 0} {$i < 5000} {incr i} {
                # Select client
                if {$clients == 1} {
                    set client $rd
                } else {
                    set client [expr {$i % 2 ? $rd1 : $rd2}]
                }
                
                # Build channel/pattern name
                if {$cmd eq "psubscribe"} {
                    set channel_name "${base_str}${i}*"
                } else {
                    set channel_name "${base_str}${i}"
                }
                
                $client $cmd $channel_name
                if {[catch {$client read} err]} {
                    if {[string match "*OOM command not allowed*" $err]} {
                        set oom_occurred 1
                        break
                    }
                    error "Unexpected error: $err"
                }
                incr success_count
            }
            
            # Verify we had at least one success and hit OOM
            assert {$success_count > 10}
            assert {$oom_occurred == 1}
            
            # Close clients
            if {$clients == 1} {
                $rd close
            } else {
                $rd1 close
                $rd2 close
            }
        }
    }

    # Helper proc for tests with single large channel that immediately fails
    proc test_subscribe_large_channel_oom {cmd channel_type} {
        test "$cmd with large $channel_type name fails due to OOM" {
            # Set maxmemory to 2MB
            r config set maxmemory 2097152
            r config set maxmemory-policy noeviction
            
            # Create large channel/pattern name: 2MB
            set channel_name [string repeat "a" 2097152]
            
            # Create a single pubsub client
            set rd [redis_deferring_client]
            
            # Subscribe should fail with OOM error
            $rd $cmd $channel_name
            assert_error "*OOM command not allowed when used memory > 'maxmemory'*" {$rd read}
            
            # Cleanup
            $rd close
        }
    }

    # Helper proc for tests with small success then large failure
    proc test_subscribe_small_then_large_oom {cmd channel_type} {
        test "$cmd succeeds with small $channel_type but fails with large $channel_type due to OOM" {
            # Set maxmemory to 10MB
            r config set maxmemory 10485760
            r config set maxmemory-policy noeviction
            
            # Create channel names: first 10KB, second 5MB
            set channel1 [string repeat "a" 10240]
            set channel2 [string repeat "b" 10485760]
            
            # Create a single pubsub client
            set rd [redis_deferring_client]
            
            # First subscribe should succeed (10KB)
            $rd $cmd $channel1
            set reply1 [$rd read]
            assert_equal [list $cmd] [lindex $reply1 0]
            
            # Second subscribe should fail with OOM error (5MB exceeds limit)
            $rd $cmd $channel2
            assert_error "*OOM command not allowed when used memory > 'maxmemory'*" {$rd read}
            
            # Cleanup
            $rd close
        }
    }

    # Multiple subscriptions until OOM tests
    test_subscribe_oom_loop "subscribe" "" 1
    test_subscribe_oom_loop "psubscribe" "" 1
    test_subscribe_oom_loop "subscribe" "with 2 clients" 2
    test_subscribe_oom_loop "psubscribe" "with 2 clients" 2

    # Single large channel immediate OOM tests
    test_subscribe_large_channel_oom "subscribe" "channel"
    test_subscribe_large_channel_oom "psubscribe" "pattern"

    # Small success then large failure tests
    test_subscribe_small_then_large_oom "subscribe" "channel"
    test_subscribe_small_then_large_oom "psubscribe" "pattern"
}
