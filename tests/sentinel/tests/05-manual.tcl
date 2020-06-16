# Test manual failover

source "../tests/includes/init-tests.tcl"

test "Manual failover works" {
    set old_port [RI $primary_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary]
    assert {[lindex $addr 1] == $old_port}
    catch {S 0 SENTINEL FAILOVER myprimary} reply
    assert {$reply eq "OK"}
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME myprimary] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }
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

