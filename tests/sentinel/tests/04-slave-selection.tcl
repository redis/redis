# Test slave selection algorithm.
#
# This unit should test:
# 1) That when there are no suitable slaves no failover is performed.
# 2) That among the available slaves, the one with better offset is picked.

# Pass the flag as a global variable to the init script
set ::setup_second_master 1
source ../tests/includes/init-tests.tcl
# Now we have two master-slave clusters. In this test we will mainly operate
# on the second master.

set master_a_id $master_id
set master_a_name "mymaster"
set master_b_id 5
set master_b_name "another_master"
set slave_id [expr $master_b_id + 1]
set slave_port [RPort $slave_id]

foreach_sentinel_id id {
    S $id sentinel debug ping-period 500
    S $id sentinel debug ask-period 500
    S $id sentinel debug info-period 500
    S $id sentinel debug default-down-after 1000

    # This is kinda hacky. The sentinels will periodically check
    # whether any slave now follows a different master, and if so,
    # convert it back via a +fix-slave-config event.
    # Here we make the failover-timeout (sentinel's wait time before
    # converting slaves back) very long to avoid such event.
    S $id SENTINEL SET $master_b_name failover-timeout 20000
}

proc change_master { slave_id new_master_id } {
    R $slave_id slaveof [get_instance_attrib redis $new_master_id host] \
                        [get_instance_attrib redis $new_master_id port]
}

test "Cannot failover when there's no good slave" {
    # Put a simple string into the database
    R $master_b_id SET "mykey" "myvalue"

    # This cluster has only one slave. Let's reconfigure the slave to
    # follow the default master instead, so that it will update its
    # replication ID.
    change_master $slave_id $master_a_id

    # The default master had 4 slaves. Now it should discover this new slave
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [llength [S $id SENTINEL replicas $master_a_name]] == 5
        } else {
            fail "mymaster should now have 5 slaves from sentinel $id's view"
        }
    }
    # The original data should be gone by now
    assert_equal [R $slave_id GET "mykey"] {}

    # We expect the manual failover to fail now that there is no
    # good slave to promote.
    set result [catch { S 0 SENTINEL FAILOVER $master_b_name } err]
    if {$result == 0 || [string match "*NOGOODSLAVE*" $err] == 0} {
        fail "Manual failover did not error with NOGOODSLAVE. Instead, it got: $err"
    }
}

test "Failover should work now that the slave's replication ID is reverted back" {
    # Reconfigure the slave again to bring the slave back to the original
    # master to revert its replication ID.
    change_master $slave_id $master_b_id

    # This time the failover should succeed.
    kill_instance redis $master_b_id
    foreach_sentinel_id id {
        wait_for_condition 200 100 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME $master_b_name] 1] == $slave_port
        } else {
            fail "Sentinel $id did not see the new master"
        }
    }

    # The new master should contain the original data
    assert_equal [R $slave_id GET "mykey"] "myvalue"
}