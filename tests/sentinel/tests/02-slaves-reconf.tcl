# Check that slaves are reconfigured at a latter time if they are partitioned.
#
# Here we should test:
# 1) That slaves point to the new master after failover.
# 2) That partitioned slaves point to new master when they are partitioned
#    away during failover and return at a latter time.

source "../tests/includes/init-tests.tcl"

proc 02_test_slaves_replication {} {
    uplevel 1 {
        test "Check that slaves replicate from current master" {
            set master_port [RPort $master_id]
            foreach_redis_id id {
                if {$id == $master_id} continue
                if {[instance_is_killed redis $id]} continue
                wait_for_condition 1000 50 {
                    ([RI $id master_port] == $master_port) &&
                    ([RI $id master_link_status] eq {up})
                } else {
                    fail "Redis slave $id is replicating from wrong master"
                }
            }
        }
    }
}

proc 02_crash_and_failover {} {
    uplevel 1 {
        test "Crash the master and force a failover" {
            set old_port [RPort $master_id]
            set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
            assert {[lindex $addr 1] == $old_port}
            kill_instance redis $master_id
            foreach_sentinel_id id {
                wait_for_condition 1000 50 {
                    [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
                } else {
                    fail "At least one Sentinel did not receive failover info"
                }
            }
            restart_instance redis $master_id
            set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
            set master_id [get_instance_id_by_port redis [lindex $addr 1]]
        }
    }
}

02_test_slaves_replication
02_crash_and_failover

foreach_sentinel_id id {
    S $id sentinel debug info-period 100
    S $id sentinel debug default-down-after 1000
    S $id sentinel debug publish-period 100
}

02_test_slaves_replication

test "Kill a slave instance" {
    foreach_redis_id id {
        if {$id == $master_id} continue
        set killed_slave_id $id
        kill_instance redis $id
        break
    }
}

02_crash_and_failover
02_test_slaves_replication

test "Wait for failover to end" {
    set inprogress 1
    while {$inprogress} {
        set inprogress 0
        foreach_sentinel_id id {
            if {[dict exists [S $id SENTINEL MASTER mymaster] failover-state]} {
                incr inprogress
            }
        }
        if {$inprogress} {after 100}
    }
}

test "Restart killed slave and test replication of slaves again..." {
    restart_instance redis $killed_slave_id
}

# Now we check if the slave rejoining the partition is reconfigured even
# if the failover finished.
02_test_slaves_replication
