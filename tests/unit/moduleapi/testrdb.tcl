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
                            # fill replication backlog with new content
                            $master config set repl-backlog-size 16384
                            for {set keyid 0} {$keyid < 10} {incr keyid} {
                                $master set "$keyid string_$keyid" [string repeat A 16384]
                            }
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

        foreach state {ABORTED COMPLETED} {
            test "Diskless load swapdb, module can use RedisModuleEvent_ReplAsyncLoad events on $state async_loading" {
                start_server [list overrides [list loadmodule "$testmodule 2"]] {
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
                        $replica config set repl-diskless-load swapdb

                        # Replica logs
                        set loglines [count_log_lines -1]

                        # Initial sync to have matching replids between master and replica
                        $replica replicaof $master_host $master_port

                        # Let replica finish initial sync with master
                        wait_for_log_messages -1 {"*MASTER <-> REPLICA sync: Finished with success*"} $loglines 100 100

                        # Set global values on module so we can check if module event callbacks will pick it up correctly
                        $master testrdb.set.before value1_master
                        $replica testrdb.set.before value1_replica

                        # Put different data sets on the master and replica
                        # We need to put large keys on the master since the replica replies to info only once in 2mb
                        $replica debug populate 2000 slave 10
                        $master debug populate 500 master 100000
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

                        # Set master with a slow rdb generation, so that we can easily intercept loading
                        # 5ms per key, with 500 keys is 2.5 seconds
                        $master config set rdb-key-save-delay 5000

                        # Wait for the replica to start reading the rdb
                        wait_for_condition 100 100 {
                            [s -1 async_loading] eq 1
                        } else {
                            fail "Replica didn't get into async_loading mode"
                        }

                        if {$state == "ABORTED"} {
                            # Kill the replica connection on the master
                            set killed [$master client kill type replica]
                        }

                        # Speed things up
                        $master config set rdb-key-save-delay 0

                        # Wait for loading to stop (when ABORTED, stop fast)
                        wait_for_condition 100 100 {
                            [s -1 async_loading] eq 0
                        } else {
                            if {$state == "ABORTED"}
                                fail "Replica didn't disconnect"
                            else
                                fail "Loading didn't stop"
                        }

                        if {$state == "ABORTED"} {
                            # Module variable is same as before
                            assert_equal [$replica dbsize] 2000
                            assert_equal value1_replica [$replica testrdb.get.before]
                        } elseif {$state == "COMPLETED"} {
                            # Module variable got value from master
                            assert_equal [$replica dbsize] 510
                            assert_equal value1_master [$replica testrdb.get.before]
                        }

                        if {$::verbose} {
                            set end [clock clicks -milliseconds]
                            set duration [expr $end - $start]
                            puts "test took $duration ms"
                        }
                    }
                }
            } {} {external:skip}
        }
    }
}
