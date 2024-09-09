# Test Sentinel configuration consistency after partitions heal.

source "../tests/includes/init-tests.tcl"

test "We can failover with Sentinel 1 crashed" {
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}

    # Crash Sentinel 1
    kill_instance sentinel 1

    kill_instance redis $master_id
    foreach_sentinel_id id {
        if {$id != 1} {
            wait_for_condition 1000 50 {
                [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
            } else {
                fail "Sentinel $id did not receive failover info"
            }
        }
    }
    restart_instance redis $master_id
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]
}

test "After Sentinel 1 is restarted, its config gets updated" {
    restart_instance sentinel 1
    wait_for_condition 1000 50 {
        [lindex [S 1 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
    } else {
        fail "Restarted Sentinel did not receive failover info"
    }
}

test "New master [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "Update log level" {
    set current_loglevel [S 0 SENTINEL CONFIG GET loglevel]
    assert {[lindex $current_loglevel 1] == {notice}}

    foreach {loglevel} {debug verbose notice warning nothing} {
        S 0 SENTINEL CONFIG SET loglevel $loglevel
        set updated_loglevel [S 0 SENTINEL CONFIG GET loglevel]
        assert {[lindex $updated_loglevel 1] == $loglevel}
    }
}
