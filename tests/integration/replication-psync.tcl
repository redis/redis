# Creates a primary-slave pair and breaks the link continuously to force
# partial resyncs attempts, all this while flooding the primary with
# write queries.
#
# You can specify backlog size, ttl, delay before reconnection, test duration
# in seconds, and an additional condition to verify at the end.
#
# If reconnect is > 0, the test actually try to break the connection and
# reconnect with the primary, otherwise just the initial synchronization is
# checked for consistency.
proc test_psync {descr duration backlog_size backlog_ttl delay cond mdl sdl reconnect} {
    start_server {tags {"repl"}} {
        start_server {} {

            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set slave [srv 0 client]

            $primary config set repl-backlog-size $backlog_size
            $primary config set repl-backlog-ttl $backlog_ttl
            $primary config set repl-diskless-sync $mdl
            $primary config set repl-diskless-sync-delay 1
            $slave config set repl-diskless-load $sdl

            set load_handle0 [start_bg_complex_data $primary_host $primary_port 9 100000]
            set load_handle1 [start_bg_complex_data $primary_host $primary_port 11 100000]
            set load_handle2 [start_bg_complex_data $primary_host $primary_port 12 100000]

            test {Slave should be able to synchronize with the primary} {
                $slave slaveof $primary_host $primary_port
                wait_for_condition 50 100 {
                    [lindex [r role] 0] eq {slave} &&
                    [lindex [r role] 3] eq {connected}
                } else {
                    fail "Replication not started."
                }
            }

            # Check that the background clients are actually writing.
            test {Detect write load to primary} {
                wait_for_condition 50 1000 {
                    [$primary dbsize] > 100
                } else {
                    fail "Can't detect write load from background clients."
                }
            }

            test "Test replication partial resync: $descr (diskless: $mdl, $sdl, reconnect: $reconnect)" {
                # Now while the clients are writing data, break the maste-slave
                # link multiple times.
                if ($reconnect) {
                    for {set j 0} {$j < $duration*10} {incr j} {
                        after 100
                        # catch {puts "MASTER [$primary dbsize] keys, REPLICA [$slave dbsize] keys"}

                        if {($j % 20) == 0} {
                            catch {
                                if {$delay} {
                                    $slave multi
                                    $slave client kill $primary_host:$primary_port
                                    $slave debug sleep $delay
                                    $slave exec
                                } else {
                                    $slave client kill $primary_host:$primary_port
                                }
                            }
                        }
                    }
                }
                stop_bg_complex_data $load_handle0
                stop_bg_complex_data $load_handle1
                stop_bg_complex_data $load_handle2

                # Wait for the slave to reach the "online"
                # state from the POV of the primary.
                set retry 5000
                while {$retry} {
                    set info [$primary info]
                    if {[string match {*slave0:*state=online*} $info]} {
                        break
                    } else {
                        incr retry -1
                        after 100
                    }
                }
                if {$retry == 0} {
                    error "assertion:Slave not correctly synchronized"
                }

                # Wait that slave acknowledge it is online so
                # we are sure that DBSIZE and DEBUG DIGEST will not
                # fail because of timing issues. (-LOADING error)
                wait_for_condition 5000 100 {
                    [lindex [$slave role] 3] eq {connected}
                } else {
                    fail "Slave still not connected after some time"
                }  

                set retry 10
                while {$retry && ([$primary debug digest] ne [$slave debug digest])}\
                {
                    after 1000
                    incr retry -1
                }
                assert {[$primary dbsize] > 0}

                if {[$primary debug digest] ne [$slave debug digest]} {
                    set csv1 [csvdump r]
                    set csv2 [csvdump {r -1}]
                    set fd [open /tmp/repldump1.txt w]
                    puts -nonewline $fd $csv1
                    close $fd
                    set fd [open /tmp/repldump2.txt w]
                    puts -nonewline $fd $csv2
                    close $fd
                    puts "Primary - Replica inconsistency"
                    puts "Run diff -u against /tmp/repldump*.txt for more info"
                }
                assert_equal [r debug digest] [r -1 debug digest]
                eval $cond
            }
        }
    }
}

foreach mdl {no yes} {
    foreach sdl {disabled swapdb} {
        test_psync {no reconnection, just sync} 6 1000000 3600 0 {
        } $mdl $sdl 0

        test_psync {ok psync} 6 100000000 3600 0 {
        assert {[s -1 sync_partial_ok] > 0}
        } $mdl $sdl 1

        test_psync {no backlog} 6 100 3600 0.5 {
        assert {[s -1 sync_partial_err] > 0}
        } $mdl $sdl 1

        test_psync {ok after delay} 3 100000000 3600 3 {
        assert {[s -1 sync_partial_ok] > 0}
        } $mdl $sdl 1

        test_psync {backlog expired} 3 100000000 1 3 {
        assert {[s -1 sync_partial_err] > 0}
        } $mdl $sdl 1
    }
}
