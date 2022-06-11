set testmodule [file normalize tests/modules/propagate.so]
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {incr notifications}
                        {incr notifications}
                        {incr testkeyspace:expired}
                        {del asdf*}
                        {incr notifications}
                        {incr notifications}
                        {incr testkeyspace:expired}
                        {del asdf*}
                        {incr notifications}
                        {incr notifications}
                        {incr testkeyspace:expired}
                        {del asdf*}
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

                    wait_for_condition 500 10 {
                        [$replica dbsize] eq 1
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
                        {select *}
                        {multi}
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
                        {incr notifications}
                        {del asdf*}
                        {incr notifications}
                        {del asdf*}
                        {incr notifications}
                        {del asdf*}
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
                        {select *}
                        {multi}
                        {incr notifications}
                        {incr notifications}
                        {set timer-maxmemory-volatile-start 1 PXAT *}
                        {incr timer-maxmemory-middle}
                        {incr notifications}
                        {incr notifications}
                        {set timer-maxmemory-volatile-end 1 PXAT *}
                        {exec}
                        {incr notifications}
                        {del timer-maxmemory-volatile-*}
                        {incr notifications}
                        {del timer-maxmemory-volatile-*}
                    }
                    close_replication_stream $repl

                    $master config set maxmemory 0
                    $master config set maxmemory-policy noeviction
                }

                test {module propagation with timer and EVAL} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer-eval

                    assert_replication_stream $repl {
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {select *}
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
                        {select *}
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
                        {select *}
                        {multi}
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
    test {Modules RM_Replicate replicates MULTI/EXEC correctly} {
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

            # Load the AOF
            r debug loadaof

            assert_equal {OK} [r module unload propagate-test]
            assert_equal [s 0 unexpected_error_replies] 0
        }
    }
}
