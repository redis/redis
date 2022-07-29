# Check that slaves are reconfigured at a latter time if they are partitioned.
#
# Here we should test:
# 1) That slaves point to the new master after failover.
# 2) That partitioned slaves point to new master when they are partitioned
#    away during failover and return at a latter time.

source "../tests/includes/init-tests.tcl"

set ::user "testuser"
set ::password "secret"

proc server_set_password {} {
    foreach_redis_id id {
        assert_equal {OK} [R $id CONFIG SET requirepass $::password]
        assert_equal {OK} [R $id AUTH $::password]
        assert_equal {OK} [R $id CONFIG SET masterauth $::password]
    }
}

proc server_reset_password {} {
    foreach_redis_id id {
        assert_equal {OK} [R $id CONFIG SET requirepass ""]
        assert_equal {OK} [R $id CONFIG SET masterauth ""]
    }
}


proc verify_sentinel_connect_replicas {id} {
    foreach replica [S $id SENTINEL REPLICAS mymaster] {
        if {[string match "*disconnected*" [dict get $replica flags]]} {
            return 0
        }
    }
    return 1
}

proc wait_for_sentinels_connect_servers { {is_connect 1} } {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [string match "*disconnected*" [dict get [S $id SENTINEL MASTER mymaster] flags]] != $is_connect
        } else {
            fail "At least some sentinel can't connect to master"
        }

        wait_for_condition 1000 50 {
            [verify_sentinel_connect_replicas $id] == $is_connect
        } else {
            fail "At least some sentinel can't connect to replica"
        }
    }
}

test "Sentinels (re)connection following SENTINEL SET mymaster auth-pass" {
    # 3 types of sentinels to test:
    # (re)started while master changed pwd. Manage to connect only after setting pwd
    set sent2re 0
    # (up)dated in advance with master new password
    set sent2up 1
    # (un)touched. Yet manage to maintain (old) connection
    set sent2un 2

    wait_for_sentinels_connect_servers
    kill_instance sentinel $sent2re
    assert_equal {OK} [S $sent2up SENTINEL SET mymaster auth-pass $::password]
    server_set_password
    restart_instance sentinel $sent2re

    # Verify sentinel that restarted failed to connect master
    if {![string match "*disconnected*" [dict get [S $sent2re SENTINEL MASTER mymaster] flags]]} {
       fail "Expected to be disconnected from master due to wrong password"
    }

    # Update restarted sentinel with master password
    assert_equal {OK} [S $sent2re SENTINEL SET mymaster auth-pass $::password]

    # All sentinels expected to connect successfully
    wait_for_sentinels_connect_servers

    # remove requirepass and verify sentinels manage to connect servers
    server_reset_password
    wait_for_sentinels_connect_servers
    # Sanity check
    verify_sentinel_auto_discovery
}



proc 02_test_slaves_replication {} {
    uplevel 1 {
        test "Check that slaves replicate from current master" {
            set master_port [RI $master_id tcp_port]
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
            set old_port [RI $master_id tcp_port]
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
