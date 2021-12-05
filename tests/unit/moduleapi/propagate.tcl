set testmodule [file normalize tests/modules/propagate.so]

tags "modules" {
    test {Modules can propagate in async and threaded contexts} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set master [srv 0 client]
                set master_host [srv 0 host]
                set master_port [srv 0 port]

                # Start the replication process...
                $replica replicaof $master_host $master_port
                wait_for_sync $replica
                after 1000

                test {module propagates from timer} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer

                    wait_for_condition 5000 10 {
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
                    wait_for_ofs_sync $master $replica

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
                    wait_for_ofs_sync $master $replica

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

                    wait_for_condition 5000 10 {
                        [$replica keys asdf*] eq {}
                    } else {
                        fail "Not all keys have expired"
                    }

                    # Note whenever there's double notification: for for "set" and one for "expire"
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
                        {incr notifications}
                        {del asdf*}
                        {incr notifications}
                        {del asdf*}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates nested ctx case1} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer-nested

                    wait_for_condition 5000 10 {
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
                    wait_for_ofs_sync $master $replica
                }

                test {module propagates nested ctx case2} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.timer-nested-repl

                    wait_for_condition 5000 10 {
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
                        {incr timer-nested-middle}
                        {incrby timer-nested-end 1}
                        {exec}
                    }
                    close_replication_stream $repl

                    # Note propagate-test.timer-nested-repl just propagates INCRBY, causing an
                    # inconsistency, so we flush
                    $master flushall
                    wait_for_ofs_sync $master $replica
                }

                test {module propagates from thread} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.thread

                    wait_for_condition 5000 10 {
                        [$replica get a-from-thread] eq "3"
                    } else {
                        fail "The two counters don't match the expected value."
                    }

                    assert_replication_stream $repl {
                        {select *}
                        {incr a-from-thread}
                        {incr b-from-thread}
                        {incr a-from-thread}
                        {incr b-from-thread}
                        {incr a-from-thread}
                        {incr b-from-thread}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from command} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.simple
                    $master propagate-test.mixed

                    # Note the 'after-call' propagation below is out of order (known limitation)
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

                    wait_for_condition 5000 10 {
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
                        {incr timer-nested-middle}
                        {incrby timer-nested-end 1}
                        {exec}
                    }
                    close_replication_stream $repl

                   # Note propagate-test.timer-nested just propagates INCRBY, causing an
                    # inconsistency, so we flush
                    $master flushall
                    wait_for_ofs_sync $master $replica
                }

                test {module RM_Call of expired key propagation} {
                    $master debug set-active-expire 0

                    $master set k1 900 px 100
                    wait_for_ofs_sync $master $replica
                    after 110

                    set repl [attach_to_replication_stream]
                    $master propagate-test.incr k1
                    wait_for_ofs_sync $master $replica

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

            assert_equal [s 0 unexpected_error_replies] 0
        }
    }
}
