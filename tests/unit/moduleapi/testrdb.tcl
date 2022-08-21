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

    test {Verify module options info} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            assert_match "*\[handle-io-errors|handle-repl-async-load\]*" [r info modules]
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

                        set res [wait_for_log_messages -1 {"*Internal error in RDB*" "*Finished with success*" "*Successful partial resynchronization*"} $loglines 500 10]
                        if {$::verbose} { puts $res }
                        set log_text [lindex $res 0]
                        set loglines [lindex $res 1]
                        if {![string match "*Internal error in RDB*" $log_text]} {
                            # force the replica to try another full sync
                            $master multi
                            $master client kill type replica
                            $master set asdf asdf
                            # fill replication backlog with new content
                            $master config set repl-backlog-size 16384
                            for {set keyid 0} {$keyid < 10} {incr keyid} {
                                $master set "$keyid string_$keyid" [string repeat A 16384]
                            }
                            $master exec
                        }

                        # wait for loading to stop (fail)
                        # After a loading successfully, next loop will enter `async_loading`
                        wait_for_condition 1000 1 {
                            [s -1 async_loading] eq 0 &&
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

        # Module events for diskless load swapdb when async_loading (matching master replid)
        foreach testType {Successful Aborted} {
            start_server [list overrides [list loadmodule "$testmodule 2"] tags [list external:skip]] {
                set replica [srv 0 client]
                set replica_host [srv 0 host]
                set replica_port [srv 0 port]
                set replica_log [srv 0 stdout]
                start_server [list overrides [list loadmodule "$testmodule 2"]] {
                    set master [srv 0 client]
                    set master_host [srv 0 host]
                    set master_port [srv 0 port]

                    set start [clock clicks -milliseconds]

                    # Set master and replica to use diskless replication on swapdb mode
                    $master config set repl-diskless-sync yes
                    $master config set repl-diskless-sync-delay 0
                    $master config set save ""
                    $replica config set repl-diskless-load swapdb
                    $replica config set save "" 

                    # Initial sync to have matching replids between master and replica
                    $replica replicaof $master_host $master_port

                    # Let replica finish initial sync with master
                    wait_for_condition 100 100 {
                        [s -1 master_link_status] eq "up"
                    } else {
                        fail "Master <-> Replica didn't finish sync"
                    }

                    # Set global values on module so we can check if module event callbacks will pick it up correctly
                    $master testrdb.set.before value1_master
                    $replica testrdb.set.before value1_replica

                    # Put different data sets on the master and replica
                    # We need to put large keys on the master since the replica replies to info only once in 2mb
                    $replica debug populate 200 slave 10
                    $master debug populate 1000 master 100000
                    $master config set rdbcompression no

                    # Force the replica to try another full sync (this time it will have matching master replid)
                    $master multi
                    $master client kill type replica
                    # Fill replication backlog with new content
                    $master config set repl-backlog-size 16384
                    for {set keyid 0} {$keyid < 10} {incr keyid} {
                        $master set "$keyid string_$keyid" [string repeat A 16384]
                    }
                    $master exec

                    switch $testType {
                        "Aborted" {
                            # Set master with a slow rdb generation, so that we can easily intercept loading
                            # 10ms per key, with 1000 keys is 10 seconds
                            $master config set rdb-key-save-delay 10000

                            test {Diskless load swapdb RedisModuleEvent_ReplAsyncLoad handling: during loading, can keep module variable same as before} {
                                # Wait for the replica to start reading the rdb and module for acknowledgement
                                # We wanna abort only after the temp db was populated by REDISMODULE_AUX_BEFORE_RDB
                                wait_for_condition 100 100 {
                                    [s -1 async_loading] eq 1 && [$replica testrdb.async_loading.get.before] eq "value1_master"
                                } else {
                                    fail "Module didn't receive or react to REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED"
                                }

                                assert_equal [$replica dbsize] 200
                                assert_equal value1_replica [$replica testrdb.get.before]
                            }

                            # Make sure that next sync will not start immediately so that we can catch the replica in between syncs
                            $master config set repl-diskless-sync-delay 5

                            # Kill the replica connection on the master
                            set killed [$master client kill type replica]

                            test {Diskless load swapdb RedisModuleEvent_ReplAsyncLoad handling: when loading aborted, can keep module variable same as before} {
                                # Wait for loading to stop (fail) and module for acknowledgement
                                wait_for_condition 100 100 {
                                    [s -1 async_loading] eq 0 && [$replica testrdb.async_loading.get.before] eq ""
                                } else {
                                    fail "Module didn't receive or react to REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED"
                                }

                                assert_equal [$replica dbsize] 200
                                assert_equal value1_replica [$replica testrdb.get.before]
                            }

                            # Speed up shutdown
                            $master config set rdb-key-save-delay 0
                        }
                        "Successful" {
                            # Let replica finish sync with master
                            wait_for_condition 100 100 {
                                [s -1 master_link_status] eq "up"
                            } else {
                                fail "Master <-> Replica didn't finish sync"
                            }

                            test {Diskless load swapdb RedisModuleEvent_ReplAsyncLoad handling: after db loaded, can set module variable with new value} {
                                assert_equal [$replica dbsize] 1010
                                assert_equal value1_master [$replica testrdb.get.before]
                            }
                        }
                    }

                    if {$::verbose} {
                        set end [clock clicks -milliseconds]
                        set duration [expr $end - $start]
                        puts "test took $duration ms"
                    }
                }
            }
        }
    }
}
