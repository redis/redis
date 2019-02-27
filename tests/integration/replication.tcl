proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

start_server {tags {"repl"}} {
    set slave [srv 0 client]
    set slave_host [srv 0 host]
    set slave_port [srv 0 port]
    set slave_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        # Configure the master in order to hang waiting for the BGSAVE
        # operation, so that the slave remains in the handshake state.
        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 1000

        # Use a short replication timeout on the slave, so that if there
        # are no bugs the timeout is triggered in a reasonable amount
        # of time.
        $slave config set repl-timeout 5

        # Start the replication process...
        $slave slaveof $master_host $master_port

        test {Slave enters handshake} {
            wait_for_condition 50 1000 {
                [string match *handshake* [$slave role]]
            } else {
                fail "Replica does not enter handshake state"
            }
        }

        # But make the master unable to send
        # the periodic newlines to refresh the connection. The slave
        # should detect the timeout.
        $master debug sleep 10

        test {Slave is able to detect timeout during handshake} {
            wait_for_condition 50 1000 {
                [log_file_matches $slave_log "*Timeout connecting to the MASTER*"]
            } else {
                fail "Replica is not able to detect timeout"
            }
        }
    }
}

start_server {tags {"repl"}} {
    set A [srv 0 client]
    set A_host [srv 0 host]
    set A_port [srv 0 port]
    start_server {} {
        set B [srv 0 client]
        set B_host [srv 0 host]
        set B_port [srv 0 port]

        test {Set instance A as slave of B} {
            $A slaveof $B_host $B_port
            wait_for_condition 50 100 {
                [lindex [$A role] 0] eq {slave} &&
                [string match {*master_link_status:up*} [$A info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }
        }

        test {BRPOPLPUSH replication, when blocking against empty list} {
            set rd [redis_deferring_client]
            $rd brpoplpush a b 5
            r lpush a foo
            wait_for_condition 50 100 {
                [$A debug digest] eq [$B debug digest]
            } else {
                fail "Master and replica have different digest: [$A debug digest] VS [$B debug digest]"
            }
        }

        test {BRPOPLPUSH replication, list exists} {
            set rd [redis_deferring_client]
            r lpush c 1
            r lpush c 2
            r lpush c 3
            $rd brpoplpush c d 5
            after 1000
            assert_equal [$A debug digest] [$B debug digest]
        }

        test {BLPOP followed by role change, issue #2473} {
            set rd [redis_deferring_client]
            $rd blpop foo 0 ; # Block while B is a master

            # Turn B into master of A
            $A slaveof no one
            $B slaveof $A_host $A_port
            wait_for_condition 50 100 {
                [lindex [$B role] 0] eq {slave} &&
                [string match {*master_link_status:up*} [$B info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }

            # Push elements into the "foo" list of the new replica.
            # If the client is still attached to the instance, we'll get
            # a desync between the two instances.
            $A rpush foo a b c
            after 100

            wait_for_condition 50 100 {
                [$A debug digest] eq [$B debug digest] &&
                [$A lrange foo 0 -1] eq {a b c} &&
                [$B lrange foo 0 -1] eq {a b c}
            } else {
                fail "Master and replica have different digest: [$A debug digest] VS [$B debug digest]"
            }
        }
    }
}

start_server {tags {"repl"}} {
    r set mykey foo

    start_server {} {
        test {Second server should have role master at first} {
            s role
        } {master}

        test {SLAVEOF should start with link status "down"} {
            r slaveof [srv -1 host] [srv -1 port]
            s master_link_status
        } {down}

        test {The role should immediately be changed to "replica"} {
            s role
        } {slave}

        wait_for_sync r
        test {Sync should have transferred keys from master} {
            r get mykey
        } {foo}

        test {The link status should be up} {
            s master_link_status
        } {up}

        test {SET on the master should immediately propagate} {
            r -1 set mykey bar

            wait_for_condition 500 100 {
                [r  0 get mykey] eq {bar}
            } else {
                fail "SET on master did not propagated on replica"
            }
        }

        test {FLUSHALL should replicate} {
            r -1 flushall
            if {$::valgrind} {after 2000}
            list [r -1 dbsize] [r 0 dbsize]
        } {0 0}

        test {ROLE in master reports master with a slave} {
            set res [r -1 role]
            lassign $res role offset slaves
            assert {$role eq {master}}
            assert {$offset > 0}
            assert {[llength $slaves] == 1}
            lassign [lindex $slaves 0] master_host master_port slave_offset
            assert {$slave_offset <= $offset}
        }

        test {ROLE in slave reports slave in connected state} {
            set res [r role]
            lassign $res role master_host master_port slave_state slave_offset
            assert {$role eq {slave}}
            assert {$slave_state eq {connected}}
        }
    }
}

foreach dl {no yes} {
    start_server {tags {"repl"}} {
        set master [srv 0 client]
        $master config set repl-diskless-sync $dl
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set slaves {}
        set load_handle0 [start_write_load $master_host $master_port 3]
        set load_handle1 [start_write_load $master_host $master_port 5]
        set load_handle2 [start_write_load $master_host $master_port 20]
        set load_handle3 [start_write_load $master_host $master_port 8]
        set load_handle4 [start_write_load $master_host $master_port 4]
        start_server {} {
            lappend slaves [srv 0 client]
            start_server {} {
                lappend slaves [srv 0 client]
                start_server {} {
                    lappend slaves [srv 0 client]
                    test "Connect multiple replicas at the same time (issue #141), diskless=$dl" {
                        # Send SLAVEOF commands to slaves
                        [lindex $slaves 0] slaveof $master_host $master_port
                        [lindex $slaves 1] slaveof $master_host $master_port
                        [lindex $slaves 2] slaveof $master_host $master_port

                        # Wait for all the three slaves to reach the "online"
                        # state from the POV of the master.
                        set retry 500
                        while {$retry} {
                            set info [r -3 info]
                            if {[string match {*slave0:*state=online*slave1:*state=online*slave2:*state=online*} $info]} {
                                break
                            } else {
                                incr retry -1
                                after 100
                            }
                        }
                        if {$retry == 0} {
                            error "assertion:Replicas not correctly synchronized"
                        }

                        # Wait that slaves acknowledge they are online so
                        # we are sure that DBSIZE and DEBUG DIGEST will not
                        # fail because of timing issues.
                        wait_for_condition 500 100 {
                            [lindex [[lindex $slaves 0] role] 3] eq {connected} &&
                            [lindex [[lindex $slaves 1] role] 3] eq {connected} &&
                            [lindex [[lindex $slaves 2] role] 3] eq {connected}
                        } else {
                            fail "Replicas still not connected after some time"
                        }

                        # Stop the write load
                        stop_write_load $load_handle0
                        stop_write_load $load_handle1
                        stop_write_load $load_handle2
                        stop_write_load $load_handle3
                        stop_write_load $load_handle4

                        # Make sure that slaves and master have same
                        # number of keys
                        wait_for_condition 500 100 {
                            [$master dbsize] == [[lindex $slaves 0] dbsize] &&
                            [$master dbsize] == [[lindex $slaves 1] dbsize] &&
                            [$master dbsize] == [[lindex $slaves 2] dbsize]
                        } else {
                            fail "Different number of keys between masted and replica after too long time."
                        }

                        # Check digests
                        set digest [$master debug digest]
                        set digest0 [[lindex $slaves 0] debug digest]
                        set digest1 [[lindex $slaves 1] debug digest]
                        set digest2 [[lindex $slaves 2] debug digest]
                        assert {$digest ne 0000000000000000000000000000000000000000}
                        assert {$digest eq $digest0}
                        assert {$digest eq $digest1}
                        assert {$digest eq $digest2}
                    }
               }
            }
        }
    }
}

start_server {tags {"repl"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set load_handle0 [start_write_load $master_host $master_port 3]
    start_server {} {
        test "Master stream is correctly processed while the replica has a script in -BUSY state" {
            set slave [srv 0 client]
            $slave config set lua-time-limit 500
            $slave slaveof $master_host $master_port

            # Wait for the slave to be online
            wait_for_condition 500 100 {
                [lindex [$slave role] 3] eq {connected}
            } else {
                fail "Replica still not connected after some time"
            }

            # Wait some time to make sure the master is sending data
            # to the slave.
            after 5000

            # Stop the ability of the slave to process data by sendig
            # a script that will put it in BUSY state.
            $slave eval {for i=1,3000000000 do end} 0

            # Wait some time again so that more master stream will
            # be processed.
            after 2000

            # Stop the write load
            stop_write_load $load_handle0

            # number of keys
            wait_for_condition 500 100 {
                [$master debug digest] eq [$slave debug digest]
            } else {
                fail "Different datasets between replica and master"
            }
        }
    }
}
