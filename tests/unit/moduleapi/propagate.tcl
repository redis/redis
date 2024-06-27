set testmodule [file normalize tests/modules/propagate.so]
set miscmodule [file normalize tests/modules/misc.so]
set keyspace_events [file normalize tests/modules/keyspace_events.so]

tags "modules" {
    test {Modules can propagate in async and threaded contexts} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            $replica module load $keyspace_events
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set master [srv 0 client]
                set master_host [srv 0 host]
                set master_port [srv 0 port]
                $master module load $keyspace_events

                # Start the replication process...
                $replica replicaof $master_host $master_port
                wait_for_sync $replica
                after 1000

                test {module propagates from timer} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer

                    wait_for_condition 500 10 {
                        [$replica get timer] eq "3"
                    } else {
                        fail "The two counters don't match the expected value."
                    }

                    assert_replication_stream $repl {
                        {select *}
                        {incr timer}
                        {incr timer}
                        {incr timer}
                    }
                    close_replication_stream $repl
                }

                test {module propagation with notifications} {
                    set repl [attach_to_replication_stream]

                    $master set x y

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr notifications}
                        {set x y}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagation with notifications with multi} {
                    set repl [attach_to_replication_stream]

                    $master multi
                    $master set x1 y1
                    $master set x2 y2
                    $master exec

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr notifications}
                        {set x1 y1}
                        {incr notifications}
                        {set x2 y2}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagation with notifications with active-expire} {
                    $master debug set-active-expire 1
                    set repl [attach_to_replication_stream]

                    $master set asdf1 1 PX 300
                    $master set asdf2 2 PX 300
                    $master set asdf3 3 PX 300

                    wait_for_condition 500 10 {
                        [$replica keys asdf*] eq {}
                    } else {
                        fail "Not all keys have expired"
                    }

                    # Note whenever there's double notification: SET with PX issues two separate
                    # notifications: one for "set" and one for "expire"
                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr notifications}
                        {incr notifications}
                        {set asdf1 1 PXAT *}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {set asdf2 2 PXAT *}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {set asdf3 3 PXAT *}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {incr testkeyspace:expired}
                        {del asdf*}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {incr testkeyspace:expired}
                        {del asdf*}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {incr testkeyspace:expired}
                        {del asdf*}
                        {exec}
                    }
                    close_replication_stream $repl

                    $master debug set-active-expire 0
                }

                test {module propagation with notifications with eviction case 1} {
                    $master flushall
                    $master set asdf1 1
                    $master set asdf2 2
                    $master set asdf3 3
          
                    $master config set maxmemory-policy allkeys-random
                    $master config set maxmemory 1

                    # Please note the following loop:
                    # We evict a key and send a notification, which does INCR on the "notifications" key, so
                    # that every time we evict any key, "notifications" key exist (it happens inside the
                    # performEvictions loop). So even evicting "notifications" causes INCR on "notifications".
                    # If maxmemory_eviction_tenacity would have been set to 100 this would be an endless loop, but
                    # since the default is 10, at some point the performEvictions loop would end.
                    # Bottom line: "notifications" always exists and we can't really determine the order of evictions
                    # This test is here only for sanity

                    # The replica will get the notification with multi exec and we have a generic notification handler
                    # that performs `RedisModule_Call(ctx, "INCR", "c", "multi");` if the notification is inside multi exec.
                    # so we will have 2 keys, "notifications" and "multi".
                    wait_for_condition 500 10 {
                        [$replica dbsize] eq 2 
                    } else {
                        fail "Not all keys have been evicted"
                    }

                    $master config set maxmemory 0
                    $master config set maxmemory-policy noeviction
                }

                test {module propagation with notifications with eviction case 2} {
                    $master flushall
                    set repl [attach_to_replication_stream]

                    $master set asdf1 1 EX 300
                    $master set asdf2 2 EX 300
                    $master set asdf3 3 EX 300

                    # Please note we use volatile eviction to prevent the loop described in the test above.
                    # "notifications" is not volatile so it always remains
                    $master config resetstat
                    $master config set maxmemory-policy volatile-ttl
                    $master config set maxmemory 1

                    wait_for_condition 500 10 {
                        [s evicted_keys] eq 3
                    } else {
                        fail "Not all keys have been evicted"
                    }

                    $master config set maxmemory 0
                    $master config set maxmemory-policy noeviction

                    $master set asdf4 4

                    # Note whenever there's double notification: SET with EX issues two separate
                    # notifications: one for "set" and one for "expire"
                    # Note that although CONFIG SET maxmemory is called in this flow (see issue #10014),
                    # eviction will happen and will not induce propagation of the CONFIG command (see #10019).
                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr notifications}
                        {incr notifications}
                        {set asdf1 1 PXAT *}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {set asdf2 2 PXAT *}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {set asdf3 3 PXAT *}
                        {exec}
                        {multi}
                        {incr notifications}
                        {del asdf*}
                        {exec}
                        {multi}
                        {incr notifications}
                        {del asdf*}
                        {exec}
                        {multi}
                        {incr notifications}
                        {del asdf*}
                        {exec}
                        {multi}
                        {incr notifications}
                        {set asdf4 4}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagation with timer and CONFIG SET maxmemory} {
                    set repl [attach_to_replication_stream]

                    $master config resetstat
                    $master config set maxmemory-policy volatile-random

                    $master propagate-test.timer-maxmemory

                    # Wait until the volatile keys are evicted
                    wait_for_condition 500 10 {
                        [s evicted_keys] eq 2
                    } else {
                        fail "Not all keys have been evicted"
                    }

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr notifications}
                        {incr notifications}
                        {set timer-maxmemory-volatile-start 1 PXAT *}
                        {incr timer-maxmemory-middle}
                        {incr notifications}
                        {incr notifications}
                        {set timer-maxmemory-volatile-end 1 PXAT *}
                        {exec}
                        {multi}
                        {incr notifications}
                        {del timer-maxmemory-volatile-*}
                        {exec}
                        {multi}
                        {incr notifications}
                        {del timer-maxmemory-volatile-*}
                        {exec}
                    }
                    close_replication_stream $repl

                    $master config set maxmemory 0
                    $master config set maxmemory-policy noeviction
                }

                test {module propagation with timer and EVAL} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer-eval

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr notifications}
                        {incrby timer-eval-start 1}
                        {incr notifications}
                        {set foo bar}
                        {incr timer-eval-middle}
                        {incr notifications}
                        {incrby timer-eval-end 1}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates nested ctx case1} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer-nested

                    wait_for_condition 500 10 {
                        [$replica get timer-nested-end] eq "1"
                    } else {
                        fail "The two counters don't match the expected value."
                    }

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incrby timer-nested-start 1}
                        {incrby timer-nested-end 1}
                        {exec}
                    }
                    close_replication_stream $repl

                    # Note propagate-test.timer-nested just propagates INCRBY, causing an
                    # inconsistency, so we flush
                    $master flushall
                }

                test {module propagates nested ctx case2} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer-nested-repl

                    wait_for_condition 500 10 {
                        [$replica get timer-nested-end] eq "1"
                    } else {
                        fail "The two counters don't match the expected value."
                    }

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incrby timer-nested-start 1}
                        {incr notifications}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr counter-3}
                        {incr counter-4}
                        {incr notifications}
                        {incr after-call}
                        {incr notifications}
                        {incr before-call-2}
                        {incr notifications}
                        {incr asdf}
                        {incr notifications}
                        {del asdf}
                        {incr notifications}
                        {incr after-call-2}
                        {incr notifications}
                        {incr timer-nested-middle}
                        {incrby timer-nested-end 1}
                        {exec}
                    }
                    close_replication_stream $repl

                    # Note propagate-test.timer-nested-repl just propagates INCRBY, causing an
                    # inconsistency, so we flush
                    $master flushall
                }

                test {module propagates from thread} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.thread

                    wait_for_condition 500 10 {
                        [$replica get a-from-thread] eq "3"
                    } else {
                        fail "The two counters don't match the expected value."
                    }

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr a-from-thread}
                        {incr notifications}
                        {incr thread-call}
                        {incr b-from-thread}
                        {exec}
                        {multi}
                        {incr a-from-thread}
                        {incr notifications}
                        {incr thread-call}
                        {incr b-from-thread}
                        {exec}
                        {multi}
                        {incr a-from-thread}
                        {incr notifications}
                        {incr thread-call}
                        {incr b-from-thread}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from thread with detached ctx} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.detached-thread

                    wait_for_condition 500 10 {
                        [$replica get thread-detached-after] eq "1"
                    } else {
                        fail "The key doesn't match the expected value."
                    }

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr thread-detached-before}
                        {incr notifications}
                        {incr thread-detached-1}
                        {incr notifications}
                        {incr thread-detached-2}
                        {incr thread-detached-after}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from command} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.simple
                    $master propagate-test.mixed

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr notifications}
                        {incr after-call}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from EVAL} {
                    set repl [attach_to_replication_stream]

                    assert_equal [ $master eval { \
                        redis.call("propagate-test.simple"); \
                        redis.call("set", "x", "y"); \
                        redis.call("propagate-test.mixed"); return "OK" } 0 ] {OK}

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr counter-1}
                        {incr counter-2}
                        {incr notifications}
                        {set x y}
                        {incr notifications}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr notifications}
                        {incr after-call}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from command after good EVAL} {
                    set repl [attach_to_replication_stream]

                    assert_equal [ $master eval { return "hello" } 0 ] {hello}
                    $master propagate-test.simple
                    $master propagate-test.mixed

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr notifications}
                        {incr after-call}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from command after bad EVAL} {
                    set repl [attach_to_replication_stream]

                    catch { $master eval { return "hello" } -12 } e
                    assert_equal $e {ERR Number of keys can't be negative}
                    $master propagate-test.simple
                    $master propagate-test.mixed

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr notifications}
                        {incr after-call}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from multi-exec} {
                    set repl [attach_to_replication_stream]

                    $master multi
                    $master propagate-test.simple
                    $master propagate-test.mixed
                    $master propagate-test.timer-nested-repl
                    $master exec

                    wait_for_condition 500 10 {
                        [$replica get timer-nested-end] eq "1"
                    } else {
                        fail "The two counters don't match the expected value."
                    }

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr counter-1}
                        {incr counter-2}
                        {incr notifications}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr notifications}
                        {incr after-call}
                        {exec}
                        {multi}
                        {incrby timer-nested-start 1}
                        {incr notifications}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr counter-3}
                        {incr counter-4}
                        {incr notifications}
                        {incr after-call}
                        {incr notifications}
                        {incr before-call-2}
                        {incr notifications}
                        {incr asdf}
                        {incr notifications}
                        {del asdf}
                        {incr notifications}
                        {incr after-call-2}
                        {incr notifications}
                        {incr timer-nested-middle}
                        {incrby timer-nested-end 1}
                        {exec}
                    }
                    close_replication_stream $repl

                   # Note propagate-test.timer-nested just propagates INCRBY, causing an
                    # inconsistency, so we flush
                    $master flushall
                }

                test {module RM_Call of expired key propagation} {
                    $master debug set-active-expire 0

                    $master set k1 900 px 100
                    after 110

                    set repl [attach_to_replication_stream]
                    $master propagate-test.incr k1

                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {del k1}
                        {propagate-test.incr k1}
                        {exec}
                    }
                    close_replication_stream $repl

                    assert_equal [$master get k1] 1
                    assert_equal [$master ttl k1] -1
                    assert_equal [$replica get k1] 1
                    assert_equal [$replica ttl k1] -1
                }

                test {module notification on set} {
                    set repl [attach_to_replication_stream]

                    $master SADD s foo

                    wait_for_condition 500 10 {
                        [$replica SCARD s] eq "1"
                    } else {
                        fail "Failed to wait for set to be replicated"
                    }

                    $master SPOP s 1

                    wait_for_condition 500 10 {
                        [$replica SCARD s] eq "0"
                    } else {
                        fail "Failed to wait for set to be replicated"
                    }

                    # Currently the `del` command comes after the notification.
                    # When we fix spop to fire notification at the end (like all other commands),
                    # the `del` will come first.
                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {incr notifications}
                        {sadd s foo}
                        {exec}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {del s}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module key miss notification do not cause read command to be replicated} {
                    set repl [attach_to_replication_stream]

                    $master flushall
                    
                    $master get unexisting_key

                    wait_for_condition 500 10 {
                        [$replica get missed] eq "1"
                    } else {
                        fail "Failed to wait for set to be replicated"
                    }

                    # Test is checking a wrong!!! behavior that causes a read command to be replicated to replica/aof.
                    # We keep the test to verify that such a wrong behavior does not cause any crashes.
                    assert_replication_stream $repl {
                        {select *}
                        {flushall}
                        {multi}
                        {incr notifications}
                        {incr missed}
                        {get unexisting_key}
                        {exec}
                    }
                    
                    close_replication_stream $repl
                }

                test "Unload the module - propagate-test/testkeyspace" {
                    assert_equal {OK} [r module unload propagate-test]
                    assert_equal {OK} [r module unload testkeyspace]
                }

                assert_equal [s -1 unexpected_error_replies] 0
            }
        }
    }
}


