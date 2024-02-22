set testmodule [file normalize tests/modules/blockedclient.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Locked GIL acquisition} {
        assert_match "OK" [r acquire_gil]
    }

    test {Locked GIL acquisition during multi} {
    	r multi
    	r acquire_gil
    	assert_equal {{Blocked client is not supported inside multi}} [r exec]
    }

    test {Locked GIL acquisition from RM_Call} {
    	assert_equal {Blocked client is not allowed} [r do_rm_call acquire_gil]
    }

    test {Blocking command are not block the client on RM_Call} {
    	r lpush l test
    	assert_equal [r do_rm_call blpop l 0] {l test}
    	
    	r lpush l test
    	assert_equal [r do_rm_call brpop l 0] {l test}
    	
    	r lpush l1 test
    	assert_equal [r do_rm_call brpoplpush l1 l2 0] {test}
    	assert_equal [r do_rm_call brpop l2 0] {l2 test}

    	r lpush l1 test
    	assert_equal [r do_rm_call blmove l1 l2 LEFT LEFT 0] {test}
    	assert_equal [r do_rm_call brpop l2 0] {l2 test}

    	r ZADD zset1 0 a 1 b 2 c
    	assert_equal [r do_rm_call bzpopmin zset1 0] {zset1 a 0}
    	assert_equal [r do_rm_call bzpopmax zset1 0] {zset1 c 2}

    	r xgroup create s g $ MKSTREAM
    	r xadd s * foo bar
    	assert {[r do_rm_call xread BLOCK 0 STREAMS s 0-0] ne {}}
    	assert {[r do_rm_call xreadgroup group g c BLOCK 0 STREAMS s >] ne {}}

    	assert {[r do_rm_call blpop empty_list 0] eq {}}
        assert {[r do_rm_call brpop empty_list 0] eq {}}
        assert {[r do_rm_call brpoplpush empty_list1 empty_list2 0] eq {}}
        assert {[r do_rm_call blmove empty_list1 empty_list2 LEFT LEFT 0] eq {}}
        
        assert {[r do_rm_call bzpopmin empty_zset 0] eq {}}
        assert {[r do_rm_call bzpopmax empty_zset 0] eq {}}
       
        r xgroup create empty_stream g $ MKSTREAM
        assert {[r do_rm_call xread BLOCK 0 STREAMS empty_stream $] eq {}}
        assert {[r do_rm_call xreadgroup group g c BLOCK 0 STREAMS empty_stream >] eq {}}

    }

    test {Monitor disallow inside RM_Call} {
        set e {}
        catch {
            r do_rm_call monitor
        } e
        set e
    } {*ERR*DENY BLOCKING*}

    test {subscribe disallow inside RM_Call} {
        set e {}
        catch {
            r do_rm_call subscribe x
        } e
        set e
    } {*ERR*DENY BLOCKING*}

    test {RM_Call from blocked client} {
        r hset hash foo bar
        r do_bg_rm_call hgetall hash
    } {foo bar}

    test {RM_Call from blocked client with script mode} {
        r do_bg_rm_call_format S hset k foo bar
    } {1}

    test {RM_Call from blocked client with oom mode} {
        r config set maxmemory 1
        # will set server.pre_command_oom_state to 1
        assert_error {OOM command not allowed*} {r hset hash foo bar}
        r config set maxmemory 0
        # now its should be OK to call OOM commands
        r do_bg_rm_call_format M hset k1 foo bar
    } {1} {needs:config-maxmemory}

    test {RESP version carries through to blocked client} {
        for {set client_proto 2} {$client_proto <= 3} {incr client_proto} {
            if {[lsearch $::denytags "resp3"] >= 0} {
                if {$client_proto == 3} {continue}
            } elseif {$::force_resp3} {
                if {$client_proto == 2} {continue}
            }
            r hello $client_proto
            r readraw 1
            set ret [r do_fake_bg_true]
            if {$client_proto == 2} {
                assert_equal $ret {:1}
            } else {
                assert_equal $ret "#t"
            }
            r readraw 0
            r hello 2
        }
    }

