# Test manual safe failover

source "../tests/includes/init-tests.tcl"

foreach_sentinel_id id {
    S $id sentinel debug info-period 2000
    S $id sentinel debug default-down-after 6000
    S $id sentinel debug publish-period 1000
}

set loop_counter 0

test "Manual safe failover works" {
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}

    # Enable the repl-read-only configuration of the master node.
    R $master_id config set replica-read-only yes

    R $master_id set counter 0

    # Perform a safe failover.
    catch {S 0 SENTINEL FAILOVER mymaster safe} reply
    assert {$reply eq "OK"}

    while {1} {
        catch {R $master_id incr counter} reply
        if {[string match "*READONLY*" $reply]} {
            break
        }
        incr loop_counter
    }

    set old_master_counter [R $master_id get counter]
    assert {$old_master_counter == $loop_counter}

    # Wait for all Sentinel nodes to update the master node information.
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }

    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]
}

test "safe failover: Check data consistency" {
    set master_counter [R $master_id get counter]
    foreach_redis_id id {
        if {$id != $master_id} {
            set slave_counter [R $id get counter]
            assert {$slave_counter == $master_counter}
        }
    }
}

test "safe failover: New master [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "safe failover: All the other slaves now point to the new master" {
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

test "safe failover: The old master eventually gets reconfigured as a slave" {
    wait_for_condition 1000 50 {
        [RI 0 master_port] == [lindex $addr 1]
    } else {
        fail "Old master not reconfigured as slave of new master"
    }
}