tags "modules aof" {
    foreach aofload_type {debug_cmd startup} {
    test "Modules RM_Replicate replicates MULTI/EXEC correctly: AOF-load type $aofload_type" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            # Enable the AOF
            r config set appendonly yes
            r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
            waitForBgrewriteaof r

            r propagate-test.simple
            r propagate-test.mixed
            r multi
            r propagate-test.simple
            r propagate-test.mixed
            r exec

            assert_equal [r get counter-1] {}
            assert_equal [r get counter-2] {}
            assert_equal [r get using-call] 2
            assert_equal [r get after-call] 2
            assert_equal [r get notifications] 4

            # Load the AOF
            if {$aofload_type == "debug_cmd"} {
                r debug loadaof
            } else {
                r config rewrite
                restart_server 0 true false
                wait_done_loading r
            }

            # This module behaves bad on purpose, it only calls
            # RM_Replicate for counter-1 and counter-2 so values
            # after AOF-load are different
            assert_equal [r get counter-1] 4
            assert_equal [r get counter-2] 4
            assert_equal [r get using-call] 2
            assert_equal [r get after-call] 2
            # 4+4+2+2 commands from AOF (just above) + 4 "INCR notifications" from AOF + 4 notifications for these INCRs
            assert_equal [r get notifications] 20

            assert_equal {OK} [r module unload propagate-test]
            assert_equal [s 0 unexpected_error_replies] 0
        }
    }
    test "Modules RM_Call does not update stats during aof load: AOF-load type $aofload_type" {
        start_server [list overrides [list loadmodule "$miscmodule"]] {
            # Enable the AOF
            r config set appendonly yes
            r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
            waitForBgrewriteaof r
            
            r config resetstat
            r set foo bar
            r EVAL {return redis.call('SET', KEYS[1], ARGV[1])} 1 foo bar2
            r test.rm_call_replicate set foo bar3
            r EVAL {return redis.call('test.rm_call_replicate',ARGV[1],KEYS[1],ARGV[2])} 1 foo set bar4
            
            r multi
            r set foo bar5
            r EVAL {return redis.call('SET', KEYS[1], ARGV[1])} 1 foo bar6
            r test.rm_call_replicate set foo bar7
            r EVAL {return redis.call('test.rm_call_replicate',ARGV[1],KEYS[1],ARGV[2])} 1 foo set bar8
            r exec

            assert_match {*calls=8,*,rejected_calls=0,failed_calls=0} [cmdrstat set r]
            
            
            # Load the AOF
            if {$aofload_type == "debug_cmd"} {
                r config resetstat
                r debug loadaof
            } else {
                r config rewrite
                restart_server 0 true false
                wait_done_loading r
            }
            
            assert_no_match {*calls=*} [cmdrstat set r]
            
        }
    }
    }
}

