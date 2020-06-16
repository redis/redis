# Test Sentinel configuration consistency after partitions heal.

source "../tests/includes/init-tests.tcl"

test "We can failover with Sentinel 1 crashed" {
    set old_port [RI $primary_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    assert {[lindex $addr 1] == $old_port}

    # Crash Sentinel 1
    kill_instance sentinel 1

    kill_instance redis $primary_id
    foreach_sentinel_id id {
        if {$id != 1} {
            wait_for_condition 1000 50 {
                [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME myprimary] 1] != $old_port
            } else {
                fail "Sentinel $id did not receive failover info"
            }
        }
    }
    restart_instance redis $primary_id
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    set primary_id [get_instance_id_by_port redis [lindex $addr 1]]
}

test "After Sentinel 1 is restarted, its config gets updated" {
    restart_instance sentinel 1
    wait_for_condition 1000 50 {
        [lindex [S 1 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary] 1] != $old_port
    } else {
        fail "Restarted Sentinel did not receive failover info"
    }
}

test "New primary [join $addr {:}] role matches" {
    assert {[RI $primary_id role] eq {primary}}
}
