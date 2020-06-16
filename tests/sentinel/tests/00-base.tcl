# Check the basic monitoring and failover capabilities.

source "../tests/includes/init-tests.tcl"

if {$::simulate_error} {
    test "This test will fail" {
        fail "Simulated error"
    }
}

test "Basic failover works if the primary is down" {
    set old_port [RI $primary_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    assert {[lindex $addr 1] == $old_port}
    kill_instance redis $primary_id
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME myprimary] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }
    restart_instance redis $primary_id
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    set primary_id [get_instance_id_by_port redis [lindex $addr 1]]
}

test "New primary [join $addr {:}] role matches" {
    assert {[RI $primary_id role] eq {primary}}
}

test "All the other slaves now point to the new primary" {
    foreach_redis_id id {
        if {$id != $primary_id && $id != 0} {
            wait_for_condition 1000 50 {
                [RI $id primary_port] == [lindex $addr 1]
            } else {
                fail "Redis ID $id not configured to replicate with new primary"
            }
        }
    }
}

test "The old primary eventually gets reconfigured as a slave" {
    wait_for_condition 1000 50 {
        [RI 0 primary_port] == [lindex $addr 1]
    } else {
        fail "Old primary not reconfigured as slave of new primary"
    }
}

test "ODOWN is not possible without N (quorum) Sentinels reports" {
    foreach_sentinel_id id {
        S $id SENTINEL SET myprimary quorum [expr $sentinels+1]
    }
    set old_port [RI $primary_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    assert {[lindex $addr 1] == $old_port}
    kill_instance redis $primary_id

    # Make sure failover did not happened.
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    assert {[lindex $addr 1] == $old_port}
    restart_instance redis $primary_id
}

test "Failover is not possible without majority agreement" {
    foreach_sentinel_id id {
        S $id SENTINEL SET myprimary quorum $quorum
    }

    # Crash majority of sentinels
    for {set id 0} {$id < $quorum} {incr id} {
        kill_instance sentinel $id
    }

    # Kill the current primary
    kill_instance redis $primary_id

    # Make sure failover did not happened.
    set addr [S $quorum SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    assert {[lindex $addr 1] == $old_port}
    restart_instance redis $primary_id

    # Cleanup: restart Sentinels to monitor the primary.
    for {set id 0} {$id < $quorum} {incr id} {
        restart_instance sentinel $id
    }
}

test "Failover works if we configure for absolute agreement" {
    foreach_sentinel_id id {
        S $id SENTINEL SET myprimary quorum $sentinels
    }

    # Wait for Sentinels to monitor the primary again
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER myprimary] info-refresh] < 100000
        } else {
            fail "At least one Sentinel is not monitoring the primary"
        }
    }

    kill_instance redis $primary_id

    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME myprimary] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }
    restart_instance redis $primary_id
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    set primary_id [get_instance_id_by_port redis [lindex $addr 1]]

    # Set the min ODOWN agreement back to strict majority.
    foreach_sentinel_id id {
        S $id SENTINEL SET myprimary quorum $quorum
    }
}

test "New primary [join $addr {:}] role matches" {
    assert {[RI $primary_id role] eq {primary}}
}
