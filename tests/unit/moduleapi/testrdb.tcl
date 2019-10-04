set testmodule [file normalize tests/modules/testrdb.so]

proc restart_and_wait {} {
    catch {
        r debug restart
    }

    # wait for the server to come back up
    set retry 50
    while {$retry} {
        if {[catch { r ping }]} {
            after 100
        } else {
            break
        }
        incr retry -1
    }
}

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {
        test {modules are able to persist types} {
            r testrdb.set.key key1 value1
            assert_equal "value1" [r testrdb.get.key key1]
            r debug reload
            assert_equal "value1" [r testrdb.get.key key1]
        }

        test {modules global are lost without aux} {
            r testrdb.set.before global1
            assert_equal "global1" [r testrdb.get.before]
            restart_and_wait
            assert_equal "" [r testrdb.get.before]
        }
    }

    start_server [list overrides [list loadmodule "$testmodule 2"]] {
        test {modules are able to persist globals before and after} {
            r testrdb.set.before global1
            r testrdb.set.after global2
            assert_equal "global1" [r testrdb.get.before]
            assert_equal "global2" [r testrdb.get.after]
            restart_and_wait
            assert_equal "global1" [r testrdb.get.before]
            assert_equal "global2" [r testrdb.get.after]
        }

    }

    start_server [list overrides [list loadmodule "$testmodule 1"]] {
        test {modules are able to persist globals just after} {
            r testrdb.set.after global2
            assert_equal "global2" [r testrdb.get.after]
            restart_and_wait
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
                    for {set k 0} {$k < 30} {incr k} {
                        r testrdb.set.key key$k [string repeat A [expr {int(rand()*1000000)}]]
                    }

                    # Start the replication process...
                    $master config set repl-diskless-sync-delay 0
                    $replica replicaof $master_host $master_port

                    # kill the replication at various points
                    set attempts 3
                    if {$::accurate} { set attempts 10 }
                    for {set i 0} {$i < $attempts} {incr i} {
                        # wait for the replica to start reading the rdb
                        # using the log file since the replica only responds to INFO once in 2mb
                        wait_for_log_message -1 "*Loading DB in memory*" 5 2000 1

                        # add some additional random sleep so that we kill the master on a different place each time
                        after [expr {int(rand()*100)}]

                        # kill the replica connection on the master
                        set killed [$master client kill type replica]

                        if {[catch {
                            set res [wait_for_log_message -1 "*Internal error in RDB*" 5 100 10]
                            if {$::verbose} {
                                puts $res
                            }
                        }]} {
                            puts "failed triggering short read"
                            # force the replica to try another full sync
                            $master client kill type replica
                            $master set asdf asdf
                            # the side effect of resizing the backlog is that it is flushed (16k is the min size)
                            $master config set repl-backlog-size [expr {16384 + $i}]
                        }
                        # wait for loading to stop (fail)
                        wait_for_condition 100 10 {
                            [s -1 loading] eq 0
                        } else {
                            fail "Replica didn't disconnect"
                        }
                    }
                    # enable fast shutdown
                    $master config set rdb-key-save-delay 0
                }
            }
        }
    }
}