foreach call_type {nested normal} {
    test "Busy module command - $call_type" {
        set busy_time_limit 50
        set old_time_limit [lindex [r config get busy-reply-threshold] 1]
        r config set busy-reply-threshold $busy_time_limit
        set rd [redis_deferring_client]

        # run command that blocks until released
        set start [clock clicks -milliseconds]
        if {$call_type == "nested"} {
            $rd do_rm_call slow_fg_command 0
        } else {
            $rd slow_fg_command 0
        }
        $rd flush

        # send another command after the blocked one, to make sure we don't attempt to process it
        $rd ping
        $rd flush

        # make sure we get BUSY error, and that we didn't get it too early
        assert_error {*BUSY Slow module operation*} {r ping}
        assert_morethan_equal [expr [clock clicks -milliseconds]-$start] $busy_time_limit

        # abort the blocking operation
        r stop_slow_fg_command
        wait_for_condition 50 100 {
            [catch {r ping} e] == 0
        } else {
            fail "Failed waiting for busy command to end"
        }
        assert_equal [$rd read] "1"
        assert_equal [$rd read] "PONG"

        # run command that blocks for 200ms
        set start [clock clicks -milliseconds]
        if {$call_type == "nested"} {
            $rd do_rm_call slow_fg_command 200000
        } else {
            $rd slow_fg_command 200000
        }
        $rd flush
        after 10 ;# try to make sure redis started running the command before we proceed

        # make sure we didn't get BUSY error, it simply blocked till the command was done
        r ping
        assert_morethan_equal [expr [clock clicks -milliseconds]-$start] 200
        $rd read

        $rd close
        r config set busy-reply-threshold $old_time_limit
    }
}

    test {RM_Call from blocked client} {
        set busy_time_limit 50
        set old_time_limit [lindex [r config get busy-reply-threshold] 1]
        r config set busy-reply-threshold $busy_time_limit

        # trigger slow operation
        r set_slow_bg_operation 1
        r hset hash foo bar
        set rd [redis_deferring_client]
        set start [clock clicks -milliseconds]
        $rd do_bg_rm_call hgetall hash

        # send another command after the blocked one, to make sure we don't attempt to process it
        $rd ping
        $rd flush

        # wait till we know we're blocked inside the module
        wait_for_condition 50 100 {
            [r is_in_slow_bg_operation] eq 1
        } else {
            fail "Failed waiting for slow operation to start"
        }

        # make sure we get BUSY error, and that we didn't get here too early
        assert_error {*BUSY Slow module operation*} {r ping}
        assert_morethan_equal [expr [clock clicks -milliseconds]-$start] $busy_time_limit
        # abort the blocking operation
        r set_slow_bg_operation 0

        wait_for_condition 50 100 {
            [r is_in_slow_bg_operation] eq 0
        } else {
            fail "Failed waiting for slow operation to stop"
        }
        assert_equal [r ping] {PONG}

        r config set busy-reply-threshold $old_time_limit
        assert_equal [$rd read] {foo bar}
        assert_equal [$rd read] {PONG}
        $rd close
    }

    test {blocked client reaches client output buffer limit} {
        r hset hash big [string repeat x 50000]
        r hset hash bada [string repeat x 50000]
        r hset hash boom [string repeat x 50000]
        r config set client-output-buffer-limit {normal 100000 0 0}
        r client setname myclient
        catch {r do_bg_rm_call hgetall hash} e
        assert_match "*I/O error*" $e
        reconnect
        set clients [r client list]
        assert_no_match "*name=myclient*" $clients
    }

    test {module client error stats} {
        r config resetstat

        # simple module command that replies with string error
        assert_error "ERR unknown command 'hgetalllll', with args beginning with:" {r do_rm_call hgetalllll}
        assert_equal [errorrstat ERR r] {count=1}

        # simple module command that replies with string error
        assert_error "ERR unknown subcommand 'bla'. Try CONFIG HELP." {r do_rm_call config bla}
        assert_equal [errorrstat ERR r] {count=2}

        # module command that replies with string error from bg thread
        assert_error "NULL reply returned" {r do_bg_rm_call hgetalllll}
        assert_equal [errorrstat NULL r] {count=1}

        # module command that returns an arity error
        r do_rm_call set x x
        assert_error "ERR wrong number of arguments for 'do_rm_call' command" {r do_rm_call}
        assert_equal [errorrstat ERR r] {count=3}

        # RM_Call that propagates an error
        assert_error "WRONGTYPE*" {r do_rm_call hgetall x}
        assert_equal [errorrstat WRONGTYPE r] {count=1}
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdrstat hgetall r]

        # RM_Call from bg thread that propagates an error
        assert_error "WRONGTYPE*" {r do_bg_rm_call hgetall x}
        assert_equal [errorrstat WRONGTYPE r] {count=2}
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=2} [cmdrstat hgetall r]

        assert_equal [s total_error_replies] 6
        assert_match {*calls=5,*,rejected_calls=0,failed_calls=4} [cmdrstat do_rm_call r]
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=2} [cmdrstat do_bg_rm_call r]
    }

    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    start_server [list overrides [list loadmodule "$testmodule"]] {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]

        # Start the replication process...
        $replica replicaof $master_host $master_port
        wait_for_sync $replica

        test {WAIT command on module blocked client} {
            pause_process [srv 0 pid]

            $master do_bg_rm_call_format ! hset bk1 foo bar

            assert_equal [$master wait 1 1000] 0
            resume_process [srv 0 pid]
            assert_equal [$master wait 1 1000] 1
            assert_equal [$replica hget bk1 foo] bar
        }
    }

    test {Unblock by timer} {
        # When the client is unlock, we will get the OK reply.
        assert_match "OK" [r unblock_by_timer 100 0]
    }

    test {block time is shorter than timer period} {
        # This command does not have the reply.
        set rd [redis_deferring_client]
        $rd unblock_by_timer 100 10
        # Wait for the client to unlock.
        after 120
        $rd close
    }

    test {block time is equal to timer period} {
        # These time is equal, they will be unlocked in the same event loop,
        # when the client is unlock, we will get the OK reply from timer.
        assert_match "OK" [r unblock_by_timer 100 100]
    }
    
    test "Unload the module - blockedclient" {
        assert_equal {OK} [r module unload blockedclient]
    }
}
