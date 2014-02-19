test "Sentinels aren't monitoring any master" {
    foreach_sentinel_id id {
        assert {[S $id sentinel masters] eq {}}
    }
}

set redis_slaves 4
test "Create a master-slaves cluster of [expr $redis_slaves+1] instances" {
    create_redis_master_slave_cluster [expr {$redis_slaves+1}]
}
set master_id 0

test "Sentinels can start monitoring a master" {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR mymaster \
              [get_instance_attrib redis $master_id host] \
              [get_instance_attrib redis $master_id port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
    }
}

test "Sentinels are able to auto-discover other sentinels" {
    set sentinels [llength $::sentinel_instances]
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [dict get [S $id SENTINEL MASTER mymaster] num-other-sentinels] == ($sentinels-1)
        } else {
            fail "At least some sentinel can't detect some other sentinel"
        }
    }
}

test "Sentinels are able to auto-discover slaves" {
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [dict get [S $id SENTINEL MASTER mymaster] num-slaves] == $redis_slaves
        } else {
            fail "At least some sentinel can't detect some slave"
        }
    }
}

test "Can change master parameters via SENTINEL SET" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster down-after-milliseconds 2000
    }
    foreach_sentinel_id id {
        assert {[dict get [S $id sentinel master mymaster] down-after-milliseconds] == 2000}
    }
}

test "Basic failover works if the master is down" {
    set old_port [RI $master_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    R $master_id debug sleep 5
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not received failover info"
        }
    }
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]
}

test "New master [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "All the other slaves now point to the new master" {
    foreach_redis_id id {
        if {$id != $master_id && $id != 0} {
            wait_for_condition 1000 50 {
                [RI $id master_port] == [lindex $addr 1]
            } else {
                fail "Redis ID $id not configured to replicate with new master"
            }
        }
    }
}

test "The old master eventually gets reconfigured as a slave" {
    wait_for_condition 1000 50 {
        [RI 0 master_port] == [lindex $addr 1]
    } else {
        fail "Old master not reconfigured as slave of new master"
    }
}

test "ODOWN is not possible without enough Sentinels reports" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum [expr $sentinels+1]
    }
    set old_port [RI $master_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    R $master_id debug sleep 5

    # Make sure failover did not happened.
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
}

test "Failover is not possible without majority agreement" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum $quorum
    }

    # Make majority of sentinels stop monitoring the master
    for {set id 0} {$id < $quorum} {incr id} {
        S $id SENTINEL REMOVE mymaster
    }
    R $master_id debug sleep 5

    # Make sure failover did not happened.
    set addr [S $quorum SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}

    # Cleanup: reconfigure the Sentinels to monitor the master.
    for {set id 0} {$id < $quorum} {incr id} {
        S $id SENTINEL MONITOR mymaster \
              [get_instance_attrib redis $master_id host] \
              [get_instance_attrib redis $master_id port] $quorum
    }
}

test "Failover works if we configure for absolute agreement" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum $sentinels
    }

    # Wait for Sentinels to monitor the master again
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER mymaster] info-refresh] < 100000
        } else {
            fail "At least one Sentinel is not monitoring the master"
        }
    }

    R $master_id debug sleep 5
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not received failover info"
        }
    }
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]

    # Set the min ODOWN agreement back to strict majority.
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum $quorum
    }
}

test "New master [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