# This test does not really test module functionality, but rather uses a module
# command to test Redis replication mechanisms.
test {Replicas that was marked as CLIENT_CLOSE_ASAP should not keep the replication backlog from been trimmed} {
    start_server [list overrides [list loadmodule "$testmodule"]] {
        set replica [srv 0 client]
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]
            $master config set client-output-buffer-limit "replica 10mb 5mb 0"

            # Start the replication process...
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            test {module propagates from timer} {
                # Replicate large commands to make the replica disconnected.
                $master write [format_command propagate-test.verbatim 100000 [string repeat "a" 1000]] ;# almost 100mb
                # Execute this command together with module commands within the same
                # event loop to prevent periodic cleanup of replication backlog.
                $master write [format_command info memory]
                $master flush
                $master read ;# propagate-test.verbatim
                set res [$master read] ;# info memory

                # Wait for the replica to be disconnected.
                wait_for_log_messages 0 {"*flags=S*scheduled to be closed ASAP for overcoming of output buffer limits*"} 0 1500 10
                # Due to the replica reaching the soft limit (5MB), memory peaks should not significantly
                # exceed the replica soft limit. Furthermore, as the replica release its reference to
                # replication backlog, it should be properly trimmed, the memory usage of replication
                # backlog should not significantly exceed repl-backlog-size (default 1MB). */
                assert_lessthan [getInfoProperty $res used_memory_peak] 10000000;# less than 10mb
                assert_lessthan [getInfoProperty $res mem_replication_backlog] 2000000;# less than 2mb
            }
        }
    }
}
