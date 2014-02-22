# Test Sentinel configuration consistency after partitions heal.

source "../sentinel-tests/includes/init-tests.tcl"

test "We can failover with Sentinel 1 crashed" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster down-after-milliseconds 2000
    }

    set old_port [RI $master_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}

    # Crash Sentinel 1
    kill_instance sentinel 1

    R $master_id debug sleep 10
    foreach_sentinel_id id {
        if {$id != 1} {
            wait_for_condition 1000 50 {
                [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
            } else {
                fail "Sentinel $id did not received failover info"
            }
        }
    }
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]
}

test "After Sentinel 1 is restarted, its config gets updated" {
    restart_instance sentinel 1
    wait_for_condition 1000 50 {
        [lindex [S 1 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
    } else {
        fail "Restarted Sentinel did not received failover info"
    }
}
