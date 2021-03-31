set testmodule [file normalize tests/modules/propagate.so]

tags "modules" {
    test {Modules can propagate in async and threaded contexts} {
        start_server {} {
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
                        {multi}
                        {incr timer}
                        {exec}
                        {multi}
                        {incr timer}
                        {exec}
                        {multi}
                        {incr timer}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates nested ctx case1} {
                    set repl [attach_to_replication_stream]

                    $master del timer-nested-start
                    $master del timer-nested-end
                    $master propagate-test.timer-nested

                    wait_for_condition 5000 10 {
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
                }

                test {module propagates nested ctx case2} {
                    set repl [attach_to_replication_stream]

                    $master del timer-nested-start
                    $master del timer-nested-end
                    $master propagate-test.timer-nested-repl

                    wait_for_condition 5000 10 {
                        [$replica get timer-nested-end] eq "1"
                    } else {
                        fail "The two counters don't match the expected value."
                    }

                    # Note the 'after-call' and 'timer-nested-start' propagation below is out of order (known limitation)
                    assert_replication_stream $repl {
                        {select *}
                        {multi}
                        {incr using-call}
                        {incr counter-1}
                        {incr counter-2}
                        {incr after-call}
                        {incr counter-3}
                        {incr counter-4}
                        {incrby timer-nested-start 1}
                        {incrby timer-nested-end 1}
                        {exec}
                    }
                    close_replication_stream $repl
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

                test {module propagates from from command} {
                    set repl [attach_to_replication_stream]

                    $master propagate-test.simple
                    $master propagate-test.mixed

                    # Note the 'after-call' propagation below is out of order (known limitation)
                    assert_replication_stream $repl {
                        {select *}
                        {multi}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                        {multi}
                        {incr using-call}
                        {incr after-call}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from from command after good EVAL} {
                    set repl [attach_to_replication_stream]

                    assert_equal [ $master eval { return "hello" } 0 ] {hello}
                    $master propagate-test.simple
                    $master propagate-test.mixed

                    # Note the 'after-call' propagation below is out of order (known limitation)
                    assert_replication_stream $repl {
                        {select *}
                        {multi}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                        {multi}
                        {incr using-call}
                        {incr after-call}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from from command after bad EVAL} {
                    set repl [attach_to_replication_stream]

                    catch { $master eval { return "hello" } -12 } e
                    assert_equal $e {ERR Number of keys can't be negative}
                    $master propagate-test.simple
                    $master propagate-test.mixed

                    # Note the 'after-call' propagation below is out of order (known limitation)
                    assert_replication_stream $repl {
                        {select *}
                        {multi}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                        {multi}
                        {incr using-call}
                        {incr after-call}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                    }
                    close_replication_stream $repl
                }

                test {module propagates from from multi-exec} {
                    set repl [attach_to_replication_stream]

                    $master multi
                    $master propagate-test.simple
                    $master propagate-test.mixed
                    $master exec
                    wait_for_ofs_sync $master $replica

                    # Note the 'after-call' propagation below is out of order (known limitation)
                    assert_replication_stream $repl {
                        {select *}
                        {multi}
                        {incr counter-1}
                        {incr counter-2}
                        {incr using-call}
                        {incr after-call}
                        {incr counter-1}
                        {incr counter-2}
                        {exec}
                    }
                    close_replication_stream $repl
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
