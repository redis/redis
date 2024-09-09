# Test manual failover

source "../tests/includes/init-tests.tcl"

foreach_sentinel_id id {
    S $id sentinel debug info-period 2000
    S $id sentinel debug default-down-after 6000
    S $id sentinel debug publish-period 1000
}

test "Manual failover works" {
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}

    # Since we reduced the info-period (default 10000) above immediately,
    # sentinel - replica may not have enough time to exchange INFO and update
    # the replica's info-period, so the test may get a NOGOODSLAVE.
    wait_for_condition 300 50 {
        [catch {S 0 SENTINEL FAILOVER mymaster}] == 0
    } else {
        catch {S 0 SENTINEL FAILOVER mymaster} reply
        puts [S 0 SENTINEL REPLICAS mymaster]
        fail "Sentinel manual failover did not work, got: $reply"
    }

    catch {S 0 SENTINEL FAILOVER mymaster} reply
    assert_match {*INPROG*} $reply ;# Failover already in progress

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

foreach flag {crash-after-election crash-after-promotion} {
    # Before each SIMULATE-FAILURE test, re-source init-tests to get a clean environment
    source "../tests/includes/init-tests.tcl"

    test "SENTINEL SIMULATE-FAILURE $flag works" {
        assert_equal {OK} [S 0 SENTINEL SIMULATE-FAILURE $flag]

        # Trigger a failover, failover will trigger leader election, replica promotion
        # Sentinel may enter failover and exit before the command, catch it and allow it
        wait_for_condition 300 50 {
            [catch {S 0 SENTINEL FAILOVER mymaster}] == 0
            ||
            ([catch {S 0 SENTINEL FAILOVER mymaster} reply] == 1 &&
            [string match {*couldn't open socket: connection refused*} $reply])
        } else {
            catch {S 0 SENTINEL FAILOVER mymaster} reply
            fail "Sentinel manual failover did not work, got: $reply"
        }

        # Wait for sentinel to exit (due to simulate-failure flags)
        wait_for_condition 1000 50 {
            [catch {S 0 PING}] == 1
        } else {
            fail "Sentinel set $flag but did not exit"
        }
        assert_error {*couldn't open socket: connection refused*} {S 0 PING}

        restart_instance sentinel 0
    }
}
