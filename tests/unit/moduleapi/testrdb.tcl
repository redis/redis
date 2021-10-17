set testmodule [file normalize tests/modules/testrdb.so]

tags "modules" {
    test {modules are able to persist types} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r testrdb.set.key key1 value1
            assert_equal "value1" [r testrdb.get.key key1]
            r debug reload
            assert_equal "value1" [r testrdb.get.key key1]
        }
    }

    test {modules global are lost without aux} {
        set server_path [tmpdir "server.module-testrdb"]
        start_server [list overrides [list loadmodule "$testmodule" "dir" $server_path] keep_persistence true] {
            r testrdb.set.before global1
            assert_equal "global1" [r testrdb.get.before]
        }
        start_server [list overrides [list loadmodule "$testmodule" "dir" $server_path]] {
            assert_equal "" [r testrdb.get.before]
        }
    }

    test {modules are able to persist globals before and after} {
        set server_path [tmpdir "server.module-testrdb"]
        start_server [list overrides [list loadmodule "$testmodule 2" "dir" $server_path] keep_persistence true] {
            r testrdb.set.before global1
            r testrdb.set.after global2
            assert_equal "global1" [r testrdb.get.before]
            assert_equal "global2" [r testrdb.get.after]
        }
        start_server [list overrides [list loadmodule "$testmodule 2" "dir" $server_path]] {
            assert_equal "global1" [r testrdb.get.before]
            assert_equal "global2" [r testrdb.get.after]
        }

    }

    test {modules are able to persist globals just after} {
        set server_path [tmpdir "server.module-testrdb"]
        start_server [list overrides [list loadmodule "$testmodule 1" "dir" $server_path] keep_persistence true] {
            r testrdb.set.after global2
            assert_equal "global2" [r testrdb.get.after]
        }
        start_server [list overrides [list loadmodule "$testmodule 1" "dir" $server_path]] {
            assert_equal "global2" [r testrdb.get.after]
        }
    }

    tags {repl} {
        test {diskless loading short read with module} {
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set replica [srv 0 client]
                set replica_host [srv 0 host]
                set replica_port [srv 0 port]
                start_server [list overrides [list loadmodule "$testmodule"]] {
                    set master [srv 0 client]
                    set master_host [srv 0 host]
                    set master_port [srv 0 port]

                    # Set master and replica to use diskless replication
                    $master config set repl-diskless-sync yes
                    $master config set rdbcompression no
                    $replica config set repl-diskless-load swapdb
                    $master config set hz 500
                    $replica config set hz 500
                    $master config set dynamic-hz no
                    $replica config set dynamic-hz no
                    set start [clock clicks -milliseconds]
                    for {set k 0} {$k < 30} {incr k} {
                        r testrdb.set.key key$k [string repeat A [expr {int(rand()*1000000)}]]
                    }

                    if {$::verbose} {
                        set end [clock clicks -milliseconds]
                        set duration [expr $end - $start]
                        puts "filling took $duration ms (TODO: use pipeline)"
                        set start [clock clicks -milliseconds]
                    }

                    # Start the replication process...
                    set loglines [count_log_lines -1]
                    $master config set repl-diskless-sync-delay 0
                    $replica replicaof $master_host $master_port

                    # kill the replication at various points
                    set attempts 100
                    if {$::accurate} { set attempts 500 }
                    for {set i 0} {$i < $attempts} {incr i} {
                        # wait for the replica to start reading the rdb
                        # using the log file since the replica only responds to INFO once in 2mb
                        set res [wait_for_log_messages -1 {"*Loading DB in memory*"} $loglines 2000 1]
                        set loglines [lindex $res 1]

                        # add some additional random sleep so that we kill the master on a different place each time
                        after [expr {int(rand()*50)}]

                        # kill the replica connection on the master
                        set killed [$master client kill type replica]

                        set res [wait_for_log_messages -1 {"*Internal error in RDB*" "*Finished with success*" "*Successful partial resynchronization*"} $loglines 1000 1]
                        if {$::verbose} { puts $res }
                        set log_text [lindex $res 0]
                        set loglines [lindex $res 1]
                        if {![string match "*Internal error in RDB*" $log_text]} {
                            # force the replica to try another full sync
                            $master multi
                            $master client kill type replica
                            $master set asdf asdf
                            # the side effect of resizing the backlog is that it is flushed (16k is the min size)
                            $master config set repl-backlog-size [expr {16384 + $i}]
                            $master exec
                        }
                        # wait for loading to stop (fail)
                        wait_for_condition 1000 1 {
                            [s -1 loading] eq 0
                        } else {
                            fail "Replica didn't disconnect"
                        }
                    }
                    if {$::verbose} {
                        set end [clock clicks -milliseconds]
                        set duration [expr $end - $start]
                        puts "test took $duration ms"
                    }
                    # enable fast shutdown
                    $master config set rdb-key-save-delay 0
                }
            }
        }

        test {Diskless load swapdb, module can use RedisModuleEvent_ReplAsyncLoad CREATE and COMPLETED events on async_loading} {
            start_server [list overrides [list loadmodule "$testmodule 2"]] {
                set replica [srv 0 client]
                set replica_host [srv 0 host]
                set replica_port [srv 0 port]
                set replica_log [srv 0 stdout]
                start_server [list overrides [list loadmodule "$testmodule 2"]] {
                    set master [srv 0 client]
                    set master_host [srv 0 host]
                    set master_port [srv 0 port]

                    # Set master and replica to use diskless replication on swapdb mode
                    $master config set repl-diskless-sync yes
                    $master config set repl-diskless-sync-delay 0
                    $replica config set repl-diskless-load swapdb

                    # Set replica writable so we can check that a key we manually added is served during replication and disappears afterwards
                    $replica config set replica-read-only no

                    # replica logs
                    set loglines [count_log_lines -1]

                    # Initial sync to have matching replids between master and replica
                    $replica replicaof $master_host $master_port

                    # Let replica finish initial sync with master
                    wait_for_log_messages -1 {"*MASTER <-> REPLICA sync: Finished with success*"} $loglines 50 100

                    # Set global value on module so we can check if module event callbacks in replica will pick it up
                    $master testrdb.set.before value1_master
                    $replica testrdb.set.before value1_replica

                    # Put different data sets on the master and replica
                    # We need to put large keys on the master since the replica replies to info only once in 2mb
                    $replica debug populate 2000 slave 10
                    $master debug populate 1000 master 100000
                    $master config set rdbcompression no

                    # Force the replica to try another full sync (this time it will have matching master replid)
                    $master multi
                    $master client kill type replica
                    $master set asdf asdf
                    # the side effect of resizing the backlog is that it is flushed (16k is the min size)
                    $master config set repl-backlog-size 16384
                    $master exec

                    # Set master with a slow rdb generation, so that we can easily intercept loading
                    # 10ms per key, with 1000 keys is 5 seconds
                    $master config set rdb-key-save-delay 5000
                    
                    # Wait for the replica to start reading the rdb
                    wait_for_condition 100 100 {
                        [s -1 async_loading] eq 1
                    } else {
                        fail "Replica didn't get into async_loading mode"
                    }

                    # Check that classic loading flag is NOT set to 1
                    assert_equal [s -1 loading] 0
                    
                    # Check that after REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED, module global still has original replica value
                    assert_equal value1_replica [$replica testrdb.get.before]

                    # Make sure we're still async_loading to validate previous assertion
                    assert_equal [s -1 async_loading] 1

                    # Wait for loading to stop
                    wait_for_condition 50 100 {
                        [s -1 async_loading] eq 0
                    } else {
                        fail "Loading didn't stop"
                    }

                    # Check that after REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_COMPLETED, module key got value from master
                    assert_equal value1_master [$replica testrdb.get.before]

                    # Enable fast shutdown
                    $master config set rdb-key-save-delay 0

                    # Make sure amount of keys matches master
                    assert_equal [$replica dbsize] 1001
                }
            }
        } {} {external:skip}

        test {Diskless load swapdb, module can use RedisModuleEvent_ReplAsyncLoad ABORTED event on aborted async_loading} {
            start_server [list overrides [list loadmodule "$testmodule 2"]] {
                set replica [srv 0 client]
                set replica_host [srv 0 host]
                set replica_port [srv 0 port]
                set replica_log [srv 0 stdout]
                start_server [list overrides [list loadmodule "$testmodule 2"]] {
                    set master [srv 0 client]
                    set master_host [srv 0 host]
                    set master_port [srv 0 port]

                    # Put different data sets on the master and slave
                    # we need to put large keys on the master since the slave replies to info only once in 2mb
                    $replica debug populate 2000 slave 10
                    $master debug populate 800 master 100000
                    $master config set rdbcompression no

                    # Set master and slave to use diskless replication
                    $master config set repl-diskless-sync yes
                    $master config set repl-diskless-sync-delay 0
                    $replica config set repl-diskless-load swapdb

                    # Set a value on module so we can check if module event callback is triggered correctly
                    $replica testrdb.set.before global1

                    # Set master with a slow rdb generation, so that we can easily disconnect it mid sync
                    # 10ms per key, with 800 keys is 8 seconds
                    $master config set rdb-key-save-delay 10000

                    set loglines [count_log_lines -1]

                    # Start the replication process...
                    $replica replicaof $master_host $master_port

                    # wait for the replica to start reading the rdb
                    wait_for_condition 50 100 {
                        [s -1 loading] eq 1
                    } else {
                        fail "Replica didn't get into loading mode"
                    }

                    # make sure that next sync will not start immediately so that we can catch the replica in between syncs
                    $master config set repl-diskless-sync-delay 5
                    # for faster server shutdown, make rdb saving fast again (the fork is already uses the slow one)
                    $master config set rdb-key-save-delay 0

                    # waiting replica to start loading
                    wait_for_log_messages -1 {"*Loading DB in memory*"} $loglines 50 100

                    # make sure we're still loading
                    assert_equal [s -1 loading] 1

                    # kill the replica connection on the master
                    set killed [$master client kill type slave]

                    # wait for loading to stop (fail)
                    wait_for_condition 50 100 {
                        [s -1 loading] eq 0
                    } else {
                        fail "Replica didn't disconnect"
                    }

                    # make sure the original keys haven't changed
                    assert_equal [$replica dbsize] 2000

                    # Check that after REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED, module variable was is maintained
                    assert_equal global1 [$replica testrdb.get.before]
                }
            }
        } {} {external:skip}
    }
}
